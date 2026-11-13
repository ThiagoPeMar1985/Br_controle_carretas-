#include "trailer_shared.hpp"
#include "trailer_coupling_internal.hpp"

#include "core.hpp"
#include "prism/offsets.hpp"
#include "settings/settings.hpp"

namespace br_carretas
{

    // 115 - chama o restore só se a opção "restaurar carga" estiver ligada
    void restore_parked_if_enabled()
    {
        if ( settings::g_settings.cargo_restore ) restore_parked();
    }

    // 116 - recoloca o conjunto após respawn de carga: re-detacha seções e teleporta pras poses salvas
    void restore_parked()
    {

        if ( cargo_freeze() ) return;
        if ( rig_armed )
        {
            rig_armed = false;

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

            if ( i > 0 && detached_idx[ i - 1 ] && canon[ i - 1 ] != nullptr && !own_head[ i ] ) continue;
            void* t = canon[ i ];
            if ( t == nullptr || !readable_ptr( t ) || joint( t ) == nullptr ) continue;

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

            if ( parent != nullptr && readable_ptr( parent )
                && ( parent_is_truck || is_trailer_obj( parent ) ) )
            {
                disconnect_now( t, parent, parent_is_truck );
            }

            for ( void* m = t; m != nullptr; )
            {

                if ( !readable_ptr( m ) || !is_trailer_obj( m ) ) break;
                const int mi = canon_index_of( m );
                if ( mi >= 0 && parked_xform_valid[ mi ] && trailer_teleport_hook != nullptr )
                {
                    teleport_safe( m, &parked_xform[ mi ] );
                    anchor_valid[ mi ] = false;
                    if ( carry_hold[ mi ] ) carry_park[ mi ] = 20;
                    carry_hold[ mi ] = false;
                }
                void* next = prism::offsets::field< void* >( m, prism::offsets::trailer_slave );
                m = ( next != m && next != nullptr && readable_ptr( next )
                    && is_trailer_obj( next ) && joint( next ) != nullptr ) ? next : nullptr;
            }
        }
    }

    // 117 - desce o apoio da carreta pela vfunc nativa (protegido)
    void carry_set_brace( void* t )
    {
        __try
        {
            vcall< prism::physics_trailer_u_set_trailer_brace_fn >( t, prism::offsets::vfunc_set_brace, true );
        }
        __except ( EXCEPTION_EXECUTE_HANDLER )
        {
            CCore::g_instance->warning( "set_brace faulted on {:p} (função 117)", t );
        }
    }

    // 118 - por frame: vigia o respawn de carga e enfileira o restore
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
            if ( !cargo_freeze() && t != nullptr && readable_ptr( t ) && joint( t ) == nullptr )
            {
                carry_set_brace( t );
                native_park_detached( t );
                CCore::g_instance->info( "carried slot {} parked natively (landing gear + brake)", i );
            }
        }

        if ( carry_brake_frames > 0 && canon_age >= 10 && !cargo_freeze() )
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
        if ( any_hold && canon_age >= 120 && !cargo_freeze() )
        {
            for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
            {
                void* t = canon[ i ];
                if ( !carry_hold[ i ] || !readable_ptr( t ) || joint( t ) != nullptr ) continue;
                if ( parked_xform_valid[ i ] && trailer_teleport_hook != nullptr )
                {
                    teleport_safe( t, &parked_xform[ i ] );
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
    }    namespace
    {
        const bool fn_idx_reg = []()
        {
            fn_index_add( fn_addr_of( &restore_parked_if_enabled ), 115, "restore_parked_if_enabled" );
            fn_index_add( fn_addr_of( &restore_parked ), 116, "restore_parked" );
            fn_index_add( fn_addr_of( &carry_set_brace ), 117, "carry_set_brace" );
            fn_index_add( fn_addr_of( &coupling_parked_update ), 118, "coupling_parked_update" );
            return true;
        }();
    }
}
