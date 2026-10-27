#pragma once
// Shared trailer-chain state and helpers used by the coupling, lines,
// cargo-restore and camera translation units.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "imgui.h"

#include "core.hpp"
#include "hooks/function_hook.hpp"
#include "memory/memory_utils.hpp"
#include "prism/common.hpp"
#include "prism/functions.hpp"
#include "prism/offsets.hpp"
#include "settings/settings.hpp"

namespace ts_extra_utilities
{
    constexpr uint32_t max_trailers = 20;

    //========================================================================
    // Memory safety probes
    //
    // Everything reached through a reverse-engineered offset is validated
    // before dereference: the pointer must be committed/readable, and an
    // object's vtable — plus the target of any vfunc slot we call — must sit
    // inside the exe image. A wrong-typed object can carry a "vtable" pointer
    // into exe data, so slot targets are checked against the code range too.
    //========================================================================
    inline bool readable_ptr( const void* p )
    {
        if ( p == nullptr ) return false;
        MEMORY_BASIC_INFORMATION mbi{};
        return VirtualQuery( p, &mbi, sizeof( mbi ) ) != 0 && mbi.State == MEM_COMMIT &&
            ( mbi.Protect & ( PAGE_NOACCESS | PAGE_GUARD ) ) == 0;
    }

    // how many bytes past p are continuously readable, capped at want. Field
    // sweeps must be bounded by this — a plausible object sitting at the end
    // of a committed region AVs the moment a scan reads past the page end
    inline uintptr_t readable_span( const void* p, const uintptr_t want )
    {
        if ( p == nullptr ) return 0;
        const auto base = reinterpret_cast< uintptr_t >( p );
        uintptr_t span = 0;
        MEMORY_BASIC_INFORMATION mbi{};
        while ( span < want &&
            VirtualQuery( reinterpret_cast< void* >( base + span ), &mbi, sizeof( mbi ) ) != 0 &&
            mbi.State == MEM_COMMIT && ( mbi.Protect & ( PAGE_NOACCESS | PAGE_GUARD ) ) == 0 )
        {
            const auto region_end = reinterpret_cast< uintptr_t >( mbi.BaseAddress ) + mbi.RegionSize;
            span = region_end > base ? region_end - base : 0;
        }
        return span < want ? span : want;
    }

    inline bool plausible_object( const void* p )
    {
        if ( !readable_ptr( p ) ) return false;
        const auto base = reinterpret_cast< uint64_t >( GetModuleHandle( nullptr ) );
        const auto vtbl = *reinterpret_cast< const uint64_t* >( p );
        return vtbl >= base && vtbl < base + 0x4000000;
    }

    inline bool plausible_vfunc( const void* obj, const uint32_t slot )
    {
        if ( !plausible_object( obj ) ) return false;
        const auto* vtbl = *reinterpret_cast< void* const* const* >( obj );
        const auto fn = reinterpret_cast< uint64_t >( vtbl[ slot ] );
        const auto base = reinterpret_cast< uint64_t >( GetModuleHandle( nullptr ) );
        return fn >= base && fn < base + 0x2000000;
    }

    // trailer-class check: every trailer vtable carries the same vfunc 99
    // (disconnect) impl at exe+0x789C20 — a readable but foreign pointer
    // (recycled trailer slot holding e.g. KERNELBASE data) passes
    // plausible_object yet never carries a trailer vtable
    inline bool is_trailer_obj( const void* p )
    {
        if ( !plausible_object( p ) ) return false;
        const auto* vtbl = *reinterpret_cast< const uint64_t* const* >( p );
        return vtbl[ 99 ] ==
            reinterpret_cast< uint64_t >( GetModuleHandle( nullptr ) ) + 0x789C20;
    }

    template < typename F, typename... A >
    auto vcall( void* object, const uint32_t slot, A&&... args )
    {
        auto* vtbl = *reinterpret_cast< void*** >( object );
        return reinterpret_cast< F* >( vtbl[ slot ] )( object, std::forward< A >( args )... );
    }

    //========================================================================
    // Game object accessors
    //========================================================================
    inline void* joint( void* trailer )
    {
        return prism::offsets::field< void* >( trailer, prism::offsets::trailer_physics_joint );
    }

    // non-null only on trailers with steerable wheels (heavy-haul / oversize)
    inline prism::vehicle_wheel_steering_data_t* steering_data( void* trailer )
    {
        return trailer == nullptr ? nullptr
            : prism::offsets::field< prism::vehicle_wheel_steering_data_t* >(
                trailer, prism::offsets::trailer_steering_data );
    }

