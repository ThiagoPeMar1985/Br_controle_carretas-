#include "trailer_shared.hpp"
#include "trailer_coupling_internal.hpp"

#include "core.hpp"
#include "prism/offsets.hpp"

namespace br_carretas
{
    using truck_mass_calc_fn = float __fastcall( void* );
    using chassis_mass_apply_fn = void __fastcall( void*, uint8_t, float );

    // 282 - massa do veiculo atual pelo gerenciador (o despachante usa no chassis)
    truck_mass_calc_fn* truck_mass_calc()
    {
        static truck_mass_calc_fn* f = memory::get_function_from_pattern< truck_mass_calc_fn >(
            "40 53 48 83 ec 30 48 8b 1d ? ? ? ? 48 85 db 75 0e f3 0f" );
        return f;
    }

    // 283 - aplica a massa no objeto-chassi do cavalo (truck+0x1F8)
    chassis_mass_apply_fn* chassis_mass_apply()
    {
        static chassis_mass_apply_fn* f = memory::get_function_from_pattern< chassis_mass_apply_fn >(
            "48 89 5c 24 08 57 48 83 ec 30 8b 81 98 01 00 00 0f b6 fa" );
        return f;
    }

    // 284 - desatrelamento manual: escreve o estado de desengate na ordem do
    // despachante nativo, sem jamais chama-lo (mesma rotina dentro e fora de empresa)
    // SEM trava de velocidade: o restore de carga (fn116) passa por aqui -
    // a trava do usuario vive nos pontos de entrada (fn086/fn191)
    void trailer_manual_detach( void* trailer, void* parent, const bool parent_is_truck )
    {
        std::lock_guard< std::recursive_mutex > claim_lk( claim_write_mutex );
        if ( trailer == nullptr || !readable_ptr( trailer ) ) return;

        void* truck = nullptr;
        if ( auto* game_actor = CCore::g_instance->get_game_actor() )
        {
            truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
        }
        const bool truck_ok = truck != nullptr && readable_ptr( truck );
        void* mgr = traffic_manager_global != nullptr ? *traffic_manager_global : nullptr;
        void* sub = mgr != nullptr ? prism::offsets::field< void* >( mgr, 0x2B30 ) : nullptr;

        native_chain_call_active.store( true, std::memory_order_release );
        arm_phys_log();
        sever_phys_claims();

        if ( parent_is_truck && truck_ok && set_towed_trailer_fn != nullptr )
        {
            set_towed_trailer_fn( truck, false, nullptr );
        }
        if ( parent_is_truck && parent != nullptr && pre_detach_fn != nullptr &&
             prism::offsets::field< uint32_t >( parent, prism::offsets::truck_coupling_state ) != 0 )
        {
            pre_detach_fn( parent, trailer );
        }

        play_brace_sound( trailer, false );
        vcall< prism::physics_trailer_u_set_trailer_brace_fn >( trailer, prism::offsets::vfunc_set_brace, true );

        unlink_phys_node( trailer );

        void* kept_slave = readable_ptr( trailer )
            ? prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) : nullptr;
        if ( kept_slave != nullptr )
        {
            prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = nullptr;
        }
        vcall< prism::physics_trailer_u_disconnect_fn >( trailer, prism::offsets::vfunc_disconnect );
        const bool section_behind = kept_slave != nullptr && joint( kept_slave ) != nullptr;
        if ( kept_slave != nullptr && readable_ptr( trailer ) )
        {
            slave_link_write( trailer, kept_slave );
        }

        if ( parent_is_truck && truck_ok )
        {
            prism::offsets::field< uint32_t >( truck, prism::offsets::truck_coupling_state ) = 0;
            prism::offsets::field< float >( truck, 0x4C4 ) = 0.f;
            if ( set_towed_trailer_fn != nullptr ) set_towed_trailer_fn( truck, false, nullptr );
            if ( distribute_towed_mass_fn != nullptr ) distribute_towed_mass_fn( truck, 0.f, 0.f );
        }
        if ( sub != nullptr )
        {
            [&]()
            {
                __try { vcall< prism::vehicle_notify_decoupled_fn >( sub, 0x140 / 8 ); }
                __except ( EXCEPTION_EXECUTE_HANDLER ) {}
            }();
        }

