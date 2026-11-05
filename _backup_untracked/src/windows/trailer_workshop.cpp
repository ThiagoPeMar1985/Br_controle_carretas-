#include "trailer_manipulation.hpp"
#include "trailer_shared.hpp"
#include "trailer_coupling_internal.hpp"

#include <algorithm>
#include <cstdint>

#include "core.hpp"
#include "prism/offsets.hpp"

namespace br_carretas
{
    int diag_frames = 0;
    //========================================================================
    // Hold de respawn por destruição (oficina) - canon troca de ponteiros com
    // poses espelhadas; oficina x load separam por game-state/heartbeat
    //========================================================================
    bool hold_jointed[ max_trailers ] = {};
    bool hold_ok[ max_trailers ] = {};
    hook_xform_t hold_xf[ max_trailers ] = {};
    void* hold_canon[ max_trailers ] = {};
    bool pose_pending[ max_trailers ] = {};
    bool workshop_detach_pending[ max_trailers ] = {};
    hook_xform_t workshop_pose[ max_trailers ] = {};
    bool workshop_pose_valid[ max_trailers ] = {};
    bool hold_had_poses = false;
    int respawn_pose_due = 0;
    hook_xform_t hold_truck_xf{};
    bool hold_truck_ok = false;
    hook_xform_t workshop_rig_xform{};
    bool workshop_rig_valid = false;
    bool workshop_rig_armed = false;
    bool workshop_restore_queued = false;
    uint32_t workshop_rig_delay = 0;
    uint64_t state_bad_ms = 0;
    uint64_t last_tick_ms = 0;
    uint64_t canon_dead_since = 0;

