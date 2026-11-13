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

namespace br_carretas
{

    void* native_mask_ws[ 32 ] = {};
    uint32_t native_mask_val[ 32 ] = {};

    // 204 - máscara de freio nativa do wheelset (cache)
    uint32_t native_mask_of( void* ws )
    {
        for ( int i = 0; i < 32; ++i )
            if ( native_mask_ws[ i ] == ws ) return native_mask_val[ i ];
        const uint32_t native = prism::offsets::field< uint32_t >( ws, 0x2BC );
        for ( int i = 0; i < 32; ++i )
            if ( native_mask_ws[ i ] == nullptr ) { native_mask_ws[ i ] = ws; native_mask_val[ i ] = native; break; }
        return native;
    }

    // 205 - reescreve a máscara de rodas freáveis por frame
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

    using brake_setter_fn = void __fastcall( void* /*vehicle*/, float /*value*/ );
    using forced_setter_fn = void __fastcall( void* /*vehicle*/, uint8_t /*value*/ );

    uint64_t truck_brake_broadcast_ret = 0;
    uint64_t truck_forced_broadcast_ret = 0;

    // 206 - engole escrita de freio vinda de peça estacionada
    bool foreign_brake_write( void* self, const void* ret )
    {
        const auto ra = reinterpret_cast< uint64_t >( ret );
        if ( ra == truck_brake_broadcast_ret || ra == truck_forced_broadcast_ret )
        {
            return false;
        }

        if ( self == nullptr || joint( self ) == nullptr ) return false;

        if ( auto* game_actor = CCore::g_instance->get_game_actor() )
        {
            if ( self == prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck ) )
            {
                return false;
            }
        }
        return true;
    }

    // 207 - acha o próximo link sem junta na cadeia
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

    // 208 - amplifica o valor do freio pela config
    float biased_brake( void* self, const float v )
    {
        if ( !license::active() ) return v;
        const float boost = settings::g_settings.trailer_brake_boost;
        if ( v <= 0.f || boost <= 0.f || self == nullptr ) return v;
        if ( auto* game_actor = CCore::g_instance->get_game_actor() )
        {
            if ( self == prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck ) )
            {
                return v;
            }
        }
        if ( joint( self ) == nullptr ) return v;
        const float mult = 1.f + boost / 100.f;
        return ( std::min )( v * mult, mult );
    }

    // 209 - hook: setter de freio vindo da cadeia
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

    // 210 - hook: setter de freio direto na carreta
    void __fastcall hk_brake_setter( void* self, float v )
    {
        if ( v > 0.f && foreign_brake_write( self, _ReturnAddress() ) ) return;
        brake_setter_hook->get_original< brake_setter_fn >()( self, biased_brake( self, v ) );
    }

    // 211 - hook: flag de freio forçado vindo da cadeia
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

    // 212 - hook: flag de freio forçado direto na carreta
    void __fastcall hk_forced_setter( void* self, uint8_t v )
    {
        if ( v != 0 && foreign_brake_write( self, _ReturnAddress() ) ) return;
        forced_setter_hook->get_original< forced_setter_fn >()( self, v );
    }

    using wheel_brake_fn = void __fastcall( void* /*wheelset*/ );
    // 213 - hook: solver de freio por roda (zera roda no ar)
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

    // 214 - instala os hooks de freio
    void brake_register_hooks()
    {
        if ( !expected_build() ) return;
        truck_brake_broadcast_ret = validated_va( 0x14064B7EF );
        truck_forced_broadcast_ret = validated_va( 0x14064B806 );
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

    // 215 - por frame: aplica o boost no fator de freio
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

    // 216 - freio de mão nas peças estacionadas do comboio
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
    namespace
    {
        const bool fn_idx_reg = []()
        {
            fn_index_add( fn_addr_of( &native_mask_of ), 204, "native_mask_of" );
            fn_index_add( fn_addr_of( &update_wheel_brake_mask ), 205, "update_wheel_brake_mask" );
            fn_index_add( fn_addr_of( &foreign_brake_write ), 206, "foreign_brake_write" );
            fn_index_add( fn_addr_of( &next_jointless_link ), 207, "next_jointless_link" );
            fn_index_add( fn_addr_of( &biased_brake ), 208, "biased_brake" );
            fn_index_add( fn_addr_of( &hk_brake_chain_setter ), 209, "hk_brake_chain_setter" );
            fn_index_add( fn_addr_of( &hk_brake_setter ), 210, "hk_brake_setter" );
            fn_index_add( fn_addr_of( &hk_forced_chain_setter ), 211, "hk_forced_chain_setter" );
            fn_index_add( fn_addr_of( &hk_forced_setter ), 212, "hk_forced_setter" );
            fn_index_add( fn_addr_of( &hk_wheel_brake ), 213, "hk_wheel_brake" );
            fn_index_add( fn_addr_of( &brake_register_hooks ), 214, "brake_register_hooks" );
            fn_index_add( fn_addr_of( &brake_boost_update ), 215, "brake_boost_update" );
            fn_index_add( fn_addr_of( &update_convoy_brakes ), 216, "update_convoy_brakes" );
            return true;
        }();
    }
}
