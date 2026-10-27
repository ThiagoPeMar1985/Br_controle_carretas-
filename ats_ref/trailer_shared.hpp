#pragma once
// Shared trailer-chain state and helpers for the usa_trailer_controls
// feature files (coupling/cables/parked lock, brake bias, axle lift,
// steering, air compressor). Everything here was file-local in the old
// single-file layout — it is `inline` now only so the split TUs can reach
// the same objects.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "imgui.h"

#include "common/scssdk_telemetry_truck_common_channels.h"

#include "core.hpp"
#include "hooks/function_hook.hpp"
#include "memory/memory_utils.hpp"
#include "prism/common.hpp"
#include "prism/functions.hpp"
#include "prism/offsets.hpp"
#include "settings/settings.hpp"

#include <winver.h>
#pragma comment( lib, "version.lib" )

namespace ts_extra_utilities
{
    constexpr uint32_t max_trailers = 20;

    // Every get_address_from_offset / base+RVA constant in this plugin was
    // derived for amtrucks.exe 1.61.3.18 — on any other build a wrong VA
    // hooks random code, so the fixed-offset paths stay off unless the exe
    // file version matches exactly.
    inline bool expected_build()
    {
        static const int8_t verdict = []() -> int8_t {
            wchar_t exe_path[ MAX_PATH ] = {};
            if ( GetModuleFileNameW( nullptr, exe_path, MAX_PATH ) == 0 ) return -1;
            DWORD unused = 0;
            const DWORD size = GetFileVersionInfoSizeW( exe_path, &unused );
            if ( size == 0 ) return -1;
            std::vector< uint8_t > buf( size );
            if ( !GetFileVersionInfoW( exe_path, 0, size, buf.data() ) ) return -1;
            VS_FIXEDFILEINFO* info = nullptr;
            UINT len = 0;
            if ( !VerQueryValueW( buf.data(), L"\\",
                    reinterpret_cast< LPVOID* >( &info ), &len ) || info == nullptr ) return -1;
            const uint64_t v =
                ( static_cast< uint64_t >( info->dwFileVersionMS ) << 32 ) | info->dwFileVersionLS;
            constexpr uint64_t want =
                ( uint64_t( 1 ) << 48 ) | ( uint64_t( 61 ) << 32 ) | ( uint64_t( 3 ) << 16 ) | 18;
            if ( v != want && CCore::g_instance != nullptr )
            {
                CCore::g_instance->error( "Unsupported ATS build (expected 1.61.3.18) — fixed-offset features disabled" );
            }
            return v == want ? 1 : 0;
        }();
        return verdict == 1;
    }

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

    template < typename F, typename... A >
    auto vcall( void* object, const uint32_t slot, A&&... args )
    {
        auto* vtbl = *reinterpret_cast< void*** >( object );
        return reinterpret_cast< F* >( vtbl[ slot ] )( object, std::forward< A >( args )... );
    }

    // fed by update() every frame — the coupling file's freeze watchdog
    // writes a minidump when it stalls for >5s
    inline std::atomic< uint64_t > watchdog_heartbeat{ 0 };

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
    inline std::shared_ptr< CFunctionHook > crashes_when_disconnected_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > connect_slave_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > brake_chain_setter_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > brake_setter_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > svc_brake_chain_hook = nullptr; // unused: +0x730 is a dead demand mirror
    inline std::shared_ptr< CFunctionHook > svc_brake_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > forced_chain_setter_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > forced_setter_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > wheel_brake_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > pre_detach_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > detach_dispatch_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > attach_dispatch_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > trailer_state_apply_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > trailer_teleport_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > actor_teleport_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > lane_obstacle_insert_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > purecall_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > exit_process_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > terminate_process_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > combo_length_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > joint_walk_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > phys_link_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > apply_cables_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > rebuild_lines_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > set_lines_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > cam_set_target_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > cam_update_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > set_individual_steering_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > lamp_mask_hook = nullptr;

    //========================================================================
    // Runtime state
    //========================================================================

    // the current trailer chain, refreshed by update() every frame on the
    // render thread
    inline void* trailers[ max_trailers ] = {};
    inline uint32_t trailer_count = 0;

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
    // the user's chosen attach target: "connect to truck" no longer performs
    // the attach itself — it marks a parked trailer as the trailer-of-
    // interest and pins game_actor+0xC8 on it, so the native T key performs
    // the entire attach (settle, joint, cables, lights, brakes) on THAT
    // trailer when the player backs under it
    inline void* attach_target = nullptr;

