#include "trailer_shared.hpp"
#include "trailer_coupling_internal.hpp"

#include "core.hpp"
#include "prism/offsets.hpp"
#include "settings/settings.hpp"

namespace br_carretas
{

    bool hold_jointed[ max_trailers ] = {};
    bool hold_ok[ max_trailers ] = {};
    hook_xform_t hold_xf[ max_trailers ] = {};
    void* hold_canon[ max_trailers ] = {};
    bool pose_pending[ max_trailers ] = {};
    bool hold_had_poses = false;
    int respawn_pose_due = 0;
    hook_xform_t hold_truck_xf{};
    bool hold_truck_ok = false;
    uint64_t state_bad_ms = 0;
    uint64_t last_tick_ms = 0;
    uint64_t canon_dead_since = 0;

    // 144 - descarta o espelho do mundo antigo após um load
    void workshop_respawn_discard()
    {
        const uint64_t now_ms = GetTickCount64();
        hold_had_poses = false;
        hold_truck_ok = false;
        respawn_pose_due = 0;
        canon_dead_since = 0;
        state_bad_ms = now_ms;
        last_tick_ms = now_ms;
        for ( uint32_t i = 0; i < max_trailers; ++i )
        {
            hold_canon[ i ] = canon[ i ];
            hold_ok[ i ] = false;
            hold_jointed[ i ] = false;
            pose_pending[ i ] = false;
        }
    }

    // 145 - por tick: detecta destruição/recriação da oficina e devolve as poses
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

            const bool in_session =
                ( state_bad_ms == 0 || now_ms - state_bad_ms > 5000 ) &&
                ( tick_gap < 4000 ) &&
                ( canon_dead_since == 0 || now_ms - canon_dead_since < 5000 ) &&
                ( actor_tp_ms == 0 || steady_ms() - actor_tp_ms > 8000 );
            if ( canon_changed && hold_had_poses && in_session )
            {

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

                    if ( !hold_jointed[ i ] )
                    {
                        detached_idx[ i ] = true;
                        parked_xform[ i ] = hold_xf[ i ];
                        parked_xform_valid[ i ] = true;
                        carry_hold[ i ] = true;
                        carry_brake_frames = 900;
                    }
                    else if ( !truck_moved )
                    {
                        pose_pending[ i ] = true;
                    }
                }
                if ( truck_moved )
                {
                    rig_truck_xform = hold_truck_xf;
                    rig_truck_valid = true;
                    rig_armed = true;
                    rig_restore_delay = 60;
                    rig_replay_a3 = 0;
                    rig_replay_a4 = 0;
                    rig_force_restore = true;
                    CCore::g_instance->info( "workshop respawn: truck moved - rig restore armed" );
                }
                else
                {
                    respawn_pose_due = 60;
                    CCore::g_instance->info( "workshop respawn: re-seating trailers at pre-rebuild poses" );
                }
            }
            else if ( canon_changed && hold_had_poses )
            {
                CCore::g_instance->info( "respawn discarded - world reload stays native" );
            }
            canon_dead_since = 0;

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

        if ( respawn_pose_due > 0 && canon_age >= 90 && --respawn_pose_due == 0 )
        {
            if ( trailer_teleport_hook != nullptr )
            {
                using teleport_fn_t = void __fastcall( void*, void* );
                for ( int i = static_cast< int >( max_trailers ) - 1; i >= 0; --i )
                {
                    if ( !pose_pending[ i ] ) continue;
                    pose_pending[ i ] = false;
                    void* t = canon[ i ];
                    if ( t == nullptr || !is_trailer_obj( t ) ) continue;
                    trailer_teleport_hook->get_original< teleport_fn_t >()( t, &hold_xf[ i ] );
                    anchor_valid[ i ] = false;
                }
                CCore::g_instance->info( "workshop respawn: rig re-seated at pre-rebuild poses" );
            }
            else
            {
                for ( auto& p : pose_pending ) p = false;
            }
        }
    }
    namespace
    {
        const bool fn_idx_reg = []()
        {
            fn_index_add( fn_addr_of( &workshop_respawn_discard ), 144, "workshop_respawn_discard" );
            fn_index_add( fn_addr_of( &workshop_respawn_update ), 145, "workshop_respawn_update" );
            return true;
        }();
    }
}
