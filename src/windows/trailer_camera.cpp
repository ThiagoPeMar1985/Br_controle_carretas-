// Chase camera - anchor enforcement, camera hooks, combination length.
// Split out of trailer_coupling.cpp.

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

namespace ts_extra_utilities
{
    // Chase-camera target setter (base vfunc 31, 0x14063C2D0): whenever the
    // native resolver tries to anchor the camera on a tracked trailer whose
    // physics joint is dead, swap the target for null - the same "no
    // trailer" state the game uses when driving solo, so camera 2 follows
    // the truck with all of its native orbit/zoom handling intact
    void __fastcall hk_cam_set_target( void* cam, void* target )
    {
        if ( target != nullptr && is_tracked_trailer( target ) && joint( target ) == nullptr )
        {
            target = nullptr;
        }
        cam_set_target_hook->get_original< cam_set_target_fn >()( cam, target );
    }
    // Chase-camera update (vfunc 32, 0x140AC75A0): the camera's own chain
    // walk also follows trailer_slave without checking joints - the same
    // shape as combo_length. Hide every live->dead link for the duration of
    // the call so the camera measures the live combination only (restored
    // right after - the links still carry the parked pieces' visibility)
    void __fastcall hk_cam_update( void* cam, float dt )
    {
        // pass through untouched while trailers are still being assembled -
        // hiding links from half-built objects corrupts the camera's view of
        // a chain the game itself is still building
        if ( canon_age < 90 )
        {
            cam_update_hook->get_original< cam_update_fn >()( cam, dt );
            return;
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
    }
    float __fastcall hk_combo_length( void* self )
    {
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
        // the maintenance rewrites slave links every frame - a claim edge
        // plus a native link can meet in a cycle for a few frames, and this
        // walk has no other exit: without a revisit check it spins here
        // until another thread happens to break the ring, eating the whole
        // frame slice every call (the 3-5 FPS crawl)
        void* seen[ max_trailers * 2 + 1 ] = { self };
        uint32_t seen_n = 1;
        for ( void* t = prism::offsets::field< void* >( self, prism::offsets::trailer_slave );
              t != nullptr;
              prev = t, t = prism::offsets::field< void* >( t, prism::offsets::trailer_slave ) )
        {
            // the only deviation from the original: a slave whose joint is
            // gone is detached - it and everything behind it is no longer
            // part of this combination
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
    // the chase camera object lives in the camera-manager slot array; find it
    // by its vehicle_behind_rotation_camera vtable (exe+0x23126F0)
    void* chase_camera_obj()
    {
        if ( !expected_build() ) return nullptr;
        if ( chase_camera != nullptr ) return chase_camera;
        const auto base = reinterpret_cast< uint64_t >( GetModuleHandle( nullptr ) );
        auto* manager = *reinterpret_cast< void** >( base + 0x36AF830 );
        if ( !readable_ptr( manager ) ) return nullptr;   // plain struct, no vtable
        const auto chase_vtbl = reinterpret_cast< void* >( base + 0x23126F0 );
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
            // camera embedded in the manager: the qword IS the vtable
            if ( cand == chase_vtbl )
            {
                chase_camera = addr;
                CCore::g_instance->info( "cam: chase camera embedded at manager+0x{:x}", off );
                return chase_camera;
            }
            // camera object pointed to by a manager field
            if ( is_chase( cand ) )
            {
                chase_camera = cand;
                CCore::g_instance->info( "cam: chase camera {:p} (manager+0x{:x})", cand, off );
                return cand;
            }
            // or a slot array pointer: elements are the camera objects
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
    // per-frame: while a trailer is detached the chase camera anchors on the
    // HEAD OF THE LIVE CHAIN - the trailer physically on the fifth wheel
    // (truck+0x15A8). Detaching a TAIL trailer does not change the head:
    // trailer 0 stays the anchor and the camera keeps measuring the live
    // combination through hk_combo_length (which stops at the first dead
    // joint). Only when the truck tows NOTHING does the anchor go null -
    // the native solo state that follows the truck. The store is a raw
    // write with NO refcounting: the native resolver writes this field
    // directly too, and releasing the previous value is what underflowed
    // refcounts and froze the camera after recoupling
    void enforce_chase_camera_anchor()
    {
        static bool override_active = false;
        static float saved_zoom_min = 0.0f;   // camera +0x480 as shipped by the game
        static float zoom_dist = -1.0f;       // wheel-driven distance, <0 = inactive
        static uint32_t last_det_recs = 0;    // detached records at last recompute
        static uint32_t last_jointless = 0;   // jointless trailers at last recompute
        static bool was_detached = false;     // detach edge tracker
        static uint32_t solo_settle = 0;      // frames waited before pinning the solo zoom
        static void* last_forced_anchor = reinterpret_cast< void* >( ~0ull );

        // a loose trailer only matters to the camera while it is near the
        // truck - one parked in another city (a job trailer waiting to be
        // picked up, a garage return) must not keep the override alive: it
        // pinned the solo zoom and re-measured the combination forever
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
        if ( !any_detached && !override_active ) return;   // pure native behaviour
        void* chase = chase_camera_obj();
        // vfunc 14 (recompute) is called on transitions - a camera mid-
        // rebuild (delivery cinematic) would carry a dead slot target
        if ( chase == nullptr || !plausible_vfunc( chase, 14 ) )
        {
            chase_camera = nullptr;
            return;
        }

        void*& anchor = prism::offsets::field< void* >( chase, 0x3F0 );
        // desired anchor = the trailer JOINTED TO THE TRUCK - the head of
        // the live chain. A trailer still jointed to a PARKED parent must
        // not count: truck+0x15A8 is authoritative. The camera measures the
        // combination by walking trailer_slave FROM the anchor and
        // hk_combo_length stops at the first dead joint, so it parks behind
        // the last attached trailer. Null = native solo state
        void* chain_head = nullptr;
        {
            auto* game_actor = CCore::g_instance->get_game_actor();
            void* truck = game_actor != nullptr && readable_ptr( game_actor )
                ? prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck )
                : nullptr;
            if ( truck != nullptr && readable_ptr( truck ) )
            {
                chain_head = prism::offsets::field< void* >( truck, 0x15A8 );
                // fallback: first trailer whose parent IS the truck, in case
                // the towed pointer lags a frame behind the joint
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
            // the anchor (chain head) does NOT change when only the TAIL of
            // the set detaches - trailer 0 stays the head - so a pure
            // anchor-change check never fires and the camera keeps the
            // distance it measured for the full combination. Any change in
            // the live set needs a recompute: vfunc 14 re-measures through
            // hk_combo_length, which now stops at the first dead joint
            uint32_t det_recs_now = 0;
            for ( const void* d : detached_trailers ) if ( d != nullptr ) ++det_recs_now;
            uint32_t jointless_now = 0;
            for ( uint32_t i = 0; i < trailer_count; ++i )
                if ( trailers[ i ] != nullptr && joint( trailers[ i ] ) == nullptr ) ++jointless_now;
            const bool set_changed = !was_detached || det_recs_now != last_det_recs ||
                jointless_now != last_jointless;
            // the native resolver re-writes the anchor to a parked trailer
            // every frame while anything sits detached - keep re-storing
            // chain_head, but vfunc 14 (recompute) RESETS the orbit state:
            // per-frame calls snap the camera back behind the truck the
            // moment the user tries to orbit. Recompute only on transitions
            if ( anchor != chain_head )
            {
                anchor = chain_head;   // raw store, no release - see comment above
                if ( last_forced_anchor != chain_head )
                {
                    vcall< cam_recompute_fn >( chase, 14, uint8_t( 1 ) );
                    last_forced_anchor = chain_head;
                    CCore::g_instance->info( "cam: anchor -> {:p} (live chain head)", chain_head );
                }
            }
            else if ( set_changed )
            {
                vcall< cam_recompute_fn >( chase, 14, uint8_t( 1 ) );
            }
            was_detached = true;
            last_det_recs = det_recs_now;
            last_jointless = jointless_now;

            // zoom distance: the native update clamps the target (+0x4D8) to
            // [+0x480, base], so raising +0x480 above the base distance pins
            // the camera at our value - pure field write, the native lerp at
            // +0x4DC still smooths every change. Only while the truck tows
            // NOTHING: with a live trailer on the fifth wheel the camera
            // must keep measuring the live combination - pinning +0x4D8
            // would freeze it at the distance the full set was measured at
            if ( chain_head == nullptr )
            {
                if ( zoom_dist < 0.0f )
                {
                    // the recompute above re-measures the solo distance but the
                    // smoothed distance (+0x4DC) still holds the full combination's
                    // value for a moment - capturing it now pins the camera far
                    // behind a truck that tows nothing. Let the native lerp settle
                    if ( ++solo_settle < 90 ) return;
                    // a distance under 3 m is a camera still in a transition
                    // (garage exit, cinematic) - pinning it puts the camera
                    // inside the cab; keep the native behaviour until it settles
                    const float live_dist = prism::offsets::field< float >( chase, 0x4DC );
                    if ( live_dist < 3.f ) { solo_settle = 60; return; }
                    saved_zoom_min = prism::offsets::field< float >( chase, 0x480 );
                    zoom_dist = live_dist;
                }
                zoom_dist -= static_cast< float >( wheel ) * ( 2.0f / 120.0f );   // ~2m per notch
                if ( zoom_dist < 0.5f ) zoom_dist = 0.5f;
                prism::offsets::field< float >( chase, 0x480 ) = zoom_dist;
                // keep the target strictly below our min so the clamp picks it
                prism::offsets::field< float >( chase, 0x4D8 ) = zoom_dist - 0.01f;
            }
            else if ( solo_settle = 0, zoom_dist >= 0.0f )
            {
                // went from detached-watch to towing while pieces stay
                // parked: release the pin so the native update drives the
                // distance off combo_length again
                prism::offsets::field< float >( chase, 0x480 ) = saved_zoom_min;
                zoom_dist = -1.0f;
            }
        }
        else
        {
            // set recoupled: the native pick is the CHAIN HEAD (first
            // trailer), not the last one. The camera measures the
            // combination length by walking trailer_slave links STARTING AT
            // the anchor; anchoring on the last trailer measures only that
            // trailer and the camera parks just behind the truck
            if ( anchor != chain_head && cam_set_target_hook != nullptr &&
                plausible_vfunc( chase, 31 ) )
            {
                // go through the real setter (vfunc 31) - refcounted
                // store, the same call the native resolver makes. The
                // field currently holds our raw null, so there's no
                // stale ref for the setter to release.
                cam_set_target_hook->get_original< cam_set_target_fn >()( chase, chain_head );
                CCore::g_instance->info( "cam: anchor -> first trailer {:p} (set coupled)", chain_head );
            }
            prism::offsets::field< float >( chase, 0x480 ) = saved_zoom_min;
            prism::offsets::field< float >( chase, 0x4D8 ) =
                prism::offsets::field< float >( chase, 0x4DC );
            zoom_dist = -1.0f;
            solo_settle = 0;
            last_det_recs = 0;
            last_jointless = 0;
            was_detached = false;
            // recompute (vfunc 14) re-reads the anchor's joint and switches
            // the camera back to the jointed distance set (full SCS range)
            // - skipped while detached because it resets the orbit state
            if ( last_forced_anchor != reinterpret_cast< void* >( ~0ull ) )
            {
                vcall< cam_recompute_fn >( chase, 14, uint8_t( 1 ) );
                last_forced_anchor = reinterpret_cast< void* >( ~0ull );
            }
        }
        override_active = any_detached;
    }
}