    // canonical order: the order each trailer was first enumerated in. When a
    // trailer is promoted to the fifth wheel the list rotates so it leads —
    // detach puts the list back to canonical order
    inline void* canon[ max_trailers ] = {};

    inline void track_canon( void* trailer )
    {
        for ( void* s : canon ) if ( s == trailer ) return;
        for ( void*& s : canon ) if ( s == nullptr ) { s = trailer; return; }
    }

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
            CCore::g_instance->warning( "Another attach/detach is still in progress" );
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
        // half-built objects during load/restore have a garbage or missing
        // vtable — readable_ptr is not enough, validate the slot points at
        // real code before calling through it (crashed the load otherwise)
        if ( !plausible_vfunc( vehicle, 0xF8 / 8 ) ) return false;
        alignas( 16 ) uint8_t buf[ 0x20 ]{};
        void* p = vcall< prism::vehicle_get_transform_fn >( vehicle, 0xF8 / 8, buf );
        if ( p == nullptr ) return false;
        std::memcpy( &out, p, sizeof( out ) );
        return std::isfinite( out.pos.x ) && std::isfinite( out.pos.y ) && std::isfinite( out.pos.z ) &&
            std::fabs( out.pos.x ) < 1e6f && std::fabs( out.pos.z ) < 1e6f;
    }

    // the truck's transform, the way the native coupling paths read it
    inline bool truck_xform( void* game_actor, uint8_t* out32 )
    {
        void* truck = game_actor != nullptr
            ? prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck ) : nullptr;
        if ( truck == nullptr || !plausible_vfunc( truck, 0xF8 / 8 ) ) return false;
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

    // emulated parked-state lock for trailers the native factory (+0x1188
    // record) cannot handle: pose snapshot at joint-drop, re-teleported on
    // drift while jointless — canon-indexed like the respawn marks
    inline hook_xform_t anchor_xform[ max_trailers ] = {};
    inline bool anchor_valid[ max_trailers ] = {};
    // the pose captured at anchor creation — the settle phase lets the
    // tracked pose follow a tipping trailer, but a free-rolling piece on a
    // grade could wander indefinitely; anchor_home bounds that wander
    inline hook_xform_t anchor_home[ max_trailers ] = {};
    // settle bookkeeping for the emulated anchor: a fresh detach is still
    // tipping/sliding, so the anchor follows the pose until the trailer sits
    // still (~30 frames, 4 s cap). anchor_settling doubles as the brake-pin
    // gate — wheels must stay free enough to roll into equilibrium while a
    // top-heavy piece (the single-axle dolly) finds its rest pose
    inline int anchor_settle[ max_trailers ] = {};
    inline int anchor_age[ max_trailers ] = {};
    inline bool anchor_settling[ max_trailers ] = {};
    // jointless-grace counter: the anchor must not capture during the first
    // frames after joint-drop — load/assembly sequences hold trailers
    // jointless while the game repositions them through staging poses like
    // (0,0,-512), and anchoring one of those teleports the trailer into the
    // void (the "trailer buried underground" load bug)
    inline int anchor_delay[ max_trailers ] = {};
    // canon slots we already ran the parked-record factory on — the record
    // persists for the trailer's lifetime, so never reset on re-joint
    inline bool record_tried[ max_trailers ] = {};

    // frames a detached_idx-marked slot spent jointed again — fires the
    // restore once a cargo respawn settles (the objects come back attached)
    inline uint32_t rejoined_frames[ max_trailers ] = {};

    inline int index_of( const void* trailer )
    {
        for ( uint32_t i = 0; i < trailer_count; ++i )
            if ( trailers[ i ] == trailer ) return static_cast< int >( i );
        return -1;
    }

    // canon-indexed variant: trailers[] rotates under promotion, so slot
    // marks (detached_idx, parked_xform) must key on the stable canon order
    // or a reorder would land a mark on the wrong trailer
    inline int canon_index_of( const void* trailer )
    {
        for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
            if ( canon[ i ] == trailer ) return static_cast< int >( i );
        return -1;
    }

    // every trailer pointer we cache is only valid for the world that
    // produced it — a save load frees the whole set and hands out new
    // objects. The first canon entry that stops being a plausible object
    // means the world reloaded: drop every cached pointer and mark so
    // nothing stale reaches a native call (the save-load attach crash —
    // a dead canon entry walked into the dispatcher's own structures)
    inline void reset_trailer_state()
    {
        for ( void*& t : canon ) t = nullptr;
        for ( void*& t : trailers ) t = nullptr;
        for ( void*& d : detached_trailers ) d = nullptr;
        for ( void*& d : plugin_detached ) d = nullptr;
        for ( uint32_t i = 0; i < max_trailers; ++i )
        {
            detached_idx[ i ] = false;
            parked_xform_valid[ i ] = false;
            anchor_valid[ i ] = false;
            anchor_settle[ i ] = 0;
            anchor_age[ i ] = 0;
            anchor_settling[ i ] = false;
            anchor_delay[ i ] = 0;
            record_tried[ i ] = false;
        }
        trailer_count = 0;
        attach_target = nullptr;
        truck_towed = nullptr;
    }

    inline bool canon_dead()
    {
        for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
        {
            void* t = canon[ i ];
            // deeper probe than plausible_object: a respawned-away trailer
            // can keep a readable vtable page while sitting at the end of a
            // committed region — the first deep field read (0x1080..0x1198)
            // AVs, and a poisoned vtable kills any vfunc we call
            if ( !plausible_vfunc( t, 0xF8 / 8 ) ||
                 readable_span( t, 0x1200 ) < 0x1200 )
            {
                return true;
            }
        }
        return false;
    }

    // the resolved chase camera object — coupling owns it; the window dtor
    // clears it
    inline void* chase_camera = nullptr;

    // set only after disconnect_now actually dropped a joint. The detach waits
    // ~30 sim ticks after g_traffic 0, and during that window every trailer
    // still reads as coupled — without this flag the restore fires mid-delay
    // and the joint lands with live traffic again
    inline std::atomic< bool > traffic_detach_done{ false };

    // attach geometry (trailer_coupling.cpp) — the shell's "connect" actions
    // use them to refuse impossible attaches like the native T does
    float hook_gap( void* trailer, void* parent, bool parent_is_truck );
    bool hook_far( void* trailer, void* parent, bool parent_is_truck );

    //========================================================================
    // Input edge detection
    //========================================================================

    // key state for edge detection, shared with key_down below — one slot
    // per capture id (Actions + per-trailer toggle/truck binds)
    inline bool held[ settings::cap_count ] = {};

    inline bool game_has_focus()
    {
        DWORD pid = 0;
        GetWindowThreadProcessId( GetForegroundWindow(), &pid );
        return pid == GetCurrentProcessId();
    }

    // true while the bound key is down; first_press only on the frame it
    // went down. held_slot is the capture id whose held[] cell tracks edges
    inline bool key_down_packed( const UINT packed, const int held_slot, bool* first_press = nullptr )
    {
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
        bool& was = held[ held_slot ];
        if ( first_press != nullptr ) *first_press = down && !was;
        was = down;
        return down;
    }

    inline bool key_down( const settings::Action action, bool* first_press = nullptr )
    {
        return key_down_packed( settings::key( action ), static_cast< int >( action ), first_press );
    }

    // per-trailer attach bind — slot = trailer index, to_truck picks the
    // fifth-wheel bind over the chain-parent one
    inline bool trailer_key_down( const int slot, const bool to_truck, bool* first_press = nullptr )
    {
        const int cap = ( to_truck ? settings::truck_cap : settings::toggle_cap ) + slot;
        return key_down_packed( settings::trailer_key( slot, to_truck ), cap, first_press );
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
    // window shell's init()/update_impl()
    //========================================================================
    void crash_probes_install();                 // trailer_coupling.cpp
    bool coupling_install();                     // hooks + native fns; false = no attach/detach this build
    void coupling_parked_update();               // cargo respawn watch + rig restore countdown
    void light_probe_tick();                     // trailer_coupling.cpp — brake-edge snapshot/diff probe
    void light_watch_tick();                     // trailer_coupling.cpp — lamp-mask/source watcher
    void unhide_detached();                      // trailer_coupling.cpp — dtor + after cada fluxo
    void brake_register_hooks();                 // trailer_brake.cpp
    void brake_boost_update();                   // trailer_brake.cpp — +0x760 boost write
    void steering_install();                     // trailer_steering.cpp — pattern + hook
    void steering_reset();                       // trailer_steering.cpp — clears lock state (dtor)
    void lift_register_channels();               // trailer_axles.cpp — reverse/gear telemetry
    void lift_axles_update( void* game_actor );  // trailer_axles.cpp — reverse assist + mask loop
    void release_all_axles();                    // trailer_axles.cpp — restores native flags (dtor)
    void air_register_channels();                // trailer_air.cpp — pressure/rpm telemetry
    void update_air( float dt );                 // trailer_air.cpp — compressor boost loop
}