    //========================================================================
    // Hooks installed across the feature files — reset together in the
    // window destructor
    //========================================================================
    inline std::shared_ptr< CFunctionHook > brake_chain_setter_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > brake_setter_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > svc_brake_chain_hook = nullptr; // unused: +0x730 is a dead demand mirror
    inline std::shared_ptr< CFunctionHook > svc_brake_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > forced_chain_setter_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > forced_setter_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > wheel_brake_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > set_individual_steering_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > crashes_when_disconnected_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > connect_slave_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > pre_detach_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > detach_dispatch_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > attach_dispatch_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > trailer_state_apply_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > trailer_teleport_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > actor_teleport_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > lane_obstacle_insert_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > combo_length_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > cam_set_target_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > cam_update_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > lamp_mask_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > lamp_propagate_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > set_lines_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > apply_cables_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > rebuild_lines_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > joint_walk_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > phys_link_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > purecall_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > trailer_disconnect_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > trailer_connect_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > trailer_connect_hook2 = nullptr;

    //========================================================================
    // Runtime state
    //========================================================================

    // the current trailer chain, refreshed by update() every frame on the
    // render thread
    inline void* trailers[ max_trailers ] = {};
    inline uint32_t trailer_count = 0;

    inline int index_of( const void* trailer )
    {
        for ( uint32_t i = 0; i < trailer_count; ++i )
            if ( trailers[ i ] == trailer ) return static_cast< int >( i );
        return -1;
    }

    //========================================================================
    // Detached tracking
    //========================================================================

    // trailers intentionally detached through this plugin. The game's own
    // attach/detach flows call connect_slave on the parent, which would
    // re-attach them right back — so the hook stays active and skips only
    // those trailers
    inline void* detached_trailers[ max_trailers ] = {};

    inline bool is_detached( const void* trailer )
    {
        for ( const void* d : detached_trailers ) if ( d == trailer ) return true;
        return false;
    }

    inline void mark_detached( void* trailer )
    {
        for ( void*& d : detached_trailers ) if ( d == nullptr ) { d = trailer; return; }
    }

    inline void unmark_detached( const void* trailer )
    {
        for ( void*& d : detached_trailers ) if ( d == trailer ) d = nullptr;
    }

    // trailers detached through the PLUGIN's own controls — a stricter set
    // than detached_trailers (which also holds natively detached heads so
    // connect_slave won't slam them back). These must survive cargo
    // load/unload untouched: no teleporting to the dock, no re-joining —
    // wherever the player left them is where they stay
    inline void* plugin_detached[ max_trailers ] = {};

    inline bool is_plugin_detached( const void* trailer )
    {
        for ( const void* d : plugin_detached ) if ( d == trailer ) return true;
        return false;
    }

    inline void mark_plugin_detached( void* trailer )
    {
        for ( void*& d : plugin_detached ) if ( d == nullptr ) { d = trailer; return; }
    }

    inline void unmark_plugin_detached( const void* trailer )
    {
        for ( void*& d : plugin_detached ) if ( d == trailer ) d = nullptr;
    }

    // any trailer object this plugin is tracking — needed before touching
    // trailer-specific fields like the physics joint on an unknown pointer
    inline bool is_tracked_trailer( const void* p )
    {
        for ( uint32_t i = 0; i < trailer_count; ++i )
            if ( trailers[ i ] == p ) return true;
        return is_detached( p ) || is_plugin_detached( p );
    }

    // trailer currently jointed straight to the truck through "connect to
    // truck"; used to refuse a second trailer on the fifth wheel
    inline void* truck_towed = nullptr;
    // a trailer whose native detach dispatcher ran but whose joint is still
    // alive — the async gear-crank sequence drops it seconds later. The
    // lines refresh must not rebuild the truck edge on it meanwhile
    inline void* pending_native_detach = nullptr;
    inline uint32_t pending_native_age = 0;
    // previous joint state per canon slot — the joint->null transition is
    // the physical moment the hook broke; hose teardown runs there, not at
    // dispatch time (the async crank drops the joint seconds after)
    inline bool canon_was_jointed[ max_trailers ] = {};

    // a native call that walks trailer_slave / lamp ctx (disconnect/connect
    // vfuncs, attach/detach dispatchers) is running on the sim thread — the
    // render-thread maintenance must not write those fields mid-call or it
    // can hand the native walker a recycled pointer (the KERNELBASE src AV
    // at the disconnect teardown)
    inline std::atomic< bool > native_chain_call_active{ false };