    void restore_workshop_respawn()
    {
        workshop_restore_queued = false;
        if ( workshop_rig_armed && workshop_rig_valid && actor_teleport_hook != nullptr )
        {
            if ( auto* game_actor = CCore::g_instance->get_game_actor() )
            {
                using fn_t = void __fastcall( void*, void*, uint8_t, uint8_t );
                in_actor_tp = true;
                actor_teleport_hook->get_original< fn_t >()(
                    game_actor, &workshop_rig_xform, 0, 0 );
                in_actor_tp = false;
                tp_cooldown = 90;
            }
        }
        workshop_rig_armed = false;
        bool retry = false;
        for ( int i = static_cast< int >( max_trailers ) - 1; i >= 0; --i )
        {
            if ( !workshop_detach_pending[ i ] && !pose_pending[ i ] ) continue;
            void* t = canon[ i ];
            if ( t == nullptr || !is_trailer_obj( t ) || !workshop_pose_valid[ i ] )
            {
                workshop_detach_pending[ i ] = false;
                pose_pending[ i ] = false;
                workshop_pose_valid[ i ] = false;
                continue;
            }
            if ( workshop_detach_pending[ i ] && joint( t ) != nullptr )
            {
                void* parent = nullptr;
                bool parent_is_truck = false;
                if ( auto* game_actor = CCore::g_instance->get_game_actor() )
                {
                    parent_is_truck = i == 0 || t == truck_towed;
                    parent = parent_is_truck
                        ? prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck )
                        : canon[ i - 1 ];
                }
                if ( parent != nullptr )
                {
                    disconnect_now( t, parent, parent_is_truck, false );
                }
                if ( joint( t ) != nullptr )
                {
                    retry = true;
                    continue;
                }
            }
            if ( workshop_detach_pending[ i ] )
            {
                mark_detached( t );
                mark_plugin_detached( t );
            }
            if ( trailer_teleport_hook != nullptr )
            {
                using teleport_fn_t = void __fastcall( void*, void* );
                trailer_teleport_hook->get_original< teleport_fn_t >()( t, &workshop_pose[ i ] );
            }
            if ( workshop_detach_pending[ i ] )
            {
                native_park_detached( t );
                workshop_detach_pending[ i ] = false;
            }
            anchor_valid[ i ] = false;
            workshop_pose_valid[ i ] = false;
            pose_pending[ i ] = false;
        }
        respawn_pose_due = retry ? 1 : 0;
    }

    void workshop_respawn_update()
    {
        const uint64_t now_ms = GetTickCount64();
        if ( traffic_game_state == nullptr || *traffic_game_state != 2 )
        {
            state_bad_ms = now_ms;
        }
        const uint64_t tick_gap = last_tick_ms != 0 ? now_ms - last_tick_ms : 0;
        last_tick_ms = now_ms;

        bool canon_any = false;
        bool canon_changed = false;
        for ( uint32_t i = 0; i < max_trailers; ++i )
        {
            if ( canon[ i ] != nullptr ) canon_any = true;
            if ( canon[ i ] != hold_canon[ i ] ) canon_changed = true;
        }
        if ( canon_any )
        {
            // canon trocado = respawn da oficina; Load Game nunca entra aqui
            const uint64_t dead_ms =
                canon_dead_since != 0 ? now_ms - canon_dead_since : 0;
            const bool in_session =
                ( state_bad_ms == 0 || now_ms - state_bad_ms > 5000 ) &&
                ( tick_gap < 4000 ) &&
                ( ( canon_dead_since != 0 && dead_ms < 1500 ) ||
                  ( canon_dead_since == 0 && tick_gap < 500 ) );
            bool cargo_carry_active = carry_arm_tick != 0 &&
                now_ms - carry_arm_tick < 15000;
            for ( bool held : carry_hold ) cargo_carry_active |= held;
            const uint64_t camera_zero_tick =
                camera_zero_block_tick.load( std::memory_order_acquire );
            const uint64_t actor_tick =
                actor_native_teleport_tick.load( std::memory_order_acquire );
            const bool another_flow_active = cargo_carry_active ||
                ( camera_zero_tick != 0 && now_ms - camera_zero_tick < 5000 ) ||
                ( actor_tick != 0 && now_ms - actor_tick < 5000 );
            if ( canon_changed && hold_had_poses && in_session )
            {
                cargo_trace_event( another_flow_active
                    ? "workshop-suppressed-other-flow" : "workshop-candidate", canon[ 0 ],
                    static_cast< int >( another_flow_active ) );
            }
            if ( canon_changed && hold_had_poses && in_session && !another_flow_active )
            {
                // caminhão moveu junto? sim = replay do rig; não = re-teleporta as peças
                bool truck_moved = false;
                if ( hold_truck_ok )
                {
                    if ( auto* ga = CCore::g_instance->get_game_actor() )
                    {
                        void* truck = prism::offsets::field< void* >(
                            ga, prism::offsets::game_actor_truck );
                        hook_xform_t tcur{};
                        if ( truck != nullptr && hook_world( truck, tcur ) )
                        {
                            const float dx = tcur.pos.x - hold_truck_xf.pos.x;
                            const float dz = tcur.pos.z - hold_truck_xf.pos.z;
                            truck_moved = dx * dx + dz * dz > 2.f * 2.f;
                        }
                    }
                }
                for ( uint32_t i = 0; i < max_trailers; ++i )
                {
                    if ( !hold_ok[ i ] ) continue;
                    // o canon reordena no respawn: o estado salvo segue o
                    // ponteiro, não o slot
                    const int ni = canon_index_of( hold_canon[ i ] );
                    if ( ni < 0 ) continue;
                    // sem junta no espelho = head estacionada de novo
                    if ( !hold_jointed[ i ] )
                    {
                        workshop_detach_pending[ ni ] = true;
                        workshop_pose[ ni ] = hold_xf[ i ];
                        workshop_pose_valid[ ni ] = true;
                    }
                    else if ( !truck_moved )
                    {
                        pose_pending[ ni ] = true;
                        workshop_pose[ ni ] = hold_xf[ i ];
                        workshop_pose_valid[ ni ] = true;
                    }
                }
                if ( truck_moved )
                {
                    workshop_rig_xform = hold_truck_xf;
                    workshop_rig_valid = true;
                    workshop_rig_armed = true;
                    workshop_rig_delay = 60;
                    respawn_pose_due = 60;
                    CCore::g_instance->info( "workshop respawn: truck moved - rig restore armed" );
                }
                else
                {
                    respawn_pose_due = 60;
                    CCore::g_instance->info( "workshop respawn: re-seating trailers at pre-rebuild poses" );
                    diag_frames = 600;
                }
            }
            else if ( canon_changed && hold_had_poses && !another_flow_active )
            {
                CCore::g_instance->info( "respawn discarded - world reload stays native" );
            }
            canon_dead_since = 0;
            // espelha ponteiros + poses (a cópia que sobrevive à destruição)
            hold_truck_ok = false;
            if ( auto* ga = CCore::g_instance->get_game_actor() )
            {
                void* truck = prism::offsets::field< void* >(
                    ga, prism::offsets::game_actor_truck );
                if ( truck != nullptr && readable_ptr( truck ) )
                {
                    hold_truck_ok = hook_world( truck, hold_truck_xf );
                }
            }
            hold_had_poses = false;
            for ( uint32_t i = 0; i < max_trailers; ++i )
            {
                void* t = canon[ i ];
                hold_canon[ i ] = t;
                if ( t == nullptr || !is_trailer_obj( t ) )
                {
                    hold_ok[ i ] = false;
                    hold_jointed[ i ] = false;
                    continue;
                }
                hook_xform_t xf{};
                if ( hook_world( t, xf ) )
                {
                    hold_xf[ i ] = xf;
                    hold_ok[ i ] = true;
                    hold_jointed[ i ] = joint( t ) != nullptr;
                    hold_had_poses = true;
                }
                else
                {
                    hold_ok[ i ] = false;
                }
            }
        }
        else if ( canon_dead_since == 0 )
        {
            canon_dead_since = now_ms;
        }
        if ( workshop_rig_armed && workshop_rig_delay > 0 ) --workshop_rig_delay;
        if ( respawn_pose_due > 0 && canon_age >= 90 && --respawn_pose_due == 0 )
        {
            if ( workshop_rig_delay > 0 )
            {
                respawn_pose_due = 1;
            }
            else if ( !workshop_restore_queued && pending.kind == pending_kind::none )
            {
                if ( GetCurrentThreadId() == sim_thread_id )
                {
                    restore_workshop_respawn();
                }
                else if ( queue_op( pending_kind::workshop_restore, nullptr, nullptr, false ) )
                {
                    workshop_restore_queued = true;
                }
                else
                {
                    respawn_pose_due = 1;
                }
            }
            else if ( pending.kind != pending_kind::none )
            {
                respawn_pose_due = 1;
            }
        }

        if ( diag_frames > 0 )
        {
            --diag_frames;
            for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
            {
                void* t = canon[ i ];
                hook_xform_t xf{};
                if ( t == nullptr || !readable_ptr( t ) || !hook_world( t, xf ) ) continue;
                CCore::g_instance->info(
                    "diag c{} pos=({:.2f},{:.2f},{:.2f}) jnt={} slave={} det={}",
                    i, xf.pos.x, xf.pos.y, xf.pos.z,
                    joint( t ) != nullptr,
                    prism::offsets::field< void* >( t, prism::offsets::trailer_slave ) != nullptr,
                    is_detached( t ) );
            }
        }
    }
}
