// Trailer brake bias (o slider "carreta freia mais"): setter hooks,
// wheel-solver mask, per-frame boost - split out of trailer_manipulation.cpp.

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
#include "license/license.hpp"
#include "settings/settings.hpp"

namespace ts_extra_utilities
{
    // each wheelset's native brake mask, captured before our first rewrite -
    // needed to fall back to the game's own braked-axle set while axles are up
    void* native_mask_ws[ 32 ] = {};
    uint32_t native_mask_val[ 32 ] = {};

    uint32_t native_mask_of( void* ws )
    {
        for ( int i = 0; i < 32; ++i )
            if ( native_mask_ws[ i ] == ws ) return native_mask_val[ i ];
        const uint32_t native = prism::offsets::field< uint32_t >( ws, 0x2BC );
        for ( int i = 0; i < 32; ++i )
            if ( native_mask_ws[ i ] == nullptr ) { native_mask_ws[ i ] = ws; native_mask_val[ i ] = native; break; }
        return native;
    }

    // rewrites the wheelset brake mask (+0x2BC): all axles down -> every wheel
    // takes the factor; any axle lifted (progress > 0.5) -> back to the native
    // mask minus the airborne wheels, so only grounded axles keep braking.
    // Wheels map to axles evenly - wheel i belongs to axle i*axles/n
    void update_wheel_brake_mask( void* trailer )
    {
        void* const ws = prism::offsets::field< void* >( trailer, 0x150 );
        if ( ws == nullptr ) return;
        const auto n = prism::offsets::field< uint64_t >( ws, 0x178 );
        if ( n == 0 || n > 32 ) return;

        uint32_t axles = 0;
        const float* progress = nullptr;
        void* const controller = prism::offsets::field< void* >( trailer, prism::offsets::trailer_lift_controller );
        if ( controller != nullptr )
        {
            const auto pc = prism::offsets::field< uint64_t >( controller, prism::offsets::lift_controller_progress_count );
            if ( pc > 0 && pc <= 16 )
            {
                progress = prism::offsets::field< float* >( controller, prism::offsets::lift_controller_progress );
                if ( progress != nullptr && readable_ptr( progress ) )
                    axles = static_cast< uint32_t >( pc );
            }
        }

        uint32_t grounded = 0;
        bool any_lifted = false;
        for ( uint64_t w = 0; w < n; ++w )
        {
            if ( axles != 0 && progress[ w * axles / n ] > 0.5f )
                any_lifted = true;
            else
                grounded |= 1u << w;
        }
        const uint32_t mask = any_lifted
            ? native_mask_of( ws ) & grounded
            : grounded;

        prism::offsets::field< uint32_t >( ws, 0x2BC ) = mask;
    }

    //------------------------------------------------------------------------
    // Brake setter hooks
    //
    // A trailer that leaves a parked set stays linked under it, so the set's
    // per-frame spring-brake push keeps reaching it through the stale slave
    // link and re-locks the wheels - no matter what the truck broadcasts.
    // Ret sites of the +0x180/+0x188 vfunc calls inside the truck's per-frame
    // update are the only legitimate source of positive brake writes on a
    // coupled trailer: at coupling_state 5 the truck copies its own brake
    // channels onto the towed chain head and the setter tail-propagates down
    // trailer_slave links, so every propagation frame carries this same
    // return address.
    //------------------------------------------------------------------------
    using brake_setter_fn = void __fastcall( void* /*vehicle*/, float /*value*/ );
    using forced_setter_fn = void __fastcall( void* /*vehicle*/, uint8_t /*value*/ );

    uint64_t truck_brake_broadcast_ret = 0;
    uint64_t truck_forced_broadcast_ret = 0;

