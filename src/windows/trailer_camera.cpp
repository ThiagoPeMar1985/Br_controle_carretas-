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

    namespace
    {
        std::atomic< void* > camera_anchor_request{ nullptr };
        std::atomic< bool > camera_anchor_override{ false };
        std::atomic< bool > camera_anchor_restore{ false };
        std::atomic< bool > camera_recompute_pending{ false };
    }

    // 198 - hook: tira carreta sem junta da âncora da câmera 2
    void __fastcall hk_cam_set_target( void* cam, void* target )
    {
        if ( target != nullptr && is_tracked_trailer( target ) && joint( target ) == nullptr )
        {
            target = nullptr;
        }
        cam_set_target_hook->get_original< cam_set_target_fn >()( cam, target );
    }

    // 199 - hook: update da câmera 2 (esconde links mortos na medida)
    void __fastcall hk_cam_update( void* cam, float dt )
    {

        if ( canon_age < 90 )
        {
            cam_update_hook->get_original< cam_update_fn >()( cam, dt );
            return;
        }
        const bool force_anchor = camera_anchor_override.load( std::memory_order_acquire );
        const bool restore_anchor = !force_anchor &&
            camera_anchor_restore.exchange( false, std::memory_order_acq_rel );
        void* requested_anchor = camera_anchor_request.load( std::memory_order_acquire );

        if ( force_anchor && requested_anchor == nullptr )
        {
            void* ga = CCore::g_instance->get_game_actor();
            void* truck = ga != nullptr && readable_ptr( ga )
                ? prism::offsets::field< void* >( ga, prism::offsets::game_actor_truck )
                : nullptr;
            void* towed = truck != nullptr &&
                readable_ptr( static_cast< uint8_t* >( truck ) + 0x1580 )
                ? prism::offsets::field< void* >( truck, 0x1580 ) : nullptr;
            if ( towed != nullptr && readable_ptr( towed ) &&
                 readable_ptr( static_cast< uint8_t* >( towed ) + prism::offsets::trailer_physics_joint ) &&
                 joint( towed ) != nullptr )
            {
                camera_anchor_request.store( towed, std::memory_order_release );
                camera_anchor_override.store( false, std::memory_order_release );
                camera_anchor_restore.store( true, std::memory_order_release );
                camera_recompute_pending.store( true, std::memory_order_release );
                CCore::g_instance->info( "cam: stale solo pin released - towing {:p}", towed );
            }
        }
        auto& anchor = prism::offsets::field< void* >( cam, 0x3F0 );
        if ( force_anchor )
        {
            if ( anchor != requested_anchor ) anchor = requested_anchor;
        }
        else if ( restore_anchor && anchor != requested_anchor )
        {
            if ( cam_set_target_hook != nullptr && plausible_vfunc( cam, 31 ) )
            {
                cam_set_target_hook->get_original< cam_set_target_fn >()( cam, requested_anchor );
            }
            else
            {
                anchor = requested_anchor;
            }
        }
        if ( camera_recompute_pending.exchange( false, std::memory_order_acq_rel ) &&
             plausible_vfunc( cam, 14 ) )
        {
            vcall< cam_recompute_fn >( cam, 14, uint8_t( 1 ) );
        }
        struct hidden_link { void* owner; void* slave; };
        hidden_link hidden[ max_trailers ];
        uint32_t hidden_n = 0;
        for ( uint32_t i = 0; i < trailer_count && hidden_n < max_trailers; ++i )
        {
            void* t = trailers[ i ];
            if ( t == nullptr || !readable_ptr( t ) ||
                 !readable_ptr( static_cast< uint8_t* >( t ) + prism::offsets::trailer_physics_joint ) ||
                 joint( t ) == nullptr )
            {
                continue;
            }
            void* slave = prism::offsets::field< void* >( t, prism::offsets::trailer_slave );
            if ( slave != nullptr && readable_ptr( slave ) &&
                 readable_ptr( static_cast< uint8_t* >( slave ) + prism::offsets::trailer_physics_joint ) &&
                 joint( slave ) == nullptr )
            {
                hidden[ hidden_n++ ] = { t, slave };
                prism::offsets::field< void* >( t, prism::offsets::trailer_slave ) = nullptr;
            }
        }
        cam_update_hook->get_original< cam_update_fn >()( cam, dt );
        for ( uint32_t i = 0; i < hidden_n; ++i )
        {
            if ( readable_ptr( hidden[ i ].owner ) )
            {
                prism::offsets::field< void* >( hidden[ i ].owner, prism::offsets::trailer_slave ) =
                    hidden[ i ].slave;
            }
        }

        if ( force_anchor && anchor != requested_anchor ) anchor = requested_anchor;
    }
    // 200 - hook: comprimento do comboio (para na primeira junta morta)
    float __fastcall hk_combo_length( void* self )
    {

        if ( !settings::g_settings.cargo_restore )
        {
            return combo_length_hook->get_original< combo_length_fn >()( self );
        }
        if ( self == nullptr || !readable_ptr( self ) || combo_hook_pos_fn_ptr == nullptr )
        {
            return combo_length_hook->get_original< combo_length_fn >()( self );
        }
        const auto f = []( void* o, const uint32_t off )
        {
            return prism::offsets::field< float >( o, off );
        };
        float acc = f( self, 0x2C ) - f( self, 0x20 );
        void* prev = self;

        void* seen[ max_trailers * 2 + 1 ] = { self };
        uint32_t seen_n = 1;
        for ( void* t = prism::offsets::field< void* >( self, prism::offsets::trailer_slave );
              t != nullptr;
              prev = t, t = prism::offsets::field< void* >( t, prism::offsets::trailer_slave ) )
        {

            if ( !readable_ptr( t ) ||
                !readable_ptr( static_cast< uint8_t* >( t ) + prism::offsets::trailer_physics_joint ) ||
                joint( t ) == nullptr )
            {
                break;
            }
            bool dup = false;
            for ( uint32_t k = 0; k < seen_n; ++k ) dup |= seen[ k ] == t;
            if ( dup || seen_n >= max_trailers * 2 + 1 ) break;
            seen[ seen_n++ ] = t;
            float vec[ 4 ] = {};
            combo_hook_pos_fn_ptr( prev, vec );
            const float gap = f( prev, 0x2C ) - vec[ 2 ];
            void* chassis = prism::offsets::field< void* >( t, 0x200 );
            const float over = ( chassis != nullptr ? f( chassis, 0x748 ) : 0.f ) - f( t, 0x20 );
            acc += ( f( t, 0x2C ) - f( t, 0x20 ) ) - gap - over;
        }
        return acc;
    }

    // 201 - acha o objeto da câmera 2
    void* chase_camera_obj()
    {
        if ( !expected_build() ) return nullptr;
        if ( chase_camera != nullptr ) return chase_camera;
        const auto mgr_va = validated_va( 0x1436AF830 );
        const auto vtbl_va = validated_va( 0x1423126F0 );
        if ( mgr_va == 0 || vtbl_va == 0 ) return nullptr;
        auto* manager = *reinterpret_cast< void** >( mgr_va );
        if ( !readable_ptr( manager ) ) return nullptr;
        const auto chase_vtbl = reinterpret_cast< void* >( vtbl_va );
        auto is_chase = [ & ]( void* p )
        {
            return p != nullptr && readable_ptr( p ) &&
                *reinterpret_cast< void** >( p ) == chase_vtbl;
        };
        for ( uintptr_t off = 8; off < 0x400; off += 8 )
        {
            auto* addr = reinterpret_cast< uint8_t* >( manager ) + off;
            if ( !readable_ptr( addr ) ) break;
            void* cand = *reinterpret_cast< void** >( addr );

            if ( cand == chase_vtbl )
            {
                chase_camera = addr;
                CCore::g_instance->info( "cam: chase camera embedded at manager+0x{:x}", off );
                return chase_camera;
            }

            if ( is_chase( cand ) )
            {
                chase_camera = cand;
                CCore::g_instance->info( "cam: chase camera {:p} (manager+0x{:x})", cand, off );
                return cand;
            }

            if ( cand != nullptr && readable_ptr( cand ) )
            {
                for ( uint32_t s = 0; s < 24; ++s )
                {
                    auto* slot = reinterpret_cast< void** >( cand ) + s;
                    if ( !readable_ptr( slot ) ) break;
                    void* obj = *slot;
                    if ( is_chase( obj ) )
                    {
                        chase_camera = obj;
                        CCore::g_instance->info(
                            "cam: chase camera {:p} (manager+0x{:x} -> slot[{}])",
                            obj, off, s );
                        return obj;
                    }
                }
            }
        }
        static bool miss_logged = false;
        if ( !miss_logged )
        {
            miss_logged = true;
            CCore::g_instance->info( "cam: chase camera NOT FOUND (manager {:p})", manager );
        }
        return nullptr;
    }

    // 202 - mantém a âncora da câmera 2 na carreta certa
    void enforce_chase_camera_anchor()
    {
        static bool override_active = false;
        static float saved_zoom_min = 0.0f;
        static float zoom_dist = -1.0f;
        static uint32_t last_det_recs = 0;
        static uint32_t last_jointless = 0;
        static bool was_detached = false;
        static uint32_t solo_settle = 0;
        static void* last_forced_anchor = reinterpret_cast< void* >( ~0ull );

        void* cam_truck = nullptr;
        hook_xform_t truck_xf{};
        bool truck_pos_ok = false;
        if ( auto* ga = CCore::g_instance->get_game_actor() )
        {
            cam_truck = prism::offsets::field< void* >( ga, prism::offsets::game_actor_truck );
            truck_pos_ok = cam_truck != nullptr && readable_ptr( cam_truck ) && hook_world( cam_truck, truck_xf );
        }
        const auto near_truck = [ & ]( void* t )
        {
            if ( !truck_pos_ok || t == nullptr || !readable_ptr( t ) ) return true;
            hook_xform_t tx{};
            if ( !hook_world( t, tx ) ) return true;
            const float dx = tx.pos.x - truck_xf.pos.x;
            const float dz = tx.pos.z - truck_xf.pos.z;
            return dx * dx + dz * dz < 150.f * 150.f;
        };
        bool any_detached = false;
        for ( uint32_t i = 0; i < trailer_count; ++i )
        {
            void* t = trailers[ i ];
            if ( t != nullptr && joint( t ) == nullptr && near_truck( t ) )
            {
                any_detached = true;
                break;
            }
        }
        for ( void* d : detached_trailers )
        {
            if ( d != nullptr && near_truck( d ) ) any_detached = true;
        }

        const int wheel = CCore::g_instance->consume_in_game_wheel();
        if ( !any_detached && !override_active ) return;
        void* chase = chase_camera_obj();

        if ( chase == nullptr || !plausible_vfunc( chase, 14 ) )
        {
            chase_camera = nullptr;
            return;
        }

        void* chain_head = nullptr;
        {
            auto* game_actor = CCore::g_instance->get_game_actor();
            void* truck = game_actor != nullptr && readable_ptr( game_actor )
                ? prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck )
                : nullptr;
            if ( truck != nullptr && readable_ptr( truck ) )
            {
                chain_head = prism::offsets::field< void* >( truck, 0x1580 );

                if ( chain_head == nullptr )
                {
                    for ( uint32_t i = 0; i < trailer_count; ++i )
                    {
                        void* t = trailers[ i ];
                        if ( t == nullptr || joint( t ) == nullptr ) continue;
                        if ( prism::offsets::field< void* >( t, prism::offsets::trailer_parent ) == truck )
                        {
                            chain_head = t;
                            break;
                        }
                    }
                }
            }
        }
        if ( any_detached )
        {

            uint32_t det_recs_now = 0;
            for ( const void* d : detached_trailers ) if ( d != nullptr ) ++det_recs_now;
            uint32_t jointless_now = 0;
            for ( uint32_t i = 0; i < trailer_count; ++i )
                if ( trailers[ i ] != nullptr && joint( trailers[ i ] ) == nullptr ) ++jointless_now;
            const bool set_changed = !was_detached || det_recs_now != last_det_recs ||
                jointless_now != last_jointless;

            const bool anchor_changed = last_forced_anchor != chain_head;
            camera_anchor_request.store( chain_head, std::memory_order_release );
            camera_anchor_override.store( true, std::memory_order_release );

            if ( anchor_changed || ( set_changed && chain_head != nullptr ) )
            {
                camera_recompute_pending.store( true, std::memory_order_release );
            }
            if ( anchor_changed )
                CCore::g_instance->info( "cam: anchor -> {:p} (live chain head)", chain_head );
            last_forced_anchor = chain_head;
            was_detached = true;
            last_det_recs = det_recs_now;
            last_jointless = jointless_now;

            if ( chain_head == nullptr )
            {
                if ( zoom_dist < 0.0f )
                {

                    if ( ++solo_settle < 90 ) return;

                    const float live_dist = prism::offsets::field< float >( chase, 0x4DC );
                    if ( live_dist < 3.f ) { solo_settle = 60; return; }
                    saved_zoom_min = prism::offsets::field< float >( chase, 0x480 );
                    zoom_dist = live_dist;
                }
                zoom_dist -= static_cast< float >( wheel ) * ( 2.0f / 120.0f );
                if ( zoom_dist < 0.5f ) zoom_dist = 0.5f;
                prism::offsets::field< float >( chase, 0x480 ) = zoom_dist;

                prism::offsets::field< float >( chase, 0x4D8 ) = zoom_dist - 0.01f;
            }
            else if ( solo_settle = 0, zoom_dist >= 0.0f )
            {

                prism::offsets::field< float >( chase, 0x480 ) = saved_zoom_min;
                zoom_dist = -1.0f;
            }
        }
        else
        {

            camera_anchor_request.store( chain_head, std::memory_order_release );
            camera_anchor_override.store( false, std::memory_order_release );
            if ( was_detached ||
                 last_forced_anchor != reinterpret_cast< void* >( ~0ull ) )
            {

                camera_anchor_restore.store( true, std::memory_order_release );
                camera_recompute_pending.store( true, std::memory_order_release );
                CCore::g_instance->info( "cam: restore native anchor {:p}", chain_head );
            }
            prism::offsets::field< float >( chase, 0x480 ) = saved_zoom_min;
            prism::offsets::field< float >( chase, 0x4D8 ) =
                prism::offsets::field< float >( chase, 0x4DC );
            zoom_dist = -1.0f;
            solo_settle = 0;
            last_det_recs = 0;
            last_jointless = 0;
            was_detached = false;

            last_forced_anchor = reinterpret_cast< void* >( ~0ull );
        }
        override_active = any_detached;
    }
    namespace
    {
        const bool fn_idx_reg = []()
        {
            fn_index_add( fn_addr_of( &hk_cam_set_target ), 198, "hk_cam_set_target" );
            fn_index_add( fn_addr_of( &hk_cam_update ), 199, "hk_cam_update" );
            fn_index_add( fn_addr_of( &hk_combo_length ), 200, "hk_combo_length" );
            fn_index_add( fn_addr_of( &chase_camera_obj ), 201, "chase_camera_obj" );
            fn_index_add( fn_addr_of( &enforce_chase_camera_anchor ), 202, "enforce_chase_camera_anchor" );
            return true;
        }();
    }
}
