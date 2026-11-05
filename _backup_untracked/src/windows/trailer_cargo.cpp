#include "trailer_manipulation.hpp"
#include "trailer_shared.hpp"
#include "trailer_coupling_internal.hpp"

#include <algorithm>
#include <cmath>

#include "imgui.h"
#include "core.hpp"
#include "prism/offsets.hpp"
#include "settings/settings.hpp"

namespace br_carretas
{
    namespace
    {
        std::atomic< uint64_t > cargo_trace_id{ 0 };
        std::atomic< uint64_t > cargo_trace_start_tick{ 0 };
        std::atomic< uint64_t > cargo_trace_deadline{ 0 };
        std::atomic< uint64_t > cargo_trace_hard_deadline{ 0 };
        std::atomic< bool > cargo_trace_enabled{ false };
        std::atomic< bool > cargo_trace_reset{ false };
    }

    bool cargo_trace_active()
    {
        const uint64_t now = GetTickCount64();
        return cargo_trace_enabled.load( std::memory_order_acquire ) &&
            now < cargo_trace_deadline.load( std::memory_order_acquire ) &&
            now < cargo_trace_hard_deadline.load( std::memory_order_acquire );
    }

    void cargo_trace_event( const char* event, void* object, const int detail )
    {
        if ( !cargo_trace_active() ) return;
        const uint64_t now = GetTickCount64();
        const uint64_t hard_deadline =
            cargo_trace_hard_deadline.load( std::memory_order_acquire );
        cargo_trace_deadline.store( ( std::min )( now + 10000, hard_deadline ),
                                    std::memory_order_release );
        const uint64_t id = cargo_trace_id.load( std::memory_order_relaxed );
        const uint64_t dt = now - cargo_trace_start_tick.load( std::memory_order_relaxed );
        if ( object != nullptr && is_trailer_obj( object ) )
        {
            hook_xform_t pose{};
            const bool pose_valid = hook_world( object, pose );
            const int i = canon_index_of( object );
            void* parent = prism::offsets::field< void* >( object, prism::offsets::trailer_parent );
            void* slave = readable_ptr( static_cast< uint8_t* >( object ) +
                prism::offsets::trailer_slave )
                ? prism::offsets::field< void* >( object, prism::offsets::trailer_slave ) : nullptr;
            CCore::g_instance->info(
                "cargo-trace id={} event={} dt={}ms c{} object={:p} detail={} thread={} "
                "pose-valid={} pos=({:.3f},{:.3f},{:.3f}) joint={:p} parent={:p} slave={:p} det={} pdet={}",
                id, event, dt, i, object, detail, GetCurrentThreadId(), pose_valid,
                pose.pos.x, pose.pos.y, pose.pos.z, joint( object ), parent, slave,
                is_detached( object ), is_plugin_detached( object ) );
            if ( i >= 0 )
            {
                CCore::g_instance->info(
                    "cargo-trace id={} event-state c{} parked={}/{} anchor={}/{} passive={} "
                    "anchor-delay={} detached={} rejoined={} carry-hold={} carry-anchor-delay={} carry-park={}/{}",
                    id, i,
                    prism::offsets::field< uint8_t >( object, prism::offsets::trailer_parked ),
                    prism::offsets::field< uint8_t >( object, prism::offsets::trailer_parked_registered ),
                    anchor_valid[ i ], anchor_settling[ i ], cargo_anchor_passive[ i ],
                    anchor_delay[ i ], detached_idx[ i ], rejoined_frames[ i ],
                    carry_hold[ i ], carry_anchor_delay[ i ], carry_park[ i ], carry_park_pending[ i ] );
            }
        }
        else
        {
            CCore::g_instance->info( "cargo-trace id={} event={} dt={}ms object={:p} detail={} thread={}",
                id, event, dt, object, detail, GetCurrentThreadId() );
        }
    }