        if ( notify_decoupled_fn != nullptr )
        {
            ++notify_calls;
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
            prism::offsets::field< uint8_t >( t, 0x25A ) = 0;
            void* next = prism::offsets::field< void* >( t, prism::offsets::trailer_slave );
            if ( next == t || next == trailer ) break;
            t = ( next != nullptr && joint( next ) != nullptr ) ? next : nullptr;
        }
        const int di = canon_index_of( trailer );
        if ( di >= 0 ) detached_idx[ di ] = true;

        if ( set_lines_fn != nullptr )
        {
            CCore::g_instance->info( "fn284 cabo: set_lines({:p},0,1)", trailer );
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
            if ( rebuild_lines_fn != nullptr )
            {
                void* slave = prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave );
                prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = nullptr;
                rebuild_lines_fn( parent, trailer );
                slave_link_write( trailer, slave );
            }
            if ( refresh_accessories_fn != nullptr )
            {
                refresh_accessories_fn( parent, trailer );
            }
            traffic_detach_tail();
        }

        if ( truck_ok )
        {
            if ( joint( trailers[ 0 ] ) == nullptr )
            {
                prism::offsets::field< float >( truck, 0x4C4 ) = 0.f;
                prism::offsets::field< uint32_t >( truck, prism::offsets::truck_coupling_state ) = 0;
            }
            if ( set_towed_trailer_fn != nullptr &&
                 prism::offsets::field< void* >( truck, 0x1580 ) == trailer )
            {
                set_towed_trailer_fn( truck, true, nullptr );
            }
            if ( apply_cables_fn != nullptr && cables_mode_cvar != nullptr && get_cvar_int_fn != nullptr )
            {
                const int mode = get_cvar_int_fn( cables_mode_cvar );
                CCore::g_instance->info( "fn284 cabo: apply_cables(cavalo,null) mode={}", mode );
                apply_cables_fn( truck, nullptr, 0, mode > 4 ? 4 : mode < 0 ? 0 : mode, 0 );
            }
        }
        if ( trailer == truck_towed ) truck_towed = nullptr;

        if ( brace_anim_fn != nullptr )
        {
            prism::offsets::field< float >( trailer, 0x1240 ) = 1.f;
            brace_anim_fn( trailer, 1, 1.f / 3.f );
        }

        if ( sub != nullptr )
        {
            [&]()
            {
                __try { vcall< prism::vehicle_notify_decoupled_fn >( sub, 0x1E8 / 8 ); }
                __except ( EXCEPTION_EXECUTE_HANDLER ) {}
            }();
        }
        if ( truck_ok )
        {
            void* chassis = prism::offsets::field< void* >( truck, 0x1F8 );
            truck_mass_calc_fn* mass_calc = truck_mass_calc();
            chassis_mass_apply_fn* mass_apply = chassis_mass_apply();
            if ( mass_calc != nullptr && mass_apply != nullptr && readable_ptr( chassis ) )
            {
                [&]()
                {
                    __try { mass_apply( chassis, 0, mass_calc( sub ) ); }
                    __except ( EXCEPTION_EXECUTE_HANDLER ) {}
                }();
            }
        }

        sever_phys_claims();
        native_chain_call_active.store( false, std::memory_order_release );
        unhide_detached();
        rebuild_parked_claim_links();
        dump_real_chain( "post-detach" );
        repair_phys_chain( "post-detach" );
        lane_dump_refs( trailer );
        CCore::g_instance->info( "fn284 detach manual trailer={:p} junta={:p}", trailer, joint( trailer ) );
    }
}