    // frames each canon slot has been continuously jointless while not live
    // to the truck — a settled parked trailer gets auto-marked past the
    // threshold so the record factory / parked pin / anchor lock keep it
    // alive, registered and attachable; a trailer mid-attach only reads
    // jointless for a few frames and never reaches it
    inline uint32_t canon_jointless_frames[ max_trailers ] = {};

    // g_traffic value saved before a forced "g_traffic 0" detach; restored once
    // the whole trailer set is jointed again. < 0 means nothing saved
    inline float saved_traffic = -1.f;
    // trailers the set had when traffic suppression started. The chain shrinks
    // when links leave it, so "all coupled" must compare against this — a
    // partially rebuilt set (e.g. just the dolly on the truck) must not count
    inline uint32_t expected_trailers = 0;
    // set only after disconnect_now actually dropped a joint. The detach waits
    // ~30 sim ticks after g_traffic 0, and during that window every trailer
    // still reads as coupled — without this flag the restore fires mid-delay
    // and the joint lands with live traffic again
    inline std::atomic< bool > traffic_detach_done{ false };

    //========================================================================
    // Sim-thread operation queue
    //
    // Connect/disconnect calls the game's own vfuncs and execute_line, which
    // must run on the simulation thread — but update()/the UI run on the
    // render thread, where calling them races the physics update (purecall
    // crashes). The buttons/keys only queue the operation; it executes inside
    // the hooked per-frame trailer call, which the game itself runs on the
    // sim thread.
    //========================================================================
    enum class pending_kind { none, connect, disconnect, restore };

    struct pending_op
    {
        pending_kind kind = pending_kind::none;
        void* trailer = nullptr;
        void* parent = nullptr;
        bool parent_is_truck = false;
        // sim ticks to wait before executing — used to let AI traffic despawn
        // after g_traffic 0 before dropping the joint
        uint32_t delay = 0;
    };

    inline pending_op pending;

    inline char pending_cmd[ 64 ] = {};
    inline std::atomic< bool > pending_cmd_queued{ false };

    inline void queue_cmd( const char* text )
    {
        std::strncpy( pending_cmd, text, sizeof( pending_cmd ) - 1 );
        pending_cmd[ sizeof( pending_cmd ) - 1 ] = '\0';
        pending_cmd_queued.store( true, std::memory_order_release );
    }

    inline bool queue_op( const pending_kind kind, void* trailer, void* parent,
        const bool parent_is_truck, const uint32_t delay = 0 )
    {
        if ( pending.kind != pending_kind::none )
        {
            CCore::g_instance->warning( "Outro engate/desengate ainda está em andamento" );
            return false;
        }
        pending = { kind, trailer, parent, parent_is_truck, delay };
        return true;
    }

    // Sim heartbeat: hk_crashes_when_disconnected bumps this every trailer
    // tick it runs. During the cargo cinematic the sim side is suspended
    // while update() keeps ticking — a stalled heartbeat is the signal that
    // queued work may safely run inline on the render thread instead of
    // waiting seconds for the sim to resume (hiding the respawned set's
    // brief re-assembly at the dock)
    inline uint64_t sim_heartbeat = 0;
    inline uint64_t sim_heartbeat_seen = 0;
    inline uint32_t sim_dead_frames = 0;
    inline uint64_t sim_thread_id = 0;

    inline void pump_sim_heartbeat()
    {
        sim_thread_id = GetCurrentThreadId();
        ++sim_heartbeat;
    }

    inline void watch_sim_heartbeat()
    {
        const uint64_t hb = sim_heartbeat;
        sim_dead_frames = ( hb == sim_heartbeat_seen ) ? sim_dead_frames + 1 : 0;
        sim_heartbeat_seen = hb;
    }

    // true when the sim side is demonstrably not running — either update()
    // itself is the thread that calls the trailer tick (single-threaded
    // engine), or it has been silent for a sustained stretch of frames
    inline bool sim_is_quiet()
    {
        return GetCurrentThreadId() == sim_thread_id || sim_dead_frames >= 30;
    }

    //========================================================================
    // World-space helpers — hook transforms, attach geometry
    //========================================================================

    // prism placement-ish { pos@0, pad, quat@0x10 } — what the 32-byte
    // transform getter (vfunc+0xF8) returns for a vehicle in world space
    struct hook_xform_t
    {
        float3_t pos;
        float pad;
        quat_t quat;
    };