    void cargo_trace_begin( const char* reason, void* object )
    {
        if ( cargo_trace_active() )
        {
            cargo_trace_event( reason, object );
            return;
        }
        const uint64_t now = GetTickCount64();
        const uint64_t id = cargo_trace_id.fetch_add( 1, std::memory_order_acq_rel ) + 1;
        cargo_trace_start_tick.store( now, std::memory_order_release );
        cargo_trace_deadline.store( now + 10000, std::memory_order_release );
        cargo_trace_hard_deadline.store( now + 60000, std::memory_order_release );
        cargo_trace_enabled.store( true, std::memory_order_release );
        cargo_trace_reset.store( true, std::memory_order_release );
        CCore::g_instance->info( "cargo-trace id={} BEGIN reason={} object={:p} thread={}",
            id, reason, object, GetCurrentThreadId() );
    }

    void cargo_trace_update()
    {
        static void* previous_object[ max_trailers ] = {};
        static hook_xform_t previous_pose[ max_trailers ] = {};
        static bool previous_pose_valid[ max_trailers ] = {};
        static uint64_t previous_sample_tick = 0;
        static uint64_t previous_line_tick = 0;
        static uint32_t sample = 0;
        if ( !cargo_trace_enabled.load( std::memory_order_acquire ) ) return;
        const uint64_t now = GetTickCount64();
        if ( now >= cargo_trace_deadline.load( std::memory_order_acquire ) ||
             now >= cargo_trace_hard_deadline.load( std::memory_order_acquire ) )
        {
            if ( cargo_trace_enabled.exchange( false, std::memory_order_acq_rel ) )
            {
                CCore::g_instance->info( "cargo-trace id={} END elapsed={}ms samples={}",
                    cargo_trace_id.load( std::memory_order_relaxed ),
                    now - cargo_trace_start_tick.load( std::memory_order_relaxed ),
                    sample );
            }
            return;
        }
        if ( cargo_trace_reset.exchange( false, std::memory_order_acq_rel ) )
        {
            for ( uint32_t i = 0; i < max_trailers; ++i )
            {
                previous_object[ i ] = nullptr;
                previous_pose_valid[ i ] = false;
            }
            previous_sample_tick = 0;
            previous_line_tick = 0;
            sample = 0;
        }
        if ( now - previous_sample_tick < 100 ) return;
        const uint64_t dt = previous_sample_tick != 0 ? now - previous_sample_tick : 0;
        previous_sample_tick = now;
        ++sample;
        CCore::g_instance->info(
            "cargo-trace id={} sample={} dt={}ms thread={} sim-thread={} canon-age={} pending={} tp-cooldown={} carry-brake={}",
            cargo_trace_id.load( std::memory_order_relaxed ),
            sample, dt, GetCurrentThreadId(), sim_thread_id, canon_age,
            static_cast< int >( pending.kind ), tp_cooldown, carry_brake_frames );
        void* truck = nullptr;
        if ( auto* game_actor = CCore::g_instance->get_game_actor() )
            truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
        hook_xform_t truck_pose{};
        const bool truck_pose_valid = truck != nullptr && hook_world( truck, truck_pose );
        void* towed = truck != nullptr && readable_ptr( truck )
            ? prism::offsets::field< void* >( truck, 0x15A8 ) : nullptr;
        CCore::g_instance->info(
            "cargo-trace id={} sample={} truck={:p} pose-valid={} pos=({:.3f},{:.3f},{:.3f}) "
            "towed={:p} coupling-state={}",
            cargo_trace_id.load( std::memory_order_relaxed ), sample, truck, truck_pose_valid,
            truck_pose.pos.x, truck_pose.pos.y, truck_pose.pos.z, towed,
            truck != nullptr && readable_ptr( truck )
                ? prism::offsets::field< uint32_t >( truck, prism::offsets::truck_coupling_state ) : 0 );
        for ( uint32_t i = 0; i < max_trailers; ++i )
        {
            void* t = canon[ i ];
            if ( t == nullptr )
            {
                if ( i < 3 )
                    CCore::g_instance->info( "cargo-trace id={} sample={} c{} object=null",
                        cargo_trace_id.load( std::memory_order_relaxed ), sample, i );
                previous_object[ i ] = nullptr;
                previous_pose_valid[ i ] = false;
                continue;
            }
            if ( !is_trailer_obj( t ) )
            {
                CCore::g_instance->info( "cargo-trace id={} sample={} c{} object={:p} invalid",
                    cargo_trace_id.load( std::memory_order_relaxed ), sample, i, t );
                previous_object[ i ] = t;
                previous_pose_valid[ i ] = false;
                continue;
            }
            hook_xform_t pose{};
            const bool pose_valid = hook_world( t, pose );
            const bool delta_valid = pose_valid && previous_object[ i ] == t &&
                previous_pose_valid[ i ] && dt != 0;
            const float dx = delta_valid ? pose.pos.x - previous_pose[ i ].pos.x : 0.f;
            const float dy = delta_valid ? pose.pos.y - previous_pose[ i ].pos.y : 0.f;
            const float dz = delta_valid ? pose.pos.z - previous_pose[ i ].pos.z : 0.f;
            const float qdot = delta_valid
                ? std::fabs( pose.quat.w * previous_pose[ i ].quat.w +
                    pose.quat.x * previous_pose[ i ].quat.x +
                    pose.quat.y * previous_pose[ i ].quat.y +
                    pose.quat.z * previous_pose[ i ].quat.z )
                : 1.f;
            const float rotation_delta = delta_valid ? 1.f - qdot : 0.f;
            const float vx = delta_valid ? dx * 1000.f / dt : 0.f;
            const float vy = delta_valid ? dy * 1000.f / dt : 0.f;
            const float vz = delta_valid ? dz * 1000.f / dt : 0.f;
            const float up_y = pose_valid
                ? 1.f - 2.f * ( pose.quat.x * pose.quat.x + pose.quat.z * pose.quat.z ) : 0.f;
            const bool slave_readable =
                readable_ptr( static_cast< uint8_t* >( t ) + prism::offsets::trailer_slave );
            void* slave = slave_readable
                ? prism::offsets::field< void* >( t, prism::offsets::trailer_slave ) : nullptr;
            void* parent = prism::offsets::field< void* >( t, prism::offsets::trailer_parent );
            CCore::g_instance->info(
                "cargo-trace id={} sample={} c{} object={:p} pose-valid={} pos=({:.3f},{:.3f},{:.3f}) "
                "delta=({:.3f},{:.3f},{:.3f}) vel=({:.2f},{:.2f},{:.2f}) "
                "quat=({:.4f},{:.4f},{:.4f},{:.4f}) drot={:.5f} up-y={:.3f} "
                "joint={:p} parent={:p} slave={:p} det={} pdet={} "
                "parked={}/{} detached={} anchor={}/{} passive={} carry={}/{} carry-anchor-delay={} carry-park={}/{} rejoined={}",
                cargo_trace_id.load( std::memory_order_relaxed ), sample, i, t, pose_valid,
                pose.pos.x, pose.pos.y, pose.pos.z, dx, dy, dz, vx, vy, vz,
                pose.quat.w, pose.quat.x, pose.quat.y, pose.quat.z, rotation_delta, up_y,
                joint( t ), parent, slave,
                is_detached( t ), is_plugin_detached( t ),
                prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked ),
                prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked_registered ),
                detached_idx[ i ], anchor_valid[ i ], anchor_settling[ i ], cargo_anchor_passive[ i ],
                carry_hold[ i ], carry_brake_frames,
                carry_anchor_delay[ i ], carry_park[ i ], carry_park_pending[ i ],
                rejoined_frames[ i ] );
            if ( anchor_valid[ i ] )
            {
                CCore::g_instance->info(
                    "cargo-trace id={} sample={} c{} anchor-pos=({:.3f},{:.3f},{:.3f}) "
                    "settle={} age={} delay={} fixes={}",
                    cargo_trace_id.load( std::memory_order_relaxed ), sample, i,
                    anchor_xform[ i ].pos.x, anchor_xform[ i ].pos.y, anchor_xform[ i ].pos.z,
                    anchor_settle[ i ], anchor_age[ i ], anchor_delay[ i ], anchor_fixes[ i ] );
            }
            previous_object[ i ] = t;
            previous_pose_valid[ i ] = pose_valid;
            if ( pose_valid ) previous_pose[ i ] = pose;
        }
        if ( now - previous_line_tick >= 500 )
        {
            previous_line_tick = now;
            CCore::g_instance->info( "cargo-trace id={} sample={} line-state convoy={} traffic-detach={}",
                cargo_trace_id.load( std::memory_order_relaxed ), sample,
                trailer_count, traffic_detach_done.load( std::memory_order_acquire ) );
        }
    }

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
            "Ative para usar o sistema de desacoplamento/acoplamento. Ligado, o jogo nao teleporta "
            "na camera 0 (Ctrl+F9) nem ao carregar/descarregar - as carretas separadas ficam presas "
            "no lugar. Desligado, engate, desengate, teleporte, carga e descarga voltam ao nativo do "
            "jogo. Para teleportar para outro lugar, desligue, teleporte e ligue de novo." );
    }

    void restore_parked_if_enabled()
    {
        if ( settings::g_settings.cargo_restore ) restore_parked();
    }

    void snapshot_rig()
    {
        rig_truck_valid = false;
        if ( auto* game_actor = CCore::g_instance->get_game_actor() )
        {
            void* truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
            rig_truck_valid = truck != nullptr && hook_world( truck, rig_truck_xform );
        }
    }
    // Devolve o que o respawn de carga destruiu: re-desengata e re-teleporta
    // cada slot marcado pra pose estacionada (ordem decrescente, na sim thread)
    void restore_parked()
    {
        cargo_trace_begin( "restore-parked-begin", nullptr );
        // primeiro o conjunto: replay do teleport de ator com a transform pré-load
        if ( rig_armed )
        {
            rig_armed = false;
            // só replaya se ainda houver peça desatrelada pra re-estacionar
            bool any_detached = false;
            for ( int i = 0; i < max_trailers; ++i )
            {
                if ( detached_idx[ i ] ) { any_detached = true; break; }
            }
            const bool do_replay = rig_force_restore || any_detached;
            rig_force_restore = false;
            if ( do_replay && rig_truck_valid && actor_teleport_hook != nullptr )
            {
                if ( auto* game_actor = CCore::g_instance->get_game_actor() )
                {
                    using fn_t = void __fastcall( void*, void*, uint8_t, uint8_t );
                    actor_teleport_hook->get_original< fn_t >()(
                        game_actor, &rig_truck_xform, rig_replay_a3, rig_replay_a4 );
                }
            }
        }

        for ( int i = static_cast< int >( max_trailers ) - 1; i >= 0; --i )
        {
            if ( !detached_idx[ i ] ) continue;
            // slot marcado atrás de outro marcado pertence à mesma seção:
            // a cabeça cobre ele - separar aqui partiria a combo estacionada
            if ( i > 0 && detached_idx[ i - 1 ] && canon[ i - 1 ] != nullptr && !own_head[ i ] ) continue;
            void* t = canon[ i ];
            if ( t == nullptr || !readable_ptr( t ) || joint( t ) == nullptr ) continue;

            // pai sob o qual ela foi respawnada (ordem canon preservada)
            void* parent = nullptr;
            bool parent_is_truck = false;
            if ( auto* game_actor = CCore::g_instance->get_game_actor() )
            {
                parent = ( i == 0 || t == truck_towed )
                    ? prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck )
                    : canon[ i - 1 ];
                parent_is_truck = ( i == 0 || t == truck_towed );
            }
            CCore::g_instance->info( "Restoring detached trailer {} after cargo respawn", i );
            cargo_trace_event( "disconnect-start", t, i );
            if ( parent != nullptr )
            {
                disconnect_now( t, parent, parent_is_truck );
            }
            cargo_trace_event( "disconnect-end", t, i );

            // reposiciona a cabeça; descendentes engatados seguem pela junta
            for ( void* m = t; m != nullptr; )
            {
                const int mi = canon_index_of( m );
                if ( mi >= 0 && parked_xform_valid[ mi ] && trailer_teleport_hook != nullptr )
                {
                    if ( joint( m ) == nullptr )
                    {
                        using teleport_fn_t = void __fastcall( void* /*trailer*/, void* /*transform*/ );
                        cargo_trace_event( "reseat-before", m, mi );
                        trailer_teleport_hook->get_original< teleport_fn_t >()( m, &parked_xform[ mi ] );
                        cargo_trace_event( "reseat-after", m, mi );
                        anchor_valid[ mi ] = false;
                        carry_anchor_delay[ mi ] = 20;
                        cargo_anchor_passive[ mi ] = true;
                        if ( carry_hold[ mi ] ) carry_park[ mi ] = 20;
                    }
                    else
                    {
                        cargo_trace_event( "skip-reseat-live-joint", m, mi );
                        anchor_valid[ mi ] = false;
                        anchor_settling[ mi ] = false;
                        anchor_delay[ mi ] = 0;
                        carry_anchor_delay[ mi ] = 0;
                        carry_park[ mi ] = 0;
                        carry_park_pending[ mi ] = false;
                        cargo_anchor_passive[ mi ] = false;
                    }
                    carry_hold[ mi ] = false;
                }
                void* next = prism::offsets::field< void* >( m, prism::offsets::trailer_slave );
                m = ( next != m && next != nullptr && joint( next ) != nullptr ) ? next : nullptr;
            }
        }
    }
    tp_slot_t tp_log[ 16 ] = {};
    int tp_n = 0;
    bool tp_active = false;
    float3_t tp_snap[ max_trailers ] = {};
    quat_t tp_snap_q[ max_trailers ] = {};
    bool tp_snap_ok[ max_trailers ] = {};
    int tp_block_watch = 0;
    bool in_actor_tp = false;

    // apply do record de estado da carreta (0x1405E14D1): caminho que o
    // cinemático de carga usa pra teleportar o conjunto pros slots da doca
    using trailer_state_apply_fn = void __fastcall( void* /*record*/ );
    void __fastcall hk_trailer_state_apply( void* record )
    {
        bool has_detached = false;
        for ( void* t : plugin_detached ) has_detached |= t != nullptr;
        for ( bool detached : detached_idx ) has_detached |= detached;
        // checkbox off = sistema todo bypassado: chamada nativa pura
        if ( !settings::g_settings.cargo_restore )
        {
            trailer_state_apply_hook->get_original< trailer_state_apply_fn >()( record );
            return;
        }
        void* trailer = record != nullptr
            ? prism::offsets::field< void* >( record, 0xB8 ) : nullptr;
        if ( has_detached ) cargo_trace_begin( "trailer-state-apply", trailer );
        if ( trailer != nullptr && is_plugin_detached( trailer ) )
        {
            cargo_trace_event( "state-apply-skipped-detached", trailer );
            CCore::g_instance->info( "trailer state apply skipped: {:p} is plugin-detached", trailer );
            return;
        }
        // mid-teleport reconcile: o re-assert de pose não briga com o placement do jogo
        if ( tp_cooldown > 0 )
        {
            cargo_trace_event( "state-apply-skipped-cooldown", trailer, tp_cooldown );
            return;
        }
        cargo_trace_event( "state-apply-native", trailer );
        trailer_state_apply_hook->get_original< trailer_state_apply_fn >()( record );
    }
    // teleport por carreta (0x140786320): o que arrasta a carreta desengatada
    // pra doca durante carga/descarga; peça plugin-detached nunca é teleportada
    using trailer_teleport_fn = void __fastcall( void* /*trailer*/, void* /*transform*/ );
    void __fastcall hk_trailer_teleport( void* self, void* transform )
    {
        // checkbox off = sistema todo bypassado: chamada nativa pura
        if ( !settings::g_settings.cargo_restore )
        {
            trailer_teleport_hook->get_original< trailer_teleport_fn >()( self, transform );
            return;
        }
        bool detached_present = false;
        for ( void* t : plugin_detached ) detached_present |= t != nullptr;
        for ( bool detached : detached_idx ) detached_present |= detached;
        if ( cargo_trace_active() )
            cargo_trace_event( "trailer-teleport-request", self );
        else if ( detached_present && self != nullptr && is_plugin_detached( self ) )
            cargo_trace_begin( "detached-trailer-teleport", self );
        if ( cargo_trace_active() )
        {
            const float* dst = static_cast< const float* >( transform );
            if ( dst != nullptr && readable_span( dst, sizeof( float ) * 3 ) >= sizeof( float ) * 3 )
            {
                cargo_trace_event( "trailer-teleport-request", self );
                CCore::g_instance->info(
                    "cargo-trace id={} event=trailer-teleport-target object={:p} target=({:.3f},{:.3f},{:.3f}) actor-tp={} tp-active={}",
                    cargo_trace_id.load( std::memory_order_relaxed ), self,
                    dst[ 0 ], dst[ 1 ], dst[ 2 ], in_actor_tp, tp_active );
            }
        }
        // recusa só cobre peça sem junta arrastada por teleport de ATOR;
        // teleports isolados têm que pousar (snap de posicionamento do load)
        if ( settings::g_settings.cargo_restore && self != nullptr && is_plugin_detached( self ) &&
             joint( self ) == nullptr && in_actor_tp )
        {
            cargo_seq_ms = steady_ms();
            carry_arm_tick = GetTickCount64();
            cargo_trace_begin( "detached-member-actor-teleport", self );
            CCore::g_instance->info( "trailer teleport skipped: {:p} is plugin-detached", self );
            return;
        }
        // um teleport isolado pousou numa peça nossa estacionada - a âncora
        // recaptura a pose nova em vez de brigar com ela
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
        // carreta viajando com o conjunto não pode manter record armado
        // (a máquina parked a puxaria de volta todo frame)
        if ( self != nullptr && joint( self ) != nullptr )
        {
            prism::offsets::field< uint8_t >( self, prism::offsets::trailer_parked ) = 0;
            prism::offsets::field< uint8_t >( self, prism::offsets::trailer_parked_registered ) = 0;
        }
        // mundo montando: snapshots em poses de staging sentariam membros no vazio
        if ( canon_age < 90 )
        {
            trailer_teleport_hook->get_original< trailer_teleport_fn >()( self, transform );
            return;
        }
        const float* dst = static_cast< const float* >( transform );
        // mesmo dst do pai = jogo perdeu o offset do membro: re-teleporta pelo
        // NOSSO buffer (o do jogo é relido pelo reconcile) no slot final do pai
        // + o offset do snapshot
        float3_t corrected{};
        bool do_fix = false;
        void* parent = self != nullptr
            ? prism::offsets::field< void* >( self, prism::offsets::trailer_parent ) : nullptr;
        if ( dst != nullptr && self != nullptr && joint( self ) != nullptr && tp_active )
        {
            const int ti = canon_index_of( self );
            const int pi = canon_index_of( parent );
            // último slot registrado do pai (busca de trás pra frente)
            for ( int i = tp_n; i-- > 0; )
            {
                if ( tp_log[ i ].t != parent ) continue;
                const float ddx = dst[ 0 ] - tp_log[ i ].orig.x;
                const float ddz = dst[ 2 ] - tp_log[ i ].orig.z;
                // membro chegando na âncora compartilhada também casa pelo primeiro dst
                const float hdx = dst[ 0 ] - tp_log[ 0 ].orig.x;
                const float hdz = dst[ 2 ] - tp_log[ 0 ].orig.z;
                if ( ( ddx * ddx + ddz * ddz < 1.f || hdx * hdx + hdz * hdz < 1.f ) &&
                     ti >= 0 && pi >= 0 && tp_snap_ok[ ti ] && tp_snap_ok[ pi ] )
                {
                    // Y vem do dst do jogo (o delta da origem assaria a rampa no pouso)
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
        // recusa slot já ocupado por outra peça do canon (overlap = rig voa)
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
        // buffer nosso no lugar do do jogo: a carreta vai direto pro slot real
        if ( do_fix )
        {
            hook_xform_t fix = *static_cast< const hook_xform_t* >( transform );
            fix.pos = corrected;
            trailer_teleport_hook->get_original< trailer_teleport_fn >()( self, &fix );
            CCore::g_instance->info( "trailer teleport {:p}: re-seated at corrected slot "
                "({:.1f},{:.1f},{:.1f})", self, corrected.x, corrected.y, corrected.z );
            return;
        }
        trailer_teleport_hook->get_original< trailer_teleport_fn >()( self, transform );
    }
    //========================================================================
    // Snapshot do transform do conjunto (restore pós-cinemático de carga)
    //========================================================================
    bool rig_armed = false;
    uint32_t rig_restore_delay = 0;
    hook_xform_t rig_truck_xform{};
    bool rig_truck_valid = false;
    uint8_t rig_replay_a3 = 0;
    uint8_t rig_replay_a4 = 0;
    // replay do rig mesmo sem slots desacoplados (oficina arrastou o caminhão)
    bool rig_force_restore = false;

    // desce o trem de pouso via vfunc_set_brace (o park sozinho não constrói o apoio)
    void carry_set_brace( void* t )
    {
        __try
        {
            vcall< prism::physics_trailer_u_set_trailer_brace_fn >( t, prism::offsets::vfunc_set_brace, true );
        }
        __except ( EXCEPTION_EXECUTE_HANDLER )
        {
            CCore::g_instance->warning( "set_brace faulted on {:p}", t );
        }
    }

    void cargo_park_detached( void* t )
    {
        const int i = canon_index_of( t );
        cargo_trace_event( "native-park-start", t, i );
        if ( i >= 0 ) carry_park_pending[ i ] = false;
        if ( t == nullptr || !readable_ptr( t ) || joint( t ) != nullptr ) return;
        carry_set_brace( t );
        native_park_detached( t );
        if ( i >= 0 )
        {
            anchor_valid[ i ] = false;
            anchor_delay[ i ] = 0;
            carry_anchor_delay[ i ] = 20;
            cargo_anchor_passive[ i ] = true;
        }
        cargo_trace_event( "native-park-end", t, i );
        CCore::g_instance->info( "carried slot {} parked natively (landing gear + brake)", i );
    }

    using actor_teleport_fn = void __fastcall( void*, void*, uint8_t, uint8_t );

    // re-assenta cada membro engatado no slot final do pai + offset pré-teleport
    void settle_all_members()
    {
        for ( int i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
        {
            void* t = canon[ i ];
            // só membros que viajaram com o conjunto (presa em pai parked não teleportou)
            if ( !readable_ptr( t ) || joint( t ) == nullptr || !trailer_is_live( t ) ) continue;
            // membro teleportado não pode manter record armado
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
            trailer_teleport_hook->get_original< teleport_fn_t >()( t, &fix );
            CCore::g_instance->info(
                "teleport settle: {:p} re-seated at formation slot ({:.1f}m off)",
                t, std::sqrt( ex * ex + ez * ez ) );
        }
    }
    void cargo_actor_teleport_rig( void* self, void* arg, uint8_t a3, uint8_t a4 )
    {
        cargo_trace_begin( "cargo-actor-teleport", self );
        bool has_detached_slots = rig_armed;
        for ( bool detached : detached_idx ) has_detached_slots |= detached;

        // settle roda em TODO teleport de ator (o reconcile empilha membros
        // quando move a cabeça de novo no meio da sequência)
        if ( !settings::g_settings.cargo_restore || !has_detached_slots )
        {
            if ( !settings::g_settings.cargo_restore ) rig_armed = false;
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
        // destinos intermediários por-carreta do jogo ficam intactos
        tp_active = false;
        // claims parked escondidas durante o reconcile (ele conta membro pela cadeia slave)
        sever_parked_claims();
        // snapshot da formação pré-teleport (leitura viva deriva entre passes do reconcile)
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
                // sem peça desatrelada não há o que restaurar - nunca arma
                bool any_detached = false;
                bool any_displaced = false;
                for ( int i = 0; i < max_trailers; ++i )
                {
                    if ( !detached_idx[ i ] ) continue;
                    any_detached = true;
                    // só a doca respawna desatreladas DENTRO do conjunto ou fora da marca
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
                    // respawn curto de doca confirmado: arma o restore
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

}
