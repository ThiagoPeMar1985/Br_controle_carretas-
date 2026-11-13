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

    prism::physics_trailer_u_get_slave_hook_position_fn* get_slave_hook_position_fn = nullptr;
    prism::vehicle_set_towed_trailer_fn* set_towed_trailer_fn = nullptr;

    prism::vehicle_clear_detached_flag_fn* clear_detached_flag_fn = nullptr;
    prism::vehicle_notify_decoupled_fn* notify_decoupled_fn = nullptr;
    prism::vehicle_pre_detach_fn* pre_detach_fn = nullptr;
    prism::vehicle_refresh_accessories_fn* refresh_accessories_fn = nullptr;
    prism::vehicle_rebuild_lines_fn* rebuild_lines_fn = nullptr;
    prism::vehicle_distribute_towed_mass_fn* distribute_towed_mass_fn = nullptr;
    prism::game_actor_refresh_axle_lift_fn* refresh_axle_lift_fn = nullptr;

    prism::console_execute_line_fn* execute_line_fn = nullptr;

    void** traffic_manager_global = nullptr;
    uint32_t* traffic_game_state = nullptr;
    using anchor_relink_fn = void __fastcall( void* /*registry*/, uint32_t /*type*/ );

    using cam_set_target_fn = void( void* /*camera*/, void* /*target*/ );
    using cam_recompute_fn = void( void* /*camera*/, uint8_t /*reset*/ );
    anchor_relink_fn* relink_anchors_fn = nullptr;

    prism::cvar_get_float_fn* get_cvar_float_fn = nullptr;
    void* g_traffic_cvar = nullptr;

    using trailer_settle_fn = void __fastcall( void* /*trailer*/, float /*cvar_value*/ );
    using vehicle_pair_fn = void __fastcall( void* /*tower*/, void* /*towed*/ );
    using vehicle_cables_fn = void __fastcall( void* /*truck*/, void* /*trailer*/,
        int /*unused*/, int /*mode*/, int /*zero*/ );
    trailer_settle_fn* trailer_settle_fn_ptr = nullptr;
    vehicle_pair_fn* pair_vehicles_fn = nullptr;
    vehicle_cables_fn* apply_cables_fn = nullptr;
    using actor_flag_fixup = void __fastcall( void* /*game_actor*/ );
    actor_flag_fixup* actor_flag_fixup_fn = nullptr;
    using cvar_get_int_fn = int __fastcall( void* /*cvar*/ );
    cvar_get_int_fn* get_cvar_int_fn = nullptr;
    void* cables_mode_cvar = nullptr;
    void* attach_settle_cvar = nullptr;

    using post_event_fn = void __fastcall( void* /*bus_d8*/, const char** /*name_slot*/,
        void* /*params*/, int /*two*/, void* /*s5_params*/, int /*s6*/, int /*s7*/,
        int /*s8*/, int /*s9*/, void* /*s10*/, void* /*s11*/ );
    post_event_fn* post_event_fn_ptr = nullptr;
    void* event_param_block = nullptr;
    const char* event_name_coupling = nullptr;
    const char* event_name_decoupling = nullptr;

    using trailer_brace_anim_fn = void __fastcall( void* /*trailer*/, uint8_t /*down*/, float /*speed*/ );
    trailer_brace_anim_fn* brace_anim_fn = nullptr;

    using native_attach_dispatch_fn = void __fastcall( void* /*game_actor*/, uint8_t /*dl*/,
        uint64_t /*r8*/, uint64_t /*r9*/, uint64_t /*arg5*/, const float* /*settle_override*/ );
    native_attach_dispatch_fn* native_attach_fn = nullptr;

    using line_state_fn = void __fastcall( void* /*vehicle*/, uint8_t /*state*/, uint8_t /*force*/ );
    line_state_fn* set_lines_fn = nullptr;
    using lamp_mask_setter_fn = uint8_t __fastcall( void* /*vehicle*/, uint8_t /*mask*/ );
    using cam_update_fn = void( void* /*camera*/, float /*dt*/ );

    void* attach_target = nullptr;

    const float attach_gap_limit = 12.f;

    // 167 - gira vetor por quaternion
    float3_t quat_rot( const quat_t& q, const float3_t& v )
    {

        const float tx = 2.f * ( q.y * v.z - q.z * v.y );
        const float ty = 2.f * ( q.z * v.x - q.x * v.z );
        const float tz = 2.f * ( q.x * v.y - q.y * v.x );
        return float3_t{
            v.x + q.w * tx + q.y * tz - q.z * ty,
            v.y + q.w * ty + q.z * tx - q.x * tz,
            v.z + q.w * tz + q.x * ty - q.y * tx };
    }

    // 168 - offset do gancho traseiro no frame do pai
    float3_t attach_offset( void* parent, const bool parent_is_truck )
    {
        float3_t hook{};
        if ( parent_is_truck )
        {
            void* chassis = prism::offsets::field< void* >( parent, prism::offsets::vehicle_chassis_data );
            if ( chassis != nullptr )
            {
                hook = prism::offsets::field< float3_t >( chassis, prism::offsets::chassis_hook_position );
            }
        }
        else if ( get_slave_hook_position_fn != nullptr )
        {
            get_slave_hook_position_fn( static_cast< prism::physics_trailer_u* >( parent ), &hook );
        }
        const float3_t loc = prism::offsets::field< float3_t >( parent, prism::offsets::vehicle_hook_locator );
        return float3_t{ hook.x - loc.x, hook.y - loc.y, hook.z - loc.z };
    }

    // 169 - distância gancho-a-gancho no mundo
    float hook_gap( void* trailer, void* parent, const bool parent_is_truck )
    {
        hook_xform_t th{}, ph{};
        if ( trailer == nullptr || parent == nullptr ||
            !hook_world( trailer, th ) || !hook_world( parent, ph ) )
        {
            return -1.f;
        }
        const float3_t v = attach_offset( parent, parent_is_truck );
        const float3_t off = quat_rot( ph.quat, v );
        const float dx = th.pos.x - ph.pos.x;
        const float dy = th.pos.y - ph.pos.y;
        const float dz = th.pos.z - ph.pos.z;
        const float g_plus = std::sqrt(
            ( dx - off.x ) * ( dx - off.x ) +
            ( dy - off.y ) * ( dy - off.y ) +
            ( dz - off.z ) * ( dz - off.z ) );
        const float g_minus = std::sqrt(
            ( dx + off.x ) * ( dx + off.x ) +
            ( dy + off.y ) * ( dy + off.y ) +
            ( dz + off.z ) * ( dz + off.z ) );
        return ( std::min )( g_plus, g_minus );
    }

    // 170 - recusa engate com ganchos longe demais
    bool hook_far( void* trailer, void* parent, const bool parent_is_truck )
    {
        const float gap = hook_gap( trailer, parent, parent_is_truck );
        if ( gap < 0.f ) return false;
        CCore::g_instance->info( "Hook gap trailer {:p} to {:p}: {:.2f}m", trailer, parent, gap );

        return gap > 8.f;
    }

    // 171 - desatrelamento pela via nativa (slot do ator)
    bool native_detach_via_slot( void* trailer )
    {
        auto* game_actor = CCore::g_instance->get_game_actor();
        if ( game_actor == nullptr || detach_dispatch_hook == nullptr || trailer == nullptr )
        {
            return false;
        }
        void* prev = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_trailer );
        prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_trailer ) = trailer;

        using detach_fn_t = void __fastcall( void*, uint8_t, uint64_t );
        repair_lamp_ctx_all();

        sanitize_slave_walk( trailer );
        sanitize_slave_walk( prev );

        sanitize_phys_walk( trailer );
        if ( prev != nullptr ) sanitize_phys_walk( prev );
        dump_chain( "pre-detach(slot)" );
        native_chain_call_active.store( true, std::memory_order_release );
        detach_dispatch_hook->get_original< detach_fn_t >()( game_actor, 1, 0 );
        native_chain_call_active.store( false, std::memory_order_release );
        dump_chain( "post-detach(slot)" );

        void* truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
        void* prev_towed = truck != nullptr && readable_ptr( truck )
            ? prism::offsets::field< void* >( truck, 0x1580 ) : nullptr;
        if ( prev_towed != nullptr && prev_towed != trailer &&
             joint( prev_towed ) != nullptr && truck != nullptr &&
             set_towed_trailer_fn != nullptr &&
             prism::offsets::field< void* >( truck, 0x1580 ) == nullptr )
        {
            set_towed_trailer_fn( truck, true, prev_towed );
        }
        if ( joint( trailer ) == nullptr ) return true;
        prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_trailer ) = prev;
        return false;
    }

    // 172 - atrelamento pela via nativa (slot do ator)
    bool native_attach_via_slot( void* trailer )
    {
        auto* game_actor = CCore::g_instance->get_game_actor();
        if ( game_actor == nullptr || attach_dispatch_hook == nullptr ) return false;
        void* prev = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_trailer );
        prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_trailer ) = trailer;

        void* claim = readable_ptr( trailer )
            ? prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) : nullptr;
        const bool severed = claim != nullptr && joint( claim ) == nullptr &&
            claim_edge_ours( trailer, claim );
        if ( severed )
        {
            prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = nullptr;
        }

        sever_parked_claims();
        native_chain_call_active.store( true, std::memory_order_release );
        attach_dispatch_hook->get_original< native_attach_dispatch_fn >()( game_actor, 1, 0, 0, 0, nullptr );
        native_chain_call_active.store( false, std::memory_order_release );
        if ( severed && readable_ptr( trailer ) )
        {
            prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = claim;
        }
        restore_parked_claims();
        prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_trailer ) = prev;
        return joint( trailer ) != nullptr;
    }

    // 173 - executa o desatrelamento na thread de física
    void disconnect_now( void* trailer, void* parent, const bool parent_is_truck )
    {

        if ( parent_is_truck && joint( trailer ) != nullptr && detach_dispatch_hook != nullptr )
        {
            CCore::g_instance->info( "disconnect {:p}: native detach via trailer-of-interest", trailer );
            if ( native_detach_via_slot( trailer ) )
            {
                for ( void* t = trailer; t != nullptr; )
                {
                    mark_detached( t );
                    mark_plugin_detached( t );
                    void* next = prism::offsets::field< void* >( t, prism::offsets::trailer_slave );
                    t = ( next != t && next != nullptr && joint( next ) != nullptr ) ? next : nullptr;
                }
                const int di = canon_index_of( trailer );
                if ( di >= 0 ) detached_idx[ di ] = true;
                if ( trailer == truck_towed ) truck_towed = nullptr;

                unhide_detached();
                if ( trailer_teleport_hook != nullptr )
                {
                    hook_xform_t xf{};
                    if ( hook_world( trailer, xf ) )
                    {
                        using teleport_fn_t = void __fastcall( void*, void* );
                        trailer_teleport_hook->get_original< teleport_fn_t >()( trailer, &xf );
                    }
                }

                play_brace_sound( trailer, false );

                rebuild_parked_claim_links();
                traffic_detach_done.store( true, std::memory_order_release );
                return;
            }
            CCore::g_instance->info( "disconnect {:p}: native detach declined - manual fallback", trailer );
        }

        if ( parent_is_truck && parent != nullptr && pre_detach_fn != nullptr &&
            prism::offsets::field< uint32_t >( parent, prism::offsets::truck_coupling_state ) != 0 )
        {
            pre_detach_fn( parent, trailer );
        }

        play_brace_sound( trailer, false );
        vcall< prism::physics_trailer_u_set_trailer_brace_fn >( trailer, prism::offsets::vfunc_set_brace, true );

        void* kept_slave = readable_ptr( trailer )
            ? prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) : nullptr;
        if ( kept_slave != nullptr )
        {
            prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = nullptr;
        }
        native_chain_call_active.store( true, std::memory_order_release );
        vcall< prism::physics_trailer_u_disconnect_fn >( trailer, prism::offsets::vfunc_disconnect );
        native_chain_call_active.store( false, std::memory_order_release );
        const bool section_behind = kept_slave != nullptr && joint( kept_slave ) != nullptr;
        if ( kept_slave != nullptr && readable_ptr( trailer ) )
        {
            prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = kept_slave;
        }

        if ( notify_decoupled_fn != nullptr )
        {
            if ( parent != nullptr ) notify_decoupled_fn( parent );
            if ( !section_behind ) notify_decoupled_fn( trailer );
        }
        if ( clear_detached_flag_fn != nullptr )
        {
            if ( parent != nullptr && parent_is_truck ) clear_detached_flag_fn( parent, 1, 1 );
            if ( !section_behind ) clear_detached_flag_fn( trailer, 1, 1 );
        }

        for ( void* t = trailer; t != nullptr; )
        {
            mark_detached( t );
            mark_plugin_detached( t );

            void* next = prism::offsets::field< void* >( t, prism::offsets::trailer_slave );
            t = ( next != t && next != nullptr && joint( next ) != nullptr ) ? next : nullptr;
        }

        const int di = canon_index_of( trailer );
        if ( di >= 0 ) detached_idx[ di ] = true;

        if ( set_lines_fn != nullptr )
        {
            set_lines_fn( trailer, 0, 1 );
        }

        if ( prism::offsets::field< void* >( trailer, prism::offsets::trailer_parked_state ) != nullptr )
        {
            prism::offsets::field< uint8_t >( trailer, prism::offsets::trailer_parked ) = 1;
        }

        if ( plausible_vfunc( trailer, 0x328 / 8 ) )
        {
            vcall< prism::physics_trailer_u_set_trailer_brace_fn >( trailer, 0x328 / 8, true );
        }

        prism::offsets::field< float >( trailer, prism::offsets::trailer_brake ) = 1.f;
        prism::offsets::field< float >( trailer, prism::offsets::trailer_service_brake ) = 0.f;
        prism::offsets::field< uint8_t >( trailer, prism::offsets::trailer_brake_forced ) = 1;
        if ( parent != nullptr )
        {
            if ( parent_is_truck )
            {

                if ( set_towed_trailer_fn != nullptr )
                {
                    set_towed_trailer_fn( parent, false, nullptr );
                }
                if ( distribute_towed_mass_fn != nullptr )
                {
                    distribute_towed_mass_fn( parent, 0.f, 0.f );
                }

                if ( joint( trailers[ 0 ] ) == nullptr )
                {
                    prism::offsets::field< uint32_t >( parent, prism::offsets::truck_coupling_state ) = 0;
                }
            }
            if ( rebuild_lines_fn != nullptr )
            {
                void* slave = prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave );
                prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = nullptr;
                rebuild_lines_fn( parent, trailer );
                prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = slave;
            }

            if ( refresh_accessories_fn != nullptr )
            {
                refresh_accessories_fn( parent, trailer );
            }

            traffic_detach_tail();
        }

        if ( parent != nullptr && pair_vehicles_fn != nullptr && !section_behind )
        {
            pair_vehicles_fn( parent, trailer );
        }
        if ( auto* game_actor = CCore::g_instance->get_game_actor() )
        {
            void* truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
            if ( parent_is_truck && truck != nullptr && readable_ptr( reinterpret_cast< uint8_t* >( truck ) + 0x4C8 ) )
            {
                prism::offsets::field< float >( truck, 0x4C4 ) = 0.f;

                prism::offsets::field< uint32_t >( truck, prism::offsets::truck_coupling_state ) = 0;
                if ( set_towed_trailer_fn != nullptr &&
                    prism::offsets::field< void* >( truck, 0x1580 ) == trailer )
                {
                    set_towed_trailer_fn( truck, true, nullptr );
                }
            }
            if ( apply_cables_fn != nullptr && cables_mode_cvar != nullptr &&
                get_cvar_int_fn != nullptr && truck != nullptr )
            {
                const int mode = get_cvar_int_fn( cables_mode_cvar );
                apply_cables_fn( truck, nullptr, 0, mode > 4 ? 4 : mode < 0 ? 0 : mode, 0 );
            }
        }
        if ( trailer == truck_towed ) truck_towed = nullptr;

        if ( brace_anim_fn != nullptr )
        {
            prism::offsets::field< float >( trailer, 0x1240 ) = 1.f;
            brace_anim_fn( trailer, 1, 1.f / 3.f );
        }

        if ( joint( trailer ) == nullptr )
        {
            native_detach_via_slot( trailer );
        }

        unhide_detached();

        rebuild_parked_claim_links();

        traffic_detach_done.store( true, std::memory_order_release );
    }

    // 174 - executa o atrelamento na thread de física
    void connect_now( void* trailer, void* parent, const bool parent_is_truck )
    {

        if ( hook_far( trailer, parent, parent_is_truck ) )
        {
            CCore::g_instance->warning( "Carreta muito longe - aproxime dando ré para engatar" );
            return;
        }

        if ( joint( trailer ) != nullptr )
        {
            void* kept_slave = prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave );
            if ( kept_slave != nullptr )
            {
                prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = nullptr;
            }
            vcall< prism::physics_trailer_u_disconnect_fn >( trailer, prism::offsets::vfunc_disconnect );
            if ( kept_slave != nullptr && readable_ptr( trailer ) )
            {
                prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = kept_slave;
            }
        }

        if ( parent_is_truck && joint( trailer ) == nullptr &&
             attach_dispatch_hook != nullptr && native_attach_via_slot( trailer ) )
        {
            CCore::g_instance->info( "connect {:p}: native attach via trailer-of-interest", trailer );
            if ( joint( trailer ) == nullptr )
            {
                if ( set_lines_fn != nullptr ) set_lines_fn( trailer, 0, 1 );
                if ( apply_cables_fn != nullptr ) apply_cables_fn( parent, nullptr, 0, 0, 0 );
                CCore::g_instance->warning( "Engate falhou - sem junta, cabos desfeitos" );
                return;
            }
            for ( void* t = trailer; t != nullptr; )
            {
                unmark_detached( t );
                unmark_plugin_detached( t );
                const int ti = canon_index_of( t );
                if ( ti >= 0 ) detached_idx[ ti ] = false;
                void* next = prism::offsets::field< void* >( t, prism::offsets::trailer_slave );
                t = ( next != t && next != nullptr && joint( next ) != nullptr ) ? next : nullptr;
            }
            if ( truck_towed == trailer )
            {
                auto* game_actor = CCore::g_instance->get_game_actor();
                void* truck = game_actor != nullptr
                    ? prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck ) : nullptr;
                if ( truck != nullptr )
                {
                    if ( set_towed_trailer_fn != nullptr &&
                         prism::offsets::field< void* >( truck, 0x1580 ) != trailer )
                    {
                        set_towed_trailer_fn( truck, true, trailer );
                    }
                    prism::offsets::field< uint32_t >( truck, prism::offsets::truck_coupling_state ) = 5;
                }
            }
            play_brace_sound( trailer, true );
            unhide_detached();
            return;
        }

        float3_t vec = attach_offset( parent, parent_is_truck );

        const auto parent_coupling = prism::offsets::field< uint8_t >( parent, prism::offsets::vehicle_coupling_flag );
        prism::offsets::field< uint8_t >( trailer, prism::offsets::vehicle_coupling_flag ) = parent_coupling;
        void* parent_chassis = prism::offsets::field< void* >( parent, prism::offsets::vehicle_chassis_data );
        const bool slave_hook = parent_chassis != nullptr &&
            prism::offsets::field< uint32_t >( parent_chassis, prism::offsets::chassis_slave_hook_type ) == 2;

        for ( void* t = trailer; t != nullptr; )
        {
            unmark_detached( t );
            unmark_plugin_detached( t );
            const int ti = canon_index_of( t );
            if ( ti >= 0 ) detached_idx[ ti ] = false;
            void* next = prism::offsets::field< void* >( t, prism::offsets::trailer_slave );
            t = ( next != t && next != nullptr && joint( next ) != nullptr ) ? next : nullptr;
        }

        if ( parent_is_truck && trailer_settle_fn_ptr != nullptr &&
            attach_settle_cvar != nullptr && get_cvar_float_fn != nullptr )
        {
            trailer_settle_fn_ptr( trailer, get_cvar_float_fn( attach_settle_cvar ) );
        }

        CCore::g_instance->info( "Connecting trailer {:p} to {:p}", trailer, parent );

        sever_parked_claims();
        native_chain_call_active.store( true, std::memory_order_release );
        vcall< prism::physics_trailer_u_connect_fn >( trailer, prism::offsets::vfunc_connect,
            parent, vec, 0, parent_is_truck || parent_coupling != 0, slave_hook );
        native_chain_call_active.store( false, std::memory_order_release );
        restore_parked_claims();

        if ( joint( trailer ) == nullptr )
        {
            if ( set_lines_fn != nullptr ) set_lines_fn( trailer, 0, 1 );
            if ( apply_cables_fn != nullptr ) apply_cables_fn( parent, nullptr, 0, 0, 0 );
            CCore::g_instance->warning( "Engate falhou - sem junta, cabos desfeitos" );
            return;
        }

        if ( !parent_is_truck && parent != nullptr )
        {
            void** pslave = &prism::offsets::field< void* >( parent, prism::offsets::trailer_slave );
            void* cur = *pslave;
            if ( cur != trailer &&
                 ( cur == nullptr || !readable_ptr( cur ) || joint( cur ) == nullptr ) )
            {
                *pslave = trailer;
                clear_claim_edge( parent );
            }
        }

        if ( parent_is_truck )
        {
            if ( set_towed_trailer_fn != nullptr )
            {
                set_towed_trailer_fn( parent, true, trailer );
            }

            prism::offsets::field< uint32_t >( parent, prism::offsets::truck_coupling_state ) = 5;

            prism::offsets::field< float >( parent, 0x4C4 ) = 1.f;
        }

        if ( clear_detached_flag_fn != nullptr )
        {
            clear_detached_flag_fn( parent, 1, 1 );
            clear_detached_flag_fn( trailer, 1, 1 );
        }

        if ( prism::offsets::field< void* >( trailer, prism::offsets::trailer_parked_state ) != nullptr )
        {
            prism::offsets::field< uint8_t >( trailer, prism::offsets::trailer_parked ) = 0;
        }

        if ( plausible_vfunc( trailer, 0x328 / 8 ) )
        {
            vcall< prism::physics_trailer_u_set_trailer_brace_fn >( trailer, 0x328 / 8, false );
        }

        prism::offsets::field< float >( trailer, prism::offsets::trailer_brake ) = 0.f;
        prism::offsets::field< uint8_t >( trailer, prism::offsets::trailer_brake_forced ) = 0;
        vcall< prism::physics_trailer_u_set_trailer_brace_fn >( trailer, prism::offsets::vfunc_set_brace, false );
        if ( rebuild_lines_fn != nullptr )
        {

            void* slave = prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave );
            prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = nullptr;
            rebuild_lines_fn( parent, trailer );
            prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = slave;
        }

        if ( set_lines_fn != nullptr )
        {
            set_lines_fn( trailer, 1, 1 );
            if ( !parent_is_truck ) set_lines_fn( parent, 1, 1 );
        }

        if ( refresh_accessories_fn != nullptr && parent != nullptr )
        {
            refresh_accessories_fn( parent, trailer );
        }
        if ( parent_is_truck )
        {
            if ( distribute_towed_mass_fn != nullptr )
            {
                distribute_towed_mass_fn( parent, prism::offsets::field< float >( trailer, prism::offsets::trailer_total_mass ), 0.f );
            }
            if ( refresh_axle_lift_fn != nullptr )
            {
                if ( auto* game_actor = CCore::g_instance->get_game_actor() ) refresh_axle_lift_fn( game_actor );
            }
        }

        if ( pair_vehicles_fn != nullptr )
        {
            pair_vehicles_fn( parent, trailer );
        }
        if ( parent_is_truck && apply_cables_fn != nullptr && cables_mode_cvar != nullptr &&
            get_cvar_int_fn != nullptr )
        {
            const int mode = get_cvar_int_fn( cables_mode_cvar );
            apply_cables_fn( parent, trailer, 0, mode > 4 ? 4 : mode < 0 ? 0 : mode, 0 );
        }
        if ( parent_is_truck && actor_flag_fixup_fn != nullptr )
        {
            if ( auto* game_actor = CCore::g_instance->get_game_actor() ) actor_flag_fixup_fn( game_actor );
        }

        play_brace_sound( trailer, true );
        if ( brace_anim_fn != nullptr )
        {
            brace_anim_fn( trailer, 0, 1.f / 3.f );
        }

        unhide_detached();
        dump_chain( "post-connect" );
    }

    // 175 - hook por-frame do sim: consome a fila de operações
    void hk_crashes_when_disconnected( prism::physics_trailer_u* self, prism::game_trailer_actor_u* trailer )
    {

        if ( !settings::g_settings.cargo_restore || load_game_active() )
        {
            crashes_when_disconnected_hook->get_original< prism::crashes_when_disconnected_fn >()( self, trailer );
            return;
        }
        pump_sim_heartbeat();

        if ( pending_cmd_queued.load( std::memory_order_acquire ) && execute_line_fn != nullptr )
        {
            pending_cmd_queued.store( false, std::memory_order_release );
            char* text = pending_cmd;
            execute_line_fn( &text, -1 );
        }
        if ( pending.kind != pending_kind::none )
        {
            if ( pending.delay > 0 )
            {
                --pending.delay;
            }
            else
            {
                const pending_op op = pending;
                pending = {};

                __try
                {
                    if ( op.kind == pending_kind::connect )
                    {
                        connect_now( op.trailer, op.parent, op.parent_is_truck );
                    }
                    else if ( op.kind == pending_kind::restore )
                    {
                        restore_parked_if_enabled();
                    }
                    else
                    {
                        disconnect_now( op.trailer, op.parent, op.parent_is_truck );
                    }
                }
                __except ( EXCEPTION_EXECUTE_HANDLER )
                {
                    const bool canon_alive = !canon_dead();
                    CCore::g_instance->warning( "pending op faulted (função 175) - canon {}",
                                                canon_alive ? "kept" : "reset" );
                    if ( !canon_alive ) reset_trailer_state();
                }
            }
        }
        if ( trailer != nullptr &&
            prism::offsets::field< void* >( trailer, prism::offsets::trailer_physics_joint ) == nullptr )
        {
            return;
        }
        crashes_when_disconnected_hook->get_original< prism::crashes_when_disconnected_fn >()( self, trailer );
    }

    // 176 - cauda de notificação do tráfego após desengate
    void traffic_detach_tail()
    {
        void* mgr = traffic_manager_global != nullptr ? *traffic_manager_global : nullptr;
        void* sub = mgr != nullptr ? prism::offsets::field< void* >( mgr, 0x2B30 ) : nullptr;
        if ( sub != nullptr ) vcall< prism::vehicle_notify_decoupled_fn >( sub, 0x138 / 8 );
        if ( relink_anchors_fn != nullptr )
        {
            void* game_actor = CCore::g_instance->get_game_actor();
            void* reg = game_actor != nullptr ? prism::offsets::field< void* >( game_actor, 0x30 ) : nullptr;
            if ( reg != nullptr && readable_ptr( reinterpret_cast< uint8_t* >( reg ) + 0xD8 ) )
            {
                relink_anchors_fn( reinterpret_cast< uint8_t* >( reg ) + 0xD8, 2 );
            }
        }
        if ( sub != nullptr ) vcall< prism::vehicle_notify_decoupled_fn >( sub, 0x1E0 / 8 );
    }

    combo_hook_pos_fn* combo_hook_pos_fn_ptr = nullptr;

    // 177 - hook: auto-engate de slave (recusa claims falsos)
    void hk_connect_slave( prism::physics_trailer_u* self )
    {
        if ( !settings::g_settings.cargo_restore || load_game_active() )
        {
            connect_slave_hook->get_original< prism::physics_trailer_u_connect_slave_fn >()( self );
            return;
        }
        void* slave = self != nullptr
            ? prism::offsets::field< void* >( self, prism::offsets::trailer_slave ) : nullptr;
        const int si = canon_index_of( self ), ti = canon_index_of( slave );
        const bool mother_parked = si >= 0 && !trailer_is_live( self ) &&
            trailer_is_known_parked( self );

        if ( !trailer_maintenance_ready() )
        {
            if ( is_detached( slave ) || is_plugin_detached( slave ) ||
                 mother_parked || claim_edge_ours( self, slave ) )
                return;
            connect_slave_hook->get_original< prism::physics_trailer_u_connect_slave_fn >()( self );
            return;
        }
        {
            const bool bad = is_detached( slave ) || is_plugin_detached( slave ) ||
                mother_parked || claim_edge_ours( self, slave ) ||
                ( ti >= 0 && joint( slave ) != nullptr ) ||
                ( si >= 0 && ti >= 0 && ti < si && claim_edge_ours( self, slave ) );
            if ( bad )
            {
                return;
            }

            if ( ti >= 0 && joint( slave ) == nullptr &&
                 hook_gap( slave, self, false ) > attach_gap_limit )
            {
                return;
            }
        }
        connect_slave_hook->get_original< prism::physics_trailer_u_connect_slave_fn >()( self );
    }

    using joint_walk_fn = float __fastcall( void* /*node*/, float /*arg*/ );
    // 178 - hook: caminhada de juntas (guarda anti-ciclo)
    float __fastcall hk_joint_walk( void* self, float arg )
    {
        if ( !settings::g_settings.cargo_restore || load_game_active() )
        {
            return joint_walk_hook->get_original< joint_walk_fn >()( self, arg );
        }
        thread_local void* jw_seen[ 96 ];
        thread_local int jw_depth = 0;
        for ( int i = 0; i < jw_depth; ++i )
        {
            if ( jw_seen[ i ] == self )
            {
                static int jw_logged = 0;
                if ( jw_logged++ < 8 )
                {
                    CCore::g_instance->warning(
                        "joint walk: cyclic node {:p} - skipped subtree", self );
                }
                return 0.f;
            }
        }
        if ( jw_depth >= 96 )
        {
            return 0.f;
        }
        jw_seen[ jw_depth++ ] = self;
        const float r = joint_walk_hook->get_original< joint_walk_fn >()( self, arg );
        --jw_depth;
        return r;
    }

    using phys_link_fn = void __fastcall( void* /*parent*/, void* /*child*/ );
    // 179 - hook: link físico (recusa ciclo)
    void __fastcall hk_phys_link( void* parent, void* child )
    {
        if ( !settings::g_settings.cargo_restore || load_game_active() )
        {
            phys_link_hook->get_original< phys_link_fn >()( parent, child );
            return;
        }
        if ( parent != nullptr && child != nullptr &&
             readable_ptr( parent ) && readable_ptr( child ) )
        {
            void* up = parent;
            for ( int i = 0; i < 128 && up != nullptr && readable_ptr( up ); ++i )
            {
                if ( up == child )
                {
                    static int pl_logged = 0;
                    if ( pl_logged++ < 8 )
                    {
                        CCore::g_instance->warning(
                            "phys link refused: child {:p} already above parent {:p} (cycle)",
                            child, parent );
                    }
                    return;
                }
                void* next = *reinterpret_cast< void** >( static_cast< uint8_t* >( up ) + 0x60 );
                if ( next == up ) break;
                up = next;
            }
        }
        phys_link_hook->get_original< phys_link_fn >()( parent, child );
    }

    // 180 - saneia a cadeia slave antes do teardown nativo
    void sanitize_slave_walk( void* head )
    {
        for ( uint32_t depth = 0; head != nullptr && depth < max_trailers * 4; ++depth )
        {
            if ( !is_trailer_obj( head ) ) return;
            repair_lamp_ctx( head );
            void* next = readable_ptr( static_cast< uint8_t* >( head ) + prism::offsets::trailer_slave )
                ? prism::offsets::field< void* >( head, prism::offsets::trailer_slave ) : nullptr;
            if ( next == head ) return;
            if ( next != nullptr && !is_trailer_obj( next ) )
            {
                CCore::g_instance->warning( "sanitize: severed dead slave link {:p} -> {:p}", head, next );
                prism::offsets::field< void* >( head, prism::offsets::trailer_slave ) = nullptr;
                return;
            }
            head = next;
        }
    }

    // 181 - saneia a cadeia física (corta arestas mortas)
    void sanitize_phys_walk( void* trailer )
    {
        if ( trailer == nullptr || !readable_ptr( trailer ) ) return;
        void* truck = nullptr;
        if ( auto* game_actor = CCore::g_instance->get_game_actor() )
        {
            truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
        }
        for ( int dir = 0; dir < 2; ++dir )
        {
            const uint32_t edge = dir == 0 ? 0x60 : 0x68;
            uint32_t pd = 0;
            for ( void* cur = trailer;
                  cur != nullptr && readable_ptr( cur ) && pd++ < max_trailers * 4; )
            {
                if ( is_trailer_obj( cur ) ) repair_lamp_ctx( cur );
                void* next = prism::offsets::field< void* >( cur, edge );
                if ( next == nullptr || next == cur ) break;
                if ( !is_trailer_obj( next ) && next != truck &&
                     !plausible_vfunc( next, 99 ) )
                {
                    break;
                }
                cur = next;
            }
        }
    }
    // 182 - hook: desengate vfunc 99 (saneia antes do teardown)
    void __fastcall hk_trailer_disconnect( void* trailer )
    {

        if ( !settings::g_settings.cargo_restore || load_game_active() )
        {
            sever_parked_claims();
            __try
            {
                trailer_disconnect_hook->get_original< prism::physics_trailer_u_disconnect_fn >()( trailer );
            }
            __except ( EXCEPTION_EXECUTE_HANDLER )
            {
                CCore::g_instance->warning( "disconnect walk faulted on {:p} (função 182, checkbox off)", trailer );
            }
            return;
        }
        if ( trailer == nullptr || !is_trailer_obj( trailer ) ) return;

        const auto repair_ctx = []( void* m ) { repair_lamp_ctx( m ); };
        repair_ctx( trailer );

        if ( tp_cooldown > 0 )
        {
            prism::offsets::field< void* >( trailer, prism::offsets::trailer_physics_joint ) = nullptr;
            prism::offsets::field< void* >( trailer, prism::offsets::trailer_parent ) = nullptr;
            return;
        }

        void* chain_members[ max_trailers * 2 ] = {};
        uint32_t chain_n = 0;

        uint32_t depth = 0;
        for ( void* t = trailer; t != nullptr && readable_ptr( t ); )
        {
            void* slave = prism::offsets::field< void* >( t, prism::offsets::trailer_slave );
            if ( slave == nullptr ) break;

            const bool bad = slave == t || !is_trailer_obj( slave ) ||
                joint( slave ) == nullptr;
            if ( bad || ++depth > max_trailers )
            {
                prism::offsets::field< void* >( t, prism::offsets::trailer_slave ) = nullptr;
                clear_claim_edge( t );
                break;
            }
            repair_ctx( slave );
            if ( chain_n < max_trailers * 2 ) chain_members[ chain_n++ ] = slave;
            t = slave;
        }
        sanitize_phys_walk( trailer );
        native_chain_call_active.store( true, std::memory_order_release );
        __try
        {
            trailer_disconnect_hook->get_original< prism::physics_trailer_u_disconnect_fn >()( trailer );
        }
        __except ( EXCEPTION_EXECUTE_HANDLER )
        {

            const bool canon_alive = !canon_dead();
            CCore::g_instance->warning( "disconnect walk faulted on {:p} (função 182) - canon {}", trailer,
                                        canon_alive ? "kept" : "reset" );
            if ( !canon_alive ) reset_trailer_state();
            if ( readable_ptr( trailer ) )
            {

                void* keep = nullptr;
                for ( uint32_t k = 0; k < chain_n; ++k )
                {
                    if ( is_trailer_obj( chain_members[ k ] ) )
                    {
                        keep = chain_members[ k ];
                        break;
                    }
                }
                prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = keep;

                mark_detached( trailer );
                mark_plugin_detached( trailer );
            }
            clear_claim_edge( trailer );
        }
        native_chain_call_active.store( false, std::memory_order_release );
    }

    struct severed_edge_t { void* parent; void* child; };
    severed_edge_t severed_edges[ max_trailers ];
    int severed_edge_count = 0;

    // 183 - corta os links-claim estacionados antes da chamada nativa
    void sever_parked_claims()
    {
        severed_edge_count = 0;

        if ( !trailer_maintenance_ready() ) return;
        for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
        {
            void* t = canon[ i ];
            if ( !readable_ptr( t ) ) continue;
            void* slave = prism::offsets::field< void* >( t, prism::offsets::trailer_slave );
            if ( slave == nullptr ) continue;
            if ( slave == t || !is_trailer_obj( slave ) )
            {
                prism::offsets::field< void* >( t, prism::offsets::trailer_slave ) = nullptr;
                clear_claim_edge( t );
                continue;
            }

            if ( joint( slave ) == nullptr )
            {
                prism::offsets::field< void* >( t, prism::offsets::trailer_slave ) = nullptr;
                clear_claim_edge( t );
                if ( severed_edge_count < static_cast< int >( max_trailers ) )
                    severed_edges[ severed_edge_count++ ] = { t, slave };
            }
        }
    }

    // 184 - corta arestas físicas que tocam peça sem junta
    void sever_phys_claims()
    {
        if ( !trailer_maintenance_ready() ) return;
        for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
        {
            void* t = canon[ i ];
            if ( !readable_ptr( t ) ) continue;
            for ( int dir = 0; dir < 2; ++dir )
            {
                const uint32_t edge = dir == 0 ? 0x60 : 0x68;
                void* next = prism::offsets::field< void* >( t, edge );
                if ( next == t ||
                     ( next != nullptr && is_trailer_obj( next ) && joint( next ) == nullptr ) )
                {
                    prism::offsets::field< void* >( t, edge ) = nullptr;
                }
            }
        }
    }

    // 185 - religa os claims cortados após a chamada nativa
    void restore_parked_claims()
    {
        for ( int i = 0; i < severed_edge_count; ++i )
        {
            void* p = severed_edges[ i ].parent;
            void* c = severed_edges[ i ].child;
            if ( readable_ptr( p ) && readable_ptr( c ) && joint( c ) == nullptr &&
                 prism::offsets::field< void* >( p, prism::offsets::trailer_slave ) == nullptr )
            {
                prism::offsets::field< void* >( p, prism::offsets::trailer_slave ) = c;
                set_claim_edge( p, c );
            }
        }
        severed_edge_count = 0;
    }

    // 186 - roda o connect nativo com a cadeia parked cortada
    void run_native_connect( void* trailer, void* parent, float3_t vec,
                             int state, bool slave_hook, bool is_slave,
                             prism::physics_trailer_u_connect_fn* orig )
    {
        if ( trailer == nullptr || !is_trailer_obj( trailer ) || orig == nullptr ) return;
        native_chain_call_active.store( true, std::memory_order_release );
        sever_parked_claims();
        sever_phys_claims();
        __try
        {
            orig( trailer, parent, vec, state, slave_hook, is_slave );
        }
        __except ( EXCEPTION_EXECUTE_HANDLER )
        {

            const bool canon_alive = !canon_dead();
            CCore::g_instance->warning( "connect walk faulted on {:p} (função 186) - canon {}", trailer,
                                        canon_alive ? "kept" : "reset" );
            if ( readable_ptr( trailer ) )
                prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = nullptr;
            clear_claim_edge( trailer );
            if ( !canon_alive ) reset_trailer_state();
        }
        restore_parked_claims();
        native_chain_call_active.store( false, std::memory_order_release );
    }
    // 187 - hook: engate impl A (vfunc 98)
    void __fastcall hk_trailer_connect_a( void* trailer, void* parent, float3_t vec,
                                          int state, bool slave_hook, bool is_slave )
    {
        if ( !settings::g_settings.cargo_restore || load_game_active() )
        {
            trailer_connect_hook->get_original< prism::physics_trailer_u_connect_fn >()(
                trailer, parent, vec, state, slave_hook, is_slave );
            return;
        }
        run_native_connect( trailer, parent, vec, state, slave_hook, is_slave,
            trailer_connect_hook->get_original< prism::physics_trailer_u_connect_fn >() );
    }
    // 188 - hook: engate impl B (vfunc 98)
    void __fastcall hk_trailer_connect_b( void* trailer, void* parent, float3_t vec,
                                          int state, bool slave_hook, bool is_slave )
    {
        if ( !settings::g_settings.cargo_restore || load_game_active() )
        {
            trailer_connect_hook2->get_original< prism::physics_trailer_u_connect_fn >()(
                trailer, parent, vec, state, slave_hook, is_slave );
            return;
        }
        run_native_connect( trailer, parent, vec, state, slave_hook, is_slave,
            trailer_connect_hook2->get_original< prism::physics_trailer_u_connect_fn >() );
    }

    // 189 - hook: pré-desengate (só passa com junta viva)
    void __fastcall hk_pre_detach( void* self, void* trailer )
    {
        if ( !settings::g_settings.cargo_restore || load_game_active() )
        {
            pre_detach_hook->get_original< prism::vehicle_pre_detach_fn >()( self, trailer );
            return;
        }
        if ( trailer == nullptr ||
            prism::offsets::field< void* >( trailer, prism::offsets::trailer_physics_joint ) == nullptr )
        {
            return;
        }
        pre_detach_hook->get_original< prism::vehicle_pre_detach_fn >()( self, trailer );
    }

    using native_detach_dispatch_fn = void __fastcall( void* /*game_actor*/, uint8_t /*dl*/, uint64_t /*r8*/ );
    void* __fastcall hk_trailer_dtor( void* self, uint64_t flags );
    void __fastcall hk_detach_dispatch_raw( void* self, uint8_t dl, uint64_t r8 );
    // 190 - hook: despachante de desengate (regra do freio de mão)
    void __fastcall hk_detach_dispatch( void* self, uint8_t dl, uint64_t r8 )
    {

        if ( !settings::g_settings.cargo_restore || load_game_active() )
        {
            sever_parked_claims();
            __try
            {
                detach_dispatch_hook->get_original< native_detach_dispatch_fn >()( self, dl, r8 );
            }
            __except ( EXCEPTION_EXECUTE_HANDLER )
            {
                CCore::g_instance->warning( "detach dispatch faulted on {:p} (função 190, checkbox off)", self );
            }
            return;
        }

        const uint64_t caller = reinterpret_cast< uint64_t >( _ReturnAddress() ) -
            reinterpret_cast< uint64_t >( GetModuleHandle( nullptr ) );
        const bool allowed = caller != 0x9F7DAE || canon_age < 90 || handbrake_ok();
        CCore::g_instance->info( "detach dispatch from exe+0x{:X}: handbrake {} -> {}",
            caller,
            parking_brake_on.load( std::memory_order_relaxed ) ? "on" : "off",
            allowed ? "allowed" : "REFUSED" );
        if ( !allowed )
        {
            CCore::g_instance->warning( "Puxe o freio de mão para desengatar" );
            return;
        }

        __try { hk_detach_dispatch_raw( self, dl, r8 ); }
        __except ( EXCEPTION_EXECUTE_HANDLER )
        {
            const bool canon_alive = !canon_dead();
            CCore::g_instance->warning( "detach dispatch faulted (função 190) - canon {}",
                                        canon_alive ? "kept" : "reset" );
            if ( !canon_alive ) reset_trailer_state();
        }
    }

    // 191 - hook: miolo do desengate (marca a cabeça)
    void __fastcall hk_detach_dispatch_raw( void* self, uint8_t dl, uint64_t r8 )
    {

        if ( canon_dead() ) reset_trailer_state();

        void* truck = self != nullptr
            ? prism::offsets::field< void* >( self, prism::offsets::game_actor_truck ) : nullptr;
        void* towed = truck != nullptr && readable_ptr( truck )
            ? prism::offsets::field< void* >( truck, 0x1580 ) : nullptr;
        if ( towed != nullptr && joint( towed ) != nullptr )
        {
            prism::offsets::field< void* >( self, prism::offsets::game_actor_trailer ) = towed;
        }

        repair_lamp_ctx_all();

        sanitize_slave_walk( prism::offsets::field< void* >( self, prism::offsets::game_actor_trailer ) );
        if ( towed != nullptr ) sanitize_slave_walk( towed );

        sanitize_phys_walk( prism::offsets::field< void* >( self, prism::offsets::game_actor_trailer ) );
        if ( towed != nullptr ) sanitize_phys_walk( towed );

        sever_phys_claims();
        dump_chain( "pre-detach(T)" );
        native_chain_call_active.store( true, std::memory_order_release );
        detach_dispatch_hook->get_original< native_detach_dispatch_fn >()( self, dl, r8 );
        native_chain_call_active.store( false, std::memory_order_release );
        convoy_refresh_requested.store( true, std::memory_order_release );
        dump_chain( "post-detach(T)" );

        for ( auto& torn : lines_torn ) torn = false;
        if ( self == nullptr ) return;
        void* t = prism::offsets::field< void* >( self, prism::offsets::game_actor_trailer );
        if ( t != nullptr && joint( t ) == nullptr )
        {
            pending_native_detach = nullptr;
            mark_detached( t );

            __try
            {
                if ( set_lines_fn != nullptr )
                {
                    set_lines_fn( t, 0, 1 );
                }
                if ( truck != nullptr && pair_vehicles_fn != nullptr )
                {
                    pair_vehicles_fn( truck, t );
                }
                if ( truck != nullptr && apply_cables_fn != nullptr )
                {

                    apply_cables_fn( truck, nullptr, 0, 0, 0 );
                }
            }
            __except ( EXCEPTION_EXECUTE_HANDLER )
            {
                CCore::g_instance->warning( "hose teardown faulted (função 191) - skipped" );
            }

        }
        else if ( t != nullptr )
        {

            pending_native_detach = t;
            pending_native_age = 0;
        }

        if ( truck != nullptr && readable_ptr( truck ) &&
            prism::offsets::field< void* >( truck, 0x1580 ) == nullptr &&
            canon[ 0 ] != nullptr && readable_ptr( canon[ 0 ] ) && joint( canon[ 0 ] ) == nullptr )
        {
            prism::offsets::field< void* >( self, prism::offsets::game_actor_trailer ) = canon[ 0 ];
        }

        rebuild_parked_claim_links();
    }

    bool attach_from_game = false;
    void __fastcall hk_attach_dispatch_raw( void* self, uint8_t dl, uint64_t r8, uint64_t r9,
        uint64_t arg5, const float* settle_override );
    // 192 - hook: despachante de engate
    void __fastcall hk_attach_dispatch( void* self, uint8_t dl, uint64_t r8, uint64_t r9,
        uint64_t arg5, const float* settle_override )
    {

        if ( !settings::g_settings.cargo_restore || load_game_active() )
        {
            attach_dispatch_hook->get_original< native_attach_dispatch_fn >()( self, dl, r8, r9, arg5, settle_override );
            return;
        }

        const uint64_t caller = reinterpret_cast< uint64_t >( _ReturnAddress() ) -
            reinterpret_cast< uint64_t >( GetModuleHandle( nullptr ) );
        const bool allowed = caller != 0x9F7DD6 || canon_age < 90 || handbrake_ok( false );
        CCore::g_instance->info( "attach dispatch from exe+0x{:X}: handbrake {} -> {}",
            caller,
            parking_brake_on.load( std::memory_order_relaxed ) ? "on" : "off",
            allowed ? "allowed" : "REFUSED" );
        if ( !allowed )
        {
            CCore::g_instance->warning( "Puxe o freio de mão para engatar" );
            return;
        }
        attach_from_game = caller != 0x9F7DD6;
        __try { hk_attach_dispatch_raw( self, dl, r8, r9, arg5, settle_override ); }
        __except ( EXCEPTION_EXECUTE_HANDLER )
        {
            const bool canon_alive = !canon_dead();
            CCore::g_instance->warning( "attach dispatch faulted (função 192) - canon {}",
                                        canon_alive ? "kept" : "reset" );
            if ( !canon_alive ) reset_trailer_state();
        }
        for ( auto& torn : lines_torn ) torn = false;
    }

    // 193 - hook: miolo do engate
    void __fastcall hk_attach_dispatch_raw( void* self, uint8_t dl, uint64_t r8, uint64_t r9,
        uint64_t arg5, const float* settle_override )
    {
        if ( canon_dead() ) reset_trailer_state();

        if ( attach_target != nullptr && self != nullptr )
        {
            if ( readable_ptr( attach_target ) && joint( attach_target ) == nullptr )
            {
                void* truck = prism::offsets::field< void* >( self, prism::offsets::game_actor_truck );
                const float tgap = truck != nullptr
                    ? hook_gap( attach_target, truck, true ) : -1.f;
                bool closest = tgap >= 0.f;
                for ( uint32_t i = 0; closest && i < max_trailers && canon[ i ] != nullptr; ++i )
                {
                    void* o = canon[ i ];
                    if ( o == attach_target || !readable_ptr( o ) || joint( o ) != nullptr )
                    {
                        continue;
                    }
                    const float g = hook_gap( o, truck, true );
                    if ( g >= 0.f && g < tgap )
                    {
                        closest = false;
                    }
                }
                if ( closest )
                {
                    prism::offsets::field< void* >( self, prism::offsets::game_actor_trailer ) = attach_target;
                }
            }
            else
            {
                attach_target = nullptr;
            }
        }
        void* head = self != nullptr
            ? prism::offsets::field< void* >( self, prism::offsets::game_actor_trailer ) : nullptr;

        bool released_marks = false;
        if ( head != nullptr && head == attach_target && is_plugin_detached( head ) )
        {
            release_detach_marks( head );
            released_marks = true;
        }
        if ( head != nullptr && is_plugin_detached( head ) )
        {

            void* truck = readable_ptr( self )
                ? prism::offsets::field< void* >( self, prism::offsets::game_actor_truck ) : nullptr;
            hook_xform_t hxf{};
            const bool under_hook = truck != nullptr && hook_world( head, hxf ) &&
                hook_gap( head, truck, true ) < attach_gap_limit;

            const bool free_mode = !settings::g_settings.cargo_restore && attach_from_game;
            if ( !under_hook && !free_mode )
            {
                CCore::g_instance->info( "attach dispatcher skipped: trailer {:p} is plugin-detached", head );
                return;
            }
            release_detach_marks( head );
            released_marks = true;
            CCore::g_instance->info( "attach dispatcher: T-attach released marks on {:p}", head );
        }

        if ( attach_target != nullptr && head == attach_target &&
            !is_trailer_obj( attach_target ) )
        {
            CCore::g_instance->warning( "attach refused: target {:p} is not a trailer object", attach_target );
            return;
        }

        void* severed_slave = nullptr;
        if ( head != nullptr && readable_ptr( head ) )
        {
            void* s = prism::offsets::field< void* >( head, prism::offsets::trailer_slave );

            if ( s != nullptr && joint( s ) == nullptr )
            {
                severed_slave = s;
                prism::offsets::field< void* >( head, prism::offsets::trailer_slave ) = nullptr;
            }
        }

        sever_parked_claims();

        sever_phys_claims();

        sanitize_slave_walk( head );
        native_chain_call_active.store( true, std::memory_order_release );
        attach_dispatch_hook->get_original< native_attach_dispatch_fn >()( self, dl, r8, r9, arg5, settle_override );
        native_chain_call_active.store( false, std::memory_order_release );
        convoy_refresh_requested.store( true, std::memory_order_release );
        if ( severed_slave != nullptr && readable_ptr( head ) )
        {
            prism::offsets::field< void* >( head, prism::offsets::trailer_slave ) = severed_slave;
        }
        restore_parked_claims();
        if ( released_marks && joint( head ) == nullptr )
        {

            mark_detached( head );
            mark_plugin_detached( head );
            const int hi = canon_index_of( head );
            if ( hi >= 0 ) detached_idx[ hi ] = true;
        }
        if ( attach_target != nullptr && joint( attach_target ) != nullptr )
        {
            CCore::g_instance->info( "attach target {:p} coupled natively", attach_target );
            attach_target = nullptr;
        }

        if ( head != nullptr && joint( head ) != nullptr )
        {
            unmark_detached( head );
            const int hi = canon_index_of( head );
            if ( hi >= 0 ) detached_idx[ hi ] = false;
            attach_target = nullptr;
            unhide_detached();
        }
    }

    using trailer_state_apply_fn = void __fastcall( void* /*record*/ );

    using trailer_teleport_fn = void __fastcall( void* /*trailer*/, void* /*transform*/ );

    using actor_teleport_fn = void __fastcall( void* /*actor*/, void* /*arg*/, uint8_t, uint8_t );

    // 194 - resolve/valida os endereços e instala os hooks
    bool resolve_native_fns()
    {
        if ( !expected_build() ) return false;

        const auto set_towed_address = validated_va( 0x140642080 );
        const auto clear_detached_address = validated_va( 0x140874010 );
        const auto rebuild_lines_address = validated_va( 0x1408663F0 );
        const auto distribute_mass_address = memory::get_address_for_pattern( "48 89 6c 24 18 57 48 83 ec 50 0f 29 74 24 40 48 8b f9 0f 57" );
        const auto refresh_lift_address = memory::get_address_for_pattern( "48 8b 91 c8 00 00 00 45 32 c0 48 85 d2 74 2c 4c 8b 8a c8 06" );

        const auto exec_line_address = validated_va( 0x1401E7F80 );

        const auto notify_decoupled_address = validated_va( 0x1408633D0 );
        const auto pre_detach_address = validated_va( 0x140864160 );

        const auto refresh_accessories_address = validated_va( 0x1408663F0 );
        if ( notify_decoupled_address != 0 ) notify_decoupled_fn = reinterpret_cast< prism::vehicle_notify_decoupled_fn* >( notify_decoupled_address );
        if ( pre_detach_address != 0 )
        {
            pre_detach_fn = reinterpret_cast< prism::vehicle_pre_detach_fn* >( pre_detach_address );
            pre_detach_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
                "vehicle_pre_detach", pre_detach_address,
                reinterpret_cast< uint64_t >( &hk_pre_detach ) );
            if ( pre_detach_hook != nullptr )
            {
                pre_detach_hook->hook();
            }
        }
        if ( refresh_accessories_address != 0 ) refresh_accessories_fn = reinterpret_cast< prism::vehicle_refresh_accessories_fn* >( refresh_accessories_address );

        get_cvar_float_fn = reinterpret_cast< prism::cvar_get_float_fn* >( validated_va( 0x1401DB560 ) );
        g_traffic_cvar = reinterpret_cast< void* >( validated_va( 0x142D45500 ) );
        traffic_manager_global = reinterpret_cast< void** >( validated_va( 0x1436AF7C8 ) );
        relink_anchors_fn = reinterpret_cast< anchor_relink_fn* >( validated_va( 0x140623C90 ) );
        traffic_game_state = reinterpret_cast< uint32_t* >( validated_va( 0x143051820 ) );

        trailer_settle_fn_ptr = reinterpret_cast< trailer_settle_fn* >( validated_va( 0x1409FB700 ) );
        pair_vehicles_fn = reinterpret_cast< vehicle_pair_fn* >( validated_va( 0x1408771A0 ) );
        apply_cables_fn = reinterpret_cast< vehicle_cables_fn* >( validated_va( 0x1408762E0 ) );

        set_lines_fn = reinterpret_cast< line_state_fn* >( validated_va( 0x140874010 ) );
        parked_record_factory = reinterpret_cast< parked_record_factory_fn* >( validated_va( 0x1407B69E0 ) );
        parked_record_mgr = reinterpret_cast< void** >( validated_va( 0x1436AF828 ) );
        if ( const auto addr = validated_va( 0x1408762E0 ) )
        {
            apply_cables_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
                "apply_cables", addr,
                reinterpret_cast< uint64_t >( &hk_apply_cables ) );
            if ( apply_cables_hook != nullptr )
            {
                apply_cables_hook->hook();
            }
        }

        if ( const auto addr = validated_va( 0x140874010 ) )
        {
            set_lines_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
                "set_lines", addr,
                reinterpret_cast< uint64_t >( &hk_set_lines ) );
            if ( set_lines_hook != nullptr )
            {
                set_lines_hook->hook();
            }
        }

        if ( const auto addr = validated_va( 0x1408653A0 ) )
        {
            hose_update_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
                "hose_update", addr,
                reinterpret_cast< uint64_t >( &hk_hose_update ) );
            if ( hose_update_hook != nullptr )
            {
                hose_update_hook->hook();
            }
        }
        actor_flag_fixup_fn = reinterpret_cast< actor_flag_fixup* >( validated_va( 0x1405DCBC0 ) );
        native_attach_fn = reinterpret_cast< native_attach_dispatch_fn* >( validated_va( 0x1405DCC20 ) );
        if ( const auto addr = validated_va( 0x1405DD410 ) )
        {
            detach_dispatch_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
                "native_detach_dispatch", addr,
                reinterpret_cast< uint64_t >( &hk_detach_dispatch ) );
            if ( detach_dispatch_hook != nullptr )
            {
                detach_dispatch_hook->hook();
            }
        }

        if ( const auto addr = validated_va( 0x1405DCC20 ) )
        {
            attach_dispatch_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
                "native_attach_dispatch", addr,
                reinterpret_cast< uint64_t >( &hk_attach_dispatch ) );
            if ( attach_dispatch_hook != nullptr )
            {
                attach_dispatch_hook->hook();
            }
        }
        if ( const auto addr = validated_va( 0x1405E14D1 ) )
        {
            trailer_state_apply_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
                "trailer_state_apply", addr,
                reinterpret_cast< uint64_t >( &hk_trailer_state_apply ) );
            if ( trailer_state_apply_hook != nullptr )
            {
                trailer_state_apply_hook->hook();
            }
        }
        if ( const auto addr = validated_va( 0x140786320 ) )
        {
            trailer_teleport_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
                "trailer_teleport", addr,
                reinterpret_cast< uint64_t >( &hk_trailer_teleport ) );
            if ( trailer_teleport_hook != nullptr )
            {
                trailer_teleport_hook->hook();
            }
        }
        if ( const auto addr = validated_va( 0x1405DE1E0 ) )
        {
            actor_teleport_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
                "actor_teleport", addr,
                reinterpret_cast< uint64_t >( &hk_actor_teleport ) );
            if ( actor_teleport_hook != nullptr )
            {
                actor_teleport_hook->hook();
            }
        }

        if ( const auto addr = validated_va( 0x14063C2D0 ) )
        {
            cam_set_target_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
                "cam_set_target", addr,
                reinterpret_cast< uint64_t >( &hk_cam_set_target ) );
            if ( cam_set_target_hook != nullptr )
            {
                cam_set_target_hook->hook();
            }
        }

        if ( const auto addr = validated_va( 0x14094DFC0 ) )
        {
            lane_obstacle_insert_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
                "lane_obstacle_insert", addr,
                reinterpret_cast< uint64_t >( &hk_lane_obstacle_insert ) );
            if ( lane_obstacle_insert_hook != nullptr )
            {
                lane_obstacle_insert_hook->hook();
            }
        }

        combo_hook_pos_fn_ptr = reinterpret_cast< combo_hook_pos_fn* >(
            validated_va( 0x140789880 ) );
        if ( const auto addr = validated_va( 0x140789AB0 ) )
        {
            combo_length_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
                "combo_length", addr,
                reinterpret_cast< uint64_t >( &hk_combo_length ) );
            if ( combo_length_hook != nullptr )
            {
                combo_length_hook->hook();
            }
        }

        init_purecall_fake();
        if ( const auto addr = validated_va( 0x141D23610 ) )
        {
            purecall_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
                "purecall", addr,
                reinterpret_cast< uint64_t >( &hk_purecall ) );
            if ( purecall_hook != nullptr )
            {
                purecall_hook->hook();
                install_crash_tracer();
            }
        }

        if ( const auto addr = validated_va( 0x140789C20 ) )
        {
            trailer_disconnect_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
                "trailer_disconnect", addr,
                reinterpret_cast< uint64_t >( &hk_trailer_disconnect ) );
            if ( trailer_disconnect_hook != nullptr )
            {
                trailer_disconnect_hook->hook();
            }
        }

        if ( const auto addr = validated_va( 0x140639090 ) )
        {
            trailer_connect_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
                "trailer_connect_a", addr,
                reinterpret_cast< uint64_t >( &hk_trailer_connect_a ) );
            if ( trailer_connect_hook != nullptr )
            {
                trailer_connect_hook->hook();
            }
        }
        if ( const auto addr = validated_va( 0x1407890A0 ) )
        {
            trailer_connect_hook2 = CCore::g_instance->get_hooks_manager()->register_function_hook(
                "trailer_connect_b", addr,
                reinterpret_cast< uint64_t >( &hk_trailer_connect_b ) );
            if ( trailer_connect_hook2 != nullptr )
            {
                trailer_connect_hook2->hook();
            }
        }

        if ( const auto addr = validated_va( 0x1409DB4B0 ) )
        {
            lamp_mask_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
                "lamp_mask", addr,
                reinterpret_cast< uint64_t >( &hk_lamp_mask ) );
            if ( lamp_mask_hook != nullptr )
            {
                lamp_mask_hook->hook();
            }
        }
        if ( const auto addr = validated_va( 0x1406359E0 ) )
        {
            trailer_dtor_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
                "trailer_dtor", addr,
                reinterpret_cast< uint64_t >( &hk_trailer_dtor ) );
            if ( trailer_dtor_hook != nullptr )
            {
                trailer_dtor_hook->hook();
            }
        }

        if ( const auto addr = validated_va( 0x140790DC0 ) )
        {
            lamp_propagate_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
                "lamp_propagate", addr,
                reinterpret_cast< uint64_t >( &hk_lamp_propagate ) );
            if ( lamp_propagate_hook != nullptr )
            {
                lamp_propagate_hook->hook();
            }
        }

        if ( const auto addr = validated_va( 0x1409555F0 ) )
        {
            joint_walk_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
                "joint_walk", addr,
                reinterpret_cast< uint64_t >( &hk_joint_walk ) );
            if ( joint_walk_hook != nullptr )
            {
                joint_walk_hook->hook();
            }
        }

        if ( const auto addr = validated_va( 0x140919680 ) )
        {
            phys_link_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
                "phys_link", addr,
                reinterpret_cast< uint64_t >( &hk_phys_link ) );
            if ( phys_link_hook != nullptr )
            {
                phys_link_hook->hook();
            }
        }

        if ( const auto addr = validated_va( 0x140AC75A0 ) )
        {
            cam_update_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
                "cam_update", addr,
                reinterpret_cast< uint64_t >( &hk_cam_update ) );
            if ( cam_update_hook != nullptr )
            {
                cam_update_hook->hook();
            }
        }
        get_cvar_int_fn = reinterpret_cast< cvar_get_int_fn* >( validated_va( 0x1401CBE10 ) );
        cables_mode_cvar = reinterpret_cast< void* >( validated_va( 0x142D485C0 ) );
        attach_settle_cvar = reinterpret_cast< void* >( validated_va( 0x142D48200 ) );

        post_event_fn_ptr = reinterpret_cast< post_event_fn* >( validated_va( 0x140623DA0 ) );
        event_param_block = reinterpret_cast< void* >( validated_va( 0x142733198 ) );
        event_name_coupling = reinterpret_cast< const char* >( validated_va( 0x14226E368 ) );
        event_name_decoupling = reinterpret_cast< const char* >( validated_va( 0x14226E4B0 ) );
        brace_anim_fn = reinterpret_cast< trailer_brace_anim_fn* >( validated_va( 0x140636F50 ) );

        if ( exec_line_address != 0 )
        {
            execute_line_fn = reinterpret_cast< prism::console_execute_line_fn* >( exec_line_address );
        }

        if ( set_towed_address != 0 && clear_detached_address != 0 && rebuild_lines_address != 0 &&
            distribute_mass_address != 0 && refresh_lift_address != 0 )
        {
            set_towed_trailer_fn = reinterpret_cast< prism::vehicle_set_towed_trailer_fn* >( set_towed_address );
            clear_detached_flag_fn = reinterpret_cast< prism::vehicle_clear_detached_flag_fn* >( clear_detached_address );
            rebuild_lines_fn = reinterpret_cast< prism::vehicle_rebuild_lines_fn* >( rebuild_lines_address );
            rebuild_lines_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
                "rebuild_lines", rebuild_lines_address,
                reinterpret_cast< uint64_t >( &hk_rebuild_lines ) );
            if ( rebuild_lines_hook != nullptr ) rebuild_lines_hook->hook();
            distribute_towed_mass_fn = reinterpret_cast< prism::vehicle_distribute_towed_mass_fn* >( distribute_mass_address );
            refresh_axle_lift_fn = reinterpret_cast< prism::game_actor_refresh_axle_lift_fn* >( refresh_lift_address );
            return true;
        }
        CCore::g_instance->warning( "Funções nativas de engate não encontradas; a carreta engatada pode ficar freiada" );
        return false;
    }

    using trailer_dtor_fn = void* __fastcall( void* /*self*/, uint64_t /*flags*/ );
    // 195 - hook: destrutor de carreta (limpa registros)
    void* __fastcall hk_trailer_dtor( void* self, uint64_t flags )
    {
        if ( self != nullptr && canon_age >= 90 && canon_index_of( self ) >= 0 && is_trailer_obj( self ) )
        {
            int cut = 0;
            for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
            {
                void* c = canon[ i ];
                if ( c == self || !readable_ptr( c ) || !is_trailer_obj( c ) ) continue;
                if ( prism::offsets::field< void* >( c, prism::offsets::trailer_slave ) == self &&
                     joint( self ) == nullptr )
                {
                    prism::offsets::field< void* >( c, prism::offsets::trailer_slave ) = nullptr;
                    clear_claim_edge( c );
                    ++cut;
                }
            }
            void* sl = prism::offsets::field< void* >( self, prism::offsets::trailer_slave );
            if ( sl != nullptr && ( !is_trailer_obj( sl ) || joint( sl ) == nullptr ) )
            {
                prism::offsets::field< void* >( self, prism::offsets::trailer_slave ) = nullptr;
                ++cut;
            }

            if ( auto* game_actor = CCore::g_instance->get_game_actor() )
            {
                if ( prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_trailer ) == self )
                {
                    prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_trailer ) = nullptr;
                    ++cut;
                }
            }
            CCore::g_instance->info( "trailer destructor on c{}: {} claim link(s) cut", canon_index_of( self ), cut );
        }
        return trailer_dtor_hook->get_original< trailer_dtor_fn >()( self, flags );
    }

    SCSAPI_VOID on_parking_brake( const scs_string_t, const scs_u32_t, const scs_value_t* const value, const scs_context_t )
    {
        parking_brake_on.store( value != nullptr && value->value_bool.value != 0, std::memory_order_relaxed );
        parking_brake_known.store( value != nullptr, std::memory_order_relaxed );
    }

    // 196 - registra os canais de telemetria
    void coupling_register_channels()
    {
        const auto* init_params = CCore::g_instance->get_init_params();
        if ( init_params->register_for_channel( SCS_TELEMETRY_TRUCK_CHANNEL_parking_brake,
                SCS_U32_NIL, SCS_VALUE_TYPE_bool, SCS_TELEMETRY_CHANNEL_FLAG_none,
                &on_parking_brake, nullptr ) != SCS_RESULT_ok )
        {
            CCore::g_instance->warning( "Could not register 'truck.brake.parking'" );
        }
    }

    // 197 - instala o sistema de engate
    bool coupling_install()
    {
        const auto crash_address = memory::get_address_for_pattern( "48 85 d2 0f 84 ? ? ? ? 48 89 74 24 18 57 48 83 ec 40" );
        const auto connect_slave_address = memory::get_address_for_pattern( "40 53 48 83 ec 60 48 83 b9 ? ? ? ? 00 48 8b d9 0f 84 ? ? ? ? 48 8d 54 24 ? e8" );

        if ( crash_address == 0 || connect_slave_address == 0 )
        {
            CCore::g_instance->warning( "Engate/desengate de carretas indisponível nesta versão do jogo" );
            return false;
        }

        get_slave_hook_position_fn = reinterpret_cast< prism::physics_trailer_u_get_slave_hook_position_fn* >(
            connect_slave_address + 29 + *reinterpret_cast< int32_t* >( connect_slave_address + 29 ) + 4 );

        const bool natives_ok = resolve_native_fns();
        brake_register_hooks();

        crashes_when_disconnected_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "crashes_when_disconnected",
            crash_address,
            reinterpret_cast< uint64_t >( &hk_crashes_when_disconnected ) );

        connect_slave_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "connect_slave",
            connect_slave_address,
            reinterpret_cast< uint64_t >( &hk_connect_slave ) );

        if ( !CCore::g_instance->is_truckersmp() && natives_ok &&
            crashes_when_disconnected_hook->hook() == CHook::HOOKED &&
            connect_slave_hook->hook() == CHook::HOOKED )
        {
            return true;
        }
        CCore::g_instance->warning( "Não foi possível instalar os hooks de engate/desengate" );
        return false;
    }

    namespace
    {
        const bool fn_idx_reg = []()
        {
            fn_index_add( fn_addr_of( &quat_rot ), 167, "quat_rot" );
            fn_index_add( fn_addr_of( &attach_offset ), 168, "attach_offset" );
            fn_index_add( fn_addr_of( &hook_gap ), 169, "hook_gap" );
            fn_index_add( fn_addr_of( &hook_far ), 170, "hook_far" );
            fn_index_add( fn_addr_of( &native_detach_via_slot ), 171, "native_detach_via_slot" );
            fn_index_add( fn_addr_of( &native_attach_via_slot ), 172, "native_attach_via_slot" );
            fn_index_add( fn_addr_of( &disconnect_now ), 173, "disconnect_now" );
            fn_index_add( fn_addr_of( &connect_now ), 174, "connect_now" );
            fn_index_add( fn_addr_of( &hk_crashes_when_disconnected ), 175, "hk_crashes_when_disconnected" );
            fn_index_add( fn_addr_of( &traffic_detach_tail ), 176, "traffic_detach_tail" );
            fn_index_add( fn_addr_of( &hk_connect_slave ), 177, "hk_connect_slave" );
            fn_index_add( fn_addr_of( &hk_joint_walk ), 178, "hk_joint_walk" );
            fn_index_add( fn_addr_of( &hk_phys_link ), 179, "hk_phys_link" );
            fn_index_add( fn_addr_of( &sanitize_slave_walk ), 180, "sanitize_slave_walk" );
            fn_index_add( fn_addr_of( &sanitize_phys_walk ), 181, "sanitize_phys_walk" );
            fn_index_add( fn_addr_of( &hk_trailer_disconnect ), 182, "hk_trailer_disconnect" );
            fn_index_add( fn_addr_of( &sever_parked_claims ), 183, "sever_parked_claims" );
            fn_index_add( fn_addr_of( &sever_phys_claims ), 184, "sever_phys_claims" );
            fn_index_add( fn_addr_of( &restore_parked_claims ), 185, "restore_parked_claims" );
            fn_index_add( fn_addr_of( &run_native_connect ), 186, "run_native_connect" );
            fn_index_add( fn_addr_of( &hk_trailer_connect_a ), 187, "hk_trailer_connect_a" );
            fn_index_add( fn_addr_of( &hk_trailer_connect_b ), 188, "hk_trailer_connect_b" );
            fn_index_add( fn_addr_of( &hk_pre_detach ), 189, "hk_pre_detach" );
            fn_index_add( fn_addr_of( &hk_detach_dispatch ), 190, "hk_detach_dispatch" );
            fn_index_add( fn_addr_of( &hk_detach_dispatch_raw ), 191, "hk_detach_dispatch_raw" );
            fn_index_add( fn_addr_of( &hk_attach_dispatch ), 192, "hk_attach_dispatch" );
            fn_index_add( fn_addr_of( &hk_attach_dispatch_raw ), 193, "hk_attach_dispatch_raw" );
            fn_index_add( fn_addr_of( &resolve_native_fns ), 194, "resolve_native_fns" );
            fn_index_add( fn_addr_of( &hk_trailer_dtor ), 195, "hk_trailer_dtor" );
            fn_index_add( fn_addr_of( &coupling_register_channels ), 196, "coupling_register_channels" );
            fn_index_add( fn_addr_of( &coupling_install ), 197, "coupling_install" );
            return true;
        }();
    }
}
