#include "trailer_manipulation.hpp"
#include "trailer_shared.hpp"
#include "trailer_coupling_internal.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <intrin.h>
#include <utility>
#include <vector>

#include "imgui.h"

#include "common/scssdk_telemetry_truck_common_channels.h"

#include "core.hpp"
#include "hooks/function_hook.hpp"
#include "memory/memory_utils.hpp"
#include "prism/common.hpp"
#include "prism/offsets.hpp"
#include "prism/functions.hpp"
#include "settings/settings.hpp"

namespace br_carretas
{

    tp_slot_t tp_log[ 16 ] = {};
    int tp_n = 0;
    bool tp_active = false;
    float3_t tp_snap[ max_trailers ] = {};
    quat_t tp_snap_q[ max_trailers ] = {};
    bool tp_snap_ok[ max_trailers ] = {};

    int tp_cooldown = 0;
    int tp_block_watch = 0;

    bool in_actor_tp = false;

    hook_xform_t anchor_xform[ max_trailers ]{};
    hook_xform_t anchor_home[ max_trailers ]{};
    bool anchor_valid[ max_trailers ] = {};
    int anchor_fixes[ max_trailers ] = {};
    bool anchor_settling[ max_trailers ] = {};
    int anchor_settle[ max_trailers ] = {};
    int anchor_age[ max_trailers ] = {};
    int anchor_delay[ max_trailers ] = {};

    void* anchor_prev_slave[ max_trailers ] = {};
    bool anchor_prev_slave_j[ max_trailers ] = {};
    bool record_tried[ max_trailers ] = {};
    int record_retry[ max_trailers ] = {};
    uint8_t record_quiet[ max_trailers ] = {};
    bool record_mgr_logged[ max_trailers ] = {};

    parked_record_factory_fn* parked_record_factory = nullptr;
    void** parked_record_mgr = nullptr;

    bool rig_armed = false;
    uint32_t rig_restore_delay = 0;
    hook_xform_t rig_truck_xform{};
    bool rig_truck_valid = false;
    uint8_t rig_replay_a3 = 0;
    uint8_t rig_replay_a4 = 0;

    bool rig_force_restore = false;

    // 119 - checkbox "Sistema de Desacoplamento" na aba Carretas
    void render_cargo_restore_option()
    {
        ImGui::SeparatorText( "Carga separada" );
        bool cargo_restore = settings::g_settings.cargo_restore;
        if ( ui::toggle_switch( "##cargo_restore", &cargo_restore ) )
        {
            settings::g_settings.cargo_restore = cargo_restore;
            settings::save();
        }
        ImGui::SameLine();
        ImGui::TextWrapped( "Sistema de Desacoplamento" );
        ImGui::TextWrapped(
            "Ative para usar o sistema de desacoplamento/acoplamento. Ligado, o jogo não teleporta "
            "na câmera 0 (Ctrl+F9) nem ao carregar/descarregar - as carretas separadas ficam presas "
            "no lugar. Desligado, engate, desengate, teleporte, carga e descarga voltam ao nativo do "
            "jogo. Para teleportar a outro lugar, desligue, teleporte e ligue de novo." );
    }

    // 120 - fotografa o transform do caminhão antes do teleporte
    void snapshot_rig()
    {
        rig_truck_valid = false;
        if ( auto* game_actor = CCore::g_instance->get_game_actor() )
        {
            void* truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
            rig_truck_valid = truck != nullptr && hook_world( truck, rig_truck_xform );
        }
    }

    using trailer_state_apply_fn = void __fastcall( void* /*record*/ );
    // 121 - hook: aplica o record de estado na carreta (doca)
    void __fastcall hk_trailer_state_apply( void* record )
    {

        if ( !settings::g_settings.cargo_restore || load_game_active() )
        {
            trailer_state_apply_hook->get_original< trailer_state_apply_fn >()( record );
            return;
        }
        void* trailer = record != nullptr
            ? prism::offsets::field< void* >( record, 0xB8 ) : nullptr;
        if ( trailer != nullptr && is_plugin_detached( trailer ) )
        {
            CCore::g_instance->info( "trailer state apply skipped: {:p} is plugin-detached", trailer );
            return;
        }

        if ( tp_cooldown > 0 ) return;
        trailer_state_apply_hook->get_original< trailer_state_apply_fn >()( record );
    }

    using trailer_teleport_fn = void __fastcall( void* /*trailer*/, void* /*transform*/ );
    // 122 - hook: teleporte por-carreta - recusa peça estacionada
    void __fastcall hk_trailer_teleport( void* self, void* transform )
    {

        if ( !settings::g_settings.cargo_restore || load_game_active() )
        {
            teleport_safe( self, transform );
            return;
        }

        if ( settings::g_settings.cargo_restore && self != nullptr && is_plugin_detached( self ) &&
             joint( self ) == nullptr && in_actor_tp )
        {
            CCore::g_instance->info( "trailer teleport skipped: {:p} is plugin-detached", self );
            return;
        }

        if ( self != nullptr && is_plugin_detached( self ) &&
             joint( self ) == nullptr )
        {
            const int ci = canon_index_of( self );
            if ( ci >= 0 && anchor_valid[ ci ] )
            {
                anchor_valid[ ci ] = false;
                anchor_settling[ ci ] = false;
                anchor_delay[ ci ] = 0;
            }
        }

        if ( self != nullptr && joint( self ) != nullptr )
        {
            prism::offsets::field< uint8_t >( self, prism::offsets::trailer_parked ) = 0;
            prism::offsets::field< uint8_t >( self, prism::offsets::trailer_parked_registered ) = 0;
        }

        if ( canon_age < 90 )
        {
            teleport_safe( self, transform );
            return;
        }
        const float* dst = static_cast< const float* >( transform );

        float3_t corrected{};
        bool do_fix = false;
        void* parent = self != nullptr
            ? prism::offsets::field< void* >( self, prism::offsets::trailer_parent ) : nullptr;
        if ( dst != nullptr && self != nullptr && joint( self ) != nullptr && tp_active )
        {
            const int ti = canon_index_of( self );
            const int pi = canon_index_of( parent );

            for ( int i = tp_n; i-- > 0; )
            {
                if ( tp_log[ i ].t != parent ) continue;
                const float ddx = dst[ 0 ] - tp_log[ i ].orig.x;
                const float ddz = dst[ 2 ] - tp_log[ i ].orig.z;

                const float hdx = dst[ 0 ] - tp_log[ 0 ].orig.x;
                const float hdz = dst[ 2 ] - tp_log[ 0 ].orig.z;
                if ( ( ddx * ddx + ddz * ddz < 1.f || hdx * hdx + hdz * hdz < 1.f ) &&
                     ti >= 0 && pi >= 0 && tp_snap_ok[ ti ] && tp_snap_ok[ pi ] )
                {

                    corrected = {
                        tp_log[ i ].dst.x + ( tp_snap[ ti ].x - tp_snap[ pi ].x ),
                        dst[ 1 ],
                        tp_log[ i ].dst.z + ( tp_snap[ ti ].z - tp_snap[ pi ].z ) };
                    do_fix = true;
                }
                break;
            }
        }
        if ( dst != nullptr && tp_active && tp_n < 16 )
        {
            tp_log[ tp_n ].t = self;
            tp_log[ tp_n ].orig = { dst[ 0 ], dst[ 1 ], dst[ 2 ] };
            tp_log[ tp_n ].dst = do_fix ? corrected : tp_log[ tp_n ].orig;
            ++tp_n;
        }

        if ( do_fix )
        {
            for ( uint32_t j = 0; j < max_trailers && canon[ j ] != nullptr; ++j )
            {
                void* other = canon[ j ];
                if ( other == self || !readable_ptr( other ) ) continue;
                hook_xform_t ox{};
                if ( !hook_world( other, ox ) ) continue;
                const float dx = ox.pos.x - corrected.x;
                const float dz = ox.pos.z - corrected.z;
                if ( dx * dx + dz * dz < 4.f * 4.f )
                {
                    CCore::g_instance->info( "trailer teleport {:p}: corrected slot occupied "
                        "by c{} ({:.1f}m) - keeping the game's slot", self, j,
                        std::sqrt( dx * dx + dz * dz ) );
                    do_fix = false;
                    break;
                }
            }
        }

        if ( do_fix )
        {
            hook_xform_t fix = *static_cast< const hook_xform_t* >( transform );
            fix.pos = corrected;
            teleport_safe( self, &fix );
            CCore::g_instance->info( "trailer teleport {:p}: re-seated at corrected slot "
                "({:.1f},{:.1f},{:.1f})", self, corrected.x, corrected.y, corrected.z );
            return;
        }
        teleport_safe( self, transform );
    }

