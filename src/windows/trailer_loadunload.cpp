// Cargo load/unload flow - everything the plugin does while the game's
// cargo sequence respawns or relocates the set lives here, so fixes to load/
// unload never touch the other flows (camera-zero, workshop, garage, load).

#include "trailer_shared.hpp"
#include "trailer_coupling_internal.hpp"

#include "core.hpp"
#include "prism/offsets.hpp"
#include "settings/settings.hpp"

namespace br_carretas
{

    void restore_parked_if_enabled()
    {
        if ( settings::g_settings.cargo_restore ) restore_parked();
    }

    void restore_parked()
    {
        // put the rig itself back first - replay the game's own actor teleport
        // with the pre-load transform and the original flags. The truck goes
        // through its proper path (vfunc +0x2A8), attached trailers follow the
        // actor-relative formation, and plugin-detached trailers are skipped
        // by our own per-trailer teleport hook. Then the detach pass below
        // re-parks the respawned sections
        if ( rig_armed )
        {
            rig_armed = false;
            // replaying the rig teleport with the snapshot transform yanks a
            // settled jointed combination across the map mid-solve - only do
            // it while a detached trailer still needs re-parking; if its
            // marks were cleared since the arm there is nothing to restore
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
            // a marked slot whose previous slot is also marked belongs to the
            // same parked section - the section head's walk below covers it;
            // disconnecting it here would split the parked combination apart
            if ( i > 0 && detached_idx[ i - 1 ] && canon[ i - 1 ] != nullptr && !own_head[ i ] ) continue;
            void* t = canon[ i ];
            if ( t == nullptr || !readable_ptr( t ) || joint( t ) == nullptr ) continue;

            // the parent it was respawned under - canon order is preserved
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
            if ( parent != nullptr )
            {
                disconnect_now( t, parent, parent_is_truck );
            }

            // teleport the head and every member still jointed to it back to
            // its saved parked transform - straight through the original, so
            // the pointer marks guarding the hook don't block the restore
            for ( void* m = t; m != nullptr; )
            {
                const int mi = canon_index_of( m );
                if ( mi >= 0 && parked_xform_valid[ mi ] && trailer_teleport_hook != nullptr )
                {
                    using teleport_fn_t = void __fastcall( void* /*trailer*/, void* /*transform*/ );
                    trailer_teleport_hook->get_original< teleport_fn_t >()( m, &parked_xform[ mi ] );
                    anchor_valid[ mi ] = false;
                    if ( carry_hold[ mi ] ) carry_park[ mi ] = 20;
                    carry_hold[ mi ] = false;
                }
                void* next = prism::offsets::field< void* >( m, prism::offsets::trailer_slave );
                m = ( next != m && next != nullptr && joint( next ) != nullptr ) ? next : nullptr;
            }
        }
    }

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

    void coupling_parked_update()
    {
        if ( load_game_active() ) return;
        workshop_respawn_update();
        garage_update();

        if ( tp_block_watch > 0 && ( --tp_block_watch == 60 || tp_block_watch == 0 ) )
            dump_chain( tp_block_watch == 0 ? "post-actor-tp-blocked+120f" : "post-actor-tp-blocked+60f" );
        for ( uint32_t i = 0; i < max_trailers; ++i )
        {
            if ( carry_park[ i ] <= 0 || --carry_park[ i ] != 0 ) continue;
            void* t = canon[ i ];
            if ( t != nullptr && readable_ptr( t ) && joint( t ) == nullptr )
            {
                carry_set_brace( t );
                native_park_detached( t );
                CCore::g_instance->info( "carried slot {} parked natively (landing gear + brake)", i );
            }
        }
        // a trailer the truck does not reach is parked: hold the spring brake
        // every frame (same fields the manual detach writes once) so a
        // respawned or game-reattached piece never rolls off its spot. Section
        // members jointed behind a loose head are not live either
        if ( carry_brake_frames > 0 && canon_age >= 10 )
        {
            --carry_brake_frames;
            for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
            {
                void* t = canon[ i ];
                if ( !is_trailer_obj( t ) || trailer_is_live( t ) ) continue;
                prism::offsets::field< float >( t, prism::offsets::trailer_brake ) = 1.f;
                prism::offsets::field< float >( t, prism::offsets::trailer_service_brake ) = 0.f;
                prism::offsets::field< uint8_t >( t, prism::offsets::trailer_brake_forced ) = 1;
            }
        }
        bool any_hold = false;
        for ( bool h : carry_hold ) any_hold |= h;
        if ( any_hold && ++carry_frames > 900 )
        {
            for ( auto& h : carry_hold ) h = false;
            any_hold = false;
        }
        if ( any_hold && canon_age >= 120 )
        {
            for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
            {
                void* t = canon[ i ];
                if ( !carry_hold[ i ] || !readable_ptr( t ) || joint( t ) != nullptr ) continue;
                if ( parked_xform_valid[ i ] && trailer_teleport_hook != nullptr )
                {
                    using teleport_fn_t = void __fastcall( void*, void* );
                    trailer_teleport_hook->get_original< teleport_fn_t >()( t, &parked_xform[ i ] );
                    anchor_valid[ i ] = false;
                }
                mark_detached( t );
                mark_plugin_detached( t );
                carry_hold[ i ] = false;
                carry_park[ i ] = 20;
                CCore::g_instance->info( "carried slot {} respawned jointless - put back at its parked pose", i );
            }
        }
        bool in_section = false;
        for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
        {
            void* t = canon[ i ];
            if ( !readable_ptr( t ) ) { detached_idx[ i ] = false; in_section = false; continue; }
            if ( detached_idx[ i ] )
            {
                if ( joint( t ) == nullptr )
                {
                    in_section = true;
                    own_head[ i ] = true;
                    rejoined_frames[ i ] = 0;
                    hook_xform_t xf{};
                    if ( !carry_hold[ i ] && hook_world( t, xf ) ) { parked_xform[ i ] = xf; parked_xform_valid[ i ] = true; }
                }
                else
                {
                    in_section = false;
                    if ( !settings::g_settings.cargo_restore || load_game_active() )
                    {
                        rejoined_frames[ i ] = 0;
                    }
                    else if ( ++rejoined_frames[ i ] >= 30 && pending.kind == pending_kind::none )
                    {
                        rejoined_frames[ i ] = 0;
                        if ( sim_is_quiet() )
                        {
                            CCore::g_instance->info( "restoring parked section early (sim suspended)" );
                            restore_parked();
                        }
                        else
                        {
                            queue_op( pending_kind::restore, nullptr, nullptr, false );
                        }
                    }
                }
            }
            else if ( in_section && joint( t ) != nullptr )
            {
                hook_xform_t xf{};
                if ( !carry_hold[ i ] && hook_world( t, xf ) ) { parked_xform[ i ] = xf; parked_xform_valid[ i ] = true; }
            }
            else
            {
                in_section = false;
            }
        }
        for ( uint32_t i = trailer_count; i < max_trailers; ++i )
        {
            detached_idx[ i ] = false;
            parked_xform_valid[ i ] = false;
        }

        if ( rig_armed )
        {
            if ( !settings::g_settings.cargo_restore || load_game_active() )
            {
                rig_armed = false;
            }
            else if ( rig_restore_delay > 0 )
            {
                --rig_restore_delay;
            }
            else if ( pending.kind == pending_kind::none )
            {
                if ( sim_is_quiet() )
                {
                    CCore::g_instance->info( "restoring rig early (sim suspended)" );
                    restore_parked();
                }
                else
                {
                    queue_op( pending_kind::restore, nullptr, nullptr, false );
                }
            }
        }
    }}