    // world transform of a vehicle through the same getter the native
    // coupling flows use (vfunc+0xF8)
    inline bool hook_world( void* vehicle, hook_xform_t& out )
    {
        alignas( 16 ) uint8_t buf[ 0x20 ]{};
        // a cached trailer pointer can outlive its object (native attach /
        // respawn): its "vtable" then points at freed memory
        if ( !plausible_vfunc( vehicle, 0xF8 / 8 ) ) return false;
        void* p = nullptr;
        __try { p = vcall< prism::vehicle_get_transform_fn >( vehicle, 0xF8 / 8, buf ); }
        __except ( EXCEPTION_EXECUTE_HANDLER ) { return false; }
        if ( p == nullptr || !readable_ptr( p ) ) return false;
        std::memcpy( &out, p, sizeof( out ) );
        return std::isfinite( out.pos.x ) && std::isfinite( out.pos.y ) && std::isfinite( out.pos.z ) &&
            std::fabs( out.pos.x ) < 1e6f && std::fabs( out.pos.z ) < 1e6f;
    }

    // the truck's transform, the way the native coupling paths read it
    inline bool truck_xform( void* game_actor, uint8_t* out32 )
    {
        void* truck = game_actor != nullptr
            ? prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck ) : nullptr;
        if ( truck == nullptr ) return false;
        return vcall< prism::vehicle_get_transform_fn >( truck, 0xF8 / 8, out32 ) != nullptr;
    }

    //========================================================================
    // Detached-by-index state (respawn-proof)
    //
    // Cargo load/unload does not just move trailers — it DESTROYS the trailer
    // objects and spawns a fresh, already-attached set at the dock slots
    // (verified: the towed trailer's pointer changes across a load). Every
    // pointer-based mark dies with the destroyed object. The chain slot does
    // not: trailers[i] is rebuilt in the same order, so a slot-level mark can
    // detect the respawn — the fresh object shows up jointed without the
    // player ever asking for a connect — and restore_parked() then detaches
    // it again and teleports it back to the transform it was parked at.
    //========================================================================
    inline bool detached_idx[ max_trailers ] = {};
    inline hook_xform_t parked_xform[ max_trailers ] = {};
    inline bool parked_xform_valid[ max_trailers ] = {};
    // frames the marked slot has spent jointed — the restore only fires after
    // the cargo cinematic's own writes settle (~half a second)
    inline uint32_t rejoined_frames[ max_trailers ] = {};

    // chase camera (vehicle_behind_rotation_camera) — resolved once from the
    // camera manager. +0x3F0 is its anchor target; the native resolver stores
    // to it directly every frame, so we never write it — only read for the
    // change log below
    inline void* chase_camera = nullptr;

    // attach geometry (trailer_coupling.cpp) — the shell's "connect" actions
    // gate on the real hook-to-hook distance like the native T
    float hook_gap( void* trailer, void* parent, bool parent_is_truck );
    bool hook_far( void* trailer, void* parent, bool parent_is_truck );

    //========================================================================
    // Input helpers — keybind polling shared by the hotkey loops of every
    // feature file
    //========================================================================

    // key state for edge detection, shared with key_down below
    inline bool held[ settings::action_count ] = {};

    inline bool game_has_focus()
    {
        DWORD pid = 0;
        GetWindowThreadProcessId( GetForegroundWindow(), &pid );
        return pid == GetCurrentProcessId();
    }

    // true while the bound key is down; first_press only on the frame it went down
    inline bool key_down( const settings::Action action, bool* first_press = nullptr )
    {
        const UINT packed = settings::key( action );
        bool down = false;
        if ( packed != 0 )
        {
            if ( packed & settings::KEY_JOYSTICK )
            {
                down = settings::joystick_button_down( packed );
            }
            else
            {
                down = ( GetAsyncKeyState( static_cast< int >( packed & settings::KEY_CODE_MASK ) ) & 0x8000 ) != 0;
                if ( down )
                {
                    if ( ( packed & settings::KEY_MOD_CTRL )  && !( GetAsyncKeyState( VK_CONTROL ) & 0x8000 ) ) down = false;
                    if ( ( packed & settings::KEY_MOD_SHIFT ) && !( GetAsyncKeyState( VK_SHIFT )   & 0x8000 ) ) down = false;
                    if ( ( packed & settings::KEY_MOD_ALT )   && !( GetAsyncKeyState( VK_MENU )    & 0x8000 ) ) down = false;
                }
            }
        }
        bool& was = held[ static_cast< int >( action ) ];
        if ( first_press != nullptr ) *first_press = down && !was;
        was = down;
        return down;
    }

    //========================================================================
    // UI helpers shared by the per-feature renderers
    //========================================================================
    namespace ui
    {
        // The game's framebuffer is sRGB-encoded: a style color is treated as
        // linear by the pipeline and gamma-encoded on the way out, which washes
        // every color toward white. Feeding colors already linearized
        // (srgb^2.2) makes them land on screen at the intended hex value.
        inline ImVec4 pdv( const uint32_t hex )
        {
            const auto lin = []( const uint32_t c )
            {
                return std::pow( ( c & 0xFF ) / 255.f, 2.2f );
            };
            return ImVec4( lin( hex >> 16 ), lin( hex >> 8 ), lin( hex ), 1.f );
        }

        inline ImU32 pdv32( const uint32_t hex )
        {
            const auto lin = []( const uint32_t c )
            {
                return static_cast< int >( std::pow( ( c & 0xFF ) / 255.f, 2.2f ) * 255.f + 0.5f );
            };
            return IM_COL32( lin( hex >> 16 ), lin( hex >> 8 ), lin( hex ), 255 );
        }

        // On/off pill switch drawn with the draw list (ImGui has no native one).
        inline bool toggle_switch( const char* id, bool* v )
        {
            const float h = ImGui::GetFrameHeight();
            const float w = h * 1.9f;
            ImGui::InvisibleButton( id, ImVec2( w, h ) );
            const bool changed = ImGui::IsItemClicked();
            if ( changed ) *v = !*v;
            auto* dl = ImGui::GetWindowDrawList();
            const ImVec2 p = ImGui::GetItemRectMin();
            const float r = h * 0.5f;
            const ImU32 bg = *v ? pdv32( 0x27AE60 ) : pdv32( 0x656565 );
            dl->AddRectFilled( p, ImVec2( p.x + w, p.y + h ), bg, r );
            const float kx = *v ? p.x + w - r : p.x + r;
            dl->AddCircleFilled( ImVec2( kx, p.y + r ), r - 3.5f, pdv32( 0xF5F5F5 ) );
            return changed;
        }

        // Moves the cursor so the next widget of `width` hugs the row's right edge.
        inline void right_side( const float width )
        {
            ImGui::SameLine( 0.f, 0.f );
            ImGui::SetCursorPosX( ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - width );
        }

        // Disabled-colored paragraph that re-wraps to the window width every
        // frame — ImGui::TextDisabled renders a single clipped line
        inline void text_disabled_wrapped( const char* text )
        {
            ImGui::PushStyleColor( ImGuiCol_Text,
                ImGui::GetStyleColorVec4( ImGuiCol_TextDisabled ) );
            ImGui::TextWrapped( "%s", text );
            ImGui::PopStyleColor();
        }
    }

    //========================================================================
    // Feature entry points — defined in their own files, called by the
    // window shell's init()/update()/dtor
    //========================================================================
    void lift_register_channels();               // trailer_axles.cpp — reverse/gear telemetry
    void lift_axles_update( void* game_actor );  // trailer_axles.cpp — reverse assist + mask loop
    void release_all_axles();                    // trailer_axles.cpp — restores native flags (dtor)
    void brake_register_hooks();                 // trailer_brake.cpp — setter + wheel-solver hooks
    void brake_boost_update();                   // trailer_brake.cpp — +0x760 boost write + mask
    void steering_install();                     // trailer_steering.cpp — pattern + hook
    void steering_reset();                       // trailer_steering.cpp — clears lock state (dtor)
    void air_register_channels();                // trailer_air.cpp — psi/rpm telemetry channels
    void update_air( float dt );                 // trailer_air.cpp — closed-loop fill boost
    bool coupling_install();                     // trailer_coupling.cpp — hooks + native fns; false = indisponível
    void coupling_parked_update();               // trailer_loadstate.cpp — cargo respawn watch + rig restore
    void render_cargo_restore_option();          // trailer_loadstate.cpp — checkbox + setting persistence
    extern void* attach_target;                  // trailer_coupling.cpp — trailer picked for the native T attach
    extern void* canon[ max_trailers ];          // trailer_coupling.cpp — stable canonical trailer order
    int canon_index_of( const void* t );         // trailer_coupling.cpp — stable canon slot of a trailer (-1)
    void release_detach_marks( void* t );        // trailer_coupling.cpp — clear detached/plugin-detached/idx marks
    void* canon_coupled_predecessor( void* t);   // trailer_coupling.cpp — canonical predecessor if it is coupled
    bool trailer_is_live( const void* t );       // trailer_coupling.cpp — joint chain bottoms out at the truck
    void light_watch_tick();                     // trailer_lines.cpp — parked-trailer lamp mask/source pin
}