    using actor_teleport_fn = void __fastcall( void* /*actor*/, void* /*arg*/, uint8_t, uint8_t );

    // 123 - assenta a formação após o teleporte de ator
    void settle_all_members()
    {
        for ( int i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
        {
            void* t = canon[ i ];

            if ( !readable_ptr( t ) || joint( t ) == nullptr || !trailer_is_live( t ) ) continue;

            prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked ) = 0;
            prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked_registered ) = 0;
            void* parent = prism::offsets::field< void* >( t, prism::offsets::trailer_parent );
            const int pi = canon_index_of( parent );
            if ( pi < 0 || !tp_snap_ok[ i ] || !tp_snap_ok[ pi ] ) continue;
            hook_xform_t pt{}, tt{};
            if ( !hook_world( parent, pt ) || !hook_world( t, tt ) ) continue;
            const float3_t world_delta{
                tp_snap[ i ].x - tp_snap[ pi ].x,
                tp_snap[ i ].y - tp_snap[ pi ].y,
                tp_snap[ i ].z - tp_snap[ pi ].z };
            const quat_t inverse_parent{
                tp_snap_q[ pi ].w, -tp_snap_q[ pi ].x,
                -tp_snap_q[ pi ].y, -tp_snap_q[ pi ].z };
            const float3_t local_delta = quat_rot( inverse_parent, world_delta );
            const float3_t rotated_delta = quat_rot( pt.quat, local_delta );
            const float wx = pt.pos.x + rotated_delta.x;
            const float wz = pt.pos.z + rotated_delta.z;
            const float ex = tt.pos.x - wx;
            const float ez = tt.pos.z - wz;
            if ( ex * ex + ez * ez < 0.5f * 0.5f ) continue;
            hook_xform_t fix = tt;
            fix.pos.x = wx;
            fix.pos.y = pt.pos.y;
            fix.pos.z = wz;
            using teleport_fn_t = void __fastcall( void*, void* );
            teleport_safe( t, &fix );
            CCore::g_instance->info(
                "teleport settle: {:p} re-seated at formation slot ({:.1f}m off)",
                t, std::sqrt( ex * ex + ez * ez ) );
        }
    }
    // 124 - hook: teleporte do ator (conjunto inteiro)
    void __fastcall hk_actor_teleport( void* self, void* arg, uint8_t a3, uint8_t a4 )
    {
        actor_tp_ms = steady_ms();

        if ( !settings::g_settings.cargo_restore || load_game_active() )
        {
            rig_armed = false;
            actor_teleport_hook->get_original< actor_teleport_fn >()( self, arg, a3, a4 );
            return;
        }

        if ( canon_age < 90 )
        {
            actor_teleport_hook->get_original< actor_teleport_fn >()( self, arg, a3, a4 );
            return;
        }
        if ( block_camera_zero_teleport( arg ) ) return;
        if ( arg != nullptr && readable_ptr( arg ) )
        {
            const auto* req = static_cast< const hook_xform_t* >( arg );
            hook_xform_t cur{};
            void* truck = nullptr;
            if ( auto* game_actor = CCore::g_instance->get_game_actor() )
                truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
            if ( truck != nullptr && hook_world( truck, cur ) )
            {
                const float dx = req->pos.x - cur.pos.x;
                const float dz = req->pos.z - cur.pos.z;
                const float dist = std::sqrt( dx * dx + dz * dz );
                const float dot = std::fabs( req->quat.w * cur.quat.w + req->quat.x * cur.quat.x +
                                             req->quat.y * cur.quat.y + req->quat.z * cur.quat.z );
                const float deg = 2.f * std::acos( dot > 1.f ? 1.f : dot ) * 57.29578f;

                if ( dist < 3000.f && settings::g_settings.cargo_restore )
                {
                    bool any_detached = false;
                    for ( void* d : plugin_detached )
                        if ( d != nullptr ) { any_detached = true; break; }
                    for ( bool f : detached_idx ) any_detached |= f;
                    if ( any_detached )
                    {
                        cargo_seq_ms = steady_ms();
                        cargo_respawn_done = false;
                    }
                }
                const bool block = settings::g_settings.cargo_restore && dist < 3000.f;
                CCore::g_instance->info(
                    "actor teleport request: {:.1f}m, {:.1f}deg turn, a3={} a4={} -> {}",
                    dist, deg, a3, a4, block ? "BLOCKED" : "passed" );
                dump_chain( "pre-actor-tp" );
                if ( block )
                {
                    tp_block_watch = 120;
                    carry_arm_tick = GetTickCount64();
                    return;
                }
            }
        }
        bool has_detached_slots = rig_armed;
        for ( bool detached : detached_idx ) has_detached_slots |= detached;

        if ( !settings::g_settings.cargo_restore || !has_detached_slots )
        {
            if ( !settings::g_settings.cargo_restore || load_game_active() ) rig_armed = false;
            if ( has_detached_slots ) sever_parked_claims();
            tp_n = 0;
            tp_active = true;
            for ( int i = 0; i < max_trailers; ++i )
            {
                tp_snap_ok[ i ] = false;
                if ( canon[ i ] == nullptr || !readable_ptr( canon[ i ] ) ) continue;
                hook_xform_t xf{};
                if ( hook_world( canon[ i ], xf ) ) { tp_snap[ i ] = xf.pos; tp_snap_q[ i ] = xf.quat; tp_snap_ok[ i ] = true; }
            }
            in_actor_tp = true;
            actor_teleport_hook->get_original< actor_teleport_fn >()( self, arg, a3, a4 );
            in_actor_tp = false;
            settle_all_members();
            restore_parked_claims();
            tp_active = false;
            tp_cooldown = 90;
            return;
        }

        tp_n = 0;

        tp_active = false;

        sever_parked_claims();

        for ( int i = 0; i < max_trailers; ++i )
        {
            tp_snap_ok[ i ] = false;
            if ( canon[ i ] == nullptr || !readable_ptr( canon[ i ] ) ) continue;
            hook_xform_t xf{};
            if ( hook_world( canon[ i ], xf ) )
            {
                tp_snap[ i ] = xf.pos;
                tp_snap_q[ i ] = xf.quat;
                tp_snap_ok[ i ] = true;
            }
        }

        const bool fresh = !rig_armed;
        if ( fresh )
        {
            snapshot_rig();
        }
        in_actor_tp = true;
        actor_teleport_hook->get_original< actor_teleport_fn >()( self, arg, a3, a4 );
        in_actor_tp = false;
        settle_all_members();
        restore_parked_claims();
        tp_active = false;
        tp_cooldown = 90;
        if ( !rig_truck_valid ) return;
        if ( auto* game_actor = CCore::g_instance->get_game_actor() )
        {
            void* truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
            hook_xform_t now{};
            if ( truck != nullptr && hook_world( truck, now ) )
            {
                const float dx = now.pos.x - rig_truck_xform.pos.x;
                const float dz = now.pos.z - rig_truck_xform.pos.z;

                bool any_detached = false;
                bool any_displaced = false;
                for ( int i = 0; i < max_trailers; ++i )
                {
                    if ( !detached_idx[ i ] ) continue;
                    any_detached = true;

                    void* d = canon[ i ];
                    if ( d == nullptr || !readable_ptr( d ) ) continue;
                    if ( joint( d ) != nullptr ) { any_displaced = true; break; }
                    if ( parked_xform_valid[ i ] )
                    {
                        hook_xform_t dx2{};
                        if ( hook_world( d, dx2 ) )
                        {
                            const float mx = dx2.pos.x - parked_xform[ i ].pos.x;
                            const float mz = dx2.pos.z - parked_xform[ i ].pos.z;
                            if ( mx * mx + mz * mz > 2.f * 2.f ) { any_displaced = true; break; }
                        }
                    }
                }
                if ( any_detached && any_displaced && dx * dx + dz * dz < 200.f * 200.f )
                {

                    tp_cooldown = 90;
                    if ( fresh )
                    {
                        rig_armed = true;
                        rig_restore_delay = 60;
                        rig_replay_a3 = a3;
                        rig_replay_a4 = a4;
                        CCore::g_instance->info( "actor teleported ~{:.1f}m - rig restore armed",
                            std::sqrt( dx * dx + dz * dz ) );
                    }
                }
            }
        }
    }

    std::pair< void*, void* > trailer_actors[ max_trailers ] = {};

    void* provider_vtbl = nullptr;

    // 125 - acha o objeto-ator da carreta
    void* find_trailer_actor( void* trailer, void* game_actor )
    {
        if ( trailer == nullptr || game_actor == nullptr ) return nullptr;
        for ( uint32_t s = 0; s < 16; ++s )
        {
            void* entry = reinterpret_cast< uint8_t* >( game_actor ) + 0x1D38 + s * 0x28;
            if ( !readable_ptr( reinterpret_cast< uint8_t* >( entry ) + 8 ) ) break;
            void* actor = *reinterpret_cast< void** >( entry );
            if ( !plausible_object( actor ) ) continue;
            for ( uint32_t bo = 8; bo < 0x400; bo += 8 )
            {
                if ( prism::offsets::field< void* >( actor, bo ) == trailer )
                {
                    return actor;
                }
            }
        }

        for ( uint32_t i = 0; i < trailer_count; ++i )
        {
            if ( trailers[ i ] != trailer ) continue;
            void* entry = reinterpret_cast< uint8_t* >( game_actor ) + 0x1D38 + i * 0x28;
            void* actor = *reinterpret_cast< void** >( entry );
            if ( plausible_object( actor ) )
            {
                return actor;
            }
            break;
        }

        for ( uint32_t i = 0; i < max_trailers; ++i )
        {
            if ( trailer_actors[ i ].first == trailer )
            {
                void* cached = trailer_actors[ i ].second;
                return plausible_object( cached ) ? cached : nullptr;
            }
        }

        if ( provider_vtbl != nullptr )
        {
            auto actor_at = [ & ]( void* obj ) -> void*
            {
                if ( obj == nullptr || !plausible_object( obj ) ||
                    !readable_ptr( reinterpret_cast< uint8_t* >( obj ) + 0x1F0 ) )
                {
                    return nullptr;
                }
                void* prov = prism::offsets::field< void* >( obj, 0x1F0 );
                return prov != nullptr && plausible_object( prov ) &&
                    *reinterpret_cast< void** >( prov ) == provider_vtbl ? obj : nullptr;
            };
            for ( uintptr_t off = 0x100; off < 0x2000; off += 8 )
            {
                if ( !readable_ptr( reinterpret_cast< uint8_t* >( trailer ) + off ) ) continue;
                if ( void* a = actor_at( prism::offsets::field< void* >( trailer, off ) ) )
                {
                    return a;
                }
            }
            for ( uintptr_t off = 0x1C00; off < 0x3000; off += 8 )
            {
                if ( !readable_ptr( reinterpret_cast< uint8_t* >( game_actor ) + off ) ) continue;
                void* a = actor_at( prism::offsets::field< void* >( game_actor, off ) );
                if ( a == nullptr ) continue;
                return a;
            }
        }
        return nullptr;
    }

    // 126 - amostra a vtable do provider de obstáculo
    void sample_provider_vtbl( void* game_actor )
    {
        if ( provider_vtbl != nullptr || game_actor == nullptr ) return;
        void* actor = prism::offsets::field< void* >( game_actor, 0x1D38 );
        if ( !plausible_object( actor ) ||
            !readable_ptr( reinterpret_cast< uint8_t* >( actor ) + 0x1F0 ) )
        {
            return;
        }
        void* provider = prism::offsets::field< void* >( actor, 0x1F0 );
        if ( provider != nullptr && plausible_object( provider ) &&
            plausible_vfunc( provider, 0x28 / 8 ) && plausible_vfunc( provider, 0x30 / 8 ) )
        {
            provider_vtbl = *reinterpret_cast< void** >( provider );
        }
    }

    // 127 - acha o provider de obstáculo na carreta
    void* find_trailer_provider( void* trailer )
    {
        if ( trailer == nullptr || provider_vtbl == nullptr ) return nullptr;
        auto scan = [ & ]( void* obj, uintptr_t end ) -> void*
        {
            if ( obj == nullptr ) return nullptr;
            for ( uintptr_t off = 0; off < end; off += 8 )
            {
                if ( !readable_ptr( reinterpret_cast< uint8_t* >( obj ) + off ) ) continue;
                void* cand = *reinterpret_cast< void** >( reinterpret_cast< uint8_t* >( obj ) + off );
                if ( cand != nullptr && plausible_object( cand ) &&
                    *reinterpret_cast< void** >( cand ) == provider_vtbl )
                {
                    return cand;
                }
            }
            return nullptr;
        };
        if ( void* p = scan( trailer, 0x1400 ) ) return p;
        return scan( prism::offsets::field< void* >( trailer, prism::offsets::trailer_parked_state ), 0x800 );
    }

    // 128 - cacheia carreta→ator
    void cache_trailer_actor( void* trailer, void* game_actor )
    {
        if ( trailer == nullptr ) return;
        sample_provider_vtbl( game_actor );
        void* actor = find_trailer_actor( trailer, game_actor );
        for ( uint32_t i = 0; i < max_trailers; ++i )
        {
            if ( trailer_actors[ i ].first == trailer || trailer_actors[ i ].first == nullptr )
            {
                trailer_actors[ i ] = { trailer, actor };
                return;
            }
        }
    }

    // 129 - filtro SEH da fábrica de parked-record
    int factory_fault_filter( uint32_t i, EXCEPTION_POINTERS* ep )
    {
        const auto rip = ep != nullptr && ep->ContextRecord != nullptr
            ? ep->ContextRecord->Rip : 0;
        const auto base = reinterpret_cast< uint64_t >( GetModuleHandle( nullptr ) );
        CCore::g_instance->warning(
            "parked record factory faulted (função 129) for c{} at exe+0x{:X} code=0x{:X}",
            i, rip >= base ? rip - base : rip,
            ep != nullptr && ep->ExceptionRecord != nullptr
                ? ep->ExceptionRecord->ExceptionCode : 0 );
        return EXCEPTION_EXECUTE_HANDLER;
    }

    // 130 - log do registro de parked do jogo
    void dump_native_registry()
    {
        void* mgr = readable_ptr( parked_record_mgr ) ? *parked_record_mgr : nullptr;
        CCore::g_instance->info( "  registry: global at {:p} (exe+0x{:X}) readable={} raw={:p}",
            static_cast< void* >( parked_record_mgr ),
            reinterpret_cast< uint64_t >( parked_record_mgr ) -
                reinterpret_cast< uint64_t >( GetModuleHandle( nullptr ) ),
            readable_ptr( parked_record_mgr ), mgr );
        __try
        {
            if ( mgr == nullptr || readable_span( mgr, 0x750 ) < 0x750 )
            {
                CCore::g_instance->info( "  registry: parked-record manager {:p} (none)", mgr );
            }
            else
            {
                void** it = prism::offsets::field< void** >( mgr, 0x740 );
                const uint64_t n = prism::offsets::field< uint64_t >( mgr, 0x748 );
                CCore::g_instance->info( "  registry: manager {:p} records={}", mgr, n );
                if ( it != nullptr && n < 0x40 && readable_span( it, n * sizeof( void* ) ) >= n * sizeof( void* ) )
                {
                    for ( uint64_t k = 0; k < n; ++k )
                    {
                        void* rec = it[ k ];
                        void* owner = rec != nullptr && readable_ptr( rec )
                            ? prism::offsets::field< void* >( rec, 0x178 ) : nullptr;
                        CCore::g_instance->info( "    record {:p} -> trailer {:p} (c{})",
                            rec, owner, canon_index_of( owner ) );
                    }
                }
            }
            if ( auto* game_actor = CCore::g_instance->get_game_actor() )
            {
                for ( uint32_t s = 0; s < 8; ++s )
                {
                    void* entry = reinterpret_cast< uint8_t* >( game_actor ) + 0x1D38 + s * 0x28;
                    if ( !readable_ptr( reinterpret_cast< uint8_t* >( entry ) + 8 ) ) break;
                    void* actor = *reinterpret_cast< void** >( entry );
                    CCore::g_instance->info( "  actor slot {}: {:p}{}", s, actor,
                        actor != nullptr && plausible_object( actor ) ? "" : " (empty)" );
                }
            }
        }
        __except ( EXCEPTION_EXECUTE_HANDLER )
        {
            CCore::g_instance->warning( "registry dump faulted (função 130)" );
        }
    }

    // 131 - carreta está na lista de parked-records
    bool parked_record_listed( void* t )
    {
        void* mgr = readable_ptr( parked_record_mgr ) ? *parked_record_mgr : nullptr;
        if ( mgr == nullptr ) return false;
        __try
        {
            void** it = readable_span( mgr, 0x750 ) >= 0x750
                ? prism::offsets::field< void** >( mgr, 0x740 ) : nullptr;
            const uint64_t n = it != nullptr
                ? prism::offsets::field< uint64_t >( mgr, 0x748 ) : 0;
            if ( it == nullptr || n == 0 || n >= 0x1000 ||
                 readable_span( it, n * sizeof( void* ) ) < n * sizeof( void* ) )
                return false;
            for ( uint64_t k = 0; k < n; ++k )
            {
                if ( it[ k ] != nullptr && readable_ptr( it[ k ] ) &&
                     prism::offsets::field< void* >( it[ k ], 0x178 ) == t )
                    return true;
            }
        }
        __except ( EXCEPTION_EXECUTE_HANDLER ) {}
        return false;
    }

    // 132 - tira a carreta da lista de parked-records
    void parked_record_unlist( void* t )
    {
        __try
        {
            void* mgr = readable_ptr( parked_record_mgr ) ? *parked_record_mgr : nullptr;
            void** it = mgr != nullptr && readable_span( mgr, 0x750 ) >= 0x750
                ? prism::offsets::field< void** >( mgr, 0x740 ) : nullptr;
            auto* nrec = it != nullptr
                ? &prism::offsets::field< uint64_t >( mgr, 0x748 ) : nullptr;
            if ( nrec == nullptr || *nrec == 0 || *nrec >= 0x1000 ||
                 readable_span( it, *nrec * sizeof( void* ) ) < *nrec * sizeof( void* ) )
                return;
            for ( uint64_t k = 0; k < *nrec; ++k )
            {
                if ( it[ k ] != nullptr && readable_ptr( it[ k ] ) &&
                     prism::offsets::field< void* >( it[ k ], 0x178 ) == t )
                {
                    std::memmove( it + k, it + k + 1, ( *nrec - k - 1 ) * sizeof( void* ) );
                    --*nrec;
                    --k;
                }
            }
        }
        __except ( EXCEPTION_EXECUTE_HANDLER ) {}
    }

    void* lane_containers[ 128 ] = {};
    uint32_t lane_prune_frames = 0;
    uint32_t lane_prune_idle = 0;

    // 133 - elemento de lane-obstacle está morto
    bool lane_element_dead( void* el )
    {
        if ( el == nullptr ) return true;
        void* owner = nullptr;
        __try
        {
            owner = *reinterpret_cast< void** >( static_cast< uint8_t* >( el ) + 0x88 );
        }
        __except ( EXCEPTION_EXECUTE_HANDLER )
        {
            return true;
        }
        return owner == nullptr;
    }

    // 134 - poda elementos mortos dos containers de lane
    void prune_lane_containers()
    {
        static uint32_t ci = 0;
        static uint64_t ei = ~0ull;
        uint32_t budget = 64;
        while ( budget > 0 && ci < 128 )
        {
            void*& c = lane_containers[ ci ];
            if ( c == nullptr || !readable_ptr( c ) ||
                 !readable_ptr( static_cast< uint8_t* >( c ) + 0x10 ) )
            {
                c = nullptr;
                ++ci;
                ei = ~0ull;
                continue;
            }
            void** elements = *reinterpret_cast< void*** >( static_cast< uint8_t* >( c ) + 8 );
            auto* count = reinterpret_cast< uint64_t* >( static_cast< uint8_t* >( c ) + 0x10 );
            if ( elements == nullptr || !readable_ptr( elements ) ||
                 *count == 0 || *count > 0x10000 )
            {
                c = nullptr;
                ++ci;
                ei = ~0ull;
                continue;
            }
            bool done = false;
            __try
            {
                uint64_t n = *count;
                if ( ei == ~0ull || ei >= n ) ei = n - 1;
                while ( budget > 0 )
                {
                    --budget;
                    void* el = elements[ ei ];
                    if ( lane_element_dead( el ) )
                    {
                        CCore::g_instance->info( "lane obstacle: pruned dead entry {:p}", el );
                        std::memmove( &elements[ ei ], &elements[ ei + 1 ],
                                      ( n - ei - 1 ) * sizeof( void* ) );
                        --n;
                        *count = n;

                        if ( n > 0 ) elements[ n ] = elements[ n - 1 ];
                        if ( n == 0 || ei >= n ) { done = true; break; }

                    }
                    else if ( ei == 0 ) { done = true; break; }
                    else { --ei; }
                }
            }
            __except ( EXCEPTION_EXECUTE_HANDLER )
            {

                c = nullptr;
                ++ci;
                ei = ~0ull;
                continue;
            }
            if ( done )
            {
                ++ci;
                ei = ~0ull;
            }
        }
        if ( ci >= 128 ) { ci = 0; ei = ~0ull; }
    }

    using lane_obstacle_insert_fn = uint8_t __fastcall( void* /*node*/, void* /*link*/, float /*pos*/ );
    // 135 - hook: sanitiza o container antes do insert nativo
    uint8_t __fastcall hk_lane_obstacle_insert( void* node, void* link, float pos )
    {
        if ( node != nullptr && link != nullptr && plausible_vfunc( link, 1 ) )
        {
            using type_fn = uint32_t __fastcall( void* );
            const auto get_type = reinterpret_cast< type_fn* >( ( *reinterpret_cast< void** const* >( link ) )[ 1 ] );

            if ( get_type( link ) == 0xFFF && plausible_vfunc( node, 0xD0 / 8 ) )
            {
                using container_fn = void* __fastcall( void* );
                const auto get_container = reinterpret_cast< container_fn* >( ( *reinterpret_cast< void** const* >( node ) )[ 0xD0 / 8 ] );
                void* container = get_container( node );
                if ( readable_ptr( container ) && readable_ptr( static_cast< uint8_t* >( container ) + 0x18 ) )
                {

                    for ( auto& c : lane_containers )
                    {
                        if ( c == container ) break;
                        if ( c == nullptr ) { c = container; break; }
                    }
                    void** elements = *reinterpret_cast< void*** >( static_cast< uint8_t* >( container ) + 8 );
                    auto* count = reinterpret_cast< uint64_t* >( static_cast< uint8_t* >( container ) + 0x10 );
                    if ( elements != nullptr && readable_ptr( elements ) && *count <= 0x10000 )
                    {
                        uint64_t n = *count;
                        for ( uint64_t i = n; i-- > 0; )
                        {
                            void* el = elements[ i ];
                            if ( lane_element_dead( el ) )
                            {
                                CCore::g_instance->info( "lane obstacle: pruned dead entry {:p}", el );
                                memmove( &elements[ i ], &elements[ i + 1 ], ( n - i - 1 ) * sizeof( void* ) );
                                --n;
                            }
                        }
                        *count = n;
                    }
                }
            }
        }
        return lane_obstacle_insert_hook->get_original< lane_obstacle_insert_fn >()( node, link, pos );
    }

    // 136 - estaciona uma carreta solta (apoio + freio + registro)
    void native_park_detached( void* t )
    {
        if ( t == nullptr || !is_trailer_obj( t ) ) return;
        __try
        {
            if ( prism::offsets::field< void* >( t, prism::offsets::trailer_parked_state ) != nullptr )
            {
                prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked ) = 1;
                prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked_registered ) = 1;
            }
            if ( plausible_vfunc( t, 0x328 / 8 ) )
            {
                vcall< prism::physics_trailer_u_set_trailer_brace_fn >( t, 0x328 / 8, true );
            }
            prism::offsets::field< float >( t, prism::offsets::trailer_brake ) = 1.f;
            prism::offsets::field< float >( t, prism::offsets::trailer_service_brake ) = 0.f;
            prism::offsets::field< uint8_t >( t, prism::offsets::trailer_brake_forced ) = 1;
            if ( brace_anim_fn != nullptr )
            {
                prism::offsets::field< float >( t, 0x1240 ) = 1.f;
                brace_anim_fn( t, 1, 1.f / 3.f );
            }
            if ( notify_decoupled_fn != nullptr ) notify_decoupled_fn( t );
            traffic_detach_tail();
            lane_prune_frames = 40;
        }
        __except ( EXCEPTION_EXECUTE_HANDLER )
        {
            CCore::g_instance->warning( "native park sequence faulted on {:p} (função 136)", t );
        }
    }

    // 235 - tratador de exceções: loga código, RIP, registradores e a função numerada
    LONG WINAPI crash_tracer( EXCEPTION_POINTERS* ep )
    {
        if ( ep == nullptr || ep->ExceptionRecord == nullptr ) return EXCEPTION_CONTINUE_SEARCH;
        const auto code = ep->ExceptionRecord->ExceptionCode;
        if ( code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW &&
             code != 0xE24C4A02 /* purecall */ )
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        const auto rip = ep->ContextRecord != nullptr ? ep->ContextRecord->Rip : 0;
        const auto base = reinterpret_cast< uint64_t >( GetModuleHandle( nullptr ) );

        if ( code == EXCEPTION_ACCESS_VIOLATION && ep->ContextRecord != nullptr &&
             rip == validated_va( 0x141BEA1C5 ) &&
             ep->ExceptionRecord->NumberParameters > 0 &&
             ep->ExceptionRecord->ExceptionInformation[ 0 ] == 0 /* leitura */ )
        {
            static int nav_curve_emulated = 0;
            if ( nav_curve_emulated < 4 )
            {
                ++nav_curve_emulated;
                CCore::g_instance->warning(
                    "nav curve read emulated (OOB index) at exe+0x1BEA1C5" );
            }
            ep->ContextRecord->Xmm5.Low = 0;
            ep->ContextRecord->Xmm5.High = 0;
            ep->ContextRecord->Rip += 5;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        if ( const auto* fe = fn_index_lookup( rip ) )
        {
            CCore::g_instance->warning( "falha 0x{:X} na função {} ({}) +0x{:X}",
                code, fe->num, fe->name, rip - fe->addr );
        }
        else
        {
            CCore::g_instance->warning( "falha 0x{:X} fora do plugin em 0x{:X}", code, rip );
        }

        if ( !settings::g_settings.debug_logs ) return EXCEPTION_CONTINUE_SEARCH;
        CCore::g_instance->warning(
            "AV 0x{:X} at exe+0x{:X} rax={:p} rbx={:p} rcx={:p} rdx={:p} rsi={:p} rdi={:p} r8={:p} addr={:p}",
            code, rip >= base ? rip - base : rip,
            ep->ContextRecord ? reinterpret_cast< void* >( ep->ContextRecord->Rax ) : nullptr,
            ep->ContextRecord ? reinterpret_cast< void* >( ep->ContextRecord->Rbx ) : nullptr,
            ep->ContextRecord ? reinterpret_cast< void* >( ep->ContextRecord->Rcx ) : nullptr,
            ep->ContextRecord ? reinterpret_cast< void* >( ep->ContextRecord->Rdx ) : nullptr,
            ep->ContextRecord ? reinterpret_cast< void* >( ep->ContextRecord->Rsi ) : nullptr,
            ep->ContextRecord ? reinterpret_cast< void* >( ep->ContextRecord->Rdi ) : nullptr,
            ep->ContextRecord ? reinterpret_cast< void* >( ep->ContextRecord->R8 ) : nullptr,
            ep->ExceptionRecord->NumberParameters > 1
                ? reinterpret_cast< void* >( ep->ExceptionRecord->ExceptionInformation[ 1 ] ) : nullptr );

        static uint64_t stack_rips[ 8 ] = {};
        static int stack_dumps = 0;
        const auto* dos = reinterpret_cast< const IMAGE_DOS_HEADER* >( base );
        const auto* nt = reinterpret_cast< const IMAGE_NT_HEADERS64* >( base + dos->e_lfanew );
        const uint64_t image_end = base + nt->OptionalHeader.SizeOfImage;
        bool seen_rip = false;
        for ( int i = 0; i < stack_dumps; ++i ) if ( stack_rips[ i ] == rip ) seen_rip = true;
        if ( ep->ContextRecord != nullptr && !seen_rip && stack_dumps < 8 )
        {
            stack_rips[ stack_dumps++ ] = rip;
            const auto* sp = reinterpret_cast< const uint64_t* >( ep->ContextRecord->Rsp );
            std::string found;
            int n = 0;
            for ( int k = 0; k < 96 && n < 14; ++k )
            {
                if ( !readable_ptr( sp + k ) ) break;
                const uint64_t v = sp[ k ];
                if ( v >= base && v < image_end )
                {
                    found += fmt::format( " [rsp+0x{:X}]=exe+0x{:X}", k * 8, v - base );
                    ++n;
                }
                else if ( const auto* fe = fn_index_lookup( v ) )
                {
                    found += fmt::format( " [rsp+0x{:X}]=fn{} {}+0x{:X}", k * 8,
                        fe->num, fe->name, v - fe->addr );
                    ++n;
                }
            }
            CCore::g_instance->warning( "AV stack (RIP outside exe):{}", found );
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // 137 - instala o rastreador de crashes (VEH)
    void install_crash_tracer()
    {
        static PVOID h = AddVectoredExceptionHandler( 1, crash_tracer );
        if ( h != nullptr )
        {
            CCore::g_instance->info( "crash tracer installed" );
        }
    }

    // 138 - vfunc fake que devolve nulo
    void* __fastcall purecall_null_vfunc( void* /*self*/ ) { return nullptr; }
    void* purecall_fake_vtbl[ 16 ] = {};
    struct purecall_fake_t { void* vtbl; uint8_t pad[ 248 ]; };
    purecall_fake_t purecall_fake = { purecall_fake_vtbl };

    // 139 - monta a vtable fake do purecall
    void init_purecall_fake()
    {
        for ( auto& slot : purecall_fake_vtbl )
        {
            slot = reinterpret_cast< void* >( &purecall_null_vfunc );
        }
    }

    // 140 - hook: engole purecall do iterador de componentes
    void* __fastcall hk_purecall( void* /*object*/ )
    {
        const auto ret = reinterpret_cast< uint64_t >( _ReturnAddress() );
        const auto base = reinterpret_cast< uint64_t >( GetModuleHandle( nullptr ) );
        const auto rva = ret - base;
        static uint64_t seen[ 32 ] = {};
        for ( auto& s : seen )
        {
            if ( s == ret ) break;
            if ( s == 0 )
            {
                s = ret;
                CCore::g_instance->warning( "purecall swallowed at exe+0x{:X} (função 140)", rva );
                break;
            }
        }
        if ( rva >= 0x927C60 && rva < 0x927CBF ) return &purecall_fake;
        return nullptr;
    }

    // 141 - reexibe carretas escondidas
    void unhide_detached()
    {

        if ( cargo_freeze() || tp_cooldown > 0 || canon_age < 90 ||
             !canon_all_trailer_objs() ) return;
        for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
        {
            void* t = canon[ i ];
            if ( !is_trailer_obj( t ) || joint( t ) != nullptr ) continue;
            if ( !is_detached( t ) && !is_plugin_detached( t ) ) continue;
            if ( prism::offsets::field< void* >( t, prism::offsets::trailer_parked_state ) != nullptr )
            {
                prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked ) = 1;
                prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked_registered ) = 1;
                prism::offsets::field< uint8_t >( t, prism::offsets::trailer_brace_state ) = 1;
            }
            else if ( trailer_teleport_hook != nullptr )
            {
                hook_xform_t xf{};
                if ( hook_world( t, xf ) )
                {
                    using teleport_fn_t = void __fastcall( void*, void* );
                    teleport_safe( t, &xf );
                }
            }
        }
    }

    // 142 - trava de estacionamento emulada (âncora de pose)
    void update_parked_locks()
    {
        if ( tp_cooldown > 0 ) --tp_cooldown;
        if ( cargo_freeze() ) return;
        for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
        {
            if ( tp_cooldown > 0 )
            {
                anchor_valid[ i ] = false;
                anchor_settling[ i ] = false;
                anchor_delay[ i ] = 0;
                continue;
            }
            void* t = canon[ i ];

            if ( readable_span( t, 0x1200 ) < 0x1200 || joint( t ) != nullptr ||
                 ( !is_detached( t ) && !is_plugin_detached( t ) ) )
            {
                anchor_valid[ i ] = false;
                anchor_settling[ i ] = false;
                anchor_delay[ i ] = 0;
                continue;
            }

            void* mgr = readable_ptr( parked_record_mgr ) ? *parked_record_mgr : nullptr;

            if ( mgr == nullptr &&
                 prism::offsets::field< void* >( t, prism::offsets::trailer_parked_state ) != nullptr )
            {
                prism::offsets::field< void* >( t, prism::offsets::trailer_parked_state ) = nullptr;
                prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked ) = 0;
                prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked_registered ) = 0;
                CCore::g_instance->info( "c{}: orphan parked record dropped (no manager)", i );
            }
            if ( record_retry[ i ] > 0 ) --record_retry[ i ];
            const bool wants_record =
                prism::offsets::field< void* >( t, prism::offsets::trailer_parked_state ) == nullptr &&
                ( is_detached( t ) || is_plugin_detached( t ) );
            if ( wants_record && mgr != nullptr ) record_mgr_logged[ i ] = false;
            if ( wants_record && mgr == nullptr && !record_mgr_logged[ i ] )
            {
                record_mgr_logged[ i ] = true;
                CCore::g_instance->info(
                    "parked record manager ainda não existe - c{} segue na âncora emulada", i );
            }
            if ( wants_record &&
                record_retry[ i ] == 0 &&
                parked_record_factory != nullptr && parked_record_mgr != nullptr &&
                mgr != nullptr )
            {
                void* rec = nullptr;
                __try
                {
                    parked_record_factory( t );
                    void** it = mgr != nullptr && readable_ptr(
                        reinterpret_cast< uint8_t* >( mgr ) + 0x748 )
                        ? prism::offsets::field< void** >( mgr, 0x740 ) : nullptr;
                    const uint64_t nrec = mgr != nullptr && readable_ptr(
                        reinterpret_cast< uint8_t* >( mgr ) + 0x748 )
                        ? prism::offsets::field< uint64_t >( mgr, 0x748 ) : 0;
                    if ( it != nullptr && nrec > 0 && nrec < 0x1000 )
                    {
                        for ( uint64_t k = 0; k < nrec; ++k )
                        {
                            void* e = it[ k ];
                            if ( e != nullptr && readable_ptr( e ) &&
                                prism::offsets::field< void* >( e, 0x178 ) == t )
                            {
                                rec = e;
                                break;
                            }
                        }
                    }
                }
                __except ( factory_fault_filter( i, GetExceptionInformation() ) )
                {
                }
                if ( rec != nullptr )
                {
                    prism::offsets::field< void* >( t, prism::offsets::trailer_parked_state ) = rec;
                    prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked ) = 1;
                    prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked_registered ) = 1;
                    record_tried[ i ] = true;
                    record_quiet[ i ] = 0;
                    CCore::g_instance->info( "parked record {:p} created for c{}", rec, i );
                }
                else
                {

                    record_retry[ i ] = 20;
                    if ( record_quiet[ i ] < 5 )
                    {
                        ++record_quiet[ i ];
                        CCore::g_instance->warning(
                            "parked record factory produced no record for c{} - retrying", i );
                    }
                }
            }

            if ( prism::offsets::field< void* >( t, prism::offsets::trailer_parked_state ) != nullptr &&
                prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked_registered ) != 0 &&
                parked_record_listed( t ) )
            {
                anchor_valid[ i ] = false;
                anchor_settling[ i ] = false;
                anchor_delay[ i ] = 0;
                continue;
            }

            if ( anchor_delay[ i ] < 7 )
            {
                ++anchor_delay[ i ];
                continue;
            }
            if ( !anchor_valid[ i ] )
            {
                anchor_valid[ i ] = hook_world( t, anchor_xform[ i ] );
                anchor_home[ i ] = anchor_xform[ i ];
                anchor_settle[ i ] = 0;
                anchor_age[ i ] = 0;
                anchor_settling[ i ] = true;
                anchor_fixes[ i ] = 0;
                continue;
            }
            hook_xform_t now{};
            if ( !hook_world( t, now ) || trailer_teleport_hook == nullptr ) continue;
            ++anchor_age[ i ];

            {
                void* slave =
                    readable_ptr( static_cast< uint8_t* >( t ) + prism::offsets::trailer_slave )
                        ? prism::offsets::field< void* >( t, prism::offsets::trailer_slave )
                        : nullptr;
                const bool slave_j = slave != nullptr && readable_ptr( slave ) &&
                    readable_ptr( static_cast< uint8_t* >( slave ) + prism::offsets::trailer_physics_joint ) &&
                    joint( slave ) != nullptr;
                if ( slave != anchor_prev_slave[ i ] ||
                     slave_j != anchor_prev_slave_j[ i ] )
                {
                    anchor_prev_slave[ i ] = slave;
                    anchor_prev_slave_j[ i ] = slave_j;
                    anchor_settle[ i ] = 0;
                    anchor_age[ i ] = 0;
                    anchor_settling[ i ] = true;
                }
            }
            const float mx = now.pos.x - anchor_xform[ i ].pos.x;
            const float my = now.pos.y - anchor_xform[ i ].pos.y;
            const float mz = now.pos.z - anchor_xform[ i ].pos.z;
            const quat_t& aq = anchor_xform[ i ].quat;
            const float qdot = aq.w * now.quat.w + aq.x * now.quat.x +
                               aq.y * now.quat.y + aq.z * now.quat.z;
            const float rotd = 1.f - std::fabs( qdot );
            const bool moving = mx * mx + my * my + mz * mz > 0.02f * 0.02f || rotd > 0.001f;
            if ( anchor_settle[ i ] < 5 && anchor_age[ i ] < 40 )
            {

                anchor_settling[ i ] = true;
                const float hx = now.pos.x - anchor_home[ i ].pos.x;
                const float hy = now.pos.y - anchor_home[ i ].pos.y;
                const float hz = now.pos.z - anchor_home[ i ].pos.z;

                if ( ( hx * hx + hz * hz > 1.5f * 1.5f || hy < -0.5f ) &&
                     trailer_teleport_hook != nullptr )
                {
                    using teleport_fn_t = void __fastcall( void* /*trailer*/, void* /*transform*/ );
                    teleport_safe( t, &anchor_home[ i ] );
                    anchor_xform[ i ] = anchor_home[ i ];
                    anchor_settle[ i ] = 0;
                    continue;
                }
                if ( moving )
                {
                    anchor_xform[ i ] = now;
                    anchor_settle[ i ] = 0;
                }
                else
                {
                    ++anchor_settle[ i ];
                }
                continue;
            }
            anchor_settling[ i ] = false;

            {
                const quat_t& q = anchor_xform[ i ].quat;
                const float up_y = 1.f - 2.f * ( q.x * q.x + q.z * q.z );
                if ( up_y < 0.85f )
                {
                    anchor_valid[ i ] = false;
                    anchor_settling[ i ] = true;
                    continue;
                }
            }

            if ( mx * mx + my * my + mz * mz > 0.08f * 0.08f || rotd > 0.004f )
            {

                using teleport_fn_t = void __fastcall( void* /*trailer*/, void* /*transform*/ );
                teleport_safe( t, &anchor_xform[ i ] );

                if ( ++anchor_fixes[ i ] >= 30 )
                {

                    CCore::g_instance->info(
                        "anchor: c{} fought the pose {}x (drift {:.3f}m) - recapturing",
                        i, anchor_fixes[ i ],
                        std::sqrt( mx * mx + my * my + mz * mz ) );
                    anchor_valid[ i ] = false;
                }
            }
        }
    }

    // 143 - desmarca "estacionada" nas carretas reengatadas
    void unpark_jointed( const bool* in_convoy )
    {
        for ( uint32_t i = 0; i < trailer_count; ++i )
        {
            if ( !in_convoy[ i ] ) continue;
            void* t = trailers[ i ];
            if ( joint( t ) == nullptr ) continue;
            const bool parked_flag = prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked ) != 0;
            const bool parked_rec = prism::offsets::field< void* >( t, prism::offsets::trailer_parked_state ) != nullptr;

            const bool parked_reg = prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked_registered ) != 0;
            if ( !parked_flag && !parked_reg ) continue;

            prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked ) = 0;
            prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked_registered ) = 0;
            static void* unparked_t[ max_trailers ] = {};
            static void* unparked_j[ max_trailers ] = {};
            uint32_t slot = max_trailers, free_slot = max_trailers;
            for ( uint32_t s = 0; s < max_trailers; ++s )
            {
                if ( unparked_t[ s ] == t ) { slot = s; break; }
                if ( unparked_t[ s ] == nullptr && free_slot == max_trailers ) free_slot = s;
            }
            if ( slot == max_trailers )
            {
                if ( free_slot == max_trailers ) continue;
                slot = free_slot;
                unparked_t[ slot ] = t;
                unparked_j[ slot ] = nullptr;
            }
            if ( unparked_j[ slot ] == joint( t ) ) continue;
            unparked_j[ slot ] = joint( t );
            prism::offsets::field< float >( t, prism::offsets::trailer_brake ) = 0.f;
            prism::offsets::field< uint8_t >( t, prism::offsets::trailer_brake_forced ) = 0;

            if ( parked_rec ) parked_record_unlist( t );
            CCore::g_instance->info( "un-parked trailer {} (flag {} record {})", i, parked_flag, parked_rec );
        }
    }
    namespace
    {
        const bool fn_idx_reg = []()
        {
            fn_index_add( fn_addr_of( &render_cargo_restore_option ), 119, "render_cargo_restore_option" );
            fn_index_add( fn_addr_of( &snapshot_rig ), 120, "snapshot_rig" );
            fn_index_add( fn_addr_of( &hk_trailer_state_apply ), 121, "hk_trailer_state_apply" );
            fn_index_add( fn_addr_of( &hk_trailer_teleport ), 122, "hk_trailer_teleport" );
            fn_index_add( fn_addr_of( &settle_all_members ), 123, "settle_all_members" );
            fn_index_add( fn_addr_of( &hk_actor_teleport ), 124, "hk_actor_teleport" );
            fn_index_add( fn_addr_of( &find_trailer_actor ), 125, "find_trailer_actor" );
            fn_index_add( fn_addr_of( &sample_provider_vtbl ), 126, "sample_provider_vtbl" );
            fn_index_add( fn_addr_of( &find_trailer_provider ), 127, "find_trailer_provider" );
            fn_index_add( fn_addr_of( &cache_trailer_actor ), 128, "cache_trailer_actor" );
            fn_index_add( fn_addr_of( &factory_fault_filter ), 129, "factory_fault_filter" );
            fn_index_add( fn_addr_of( &dump_native_registry ), 130, "dump_native_registry" );
            fn_index_add( fn_addr_of( &parked_record_listed ), 131, "parked_record_listed" );
            fn_index_add( fn_addr_of( &parked_record_unlist ), 132, "parked_record_unlist" );
            fn_index_add( fn_addr_of( &lane_element_dead ), 133, "lane_element_dead" );
            fn_index_add( fn_addr_of( &prune_lane_containers ), 134, "prune_lane_containers" );
            fn_index_add( fn_addr_of( &hk_lane_obstacle_insert ), 135, "hk_lane_obstacle_insert" );
            fn_index_add( fn_addr_of( &native_park_detached ), 136, "native_park_detached" );
            fn_index_add( fn_addr_of( &crash_tracer ), 235, "crash_tracer" );
            fn_index_add( fn_addr_of( &install_crash_tracer ), 137, "install_crash_tracer" );
            fn_index_add( fn_addr_of( &purecall_null_vfunc ), 138, "purecall_null_vfunc" );
            fn_index_add( fn_addr_of( &init_purecall_fake ), 139, "init_purecall_fake" );
            fn_index_add( fn_addr_of( &hk_purecall ), 140, "hk_purecall" );
            fn_index_add( fn_addr_of( &unhide_detached ), 141, "unhide_detached" );
            fn_index_add( fn_addr_of( &update_parked_locks ), 142, "update_parked_locks" );
            fn_index_add( fn_addr_of( &unpark_jointed ), 143, "unpark_jointed" );
            return true;
        }();
    }
}