    // the only legitimate source of positive brake writes on a jointed trailer
    // is the truck's own broadcast. Anything else - a stale parked set pushing
    // through the shared trailer_slave link, or the per-frame spring-brake
    // loop while the coupling state is mid-change - is swallowed: a trailer
    // with a live joint takes its brakes from the truck, period.
    bool foreign_brake_write( void* self, const void* ret )
    {
        const auto ra = reinterpret_cast< uint64_t >( ret );
        if ( ra == truck_brake_broadcast_ret || ra == truck_forced_broadcast_ret )
        {
            return false;
        }
        // only jointed trailers are candidates - a free (jointless) trailer must
        // keep receiving its spring brake or it rolls away
        if ( self == nullptr || joint( self ) == nullptr ) return false;
        // trailer_physics_joint is a trailer field - on the truck object that
        // offset holds live unrelated data, so a nonzero read would misclassify
        // the truck as a jointed trailer and eat its own parking-brake writes
        if ( auto* game_actor = CCore::g_instance->get_game_actor() )
        {
            if ( self == prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck ) )
            {
                return false;
            }
        }
        return true;
    }

    // the stale parked chain may continue THROUGH a coupled link to trailers
    // still parked behind it: resume the write on the first jointless link so
    // those keep their spring brake
    void* next_jointless_link( void* self )
    {
        void* cur = prism::offsets::field< void* >( self, prism::offsets::trailer_slave );
        for ( int guard = 0; cur != nullptr && joint( cur ) != nullptr; ++guard )
        {
            if ( guard >= 32 ) return nullptr;
            cur = prism::offsets::field< void* >( cur, prism::offsets::trailer_slave );
        }
        return cur;
    }

    // brake bias: legitimate truck broadcasts aimed at a coupled trailer are
    // amplified by trailer_brake_boost percent so the trailers take the
    // braking load first (they lock and drag straight instead of the truck
    // folding into a jackknife). The tractor's own channel is never touched -
    // the truck check must run BEFORE joint(): +0xFE0 is a trailer field and
    // holds unrelated live data on the truck object.
    // The wheel solver (0x14090C570) applies the factor as
    // max(trailer_brake, per-wheel floor) * grip/load with NO upper clamp,
    // so the amplified value is allowed to exceed 1.0 - that is what makes
    // loaded axles actually lock once their wheels unload under braking.
    float biased_brake( void* self, const float v )
    {
        if ( !license::active() ) return v; // locked: native braking only
        const float boost = settings::g_settings.trailer_brake_boost;
        if ( v <= 0.f || boost <= 0.f || self == nullptr ) return v;
        if ( auto* game_actor = CCore::g_instance->get_game_actor() )
        {
            if ( self == prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck ) )
            {
                return v;
            }
        }
        if ( joint( self ) == nullptr ) return v; // detached: native spring brake
        const float mult = 1.f + boost / 100.f;
        return ( std::min )( v * mult, mult ); // cap at the multiplier itself
    }

    void __fastcall hk_brake_chain_setter( void* self, float v )
    {
        if ( v > 0.f && foreign_brake_write( self, _ReturnAddress() ) )
        {
            if ( void* s = next_jointless_link( self ) )
            {
                brake_chain_setter_hook->get_original< brake_setter_fn >()( s, v );
            }
            return;
        }
        brake_chain_setter_hook->get_original< brake_setter_fn >()( self, biased_brake( self, v ) );
    }

    void __fastcall hk_brake_setter( void* self, float v )
    {
        if ( v > 0.f && foreign_brake_write( self, _ReturnAddress() ) ) return;
        brake_setter_hook->get_original< brake_setter_fn >()( self, biased_brake( self, v ) );
    }

    void __fastcall hk_forced_chain_setter( void* self, uint8_t v )
    {
        if ( v != 0 && foreign_brake_write( self, _ReturnAddress() ) )
        {
            if ( void* s = next_jointless_link( self ) )
            {
                forced_chain_setter_hook->get_original< forced_setter_fn >()( s, v );
            }
            return;
        }
        forced_chain_setter_hook->get_original< forced_setter_fn >()( self, v );
    }

    void __fastcall hk_forced_setter( void* self, uint8_t v )
    {
        if ( v != 0 && foreign_brake_write( self, _ReturnAddress() ) ) return;
        forced_setter_hook->get_original< forced_setter_fn >()( self, v );
    }

    // per-wheel brake solver (0x14090C510): rcx is the wheelset, +0x10 the
    // vehicle, +0x178 the wheel count, +0x2BC the brake-factor mask and
    // +0x260/+0x268 the per-wheel output array. Natively lifted wheels keep
    // their floor brake, so an airborne wheel dead-stops the moment the pedal
    // is touched - after the original runs we zero the outputs of every wheel
    // whose axle is raised (lift controller progress > 0.5). Runs on the sim
    // thread, which is why this can't be done per-frame in update()
    using wheel_brake_fn = void __fastcall( void* /*wheelset*/ );
    void __fastcall hk_wheel_brake( void* ws )
    {
        wheel_brake_hook->get_original< wheel_brake_fn >()( ws );
        if ( ws == nullptr ) return;
        void* const veh = prism::offsets::field< void* >( ws, 0x10 );
        if ( veh == nullptr || index_of( veh ) < 0 ) return;
        void* const controller = prism::offsets::field< void* >( veh, prism::offsets::trailer_lift_controller );
        if ( controller == nullptr || !readable_ptr( controller ) ) return;
        const auto pc = prism::offsets::field< uint64_t >( controller, prism::offsets::lift_controller_progress_count );
        if ( pc == 0 || pc > 16 ) return;
        const float* const progress = prism::offsets::field< float* >( controller, prism::offsets::lift_controller_progress );
        if ( progress == nullptr || !readable_ptr( progress ) ) return;
        const auto n = prism::offsets::field< uint64_t >( ws, 0x178 );
        float* const out = prism::offsets::field< float* >( ws, 0x260 );
        if ( n == 0 || n > 32 || out == nullptr || !readable_ptr( out ) ) return;
        if ( prism::offsets::field< uint64_t >( ws, 0x268 ) != n ) return;
        for ( uint64_t w = 0; w < n; ++w )
            if ( progress[ w * pc / n ] > 0.5f ) out[ w ] = 0.f;
    }

    // service-brake note: the pedal broadcasts through +0x730 (vfunc 0x178),
    // but that field is a demand mirror - writing 10x to it produced zero
    // physical effect. The wheel solver only turns +0x760 into torque, so the
    // bias is applied there per-frame in update() instead

    // hooks on the trailer+0x760/+0x764 brake setters: they swallow positive
    // writes that did not come from the truck's own broadcast when aimed at a
    // coupled trailer still owned by a parked set, so a stale parked-set
    // chain cannot re-brake it through the shared trailer_slave link
    void brake_register_hooks()
    {
        if ( !expected_build() ) return;
        truck_brake_broadcast_ret = memory::get_address_from_offset( 0x14064B7EF );
        truck_forced_broadcast_ret = memory::get_address_from_offset( 0x14064B806 );
        const auto brake_chain_setter_address = memory::get_address_for_pattern(
            "f3 0f 11 89 60 07 00 00 48 8b 89 88 10 00 00" );
        const auto brake_setter_address = memory::get_address_for_pattern(
            "f3 0f 11 89 60 07 00 00 c3" );
        const auto forced_chain_setter_address = memory::get_address_for_pattern(
            "88 91 64 07 00 00 48 8b 89 88 10 00 00" );
        const auto forced_setter_address = memory::get_address_for_pattern(
            "88 91 64 07 00 00 c3" );
        if ( brake_chain_setter_address != 0 )
        {
            brake_chain_setter_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
                "brake_chain_setter", brake_chain_setter_address,
                reinterpret_cast< uint64_t >( &hk_brake_chain_setter ) );
            brake_chain_setter_hook->hook();
        }
        if ( brake_setter_address != 0 )
        {
            brake_setter_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
                "brake_setter", brake_setter_address,
                reinterpret_cast< uint64_t >( &hk_brake_setter ) );
            brake_setter_hook->hook();
        }
        if ( forced_chain_setter_address != 0 )
        {
            forced_chain_setter_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
                "forced_chain_setter", forced_chain_setter_address,
                reinterpret_cast< uint64_t >( &hk_forced_chain_setter ) );
            forced_chain_setter_hook->hook();
        }
        if ( forced_setter_address != 0 )
        {
            forced_setter_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
                "forced_setter", forced_setter_address,
                reinterpret_cast< uint64_t >( &hk_forced_setter ) );
            forced_setter_hook->hook();
        }
        // per-wheel brake solver - zeroing lifted-axle outputs must happen on
        // the sim thread right after each solve, not per-frame
        const auto wheel_brake_address = memory::get_address_for_pattern(
            "48 89 5c 24 18 48 89 74 24 20 57 48 83 ec 50 48 8b 81 78 01 00 00 33 ff 48 8b d9 0f 29 74 24 40" );
        if ( wheel_brake_address != 0 )
        {
            wheel_brake_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
                "wheel_brake", wheel_brake_address,
                reinterpret_cast< uint64_t >( &hk_wheel_brake ) );
            wheel_brake_hook->hook();
        }
    }

    // trailer brake bias: +0x730 is only a demand mirror - the wheel
    // solver (0x14090C570) turns +0x760 into real torque via
    // max(factor, per-wheel min) * grip, no upper clamp, and ONLY for
    // wheels whose bit is set in the wheelset mask (+0x2BC). The native
    // mask marks just the braked def axles (e.g. 0xC = middle axle of a
    // vanderleia); we rewrite it to cover every wheel of every GROUNDED
    // axle - lifted axles lose their bits, which matches both the user's
    // request and reality (a wheel in the air brakes nothing)
    void brake_boost_update()
    {
        const float brake_mult = 1.f + settings::g_settings.trailer_brake_boost / 100.f;
        if ( brake_mult <= 1.001f ) return;
        for ( uint32_t i = 0; i < trailer_count; ++i )
        {
            if ( joint( trailers[ i ] ) == nullptr ) continue;
            update_wheel_brake_mask( trailers[ i ] );
            const float v = prism::offsets::field< float >( trailers[ i ], prism::offsets::trailer_service_brake );
            if ( v <= 0.05f ) continue;
            float& park = prism::offsets::field< float >( trailers[ i ], prism::offsets::trailer_brake );
            const float boosted = ( std::min )( v * brake_mult, brake_mult );
            if ( boosted > park )
                park = boosted;
        }
    }

    //========================================================================
    // Canais de freio do convoy + spring-brake das peças desatreladas
    //========================================================================

    // Espelha os canais de freio do caminhão direto em cada carreta do convoy:
    // o caminho nativo de service-brake precisa das flags de conector
    // pneumático que o pair_vehicles_fn arma, que a nossa réplica não
    // reconstrói por completo - então uma carreta engatada pelo plugin rola
    // sem freio e empurra o caminhão. A física consome +0x730 direto (o loop
    // de spring-brake também escreve lá), então jogar os valores do caminhão
    // lá freia a carreta. Broadcasts nativos trazem os mesmos números quando
    // o caminho nativo funciona.
    // E a carreta desatrelada TEM que continuar com spring-brake: o detach
    // escreve trailer_brake (+0x760) = 1 uma vez, mas o loop nativo por frame
    // que mantém carretas sem junta freadas é gated pelo coupling-state do
    // caminhão - enquanto outra carreta continua engatada (state 5) nada
    // reaplica. Pina o fator de spring + demanda zerada a cada frame. O teste
    // é live-reachability, NÃO "joint == nullptr": uma carreta ainda jointada
    // a um pai ESTACIONADO faz parte de uma seção estacionada e tem que ficar
    // freada também. Enquanto a peça está assentando (anchor_settling -
    // tombando em cima do apoio, rolando até o equilíbrio) as rodas NÃO podem
    // travar: um dolly de eixo único com CG atrás do eixo precisa rolar uns
    // centímetros pra achar a pose de repouso - rodas travadas o deixam
    // balançando num ponto de contato só.
    void update_convoy_brakes( void* truck, const bool* in_convoy )
    {
        if ( truck != nullptr )
        {
            const float pedal = prism::offsets::field< float >( truck, prism::offsets::trailer_service_brake );
            const float retarder = prism::offsets::field< float >( truck, 0x14C8 );
            const float service = pedal > retarder ? pedal : retarder;
            const float brake = prism::offsets::field< float >( truck, prism::offsets::trailer_brake );
            const uint8_t forced = prism::offsets::field< uint8_t >( truck, prism::offsets::trailer_brake_forced );
            for ( uint32_t i = 0; i < trailer_count; ++i )
            {
                if ( !in_convoy[ i ] ) continue;
                void* t = trailers[ i ];
                prism::offsets::field< float >( t, prism::offsets::trailer_service_brake ) = service;
                prism::offsets::field< float >( t, prism::offsets::trailer_brake ) = brake;
                prism::offsets::field< uint8_t >( t, prism::offsets::trailer_brake_forced ) = forced;
            }
        }
        const auto pin_dead = []( void* t )
        {
            if ( t == nullptr || !readable_ptr( t ) ||
                 !readable_ptr( static_cast< uint8_t* >( t ) + prism::offsets::trailer_physics_joint ) ||
                 trailer_is_live( t ) )
            {
                return;
            }
            const int bi = canon_index_of( t );
            const float brake = bi >= 0 && anchor_settling[ bi ] ? 0.25f : 1.f;
            prism::offsets::field< float >( t, prism::offsets::trailer_brake ) = brake;
            prism::offsets::field< float >( t, prism::offsets::trailer_service_brake ) = 0.f;
            prism::offsets::field< uint8_t >( t, prism::offsets::trailer_brake_forced ) = brake >= 1.f ? 1 : 0;
        };
        for ( uint32_t i = 0; i < trailer_count; ++i ) pin_dead( trailers[ i ] );
        for ( void* d : detached_trailers ) pin_dead( d );
    }
}
