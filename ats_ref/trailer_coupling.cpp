// Coupling/decoupling, cable state, parked lock, chain and
// camera maintenance — split out of trailer_manipulation.cpp.

#include "trailer_manipulation.hpp"
#include "trailer_shared.hpp"

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
    // crash->dump plumbing: the VEH records the exception into statics and
    // signals the watchdog thread, which has a healthy stack and writes the
    // minidump with the exception context — a stack overflow leaves almost
    // no stack on the faulting thread, so the dump must happen elsewhere
    CONTEXT veh_ctx{};
    EXCEPTION_RECORD veh_rec{};
    DWORD veh_tid = 0;
    std::atomic< bool > veh_exc_pending{ false };
    HANDLE veh_dump_event = nullptr;
    std::atomic< bool > veh_dump_done{ false };

    // vectored crash probe: the SCS crash logger misses whole classes of
    // death — stack overflows, fastfails, terminate paths — which is why the
    // silent attach/detach crashes never produced a game.crash.txt. This
    // handler runs BEFORE every other handler, dumps the exception to its
    // own file with bare WinAPI (minimal stack — safe even on an overflowed
    // stack), then passes the exception on untouched so the game still
    // writes its own crash log when it can
    LONG CALLBACK veh_crash_probe( EXCEPTION_POINTERS* ep )
    {
        if ( ep == nullptr || ep->ExceptionRecord == nullptr )
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        // stash the first serious exception for the watchdog's dump — skip
        // the benign thread-naming raise (0x406D1388) the runtime fires
        if ( ep->ExceptionRecord->ExceptionCode != 0x406D1388 &&
            !veh_exc_pending.exchange( true, std::memory_order_acq_rel ) )
        {
            veh_rec = *ep->ExceptionRecord;
            if ( ep->ContextRecord != nullptr ) veh_ctx = *ep->ContextRecord;
            veh_tid = GetCurrentThreadId();
            if ( veh_dump_event != nullptr ) SetEvent( veh_dump_event );
        }
        const auto* rec = ep->ExceptionRecord;
        const auto* ctx = ep->ContextRecord;
        HMODULE hm = nullptr;
        GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast< LPCSTR >( rec->ExceptionAddress ), &hm );
        char modname[ 128 ] = {};
        GetModuleFileNameA( hm, modname, sizeof( modname ) );
        const char* base_name = modname;
        for ( const char* p = modname; *p != 0; ++p )
            if ( *p == '\\' || *p == '/' ) base_name = p + 1;
        char buf[ 640 ];
        const int n = _snprintf( buf, sizeof( buf ) - 1,
            "code=0x%08lX addr=%p (%s+0x%llX) rip=%p rsp=%p rcx=%p rdx=%p r8=%p r9=%p\r\n",
            static_cast< unsigned long >( rec->ExceptionCode ),
            rec->ExceptionAddress, base_name,
            hm != nullptr ? static_cast< unsigned long long >(
                reinterpret_cast< uintptr_t >( rec->ExceptionAddress ) - reinterpret_cast< uintptr_t >( hm ) ) : 0ull,
            ctx != nullptr ? reinterpret_cast< void* >( ctx->Rip ) : nullptr,
            ctx != nullptr ? reinterpret_cast< void* >( ctx->Rsp ) : nullptr,
            ctx != nullptr ? reinterpret_cast< void* >( ctx->Rcx ) : nullptr,
            ctx != nullptr ? reinterpret_cast< void* >( ctx->Rdx ) : nullptr,
            ctx != nullptr ? reinterpret_cast< void* >( ctx->R8 ) : nullptr,
            ctx != nullptr ? reinterpret_cast< void* >( ctx->R9 ) : nullptr );
        if ( n > 0 )
        {
            char path[ MAX_PATH ] = {};
            ExpandEnvironmentStringsA(
                "%USERPROFILE%\\Documents\\American Truck Simulator\\usa_trailer_veh.log",
                path, sizeof( path ) );
            const HANDLE f = CreateFileA( path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr );
            if ( f != INVALID_HANDLE_VALUE )
            {
                DWORD w = 0;
                WriteFile( f, buf, static_cast< DWORD >( n ), &w, nullptr );
                // raw stack scan: a stack overflow leaves no room for a real
                // unwind, but the faulting thread's frames still sit in
                // memory — every qword pointing into the exe image is a
                // return address, and the repeating run IS the recursion.
                // Only pointer reads and one WriteFile per line — safe on
                // the remaining sliver of stack
                if ( ctx != nullptr && rec->ExceptionCode != 0x406D1388 )
                {
                    const uintptr_t img = reinterpret_cast< uintptr_t >( GetModuleHandle( nullptr ) );
                    const uintptr_t top = __readgsqword( 0x08 ); // TIB StackBase
                    uintptr_t p = static_cast< uintptr_t >( ctx->Rsp ) & ~uintptr_t( 7 );
                    char line[ 96 ];
                    int out = 0;
                    for ( ; p + 8 <= top && out < 64; p += 8 )
                    {
                        const uintptr_t v = *reinterpret_cast< const uintptr_t* >( p );
                        if ( v >= img && v < img + 0x2000000 )
                        {
                            const int m = _snprintf( line, sizeof( line ) - 1,
                                "  stk+%06llX = exe+0x%llX\r\n",
                                static_cast< unsigned long long >( p - ctx->Rsp ),
                                static_cast< unsigned long long >( v - img ) );
                            if ( m > 0 ) { WriteFile( f, line, m, &w, nullptr ); ++out; }
                        }
                    }
                }
                CloseHandle( f );
            }
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // freeze watchdog: a hard hang produces no exception and no crash log —
    // the only way to see where a native walker deadlocked is a dump of the
    // live process. update() feeds a heartbeat every frame; if it stalls
    // for >5s the watchdog writes a minidump with every thread's stack to
    // usa_trailer_freeze.dmp, then re-arms a minute later (loading screens
    // legitimately stall the main thread, so a single stall may be noise)

    void write_minidump( const char* name, const bool with_exception )
    {
        // MiniDumpWriteDump resolved lazily — dbghelp.h can't be included
        // this early in the TU; MiniDumpNormal=0, WithThreadInfo=0x1000,
        // WithHandleData=0x4
        char full[ MAX_PATH ] = {};
        ExpandEnvironmentStringsA(
            "%USERPROFILE%\\Documents\\American Truck Simulator\\", full, sizeof( full ) );
        strncat( full, name, sizeof( full ) - strlen( full ) - 1 );
        const HANDLE f = CreateFileA( full, GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr );
        if ( f == INVALID_HANDLE_VALUE ) return;
        using write_fn = BOOL( WINAPI* )( HANDLE, DWORD, HANDLE, int, void*, void*, void* );
        if ( const HMODULE dh = LoadLibraryA( "dbghelp.dll" ) )
        {
            if ( const auto write = reinterpret_cast< write_fn >( GetProcAddress( dh, "MiniDumpWriteDump" ) ) )
            {
                if ( with_exception )
                {
                    struct { DWORD tid; void* ep; int client; } exi{ veh_tid, nullptr, 1 };
                    struct { void* rec; void* ctx; } eps{ &veh_rec, &veh_ctx };
                    exi.ep = &eps;
                    write( GetCurrentProcess(), GetCurrentProcessId(), f, 0x1004,
                        &exi, nullptr, nullptr );
                }
                else
                {
                    write( GetCurrentProcess(), GetCurrentProcessId(), f, 0x1004,
                        nullptr, nullptr, nullptr );
                }
            }
        }
        CloseHandle( f );
    }

    DWORD WINAPI freeze_watchdog( void* )
    {
        for ( ;; )
        {
            const DWORD wr = veh_dump_event != nullptr
                ? WaitForSingleObject( veh_dump_event, 500 ) : ( Sleep( 500 ), WAIT_TIMEOUT );
            if ( wr == WAIT_OBJECT_0 && veh_exc_pending.load( std::memory_order_acquire ) &&
                !veh_dump_done.exchange( true, std::memory_order_acq_rel ) )
            {
                write_minidump( "usa_trailer_crash.dmp", true );
                CCore::g_instance->warning( "watchdog: wrote usa_trailer_crash.dmp (exception 0x{:X})",
                    veh_rec.ExceptionCode );
                continue;
            }
            const uint64_t hb = watchdog_heartbeat.load( std::memory_order_relaxed );
            if ( hb == 0 || GetTickCount64() - hb < 5000 ) continue;
            write_minidump( "usa_trailer_freeze.dmp", false );
            CCore::g_instance->warning( "watchdog: main thread stalled 5s — wrote usa_trailer_freeze.dmp" );
            Sleep( 60000 );
            watchdog_heartbeat.store( GetTickCount64(), std::memory_order_relaxed );
        }
        return 0;
    }

    // process-kill probes: a silent exit (no exception, no crash log) means
    // something decided the state was fatal and called for termination —
    // the return address of the call is the verdict signer
    using exit_process_fn = void __stdcall( uint32_t );
    using terminate_process_fn = int __stdcall( void*, uint32_t );
    void __stdcall hk_ExitProcess( uint32_t code )
    {
        CCore::g_instance->warning( "ExitProcess({}) called from exe+0x{:X}",
            code, reinterpret_cast< uint64_t >( _ReturnAddress() ) -
                reinterpret_cast< uint64_t >( GetModuleHandle( nullptr ) ) );
        exit_process_hook->get_original< exit_process_fn >()( code );
    }

    int __stdcall hk_TerminateProcess( void* process, uint32_t code )
    {
        if ( process == GetCurrentProcess() )
        {
            CCore::g_instance->warning( "TerminateProcess(self,{}) called from exe+0x{:X}",
                code, reinterpret_cast< uint64_t >( _ReturnAddress() ) -
                    reinterpret_cast< uint64_t >( GetModuleHandle( nullptr ) ) );
        }
        return terminate_process_hook->get_original< terminate_process_fn >()( process, code );
    }


    // native helpers replayed around connect/disconnect so a manually handled
    // trailer gets the same treatment as the game's own T-key flow
    prism::physics_trailer_u_get_slave_hook_position_fn* get_slave_hook_position_fn = nullptr;
    prism::vehicle_set_towed_trailer_fn* set_towed_trailer_fn = nullptr;
    // actually the vehicle coupled-state component notify (0x140874010), the
    // same function the native T attach calls on both vehicles
    prism::vehicle_clear_detached_flag_fn* clear_detached_flag_fn = nullptr;
    prism::vehicle_notify_decoupled_fn* notify_decoupled_fn = nullptr;
    prism::vehicle_pre_detach_fn* pre_detach_fn = nullptr;
    prism::vehicle_refresh_accessories_fn* refresh_accessories_fn = nullptr;
    prism::vehicle_rebuild_lines_fn* rebuild_lines_fn = nullptr;
    prism::vehicle_distribute_towed_mass_fn* distribute_towed_mass_fn = nullptr;
    prism::game_actor_refresh_axle_lift_fn* refresh_axle_lift_fn = nullptr;

    // console execution (sim thread only — see pending_cmd)
    prism::console_execute_line_fn* execute_line_fn = nullptr;

    // traffic-obstacle machinery read out of the native detach body:
    // - the world/vehicle manager singleton (rbx in the native detach); its
    //   +0x2B30 subsystem gets vfunc+0x138 (pre) and +0x1E0 (post) notifies
    // - the parked-obstacle anchor update 0x140284250 (a locked transform
    //   push through the anchor's internal buffer, not a register)
    // - the type-2 anchor re-link the native detach runs between the notifies
    // - the game-state dword (== 2 while in game) gating the update
    prism::traffic_register_anchor_fn* register_anchor_fn = nullptr;
    void** traffic_manager_global = nullptr;
    uint32_t* traffic_game_state = nullptr;
    uint32_t* anchor_reg_guard = nullptr;
    using anchor_relink_fn = void __fastcall( void* /*registry*/, uint32_t /*type*/ );

    // chase camera (vehicle_behind_rotation_camera): the followed vehicle is
    // a refcounted pointer at camera+0x3F0, resolved at activation by walking
    // the trailer slave chain to its end. Our detach leaves the link alive
    // chase camera (vehicle_behind_rotation_camera) — resolved once from the
    // camera manager. +0x3F0 is its anchor target; the native resolver stores
    // to it directly every frame, so we never write it — only read for the
    // change log below
    using cam_set_target_fn = void( void* /*camera*/, void* /*target*/ );
    using cam_update_fn = void( void* /*camera*/, float /*dt*/ );   // vfunc 32, 0x140AC89A0
    using cam_recompute_fn = void( void* /*camera*/, uint8_t /*reset*/ );   // vfunc 14, 0x140AC7290
    anchor_relink_fn* relink_anchors_fn = nullptr;

    // g_traffic cvar access for the detach/restore traffic suppression
    prism::cvar_get_float_fn* get_cvar_float_fn = nullptr;
    void* g_traffic_cvar = nullptr;

    // the remaining native attach/detach calls, lifted from the T-key
    // dispatchers (attach 0x1405DCC20, detach 0x1405DD410):
    // - 0x1409FB700 settles the trailer on the hook before the connect vfunc,
    //   taking a cvar float (the one read at attach time, 0x142D48200)
    // - 0x1408771A0 pairs/unpairs the two vehicles' connector lists (the
    //   pneumatic/electric links — a trailer attached without it keeps its
    //   standalone spring brake, i.e. stays braked with a live joint)
    // - 0x1408762E0 applies the trailer-cables cvar; attach passes the trailer
    //   in rdx, detach passes null
    // - 0x1405DCBC0 refreshes the actor's +0x248 flags from the trailer chain
    using trailer_settle_fn = void __fastcall( void* /*trailer*/, float /*cvar_value*/ );
    using vehicle_pair_fn = void __fastcall( void* /*tower*/, void* /*towed*/ );
    using vehicle_cables_fn = void __fastcall( void* /*truck*/, void* /*trailer*/,
        int /*unused*/, int /*mode*/, int /*zero*/ );
    trailer_settle_fn* trailer_settle_fn_ptr = nullptr;
    vehicle_pair_fn* pair_vehicles_fn = nullptr;
    vehicle_cables_fn* apply_cables_fn = nullptr;
    // native line-state setter (exe+0x875410): fn(vehicle, state, force) —
    // state=0 tears down the trailer's +0xE98..+0xEA0 line entities and
    // clears the +0xE8D connected flag through the real destroy path
    using line_state_fn = void __fastcall( void* /*vehicle*/, uint8_t /*state*/, uint8_t /*force*/ );
    line_state_fn* set_lines_fn = nullptr;
    // parked-state record factory (0x1407B7DE0): builds the 0x198 record with
    // the trailer back-link at record+0x178 and registers it into the parked
    // manager's active list (the tail-call to 0x140C38BA0). Its internal
    // component vfunc (+0x78) purecalls on trailers that never sat parked —
    // hk_purecall swallows that and the record completes with +0x180 = null.
    // The caller still has to write the returned record into trailer+0x1188
    // itself — the factory only registers it
    using parked_record_factory_fn = void __fastcall( void* /*trailer*/ );
    parked_record_factory_fn* parked_record_factory = nullptr;
    // parked-record manager singleton: active records live in the
    // [mgr+0x740, mgr+0x748) pointer array
    void** parked_record_mgr = nullptr;
    using actor_flag_fixup = void __fastcall( void* /*game_actor*/ );
    actor_flag_fixup* actor_flag_fixup_fn = nullptr;
    using cvar_get_int_fn = int __fastcall( void* /*cvar*/ );
    cvar_get_int_fn* get_cvar_int_fn = nullptr;
    void* cables_mode_cvar = nullptr;
    void* attach_settle_cvar = nullptr;


    // the @@token@@ event the native attach/detach bodies post on the actor's
    // event bus ([game_actor+0x30]+0xD8): it raises the on-screen
    // coupling/decoupling banner and plays the gear-crank sound. Arg shape
    // lifted from the native call sites (attach tail 0x1405DD845, detach tail
    // 0x1405DDCD4): rcx = bus+0xD8, rdx = &name, r8 = const param block
    // (ETS2 0x142733198 / ATS 0x14273E748), r9d = 2,
    // stack tail {param_block, 2, 0, 2, -1, 0, &opts}
    using post_event_fn = void __fastcall( void* /*bus_d8*/, const char** /*name_slot*/,
        void* /*params*/, int /*two*/, void* /*s5_params*/, int /*s6*/, int /*s7*/,
        int /*s8*/, int /*s9*/, void* /*s10*/, void* /*s11*/ );
    post_event_fn* post_event_fn_ptr = nullptr;
    void* event_param_block = nullptr;
    const char* event_name_coupling = nullptr;
    const char* event_name_decoupling = nullptr;

    // the landing-gear animation player the native attach/detach run on the
    // trailer (0x140636F50): builds/plays trailer+0x1260 whose soundref is the
    // trailer_brace_down/up_sound — i.e. the gear-crank noise. dl=1 lowers
    // (detach), dl=0 raises (attach), xmm2 is the anim speed (native: 1/3)
    using trailer_brace_anim_fn = void __fastcall( void* /*trailer*/, uint8_t /*down*/, float /*speed*/ );
    trailer_brace_anim_fn* brace_anim_fn = nullptr;

    // the native attach dispatcher itself (0x1405DCC20, a game_actor method):
    // the same function the T key calls — it runs the complete native attach
    // sequence (hook settle, connect vfunc, towed registration, connector
    // pairing, cables, anchors) which our manual replica could never fully
    // reproduce. It takes the trailer to attach from game_actor+0xC8
    // real signature is SIX params: the settle-distance float* sits at stack
    // arg6 ([rbp+0x260], null-checked -> cvar default). Declaring fewer args
    // leaves that slot holding stack garbage — the dispatcher dereferenced
    // it with movss and crashed (amtrucks+0x5DE07E)
    using native_attach_dispatch_fn = void __fastcall( void* /*game_actor*/, uint8_t /*dl*/,
        uint64_t /*r8*/, uint64_t /*r9*/, uint64_t /*arg5*/, const float* /*settle_override*/ );
    native_attach_dispatch_fn* native_attach_fn = nullptr;
    using native_detach_dispatch_fn = void __fastcall( void* /*game_actor*/, uint8_t /*dl*/, uint64_t /*r8*/ );
    using trailer_teleport_fn = void __fastcall( void* /*trailer*/, void* /*transform*/ );

    // g_traffic value saved before a forced "g_traffic 0" detach; restored once
    // the whole trailer set is jointed again. < 0 means nothing saved
    float saved_traffic = -1.f;
    // trailers the set had when traffic suppression started. The chain shrinks
    // when links leave it, so "all coupled" must compare against this — a
    // partially rebuilt set (e.g. just the dolly on the truck) must not count
    uint32_t expected_trailers = 0;
    // traffic_detach_done lives in trailer_shared.hpp — the shell's user
    // actions clear it when queueing a disconnect

    //========================================================================
    // Rig transform snapshot
    //
    // The cargo cinematic rotates the whole attached set — truck included —
    // into the dock slots through the actor teleport (0x1405DE1E0). That
    // hook snapshots the truck and every trailer slot right before the move;
    // after the sequence settles, restore_parked puts the rig back the way
    // the player parked it (backed-in stays backed-in). Only short moves
    // arm the restore — a teleport across the map is travel, not a dock
    // alignment
    //========================================================================
    bool rig_armed = false;
    uint32_t rig_restore_delay = 0;
    hook_xform_t rig_truck_xform{};
    bool rig_truck_valid = false;
    uint8_t rig_replay_a3 = 0;
    uint8_t rig_replay_a4 = 0;

    void snapshot_rig()
    {
        rig_truck_valid = false;
        if ( auto* game_actor = CCore::g_instance->get_game_actor() )
        {
            void* truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
            rig_truck_valid = truck != nullptr && hook_world( truck, rig_truck_xform );
        }
    }

    float3_t quat_rot( const quat_t& q, const float3_t& v )
    {
        // t = 2 * cross(q.xyz, v);  v' = v + w*t + cross(q.xyz, t)
        const float tx = 2.f * ( q.y * v.z - q.z * v.y );
        const float ty = 2.f * ( q.z * v.x - q.x * v.z );
        const float tz = 2.f * ( q.x * v.y - q.y * v.x );
        return float3_t{
            v.x + q.w * tx + q.y * tz - q.z * ty,
            v.y + q.w * ty + q.z * tx - q.x * tz,
            v.z + q.w * tz + q.x * ty - q.y * tx };
    }

    // attach offset in the parent's frame — the same vec passed to the
    // connect vfunc: rear hook point minus the parent's own hook locator
    float3_t attach_offset( void* parent, const bool parent_is_truck )
    {
        float3_t hook{};
        if ( parent_is_truck )
        {
            void* chassis = prism::offsets::field< void* >( parent, prism::offsets::vehicle_chassis_data );
            if ( chassis != nullptr )
            {
                hook = prism::offsets::field< float3_t >( chassis, prism::offsets::chassis_hook_position );
            }
        }
        else if ( get_slave_hook_position_fn != nullptr )
        {
            get_slave_hook_position_fn( static_cast< prism::physics_trailer_u* >( parent ), &hook );
        }
        const float3_t loc = prism::offsets::field< float3_t >( parent, prism::offsets::vehicle_hook_locator );
        return float3_t{ hook.x - loc.x, hook.y - loc.y, hook.z - loc.z };
    }

    // the native T only connects when the hooks nearly touch; our connect
    // vfunc teleports a far trailer onto the hook in one step — an instant
    // jump that, over a live traffic lane, leaves a component half-initialized
    // (purecall). Gate on real hook-to-hook distance: trailer front hook vs
    // parent rear hook in world space. The locator frame's sign convention
    // isn't fully nailed down, so both signed offsets are measured and the
    // nearer wins — attached-or-not, one of them is the true rear hook.
    // Returns < 0 when it cannot be measured.
    float hook_gap( void* trailer, void* parent, const bool parent_is_truck )
    {
        hook_xform_t th{}, ph{};
        if ( trailer == nullptr || parent == nullptr ||
            !hook_world( trailer, th ) || !hook_world( parent, ph ) )
        {
            return -1.f;
        }
        const float3_t v = attach_offset( parent, parent_is_truck );
        const float3_t off = quat_rot( ph.quat, v );
        const float dx = th.pos.x - ph.pos.x;
        const float dy = th.pos.y - ph.pos.y;
        const float dz = th.pos.z - ph.pos.z;
        const float g_plus = std::sqrt(
            ( dx - off.x ) * ( dx - off.x ) +
            ( dy - off.y ) * ( dy - off.y ) +
            ( dz - off.z ) * ( dz - off.z ) );
        const float g_minus = std::sqrt(
            ( dx + off.x ) * ( dx + off.x ) +
            ( dy + off.y ) * ( dy + off.y ) +
            ( dz + off.z ) * ( dz + off.z ) );
        return ( std::min )( g_plus, g_minus );
    }

    // refuse the connect when the hooks are farther apart than the native T
    // would allow — no teleporting the trailer across the map onto the hook
    bool hook_far( void* trailer, void* parent, const bool parent_is_truck )
    {
        const float gap = hook_gap( trailer, parent, parent_is_truck );
        if ( gap < 0.f ) return false; // cannot measure: keep the old fail-open
        CCore::g_instance->info( "Hook gap trailer {:p} to {:p}: {:.2f}m", trailer, parent, gap );
        // bitrem chains measure ~6-7 m at touching distance, long dolly +
        // trailer combos need ~12 — the limit is one value for every
        // layout, tuned through [connect] max_gap in settings.ini
        return gap > settings::g_settings.attach_max_gap;
    }

    //========================================================================
    // Trailer actor lookup and traffic-obstacle anchor update
    //========================================================================

    // trailer -> trailer-actor cache: populated whenever the slot scan
    // succeeds so the actor can still be reached after the trailer detaches
    // and falls out of game_actor's slots
    std::pair< void*, void* > trailer_actors[ max_trailers ] = {};

    // the native detach resolves the detached trailer's actor from the slot
    // array at game_actor+0x1D38 (0x28-stride entries, entry+0 = actor ptr).
    // The actor's backpointer to its trailer sits at an offset this build
    // doesn't document, so instead of guessing it the actor's own fields are
    // scanned for a pointer back to the trailer — self-discovering
    // the parked trailer's traffic-obstacle anchor is a component of the
    // trailer itself, not of the game_actor slot array — so the provider's
    // vtable is sampled from the head trailer's actor (the only slot the
    // native code ever fills) and then matched against the trailer's own
    // fields at detach time
    void* provider_vtbl = nullptr;

    void* find_trailer_actor( void* trailer, void* game_actor )
    {
        if ( trailer == nullptr || game_actor == nullptr ) return nullptr;
        for ( uint32_t s = 0; s < 16; ++s )
        {
            void* entry = reinterpret_cast< uint8_t* >( game_actor ) + 0x1D38 + s * 0x28;
            if ( !readable_ptr( reinterpret_cast< uint8_t* >( entry ) + 8 ) ) break;
            void* actor = *reinterpret_cast< void** >( entry );
            if ( !plausible_object( actor ) ) continue;
            const uintptr_t span = readable_span( actor, 0x400 );
            for ( uint32_t bo = 8; bo + 8 <= span; bo += 8 )
            {
                if ( prism::offsets::field< void* >( actor, bo ) == trailer )
                {
                    return actor;
                }
            }
        }
        // no backpointer anywhere: the trailer's position in the chain maps
        // to the same slot index — take it if the slot holds a real actor
        for ( uint32_t i = 0; i < trailer_count; ++i )
        {
            if ( trailers[ i ] != trailer ) continue;
            void* entry = reinterpret_cast< uint8_t* >( game_actor ) + 0x1D38 + i * 0x28;
            void* actor = *reinterpret_cast< void** >( entry );
            if ( plausible_object( actor ) )
            {
                return actor;
            }
            break;
        }
        // cache fallback: once a trailer leaves the convoy its actor drops out
        // of the game_actor slots entirely, so a detached trailer is only
        // findable through the pointer we cached while it was still coupled
        for ( uint32_t i = 0; i < max_trailers; ++i )
        {
            if ( trailer_actors[ i ].first == trailer )
            {
                void* cached = trailer_actors[ i ].second;
                return plausible_object( cached ) ? cached : nullptr;
            }
        }
        // last resort with a sampled vtable: find the actor-shaped object —
        // anything whose +0x1F0 field holds a provider of the right type —
        // inside the trailer itself and across the game_actor. Mid-chain
        // trailers keep their actor off the head-only slot array
        if ( provider_vtbl != nullptr )
        {
            auto actor_at = [ & ]( void* obj ) -> void*
            {
                if ( obj == nullptr || !plausible_object( obj ) ||
                    !readable_ptr( reinterpret_cast< uint8_t* >( obj ) + 0x1F0 ) )
                {
                    return nullptr;
                }
                void* prov = prism::offsets::field< void* >( obj, 0x1F0 );
                return prov != nullptr && plausible_object( prov ) &&
                    *reinterpret_cast< void** >( prov ) == provider_vtbl ? obj : nullptr;
            };
            for ( uintptr_t off = 0x100; off < 0x2000; off += 8 )
            {
                if ( !readable_ptr( reinterpret_cast< uint8_t* >( trailer ) + off ) ) continue;
                if ( void* a = actor_at( prism::offsets::field< void* >( trailer, off ) ) )
                {
                    return a;
                }
            }
            for ( uintptr_t off = 0x1C00; off < 0x3000; off += 8 )
            {
                if ( !readable_ptr( reinterpret_cast< uint8_t* >( game_actor ) + off ) ) continue;
                void* a = actor_at( prism::offsets::field< void* >( game_actor, off ) );
                if ( a == nullptr ) continue;
                return a;
            }
        }
        return nullptr;
    }

    void sample_provider_vtbl( void* game_actor )
    {
        if ( provider_vtbl != nullptr || game_actor == nullptr ) return;
        void* actor = prism::offsets::field< void* >( game_actor, 0x1D38 );
        if ( !plausible_object( actor ) ||
            !readable_ptr( reinterpret_cast< uint8_t* >( actor ) + 0x1F0 ) )
        {
            return;
        }
        void* provider = prism::offsets::field< void* >( actor, 0x1F0 );
        if ( provider != nullptr && plausible_object( provider ) &&
            plausible_vfunc( provider, 0x28 / 8 ) && plausible_vfunc( provider, 0x30 / 8 ) )
        {
            provider_vtbl = *reinterpret_cast< void** >( provider );
        }
    }

    // scans the trailer's own fields (and the parked-state object's) for a
    // component whose vtable matches the sampled provider — the parked
    // trailer carries its traffic-obstacle anchor inside itself
    void* find_trailer_provider( void* trailer )
    {
        if ( trailer == nullptr || provider_vtbl == nullptr ) return nullptr;
        auto scan = [ & ]( void* obj, uintptr_t end ) -> void*
        {
            if ( obj == nullptr ) return nullptr;
            for ( uintptr_t off = 0; off < end; off += 8 )
            {
                if ( !readable_ptr( reinterpret_cast< uint8_t* >( obj ) + off ) ) continue;
                void* cand = *reinterpret_cast< void** >( reinterpret_cast< uint8_t* >( obj ) + off );
                if ( cand != nullptr && plausible_object( cand ) &&
                    *reinterpret_cast< void** >( cand ) == provider_vtbl )
                {
                    return cand;
                }
            }
            return nullptr;
        };
        if ( void* p = scan( trailer, 0x1400 ) ) return p;
        return scan( prism::offsets::field< void* >( trailer, prism::offsets::trailer_parked_state ), 0x800 );
    }

    void cache_trailer_actor( void* trailer, void* game_actor )
    {
        if ( trailer == nullptr ) return;
        sample_provider_vtbl( game_actor );
        void* actor = find_trailer_actor( trailer, game_actor );
        for ( uint32_t i = 0; i < max_trailers; ++i )
        {
            if ( trailer_actors[ i ].first == trailer || trailer_actors[ i ].first == nullptr )
            {
                trailer_actors[ i ] = { trailer, actor };
                return;
            }
        }
    }

    // native attach/detach anchor update: provider -> vfunc+0x28 yields the
    // trailer's traffic-obstacle anchor, which then gets a transform pushed
    // through 0x140284250 (a locked update, not a register), followed by
    // provider vfunc+0x30. Runs only while the world is in state 2.
    void push_anchor_update( void* provider, const void* xform32 )
    {
        if ( provider == nullptr || register_anchor_fn == nullptr ||
            !plausible_vfunc( provider, 0x28 / 8 ) || !plausible_vfunc( provider, 0x30 / 8 ) )
        {
            return;
        }
        void* anchor = vcall< prism::provider_get_anchor_fn >( provider, 0x28 / 8 );
        // the register call writes the 32-byte transform through the anchor's
        // internal buffer — a wrong-typed provider yields a differently-shaped
        // object whose buffer pointer is garbage, so the write lands wild
        if ( anchor != nullptr && plausible_object( anchor ) &&
            traffic_game_state != nullptr && *traffic_game_state == 2 )
        {
            struct { void* p; uint32_t d; } ctx{};
            if ( anchor_reg_guard != nullptr ) ++*anchor_reg_guard;
            register_anchor_fn( anchor, xform32, &ctx );
            if ( anchor_reg_guard != nullptr ) --*anchor_reg_guard;
        }
        vcall< prism::vehicle_notify_decoupled_fn >( provider, 0x30 / 8 );
    }

    //========================================================================
    // Native flow replicas — sim thread only
    //========================================================================

    // plays the trailer's registered brace sound event directly. The audio
    // object sits at trailer+0x12D8 with channel slots +0x50 (brace_up) and
    // +0x58 (brace_down); each slot points at a 16-byte channel whose +8 is
    // the pooled sound-event object (vtable base+0x2220EE60, vfunc+0x18 =
    // "start" — it early-returns unless the event is in ready state 2, so a
    // wrong guess fails silent, not fatal)
    using sound_event_play_fn = void __fastcall( void* /*event*/, uint8_t /*flag*/ );
    void play_brace_sound( void* trailer, const bool up )
    {
        if ( !expected_build() ) return;
        const auto base = reinterpret_cast< uint64_t >( GetModuleHandle( nullptr ) );
        void* audio = readable_ptr( trailer ) ? prism::offsets::field< void* >( trailer, 0x12D8 ) : nullptr;
        if ( !readable_ptr( audio ) || !readable_ptr( reinterpret_cast< uint8_t* >( audio ) + 0x60 ) )
        {
            return;
        }
        void* channel = prism::offsets::field< void* >( audio, up ? 0x50 : 0x58 );
        void* event = nullptr;
        // tolerate both layouts: the slot may hold the event itself or a
        // 16-byte channel wrapper with the event at +8
        if ( readable_ptr( channel ) &&
            *reinterpret_cast< uint64_t* >( channel ) == base + 0x2220EE60 )
        {
            event = channel;
        }
        else if ( readable_ptr( channel ) &&
            readable_ptr( reinterpret_cast< uint8_t* >( channel ) + 0x10 ) )
        {
            void* inner = prism::offsets::field< void* >( channel, 0x8 );
            if ( readable_ptr( inner ) &&
                *reinterpret_cast< uint64_t* >( inner ) == base + 0x2220EE60 )
            {
                event = inner;
            }
        }
        if ( event != nullptr )
        {
            CCore::g_instance->info( "brace sound {} on trailer {:p}", up ? "up" : "down", trailer );
            vcall< sound_event_play_fn >( event, 0x18 / 8, uint8_t( 0 ) );
        }
    }

    // drives a native dispatcher on a chosen trailer: the trailer-of-interest
    // slot (+0xC8) is swapped in for the call — the same thing the game does
    // when the T key targets a trailer. The call goes through the hook's
    // ORIGINAL so our own guards don't run. Attach = real chain head (joint,
    // towed registration, cables); detach = real parked trailer (record,
    // obstacle, marker — the game keeps it visible itself)
    bool native_attach_via_slot( void* trailer )
    {
        auto* game_actor = CCore::g_instance->get_game_actor();
        if ( game_actor == nullptr || attach_dispatch_hook == nullptr || trailer == nullptr )
        {
            return false;
        }
        void* prev = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_trailer );
        prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_trailer ) = trailer;
        // arg4 null is legal — the dispatcher null-checks it and falls back
        // to the cvar default
        attach_dispatch_hook->get_original< native_attach_dispatch_fn >()( game_actor, 0, 0, 0, 0, nullptr );
        if ( joint( trailer ) != nullptr ) return true;
        prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_trailer ) = prev;
        return false;
    }

    bool native_detach_via_slot( void* trailer )
    {
        auto* game_actor = CCore::g_instance->get_game_actor();
        if ( game_actor == nullptr || detach_dispatch_hook == nullptr || trailer == nullptr )
        {
            return false;
        }
        void* prev = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_trailer );
        prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_trailer ) = trailer;
        // dl=1 matches the T-key call: full component/accessory detach notify
        detach_dispatch_hook->get_original< native_detach_dispatch_fn >()( game_actor, 1, 0 );
        if ( joint( trailer ) == nullptr ) return true;
        prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_trailer ) = prev;
        return false;
    }

    // the game hides a trailer piece it no longer owns while physics keeps
    // it alive. Rewrite it in place: re-register the parked obstacle and
    // re-anchor the actor at its own transform — the model reappears where
    // physics kept it
    void unhide_detached()
    {
        for ( uint32_t i = 0; i < trailer_count; ++i )
        {
            void* t = trailers[ i ];
            if ( t == nullptr || !readable_ptr( t ) || joint( t ) != nullptr ) continue;
            // a trailer holding a parked-state record (+0x1188) is parked by
            // the game itself: when a section member leaves the record stays
            // but the parked flags get torn down (+0x1198/+0x1258 cleared) —
            // a half-state the actor machine hides. Re-assert the flags so
            // the parked piece stays visible; the record owns the pose, so
            // no teleport is needed (the object xform may be stale anyway)
            if ( prism::offsets::field< void* >( t, prism::offsets::trailer_parked_state ) != nullptr )
            {
                if ( readable_ptr( reinterpret_cast< uint8_t* >( t ) + 0x1198 ) )
                {
                    prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked ) = 1;
                    prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked_registered ) = 1;
                }
                if ( plausible_vfunc( t, 0x328 / 8 ) )
                {
                    vcall< prism::physics_trailer_u_set_trailer_brace_fn >( t, 0x328 / 8, true );
                }
                continue;
            }
            if ( plausible_vfunc( t, 0x328 / 8 ) )
            {
                vcall< prism::physics_trailer_u_set_trailer_brace_fn >( t, 0x328 / 8, true );
            }
            hook_xform_t xf{};
            if ( trailer_teleport_hook != nullptr && hook_world( t, xf ) )
            {
                trailer_teleport_hook->get_original< trailer_teleport_fn >()( t, &xf );
            }
        }
    }

    // one-shot combination dump at each attach/detach boundary: roots, per-
    // trailer link state and the actor-slot ownership — the log then shows
    // exactly which pointer transition hid or dropped a trailer. SEH-wrapped:
    // workshop respawns leave canon pointers half-dead (readable_ptr passes,
    // an interior field read faults) and a diagnostic must never kill the game
    void dump_state_raw( const char* tag );
    void dump_state( const char* tag )
    {
        __try { dump_state_raw( tag ); }
        __except ( EXCEPTION_EXECUTE_HANDLER )
        {
            CCore::g_instance->warning( "dump {} faulted on stale canon", tag );
        }
    }

    // light-path probe: parked trailers chained under the live tail showed
    // live brake/turn lamps — the writer field is unknown. F9 snapshots every
    // canon trailer's first 0x1500 bytes; F9 again diffs and logs the changed
    // offsets, pinning whichever field the broadcast touches (brake pedal =
    // brake-light field, indicator = blinker field)
    inline uint8_t probe_snap[ max_trailers ][ 0x1500 ];
    inline void* probe_obj[ max_trailers ];
    inline uintptr_t probe_len[ max_trailers ];
    inline bool probe_armed = false;

    void light_probe_tick()
    {
        static bool prev = false;
        // Ctrl+F9 — plain F9 is the game's own devcam teleport key
        const bool down = ( GetAsyncKeyState( VK_F9 ) & 0x8000 ) != 0 &&
                          ( GetAsyncKeyState( VK_CONTROL ) & 0x8000 ) != 0;
        if ( !down || prev ) { prev = down; return; }
        prev = true;
        if ( !probe_armed )
        {
            for ( uint32_t i = 0; i < max_trailers; ++i )
            {
                void* t = canon[ i ];
                probe_obj[ i ] = t;
                probe_len[ i ] = 0;
                if ( t != nullptr && readable_ptr( t ) )
                {
                    const uintptr_t n = readable_span( t, 0x1500 );
                    probe_len[ i ] = n;
                    if ( n != 0 ) memcpy( probe_snap[ i ], t, n );
                }
            }
            probe_armed = true;
            CCore::g_instance->info( "light-probe: snapshot armed" );
            return;
        }
        probe_armed = false;
        for ( uint32_t i = 0; i < max_trailers; ++i )
        {
            void* t = canon[ i ];
            if ( t == nullptr || t != probe_obj[ i ] || !readable_ptr( t ) ) continue;
            const uintptr_t n = probe_len[ i ] < readable_span( t, 0x1500 )
                ? probe_len[ i ] : readable_span( t, 0x1500 );
            for ( uintptr_t o = 0; o + 4 <= n; o += 4 )
            {
                const uint32_t a = *reinterpret_cast< const uint32_t* >( probe_snap[ i ] + o );
                const uint32_t b = *reinterpret_cast< const uint32_t* >(
                    reinterpret_cast< const uint8_t* >( t ) + o );
                if ( a != b )
                {
                    CCore::g_instance->info(
                        "light-probe: c{} +0x{:03X} {:08X} -> {:08X}", i, o, a, b );
                }
            }
        }
        CCore::g_instance->info( "light-probe: diff done" );
    }

    // light-watch: the lamp update (exe 0x1405418FD) reads the SOURCE
    // vehicle's +0x2A0 lamp mask through [[veh+0x290]+0x110]+0x18 — nothing
    // is written on a parked trailer itself. This watcher diffs the mask
    // qwords of the truck and every canon trailer per tick and tracks the
    // source pointer, so each control (hazard/brake/headlight/strobe)
    // reveals which bits it flips and which vehicle a parked trailer's
    // lamps are mirroring
    bool trailer_is_live( void* t );

    inline void* light_source_of( void* veh )
    {
        if ( veh == nullptr || !readable_ptr( veh ) ) return nullptr;
        void* ctx = prism::offsets::field< void* >( veh, 0x110 );
        if ( ctx == nullptr || !readable_ptr( ctx ) ) return nullptr;
        return prism::offsets::field< void* >( ctx, 0x18 );
    }

    inline bool lw_valid = false;
    inline uint8_t lw_mask_truck[ 0x20 ];
    inline uint8_t lw_mask_prev[ max_trailers ][ 0x20 ];
    inline void* lw_src_prev[ max_trailers ];

    // the per-vehicle lamp update (0x140540D60) iterates the vehicle's OWN
    // component registry at veh+0x130 — an array of channel buckets (0x38
    // each): +8 = comp ptr array, +0x10 = count. A trailer's +0x110 light ctx
    // is null (lightwatch src=null even on live trailers), so trailer lamps
    // can only be driven while their components sit inside another vehicle's
    // registry — almost certainly the truck's. To darken a parked trailer its
    // components must leave the truck's lists; this scan reveals whether they
    // are there and which component field back-references the owner trailer
    void lamp_registry_scan( void* truck )
    {
        if ( truck == nullptr ||
             !readable_ptr( reinterpret_cast< uint8_t* >( truck ) + 0x140 ) ) return;
        void** arr = prism::offsets::field< void** >( truck, 0x138 );
        const uint64_t chn = prism::offsets::field< uint64_t >( truck, 0x140 );
        if ( arr == nullptr || !readable_ptr( arr ) || chn > 0x80 ) return;
        CCore::g_instance->info( "lampreg: truck channels={}", chn );
        for ( uint64_t c = 0; c < chn; ++c )
        {
            uint8_t* entry = reinterpret_cast< uint8_t* >( arr ) + c * 0x38;
            void** begin = *reinterpret_cast< void*** >( entry + 8 );
            const uint64_t cnt = *reinterpret_cast< uint64_t* >( entry + 0x10 );
            if ( begin == nullptr || cnt == 0 || cnt > 0x400 ||
                 !readable_ptr( begin ) ) continue;
            for ( uint64_t k = 0; k < cnt; ++k )
            {
                void* comp = begin[ k ];
                if ( comp == nullptr || !readable_ptr( comp ) ) continue;
                // find which field of the component names its owner trailer
                const uintptr_t span = readable_span( comp, 0x200 );
                for ( uint32_t bo = 8; bo + 8 <= span; bo += 8 )
                {
                    const int ci = canon_index_of(
                        prism::offsets::field< void* >( comp, bo ) );
                    if ( ci >= 0 )
                    {
                        CCore::g_instance->info(
                            "lampreg: ch=0x{:02X} comp={:p} -> c{} @comp+0x{:X}",
                            c, comp, ci, bo );
                        break;
                    }
                }
            }
        }
        // same census on each parked trailer's own registry — empty means its
        // lamps can only live under the truck
        for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
        {
            void* t = canon[ i ];
            if ( !readable_ptr( t ) ||
                 !readable_ptr( reinterpret_cast< uint8_t* >( t ) + 0x140 ) ) continue;
            const uint64_t tcnt = prism::offsets::field< uint64_t >( t, 0x140 );
            CCore::g_instance->info( "lampreg: c{} channels={} live={}",
                i, tcnt, trailer_is_live( t ) ? 1 : 0 );
        }
    }

    void light_watch_tick()
    {
        auto* game_actor = CCore::g_instance->get_game_actor();
        void* truck = game_actor != nullptr
            ? prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck )
            : nullptr;
        if ( truck == nullptr || !readable_ptr( truck ) ||
             !readable_ptr( reinterpret_cast< uint8_t* >( truck ) + 0x2BF ) )
        {
            lw_valid = false;
            return;
        }

        uint8_t cur[ 0x20 ];
        memcpy( cur, reinterpret_cast< const uint8_t* >( truck ) + 0x2A0, 0x20 );
        const bool truck_changed = !lw_valid || memcmp( cur, lw_mask_truck, 0x20 ) != 0;
        if ( truck_changed && lw_valid )
        {
            CCore::g_instance->info( "lightwatch: truck mask {:016X}/{:016X} -> {:016X}/{:016X}",
                *reinterpret_cast< uint64_t* >( lw_mask_truck ),
                *reinterpret_cast< uint64_t* >( lw_mask_truck + 8 ),
                *reinterpret_cast< uint64_t* >( cur ),
                *reinterpret_cast< uint64_t* >( cur + 8 ) );
        }
        memcpy( lw_mask_truck, cur, 0x20 );

        for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
        {
            void* t = canon[ i ];
            if ( !readable_ptr( t ) ||
                 !readable_ptr( reinterpret_cast< uint8_t* >( t ) + 0x2BF ) ) continue;
            uint8_t m[ 0x20 ];
            memcpy( m, reinterpret_cast< const uint8_t* >( t ) + 0x2A0, 0x20 );
            // lamp mask pin: a parked trailer keeps mirroring the truck's
            // light byte (native propagation path is still being traced), so
            // clamp it to 0 — same technique as the parked brake-field pins.
            // Only byte +0x2A0 is the lamp mask; +0x2A1/+0x2A2 are separate
            // state and must be left alone. Non-live is the test (not
            // joint==null): a trailer bolted to a parked parent is parked too
            const bool parked_piece = !trailer_is_live( t );
            if ( parked_piece && m[ 0 ] != 0 )
            {
                reinterpret_cast< uint8_t* >( t )[ 0x2A0 ] = 0;
                m[ 0 ] = 0;
            }
            // lamp-source repoint: the lamp update reads the mask through
            // [[veh+0x110]+0x18] — a parked trailer still points at the truck,
            // so zeroing its own mask does nothing. Source = self makes the
            // trailer read its own (pinned-zero) mask → natively dark, and
            // the native attach flow rewrites the source on recouple
            if ( parked_piece )
            {
                void* ctx = readable_ptr( reinterpret_cast< uint8_t* >( t ) + 0x110 )
                    ? prism::offsets::field< void* >( t, 0x110 ) : nullptr;
                if ( ctx != nullptr && readable_ptr( ctx ) &&
                     prism::offsets::field< void* >( ctx, 0x18 ) != t )
                {
                    prism::offsets::field< void* >( ctx, 0x18 ) = t;
                }
            }
            void* src = light_source_of( t );
            const bool mask_changed = lw_valid && memcmp( m, lw_mask_prev[ i ], 0x20 ) != 0;
            const bool src_changed = lw_valid && src != lw_src_prev[ i ];
            if ( truck_changed || mask_changed || src_changed )
            {
                char tag[ 4 ] = "?";
                if ( src == truck ) { tag[0]='t'; tag[1]='r'; tag[2]='k'; tag[3]=0; }
                else
                {
                    const int ci = canon_index_of( src );
                    if ( ci >= 0 ) { tag[0] = 'c'; tag[1] = static_cast< char >( '0' + ci ); tag[2] = 0; }
                }
                CCore::g_instance->info(
                    "lightwatch: c{} mask={:016X}/{:016X}{} src={:p}({}) joint={:p}",
                    i,
                    *reinterpret_cast< uint64_t* >( m ),
                    *reinterpret_cast< uint64_t* >( m + 8 ),
                    mask_changed ? " MASK*" : "",
                    src, src == nullptr ? "null" : tag, joint( t ) );
            }
            memcpy( lw_mask_prev[ i ], m, 0x20 );
            lw_src_prev[ i ] = src;
        }
        lw_valid = true;

        // component census: run once per parked-set change — which lamp
        // components sit in the truck's registry and who they belong to
        uint32_t parked_sig = 0;
        for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
        {
            if ( readable_ptr( canon[ i ] ) && !trailer_is_live( canon[ i ] ) )
            {
                parked_sig |= 1u << i;
            }
        }
        static uint32_t last_parked_sig = UINT32_MAX;
        if ( parked_sig != last_parked_sig )
        {
            last_parked_sig = parked_sig;
            if ( parked_sig != 0 ) lamp_registry_scan( truck );
        }
    }

    void dump_state_raw( const char* tag )
    {
        auto* game_actor = CCore::g_instance->get_game_actor();
        if ( game_actor == nullptr ) return;
        void* head = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_trailer );
        void* truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
        void* towed = truck != nullptr && readable_ptr( truck )
            ? prism::offsets::field< void* >( truck, 0x15A8 ) : nullptr;
        CCore::g_instance->info( "dump {}: +0xC8={:p} truck_towed={:p}", tag, head, towed );
        for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
        {
            void* t = canon[ i ];
            if ( !readable_ptr( t ) )
            {
                CCore::g_instance->info( "dump {}: c{}={:p} DEAD", tag, i, t );
                continue;
            }
            void* parent = prism::offsets::field< void* >( t, prism::offsets::trailer_parent );
            void* slave = prism::offsets::field< void* >( t, prism::offsets::trailer_slave );
            const uint8_t e8d = readable_ptr( reinterpret_cast< uint8_t* >( t ) + 0xE8D )
                ? prism::offsets::field< uint8_t >( t, 0xE8D ) : uint8_t( 0xFF );
            const uint8_t parked = readable_ptr( reinterpret_cast< uint8_t* >( t ) + 0x1198 )
                ? prism::offsets::field< uint8_t >( t, 0x1198 ) : uint8_t( 0xFF );
            void* prec = readable_ptr( reinterpret_cast< uint8_t* >( t ) + 0x1188 )
                ? prism::offsets::field< void* >( t, 0x1188 ) : nullptr;
            const uint8_t reg = readable_ptr( reinterpret_cast< uint8_t* >( t ) + 0x1258 )
                ? prism::offsets::field< uint8_t >( t, 0x1258 ) : uint8_t( 0xFF );
            void* towed15 = readable_ptr( reinterpret_cast< uint8_t* >( t ) + 0x15A8 )
                ? prism::offsets::field< void* >( t, 0x15A8 ) : nullptr;
            hook_xform_t xf{};
            const bool has_pos = hook_world( t, xf );
            CCore::g_instance->info(
                "dump {}: c{}={:p} parent={:p} slave={:p} 15a8={:p} joint={:p} e8d={} parked={} prec={:p} reg={} actor={:p} pos=({:.1f},{:.1f},{:.1f})",
                tag, i, t, parent, slave, towed15, joint( t ), e8d, parked, prec, reg,
                find_trailer_actor( t, game_actor ),
                has_pos ? xf.pos.x : 0.f, has_pos ? xf.pos.y : 0.f, has_pos ? xf.pos.z : 0.f );
            // parked-record internals: dump the first 0x100 bytes as pointers
            // so the member list / anchor transform can be identified — the
            // record drags a detached section head when a member leaves
            if ( prec != nullptr && readable_ptr( prec ) )
            {
                const uintptr_t span = readable_span( prec, 0x100 );
                for ( uintptr_t o = 0; o + 8 <= span; o += 8 )
                {
                    void* v = *reinterpret_cast< void** >( reinterpret_cast< uint8_t* >( prec ) + o );
                    const int owner = canon_index_of( v );
                    const float f = *reinterpret_cast< const float* >(
                        reinterpret_cast< uint8_t* >( prec ) + o );
                    if ( owner >= 0 || v == t ||
                        ( plausible_object( v ) ) ||
                        ( std::isfinite( f ) && std::fabs( f ) > 0.5f && std::fabs( f ) < 4000.f ) )
                    {
                        CCore::g_instance->info( "dump {}:   c{} prec+0x{:02X} = {:p} (f={:.2f} c{})",
                            tag, i, o, v, f, owner );
                    }
                }
            }
        }
        for ( uint32_t s = 0; s < 8; ++s )
        {
            void* entry = reinterpret_cast< uint8_t* >( game_actor ) + 0x1D38 + s * 0x28;
            void* actor = *reinterpret_cast< void** >( entry );
            if ( actor == nullptr ) break;
            int owner = -1;
            if ( plausible_object( actor ) )
            {
                const uintptr_t span = readable_span( actor, 0x400 );
                for ( uint32_t bo = 8; bo + 8 <= span; bo += 8 )
                {
                    const int ci = canon_index_of( prism::offsets::field< void* >( actor, bo ) );
                    if ( ci >= 0 ) { owner = ci; break; }
                }
            }
            CCore::g_instance->info( "dump {}: slot[{}] actor={:p} -> c{}", tag, s, actor, owner );
        }
    }

    // full-struct diff: snapshot each canon trailer at detach start, then
    // log only the qwords that changed afterwards — the trailer BEHIND the
    // released one loses its hoses without any visible call touching it, so
    // whatever field flips on it IS the render gate
    static constexpr uint32_t SNAP_QWORDS = 0x1600 / 8;
    static uint64_t trailer_snap[ max_trailers ][ SNAP_QWORDS ];
    static bool snap_valid[ max_trailers ];
    static int diff_probe_frames = 0;

    void snap_trailers()
    {
        for ( uint32_t i = 0; i < max_trailers; ++i )
        {
            snap_valid[ i ] = false;
            void* t = canon[ i ];
            if ( t == nullptr ) continue;
            const uintptr_t span = readable_span( t, 0x1600 );
            if ( span < 0x1600 ) continue;
            memcpy( trailer_snap[ i ], t, 0x1600 );
            snap_valid[ i ] = true;
        }
    }

    void diff_trailers( const char* tag )
    {
        for ( uint32_t i = 0; i < max_trailers; ++i )
        {
            if ( !snap_valid[ i ] ) continue;
            void* t = canon[ i ];
            if ( t == nullptr ) continue;
            const uintptr_t span = readable_span( t, 0x1600 );
            if ( span < 0x1600 ) continue;
            const uint64_t* now = reinterpret_cast< const uint64_t* >( t );
            for ( uint32_t q = 0; q < SNAP_QWORDS; ++q )
            {
                if ( now[ q ] != trailer_snap[ i ][ q ] )
                {
                    CCore::g_instance->info( "diff {}: c{} +0x{:03X} {:016X} -> {:016X}",
                        tag, i, q * 8, trailer_snap[ i ][ q ], now[ q ] );
                }
            }
        }
    }

    // resolve the line-entity class: log the entity's vtable as a static RVA
    // so its update/show/hide vfuncs can be disassembled offline
    void dump_entity_vtables( const char* tag )
    {
        const uint64_t base = reinterpret_cast< uint64_t >( GetModuleHandle( nullptr ) );
        for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
        {
            void* t = canon[ i ];
            if ( !readable_ptr( t ) ) continue;
            void* lb = readable_ptr( reinterpret_cast< uint8_t* >( t ) + 0xE98 )
                ? prism::offsets::field< void* >( t, 0xE98 ) : nullptr;
            if ( lb == nullptr || !readable_ptr( lb ) ) continue;
            void* ent = *reinterpret_cast< void** >( lb );
            if ( ent == nullptr || !readable_ptr( ent ) ) continue;
            void* vt = *reinterpret_cast< void** >( ent );
            if ( vt == nullptr || !readable_ptr( vt ) ) continue;
            CCore::g_instance->info( "entvt {}: c{} ent={:p} vt RVA={:#x}", tag, i, ent,
                reinterpret_cast< uint64_t >( vt ) - base + 0x140000000 );
            const uintptr_t vspan = readable_span( vt, 0x100 );
            for ( uintptr_t s = 0; s + 8 <= vspan; s += 8 )
            {
                const uint64_t fn = *reinterpret_cast< uint64_t* >(
                    reinterpret_cast< uint8_t* >( vt ) + s );
                if ( fn > base && fn < base + 0x4000000 )
                {
                    CCore::g_instance->info( "entvt {}:   c{} vt+0x{:02X} -> RVA {:#x}",
                        tag, i, s, fn - base + 0x140000000 );
                }
            }
        }
    }

    void disconnect_now( void* trailer, void* parent, const bool parent_is_truck )
    {
        dump_state( "pre-detach" );
        dump_entity_vtables( "pre" );
        snap_trailers();
        // the native T-detach produces a real parked trailer — record,
        // obstacle registration, marker, persistent visual — instead of our
        // hand-built half-state. BUT it is the head-detach path: run on a
        // mid-chain trailer it tears down the whole towed set's registration
        // (proven: detaching trailer 2 made trailer 1 vanish). Only the
        // truck-direct detach goes native; mid-chain stays on the manual
        // replica below
        void* truck = nullptr;
        void* prev_towed = nullptr;
        if ( auto* game_actor = CCore::g_instance->get_game_actor() )
        {
            truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
            prev_towed = truck != nullptr && readable_ptr( truck )
                ? prism::offsets::field< void* >( truck, 0x15A8 ) : nullptr;
        }
        if ( parent_is_truck && native_detach_via_slot( trailer ) )
        {
            mark_detached( trailer );
            if ( trailer == truck_towed ) truck_towed = nullptr;
            // the dispatcher unconditionally clears the truck's towed
            // bookkeeping — put it back when the trailer we dropped was
            // mid-chain and something is still on the wheel
            if ( truck != nullptr && prev_towed != nullptr && prev_towed != trailer &&
                joint( prev_towed ) != nullptr )
            {
                if ( set_towed_trailer_fn != nullptr ) set_towed_trailer_fn( truck, true, prev_towed );
                prism::offsets::field< uint32_t >( truck, prism::offsets::truck_coupling_state ) = 5;
            }
            unhide_detached();
            dump_state( "post-detach" );
            diff_trailers( "post-detach" );
            diff_probe_frames = 90;
            return;
        }

        // native T detach order: while the truck's coupling state is nonzero it
        // releases the joint and fifth-wheel lock first, then drops the joint
        if ( parent_is_truck && parent != nullptr && pre_detach_fn != nullptr &&
            prism::offsets::field< uint32_t >( parent, prism::offsets::truck_coupling_state ) != 0 )
        {
            pre_detach_fn( parent, trailer );
        }
        // the gear-crank noise: play the trailer's registered brace sound
        // event directly (the brace-anim path can't build on models without
        // the brace_model_anim node — proven silent in testing)
        play_brace_sound( trailer, false );

        // the native detach pushes the parked trailer's position into its
        // traffic-obstacle anchor BEFORE the joint drops: actor slot at
        // game_actor+0x1D38 -> provider +0x1F0 -> anchor update. Skipping it
        // leaves the lane modifier owner stale and the traffic sweep hits it
        // whenever the trailer sits on a road. Unlike the head trailer — which
        // parks right under the truck — a mid-chain detach can sit anywhere,
        // so the trailer's own transform is the one that goes to the anchor
        // the actor->provider->anchor path is head-trailer-only in the native
        // code; the parked vfunc +0x328 below already registers the detached
        // trailer as a traffic obstacle, and pushing a mid-chain trailer's
        // transform through the head trailer's anchor corrupts it (AV in this
        // plugin). Intentionally left out.
        vcall< prism::physics_trailer_u_disconnect_fn >( trailer, prism::offsets::vfunc_disconnect );

        // tell every component on both vehicles the coupling is gone — the
        // native detach notifies the vehicle component list (+0xE98) on both
        // sides; a loaded trailer's cargo constraints corrupt the physics
        // sweep if they are never told
        if ( notify_decoupled_fn != nullptr )
        {
            if ( parent != nullptr ) notify_decoupled_fn( parent );
            notify_decoupled_fn( trailer );
        }
        // coupled-state notify with state 0 ("decoupled"): the connector
        // virtuals play the disconnect hiss, same as the trailer leaving
        // coupling range in the native flow. Only the DETACHED trailer is
        // marked — a trailer parent keeps its own joint to the truck and
        // flagging it decoupled would cut the feed it still receives
        if ( clear_detached_flag_fn != nullptr )
        {
            if ( parent != nullptr && parent_is_truck ) clear_detached_flag_fn( parent, 1, 1 );
            clear_detached_flag_fn( trailer, 1, 1 );
        }

        // registered as intentionally detached: native flows must not re-attach
        // it. The whole detached section is marked, not just the head — a slave
        // that stays jointed behind it (dolly + rear trailer) moves as a rigid
        // unit, and an unmarked member would be teleported to a dock slot and
        // drag the section back through its live joint
        for ( void* t = trailer; t != nullptr; )
        {
            mark_detached( t );
            mark_plugin_detached( t );
            // keep walking only while the slave is still jointed — i.e. part of
            // this rigid section. A slave link to an already-parked trailer is
            // not followed, or its own mark would be lifted on re-attach
            void* next = prism::offsets::field< void* >( t, prism::offsets::trailer_slave );
            t = ( next != t && next != nullptr && joint( next ) != nullptr ) ? next : nullptr;
        }
        // slot-level mark too: pointer marks die when a cargo op destroys the
        // object — the canon index is what lets the restore spot the fresh
        // jointed replacement and put it back
        const int di = canon_index_of( trailer );
        if ( di >= 0 ) detached_idx[ di ] = true;

        // rest of the native detach sequence: flag the trailer as parked, drop the
        // parent's wheel load, and rebuild the lines so the hoses between the
        // pair come down
        if ( prism::offsets::field< void* >( trailer, prism::offsets::trailer_parked_state ) != nullptr )
        {
            prism::offsets::field< uint8_t >( trailer, prism::offsets::trailer_parked ) = 1;
        }
        // native detach calls trailer vfunc +0x328(trailer, true) — the real
        // "set parked" that registers the detached trailer as a traffic
        // obstacle with a live owner. Setting the flag alone leaves the
        // obstacle unregistered and the traffic sweep hits a dead entry
        if ( plausible_vfunc( trailer, 0x328 / 8 ) )
        {
            vcall< prism::physics_trailer_u_set_trailer_brace_fn >( trailer, 0x328 / 8, true );
        }
        // crank the legs down progressively like the native detach — the
        // +0x328 call above snaps the logical state for obstacle
        // registration, so rewind the progress field and let the anim
        // player run the wind-down in sync with the sound (models without
        // the brace markers just snap, same as the old instant write)
        if ( brace_anim_fn != nullptr )
        {
            prism::offsets::field< float >( trailer, 0x1270 ) = 1.f;
            brace_anim_fn( trailer, 1, 0.3333333f );
        }
        // the game applies spring brakes the same way on a jointless trailer
        prism::offsets::field< float >( trailer, prism::offsets::trailer_brake ) = 1.f;
        prism::offsets::field< float >( trailer, prism::offsets::trailer_service_brake ) = 0.f;
        prism::offsets::field< uint8_t >( trailer, prism::offsets::trailer_brake_forced ) = 1;
        // drop the trailer's own line entities now — they are its half of
        // the pair's hoses and the stale-state sweeper only reaches them a
        // frame later, leaving the detached piece visibly cabled meanwhile
        if ( set_lines_fn != nullptr )
        {
            set_lines_fn( trailer, 0, 1 );
        }
        if ( parent != nullptr )
        {
            if ( parent_is_truck )
            {
                // truck-side bookkeeping only; on a trailer parent those fields are
                // something else and the refcounted store would corrupt them
                if ( set_towed_trailer_fn != nullptr )
                {
                    set_towed_trailer_fn( parent, false, nullptr );
                }
                if ( distribute_towed_mass_fn != nullptr )
                {
                    distribute_towed_mass_fn( parent, 0.f, 0.f );
                }
                // nothing tows anymore: back to the uncoupled state or jointless
                // trailers would stop being spring-braked by the per-frame loop
                // (skipped while trailer 1 still sits on the fifth wheel)
                if ( joint( trailers[ 0 ] ) == nullptr )
                {
                    prism::offsets::field< uint32_t >( parent, prism::offsets::truck_coupling_state ) = 0;
                }
            }
            if ( rebuild_lines_fn != nullptr )
            {
                void* slave = prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave );
                prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = nullptr;
                rebuild_lines_fn( parent, trailer );
                prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = slave;
            }
            // native detach fires coupling events at every accessory on the
            // trailer (cargo, traffic-obstacle anchor) — without it the parked
            // trailer registers a lane modifier with a null owner and the
            // traffic sweep crashes on it near roads/docks
            if ( refresh_accessories_fn != nullptr )
            {
                refresh_accessories_fn( parent, trailer );
            }

            // native detach tail: notify the traffic subsystem, re-link the
            // type-2 obstacle anchors (rebinds the parked trailer's lane
            // modifier to its new owner), then the post notify
            if ( traffic_manager_global != nullptr )
            {
                void* mgr = *traffic_manager_global;
                void* sub = mgr != nullptr ? prism::offsets::field< void* >( mgr, 0x2B30 ) : nullptr;
                if ( sub != nullptr ) vcall< prism::vehicle_notify_decoupled_fn >( sub, 0x138 / 8 );
            }
            if ( relink_anchors_fn != nullptr )
            {
                void* game_actor = CCore::g_instance->get_game_actor();
                void* reg = game_actor != nullptr ? prism::offsets::field< void* >( game_actor, 0x30 ) : nullptr;
                if ( reg != nullptr && readable_ptr( reinterpret_cast< uint8_t* >( reg ) + 0xD8 ) )
                {
                    relink_anchors_fn( reinterpret_cast< uint8_t* >( reg ) + 0xD8, 2 );
                }
            }
            if ( traffic_manager_global != nullptr )
            {
                void* mgr = *traffic_manager_global;
                void* sub = mgr != nullptr ? prism::offsets::field< void* >( mgr, 0x2B30 ) : nullptr;
                if ( sub != nullptr ) vcall< prism::vehicle_notify_decoupled_fn >( sub, 0x1E0 / 8 );
            }
        }
        // native detach also unpairs the two vehicles' connectors and
        // re-applies the cables cvar with a null trailer (cables drop to the
        // remaining set). The truck clears its coupling progress scalar too.
        if ( parent != nullptr && pair_vehicles_fn != nullptr )
        {
            pair_vehicles_fn( parent, trailer );
        }
        if ( auto* game_actor = CCore::g_instance->get_game_actor() )
        {
            void* truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
            if ( parent_is_truck && truck != nullptr && readable_ptr( reinterpret_cast< uint8_t* >( truck ) + 0x4C8 ) )
            {
                prism::offsets::field< float >( truck, 0x4C4 ) = 0.f;
                // native detach resets the coupling state machine and drops
                // the towed registration — leave them pinned and the game
                // believes a trailer is still coupled, refusing every new
                // attach (ours and the T key's alike)
                prism::offsets::field< uint32_t >( truck, prism::offsets::truck_coupling_state ) = 0;
                if ( set_towed_trailer_fn != nullptr &&
                    prism::offsets::field< void* >( truck, 0x15A8 ) == trailer )
                {
                    set_towed_trailer_fn( truck, true, nullptr );
                }
            }
            if ( apply_cables_fn != nullptr && cables_mode_cvar != nullptr &&
                get_cvar_int_fn != nullptr && truck != nullptr )
            {
                const int mode = get_cvar_int_fn( cables_mode_cvar );
                apply_cables_fn( truck, nullptr, 0, mode > 4 ? 4 : mode < 0 ? 0 : mode, 0 );
            }
        }
        if ( trailer == truck_towed ) truck_towed = nullptr;
        // joint is dead now — a detach genuinely completed, so a later "whole
        // set coupled" state means the user re-assembled it and traffic may
        // be restored
        traffic_detach_done.store( true, std::memory_order_release );
        // the parked piece must keep its visual — rewrite it in place
        unhide_detached();
        dump_state( "post-detach" );
        diff_trailers( "post-detach" );
        diff_probe_frames = 90;
    }

    void connect_now( void* trailer, void* parent, const bool parent_is_truck )
    {

        // re-check hook proximity at execution time: the trailer may have been
        // nudged between queueing and this sim tick — never teleport it
        if ( hook_far( trailer, parent, parent_is_truck ) )
        {
            CCore::g_instance->warning( "Trailer too far — back up to attach" );
            return;
        }
        dump_state( "pre-attach" );

        // an attach to the fifth wheel goes through the native T dispatcher
        // with +0xC8 pointed at the chosen trailer — a real chain head with
        // native cables/lights/brakes, not a hand-built replica
        if ( parent_is_truck )
        {
            // marks drop first: the dispatcher's settle teleports through
            // hooked paths that refuse marked trailers
            for ( void* t = trailer; t != nullptr; )
            {
                unmark_detached( t );
                unmark_plugin_detached( t );
                const int ti = canon_index_of( t );
                if ( ti >= 0 ) detached_idx[ ti ] = false;
                void* next = prism::offsets::field< void* >( t, prism::offsets::trailer_slave );
                t = ( next != t && next != nullptr && joint( next ) != nullptr ) ? next : nullptr;
            }
            if ( native_attach_via_slot( trailer ) )
            {
                unhide_detached();
                dump_state( "post-attach" );
                return;
            }
            CCore::g_instance->warning( "native attach declined — falling back to manual connect" );
        }

        float3_t vec = attach_offset( parent, parent_is_truck );

        // connect_slave also propagates the parent's coupling byte to the slave and
        // passes the parent's chassis-type flag to connect(); replay both since the
        // permanent hook suppresses connect_slave for plugin-detached trailers
        const auto parent_coupling = prism::offsets::field< uint8_t >( parent, prism::offsets::vehicle_coupling_flag );
        prism::offsets::field< uint8_t >( trailer, prism::offsets::vehicle_coupling_flag ) = parent_coupling;
        void* parent_chassis = prism::offsets::field< void* >( parent, prism::offsets::vehicle_chassis_data );
        const bool slave_hook = parent_chassis != nullptr &&
            prism::offsets::field< uint32_t >( parent_chassis, prism::offsets::chassis_slave_hook_type ) == 2;

        // the trailer is coming back under a live joint by explicit request —
        // drop the detached marks BEFORE the settle: the settle snaps the
        // trailer onto the hook through the per-trailer teleport, which the
        // teleport hook refuses while the mark is still up. The whole slave
        // chain behind it is rejoining too (jointed members would otherwise
        // stay cargo-locked forever)
        for ( void* t = trailer; t != nullptr; )
        {
            unmark_detached( t );
            unmark_plugin_detached( t );
            const int ti = canon_index_of( t );
            if ( ti >= 0 ) detached_idx[ ti ] = false;
            void* next = prism::offsets::field< void* >( t, prism::offsets::trailer_slave );
            t = ( next != t && next != nullptr && joint( next ) != nullptr ) ? next : nullptr;
        }

        // the native T attach settles the trailer on the hook first
        // (0x1409FB700, trailer + a cvar float from 0x142D48200)
        if ( parent_is_truck && trailer_settle_fn_ptr != nullptr &&
            attach_settle_cvar != nullptr && get_cvar_float_fn != nullptr )
        {
            trailer_settle_fn_ptr( trailer, get_cvar_float_fn( attach_settle_cvar ) );
        }

        CCore::g_instance->info( "Connecting trailer {:p} to {:p}", trailer, parent );
        vcall< prism::physics_trailer_u_connect_fn >( trailer, prism::offsets::vfunc_connect,
            parent, vec, 0, parent_is_truck || parent_coupling != 0, slave_hook );

        // rest of the native attach sequence (T-key flow): register the trailer on
        // the truck, clear the detached flag on both vehicles, unpark it, raise the
        // brace and rebuild the air/electric lines between the pair.
        // The truck-side bookkeeping (towed pointer, wheel load) is truck-only — the
        // fields it writes belong to the truck's own layout, running them on a
        // trailer parent corrupts whatever lives at those offsets.
        if ( parent_is_truck )
        {
            if ( set_towed_trailer_fn != nullptr )
            {
                set_towed_trailer_fn( parent, true, trailer );
            }
            // mark the truck's coupling state machine as fully coupled: the
            // per-frame trailer loop spring-brakes jointed trailers while it is
            // below 5, and the truck only broadcasts its brake/light state down
            // the towed chain at 5
            prism::offsets::field< uint32_t >( parent, prism::offsets::truck_coupling_state ) = 5;
            // the native attach also writes the coupling progress scalar
            prism::offsets::field< float >( parent, 0x4C4 ) = 1.f;
        }
        // coupled-state component notify (0x140874010): flips vehicle+0xE8D
        // to "coupled" and forces the connector virtuals — the air-hose
        // connect sound the native progression plays. r8=1 forces the notify
        // even when the state byte already matches, so the sound always fires
        if ( clear_detached_flag_fn != nullptr )
        {
            clear_detached_flag_fn( parent, 0, 0 );
            clear_detached_flag_fn( trailer, 0, 0 );
        }
        // un-park: native attach only clears the flag byte (when a parked-state
        // record exists) and never nulls the backpointer — the traffic obstacle
        // registration may still reference it, and orphaning it leaves a dead
        // entry the traffic sweep crashes on even after the trailer re-couples
        if ( prism::offsets::field< void* >( trailer, prism::offsets::trailer_parked_state ) != nullptr )
        {
            prism::offsets::field< uint8_t >( trailer, prism::offsets::trailer_parked ) = 0;
        }
        // native attach calls trailer vfunc +0x328(trailer, false) — un-registers
        // the parked traffic obstacle so the trailer rejoins the convoy clean
        if ( plausible_vfunc( trailer, 0x328 / 8 ) )
        {
            vcall< prism::physics_trailer_u_set_trailer_brace_fn >( trailer, 0x328 / 8, false );
        }
        // a jointless trailer sits on spring brakes (trailer_brake = 1.0), applied by
        // the game's own update; now that the joint exists, release it or the trailer
        // keeps dragging along with locked wheels
        prism::offsets::field< float >( trailer, prism::offsets::trailer_brake ) = 0.f;
        prism::offsets::field< uint8_t >( trailer, prism::offsets::trailer_brake_forced ) = 0;
        vcall< prism::physics_trailer_u_set_trailer_brace_fn >( trailer, prism::offsets::vfunc_set_brace, false );
        if ( rebuild_lines_fn != nullptr )
        {
            // rebuild_lines walks the trailer's own slave chain; hide it so the rebuild
            // stops at this link instead of reaching a trailer parked somewhere else
            void* slave = prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave );
            prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = nullptr;
            rebuild_lines_fn( parent, trailer );
            prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = slave;
        }
        // same accessory event refresh the native attach runs on the trailer
        if ( refresh_accessories_fn != nullptr && parent != nullptr )
        {
            refresh_accessories_fn( parent, trailer );
        }
        if ( parent_is_truck )
        {
            if ( distribute_towed_mass_fn != nullptr )
            {
                distribute_towed_mass_fn( parent, prism::offsets::field< float >( trailer, prism::offsets::trailer_total_mass ), 0.f );
            }
            if ( refresh_axle_lift_fn != nullptr )
            {
                if ( auto* game_actor = CCore::g_instance->get_game_actor() ) refresh_axle_lift_fn( game_actor );
            }
        }
        // the native attach tail (0x1405DCC20): pair the two vehicles'
        // pneumatic/electric connectors — without it the trailer keeps its
        // standalone spring brake and stays braked with a live joint — then
        // re-apply the cables cvar and refresh the actor's trailer flags.
        // The native calls pair(tower, towed), which for a chain attach is
        // the parent trailer — the connector pairing is between neighbours
        if ( pair_vehicles_fn != nullptr )
        {
            pair_vehicles_fn( parent, trailer );
        }
        if ( parent_is_truck && apply_cables_fn != nullptr && cables_mode_cvar != nullptr &&
            get_cvar_int_fn != nullptr )
        {
            const int mode = get_cvar_int_fn( cables_mode_cvar );
            apply_cables_fn( parent, trailer, 0, mode > 4 ? 4 : mode < 0 ? 0 : mode, 0 );
        }
        if ( parent_is_truck && actor_flag_fixup_fn != nullptr )
        {
            if ( auto* game_actor = CCore::g_instance->get_game_actor() ) actor_flag_fixup_fn( game_actor );
        }
        // the native attach pushes the trailer's new position into its
        // traffic-obstacle anchor through the trailer actor's +0x1F8 provider.
        // The second anchor the native updates sits under [game_actor+0xC8] —
        // replicating it kept resolving wrong-typed objects whose vfuncs jumped
        // into space, so it stays out: the truck-side anchor gets refreshed by
        // the game's own per-tick updates anyway
        // no anchor push here either: the only actor slot is the head
        // trailer's, and +0x328 already un-registers the parked obstacle —
        // writing through a foreign anchor corrupts it
        // the T attach raises the landing gear with its crank sound —
        // play the registered brace-up sound event on the trailer
        play_brace_sound( trailer, true );
        // crank the legs up progressively like the native attach — the
        // +0x328 calls above snapped the raised state for parked-obstacle
        // bookkeeping, so rewind the progress field and let the anim
        // player run the wind-up in sync with the sound
        if ( brace_anim_fn != nullptr )
        {
            prism::offsets::field< float >( trailer, 0x1270 ) = 0.f;
            brace_anim_fn( trailer, 0, 0.3333333f );
        }
        dump_state( "post-attach" );
    }

    // puts back what a cargo load/unload respawn destroyed: every marked
    // chain slot whose fresh object came back jointed is disconnected again
    // and teleported to the transform it was parked at. Runs on the sim
    // thread (queued op), descending index order so a tail trailer parked
    // on its own leaves before the section it was taken with — each marked
    // slot is the head of what the player actually detached
    void restore_parked()
    {
        // put the rig itself back first — replay the game's own actor teleport
        // with the pre-load transform and the original flags. The truck goes
        // through its proper path (vfunc +0x2A8), attached trailers follow the
        // actor-relative formation, and plugin-detached trailers are skipped
        // by our own per-trailer teleport hook. Then the detach pass below
        // re-parks the respawned sections
        if ( rig_armed )
        {
            rig_armed = false;
            // replaying the rig teleport with the snapshot transform yanks a
            // settled jointed combination across the map mid-solve — only do
            // it while a detached trailer still needs re-parking; if its marks
            // were cleared since the arm there is nothing to restore
            bool any_detached = false;
            for ( int i = 0; i < max_trailers; ++i )
            {
                if ( detached_idx[ i ] ) { any_detached = true; break; }
            }
            if ( any_detached && rig_truck_valid && actor_teleport_hook != nullptr )
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
            // same parked section — the section head's walk below covers it;
            // disconnecting it here would split the parked combination apart
            if ( i > 0 && detached_idx[ i - 1 ] && canon[ i - 1 ] != nullptr ) continue;
            void* t = canon[ i ];
            if ( t == nullptr || !readable_ptr( t ) || joint( t ) == nullptr ) continue;

            // the parent it was respawned under — canon order is preserved
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
            // its saved parked transform — straight through the original, so
            // the pointer marks guarding the hook don't block the restore
            for ( void* m = t; m != nullptr; )
            {
                const int mi = canon_index_of( m );
                if ( mi >= 0 && parked_xform_valid[ mi ] && trailer_teleport_hook != nullptr )
                {
                    using teleport_fn_t = void __fastcall( void* /*trailer*/, void* /*transform*/ );
                    trailer_teleport_hook->get_original< teleport_fn_t >()( m, &parked_xform[ mi ] );
                    anchor_valid[ mi ] = false;
                }
                void* next = prism::offsets::field< void* >( m, prism::offsets::trailer_slave );
                m = ( next != m && next != nullptr && joint( next ) != nullptr ) ? next : nullptr;
            }
        }
    }


    //========================================================================
    // Game hooks
    //========================================================================

    // the original dereferences the trailer's physics joint, which is null on a
    // detached trailer. Skip only that case and run the original otherwise.
    // Also pumps the sim-thread queues (ops and console lines).
    void hk_crashes_when_disconnected( prism::physics_trailer_u* self, prism::game_trailer_actor_u* trailer )
    {
        pump_sim_heartbeat();
        // console commands run first: a queued "g_traffic 0" must land before a
        // delayed disconnect drops the joint
        if ( pending_cmd_queued.load( std::memory_order_acquire ) && execute_line_fn != nullptr )
        {
            pending_cmd_queued.store( false, std::memory_order_release );
            char* text = pending_cmd;
            execute_line_fn( &text, -1 );
        }
        if ( pending.kind != pending_kind::none )
        {
            if ( pending.delay > 0 )
            {
                --pending.delay;
            }
            else
            {
                const pending_op op = pending;
                pending = {};
                if ( op.kind == pending_kind::connect )
                {
                    connect_now( op.trailer, op.parent, op.parent_is_truck );
                }
                else if ( op.kind == pending_kind::restore )
                {
                    if ( settings::g_settings.cargo_restore ) restore_parked();
                }
                else
                {
                    disconnect_now( op.trailer, op.parent, op.parent_is_truck );
                }
            }
        }
        if ( trailer != nullptr &&
            prism::offsets::field< void* >( trailer, prism::offsets::trailer_physics_joint ) == nullptr )
        {
            return;
        }
        crashes_when_disconnected_hook->get_original< prism::crashes_when_disconnected_fn >()( self, trailer );
    }

    // Lane-obstacle sorted insert (0x14094DFC0, runs on traffic worker
    // threads): the scan dereferences each existing element's +0x88 — the
    // owner-node pointer inside the obstacle's inline link — so a single dead
    // entry (owner node cleared without the array slot being removed) crashes
    // the whole job. That is exactly the state an orphaned parked-trailer
    // obstacle can end up in. Sanitize the target container before the
    // original runs: elements whose owner node is null (or whose memory is
    // gone entirely) are pruned, then the original sees only live entries.
    using lane_obstacle_insert_fn = uint8_t __fastcall( void* /*node*/, void* /*link*/, float /*pos*/ );
    uint8_t __fastcall hk_lane_obstacle_insert( void* node, void* link, float pos )
    {
        if ( node != nullptr && link != nullptr && plausible_vfunc( link, 1 ) )
        {
            using type_fn = uint32_t __fastcall( void* );
            const auto get_type = reinterpret_cast< type_fn* >( ( *reinterpret_cast< void** const* >( link ) )[ 1 ] );
            // link type 0xFFF = the position-sorted lane element list — the
            // path whose element scan dereferences +0x88 unchecked
            if ( get_type( link ) == 0xFFF && plausible_vfunc( node, 0xD0 / 8 ) )
            {
                using container_fn = void* __fastcall( void* );
                const auto get_container = reinterpret_cast< container_fn* >( ( *reinterpret_cast< void** const* >( node ) )[ 0xD0 / 8 ] );
                void* container = get_container( node );
                if ( readable_ptr( container ) && readable_ptr( static_cast< uint8_t* >( container ) + 0x18 ) )
                {
                    void** elements = *reinterpret_cast< void*** >( static_cast< uint8_t* >( container ) + 8 );
                    auto* count = reinterpret_cast< uint64_t* >( static_cast< uint8_t* >( container ) + 0x10 );
                    if ( elements != nullptr && readable_ptr( elements ) && *count <= 0x10000 )
                    {
                        uint64_t n = *count;
                        for ( uint64_t i = n; i-- > 0; )
                        {
                            void* el = elements[ i ];
                            const bool dead = el == nullptr || !readable_ptr( el ) ||
                                !readable_ptr( static_cast< uint8_t* >( el ) + 0x88 ) ||
                                *reinterpret_cast< void** >( static_cast< uint8_t* >( el ) + 0x88 ) == nullptr;
                            if ( dead )
                            {
                                CCore::g_instance->info( "lane obstacle: pruned dead entry {:p}", el );
                                memmove( &elements[ i ], &elements[ i + 1 ], ( n - i - 1 ) * sizeof( void* ) );
                                --n;
                            }
                        }
                        *count = n;
                    }
                }
            }
        }
        return lane_obstacle_insert_hook->get_original< lane_obstacle_insert_fn >()( node, link, pos );
    }

    // pure virtual call handler (0x141D24910 on ATS 1.61.3.18, twin of ETS2
    // 0x1D23610): a mid-chain detached trailer leaves listener nodes whose
    // vfunc slots point here — every per-frame consumer that walks them
    // (node dispatch, parked machine, obstacle lists) would die on a
    // purecall, and purecalls never reach the SCS crash logger, which is
    // why those crashes leave no game.crash.txt. Swallowing it as a null
    // return keeps the frame alive and logs each call site once
    void* __fastcall hk_purecall( void* /*object*/ )
    {
        const auto ret = reinterpret_cast< uint64_t >( _ReturnAddress() );
        static uint64_t seen[ 32 ] = {};
        for ( auto& s : seen )
        {
            if ( s == ret ) return nullptr;
            if ( s == 0 )
            {
                s = ret;
                const auto base = reinterpret_cast< uint64_t >( GetModuleHandle( nullptr ) );
                CCore::g_instance->warning( "purecall swallowed at exe+0x{:X}", ret - base );
                break;
            }
        }
        return nullptr;
    }

    // Chase-camera target setter (base vfunc 31, 0x14063C2D0): whenever the
    // native resolver tries to anchor the camera on a tracked trailer whose
    // physics joint is dead, swap the target for null — the same "no
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

    // Chase-camera per-frame update (vfunc 32, 0x140AC89A0): the camera finds
    // the combination's rear by walking trailer_slave links in THIS code
    // path — not through the hooked combo_length — so a parked trailer still
    // linked off a live one keeps the camera behind it. Hide every
    // live->dead edge only for the duration of the call (the same
    // temporary-hide pattern used around rebuild_lines): the link stays
    // alive for everything else, so the parked trailer does not despawn.
    void __fastcall hk_cam_update( void* cam, float dt )
    {
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

    // Combination-length accumulator (0x140789AB0): the game measures a
    // combination by walking trailer_slave links until null and never checks
    // physics joints. A plugin-detached trailer keeps its slave link alive on
    // purpose (physics / visibility / parked bookkeeping), so every consumer —
    // the chase camera's rear-distance math included — measures the whole
    // ORIGINAL set and camera 2 keeps backing up as if the detached trailer
    // were still coupled.
    // Faithful port of the loop with a single change: the walk stops at the
    // first trailer whose physics joint is gone. No shared state is touched —
    // the native slave links stay exactly as they are.
    //
    // original, disassembled:
    //   acc  = self[0x2C] - self[0x20]
    //   prev = self
    //   for t = self->slave; t; prev = t, t = t->slave:
    //       hook_pos(prev, vec)                       // 0x140789880
    //       gap  = prev[0x2C] - vec.z
    //       over = t->chassis[0x200][0x748] - t[0x20]
    //       acc += (t[0x2C] - t[0x20]) - gap - over
    //   return acc
    using combo_length_fn = float __fastcall( void* /*trailer*/ );
    using combo_hook_pos_fn = void __fastcall( void* /*trailer*/, float* /*out vec3*/ );
    combo_hook_pos_fn* combo_hook_pos_fn_ptr = nullptr;
    // is this trailer inside the LIVE combination — reachable from the
    // truck's towed pointer walking trailer_slave while joints are alive.
    // A slave link into a jointless piece is a visibility leaf, not a live
    // member: the walk must test the joint BEFORE counting membership, so
    // parked pieces claimed at the tail (and trailers still jointed to a
    // parked piece) are outside
    bool trailer_is_live( void* t )
    {
        auto* game_actor = CCore::g_instance->get_game_actor();
        void* truck = game_actor != nullptr
            ? prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck )
            : nullptr;
        void* cur = truck != nullptr && readable_ptr( truck )
            ? prism::offsets::field< void* >( truck, 0x15A8 ) : nullptr;
        for ( int hops = 0; cur != nullptr && hops <= max_trailers; ++hops )
        {
            if ( !readable_ptr( cur ) || joint( cur ) == nullptr ) break;
            if ( cur == t ) return true;
            void* next = prism::offsets::field< void* >( cur, prism::offsets::trailer_slave );
            if ( next == cur ) break;
            cur = next;
        }
        return false;
    }

    // native lamp-mask setter (this build): writes vehicle+0x2A0 and pushes
    // the change to the light component. The mask propagator (0x1408E8352)
    // walks the attach tree — which keeps parked trailers for visibility —
    // copying the source mask into every node, so parked trailers mirror
    // truck brakes/blinkers/headlights. A canon trailer outside the live
    // combination must stay dark: force the written mask to 0 so the setter
    // pushes a clean lamp-off instead of freezing the last live state
    using lamp_mask_setter_fn = uint8_t __fastcall( void* /*vehicle*/, uint8_t /*mask*/ );

    uint8_t __fastcall hk_lamp_mask( void* self, uint8_t mask )
    {
        // diag: is this setter even the funnel for trailer mask writes?
        static uint64_t calls = 0;
        const int ci = canon_index_of( self );
        if ( ci >= 0 )
        {
            static uint64_t canon_calls = 0;
            if ( canon_calls++ < 40 )
                CCore::g_instance->info( "lamp: c{} self={:p} mask={:02X} live={}",
                    ci, self, mask, trailer_is_live( self ) ? 1 : 0 );
        }
        else if ( mask != 0 && ( calls++ & 0x3F ) == 0 )
        {
            CCore::g_instance->info( "lamp: foreign self={:p} mask={:02X} (calls={})",
                self, mask, calls );
        }
        if ( mask != 0 && ci >= 0 && !trailer_is_live( self ) )
        {
            mask = 0;
        }
        return lamp_mask_hook->get_original< lamp_mask_setter_fn >()( self, mask );
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
        // step cap: a cyclic slave chain (concurrent writes elsewhere) must
        // never spin this walk — the original has no bound either, but it
        // only ever runs on acyclic native chains
        uint32_t steps = 0;
        for ( void* t = prism::offsets::field< void* >( self, prism::offsets::trailer_slave );
              t != nullptr && steps++ < 64;
              prev = t, t = prism::offsets::field< void* >( t, prism::offsets::trailer_slave ) )
        {
            // the only deviation from the original: a slave whose joint is
            // gone is detached — it and everything behind it is no longer
            // part of this combination
            if ( !readable_ptr( t ) ||
                !readable_ptr( static_cast< uint8_t* >( t ) + prism::offsets::trailer_physics_joint ) ||
                joint( t ) == nullptr )
            {
                break;
            }
            float vec[ 4 ] = {};
            combo_hook_pos_fn_ptr( prev, vec );
            const float gap = f( prev, 0x2C ) - vec[ 2 ];
            void* chassis = prism::offsets::field< void* >( t, 0x200 );
            const float over = ( chassis != nullptr ? f( chassis, 0x748 ) : 0.f ) - f( t, 0x20 );
            acc += ( f( t, 0x2C ) - f( t, 0x20 ) ) - gap - over;
        }
        return acc;
    }

    // the game calls this to auto-attach a parent's slave trailer; refuse only
    // when that slave was detached through this plugin, so native attach flows
    // keep working
    void hk_connect_slave( prism::physics_trailer_u* self )
    {
        void* slave = self != nullptr
            ? prism::offsets::field< void* >( self, prism::offsets::trailer_slave )
            : nullptr;
        if ( slave != nullptr )
        {
            const int si = canon_index_of( self ), ti = canon_index_of( slave );
            // a parked (jointless) parent must NEVER auto-attach a slave —
            // it has no joint to carry anything, and its stale canonical
            // claim would physically joint whatever sits behind it,
            // including the trailer that is ON THE TRUCK (the post-attach
            // crash) or another parked trailer (the spontaneous re-join).
            // Live (jointed) parents keep every native behaviour: picking
            // up the head re-joins the rest of its section through the
            // forward link, exactly as before
            const bool bad = is_detached( slave ) ||
                ( si >= 0 && joint( self ) == nullptr ) ||
                ( ti >= 0 && joint( slave ) != nullptr ) ||
                ( si >= 0 && ti >= 0 && ti < si );
            if ( bad )
            {
                return;
            }
            // a jointless canon slave the machine wants to join while it
            // sits beyond real hook distance is our visibility leaf, not a
            // coupling — the head's own attach would otherwise teleport the
            // separated piece onto its rear hook (the spontaneous re-join).
            // A slave genuinely backed under the mother measures inside
            // attach_max_gap and joins natively
            if ( ti >= 0 && joint( slave ) == nullptr &&
                 hook_gap( slave, self, false ) > settings::g_settings.attach_max_gap )
            {
                static int cs_far_logged = 0;
                if ( cs_far_logged++ < 12 )
                {
                    CCore::g_instance->info(
                        "connect_slave refused: slave {:p} beyond coupling range (visibility leaf)",
                        slave );
                }
                return;
            }
        }
        connect_slave_hook->get_original< prism::physics_trailer_u_connect_slave_fn >()( self );
    }

    // physics joint-tree accumulator (exe+0x9569F0): every node calls the
    // SAME vfunc (+0x18) on the child linked at node+0x68, so a cyclic body
    // graph — our leaf link plus the parked machine's canonical back-claim
    // landing between maintenance passes — recurses until the stack
    // overflows (the silent attach crash, 0xC00000FD at exe+0x956A87).
    // Track the nodes already on this thread's path: a revisit returns a
    // zero contribution instead of diving again — the solver skips one
    // subtree for one frame rather than dying. Depth-capped too
    using joint_walk_fn = float __fastcall( void* /*node*/, float /*arg*/ );
    float __fastcall hk_joint_walk( void* self, float arg )
    {
        thread_local void* jw_seen[ 96 ];
        thread_local int jw_depth = 0;
        for ( int i = 0; i < jw_depth; ++i )
        {
            if ( jw_seen[ i ] == self )
            {
                static int jw_logged = 0;
                if ( jw_logged++ < 8 )
                {
                    CCore::g_instance->warning(
                        "joint walk: cyclic node {:p} — skipped subtree", self );
                }
                return 0.f;
            }
        }
        if ( jw_depth >= 96 )
        {
            return 0.f;
        }
        jw_seen[ jw_depth++ ] = self;
        const float r = joint_walk_hook->get_original< joint_walk_fn >()( self, arg );
        --jw_depth;
        return r;
    }

    // physical joint link (exe+0x91AA80): writes child+0x60 = parent and
    // parent+0x68 = child, then vfunc-walks the graph. The root finder
    // (exe+0x54F4D0) iterates node+0x60 with NO cycle guard — a cyclic
    // pair hangs it forever (the detach freeze: our visibility leaf was
    // physicalized by the machine while the canonical back-claim also
    // linked, making two nodes each other's parent). A link that would
    // close a cycle can never be legitimate — refuse just that edge and
    // let every real link through
    using phys_link_fn = void __fastcall( void* /*parent*/, void* /*child*/ );
    void __fastcall hk_phys_link( void* parent, void* child )
    {
        if ( parent != nullptr && child != nullptr &&
             readable_ptr( parent ) && readable_ptr( child ) )
        {
            void* up = parent;
            for ( int i = 0; i < 128 && up != nullptr && readable_ptr( up ); ++i )
            {
                if ( up == child )
                {
                    static int pl_logged = 0;
                    if ( pl_logged++ < 8 )
                    {
                        CCore::g_instance->warning(
                            "phys link refused: child {:p} already above parent {:p} (cycle)",
                            child, parent );
                    }
                    return;
                }
                void* next = *reinterpret_cast< void** >( static_cast< uint8_t* >( up ) + 0x60 );
                if ( next == up ) break;
                up = next;
            }
            void* down = child;
            for ( int i = 0; i < 128 && down != nullptr && readable_ptr( down ); ++i )
            {
                if ( down == parent )
                {
                    static int pl_logged2 = 0;
                    if ( pl_logged2++ < 8 )
                    {
                        CCore::g_instance->warning(
                            "phys link refused: parent {:p} already below child {:p} (cycle)",
                            parent, child );
                    }
                    return;
                }
                void* next = *reinterpret_cast< void** >( static_cast< uint8_t* >( down ) + 0x68 );
                if ( next == down ) break;
                down = next;
            }
        }
        phys_link_hook->get_original< phys_link_fn >()( parent, child );
    }

    // apply_cables (the air/electric line applier): every caller resolves
    // the trailer to cable through the +0xC8 chain head — which we keep on
    // the canonical parked root so the parked pieces stay visible. While a
    // LATER trailer sits on the fifth wheel the resolver still picks the
    // head and the hoses land on the parked piece. A jointless cable
    // target while the truck tows a jointed trailer is always that wrong
    // pick — redirect the lines to what's actually on the wheel
    void __fastcall hk_apply_cables( void* truck, void* trailer, int unused, int mode, int zero )
    {
        static int ac_log = 0;
        void* towed = truck != nullptr && readable_ptr( truck )
            ? prism::offsets::field< void* >( truck, 0x15A8 ) : nullptr;
        if ( ac_log++ < 40 )
        {
            CCore::g_instance->info( "apply_cables(truck={:p} trailer={:p} mode={}) joint={} towed={:p}",
                truck, trailer, mode,
                trailer != nullptr && readable_ptr( trailer ) ? joint( trailer ) : nullptr,
                towed );
        }
        // same cascade guard as hk_rebuild_lines: apply_cables also walks
        // trailer_slave — a canon trailer outside the live (towed) chain
        // that still takes part in a real coupling keeps its cable state;
        // a jointless trailer arg marks a real edge break and falls through
        const int afi = canon_index_of( truck );
        if ( afi >= 0 && readable_ptr( truck ) )
        {
            bool live = false;
            for ( void* p = towed; p != nullptr && readable_ptr( p ); )
            {
                if ( p == truck ) { live = true; break; }
                if ( !readable_ptr( static_cast< uint8_t* >( p ) + prism::offsets::trailer_slave ) ) break;
                void* s = prism::offsets::field< void* >( p, prism::offsets::trailer_slave );
                if ( s == nullptr || !readable_ptr( s ) ||
                     !readable_ptr( static_cast< uint8_t* >( s ) + prism::offsets::trailer_physics_joint ) ||
                     joint( s ) == nullptr )
                {
                    break;
                }
                p = s;
            }
            if ( !live )
            {
                const bool front_coupled =
                    ( readable_ptr( static_cast< uint8_t* >( truck ) + prism::offsets::trailer_physics_joint ) &&
                      joint( truck ) != nullptr );
                void* fslave =
                    readable_ptr( static_cast< uint8_t* >( truck ) + prism::offsets::trailer_slave )
                        ? prism::offsets::field< void* >( truck, prism::offsets::trailer_slave ) : nullptr;
                const bool slave_intact = fslave != nullptr && readable_ptr( fslave ) &&
                    readable_ptr( static_cast< uint8_t* >( fslave ) + prism::offsets::trailer_physics_joint ) &&
                    joint( fslave ) != nullptr;
                const bool arg_live_edge = trailer != nullptr && readable_ptr( trailer ) &&
                    readable_ptr( static_cast< uint8_t* >( trailer ) + prism::offsets::trailer_physics_joint ) &&
                    joint( trailer ) != nullptr;
                if ( ( front_coupled || slave_intact ) && ( trailer == nullptr || arg_live_edge ) )
                {
                    return;
                }
            }
        }
        // the towed-head substitution is only meaningful when `front` is the
        // truck — with front=detached trailer it would pair it against the
        // live towed piece and draw phantom hoses across separate sections
        void* real_truck = nullptr;
        if ( auto* game_actor = CCore::g_instance->get_game_actor() )
        {
            real_truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
        }
        const bool front_is_truck = truck != nullptr && truck == real_truck;
        if ( trailer == nullptr && front_is_truck &&
             towed != nullptr && readable_ptr( towed ) && joint( towed ) != nullptr )
        {
            // the native detach uncables with (truck, nullptr) — a global
            // clear that drops the lines on the whole live chain, not just
            // the dead edge. While the truck still tows, substitute the
            // towed head so it re-applies the live set instead; a real
            // "nothing towed" reaches the original as nullptr (full clear)
            trailer = towed;
        }
        else if ( trailer != nullptr && readable_ptr( trailer ) && joint( trailer ) == nullptr )
        {
            // a jointless cable target: the +0xC8 head pick again — send the
            // lines to what's on the wheel, or drop them entirely (null =
            // the native "no trailer" uncable) when nothing is coupled
            trailer = ( front_is_truck && towed != nullptr && joint( towed ) != nullptr )
                ? towed : nullptr;
        }
        // the applier walks trailer_slave links without checking joints —
        // hide every live->dead link for the duration of the call so it can
        // only raise hoses across real couplings (restored right after;
        // parked->parked links stay, a parked section keeps its own lines)
        void* hidden_from[ max_trailers ] = {};
        void* hidden_link[ max_trailers ] = {};
        uint32_t hidden = 0;
        for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr && hidden < max_trailers; ++i )
        {
            void* t = canon[ i ];
            if ( !readable_ptr( t ) ||
                 !readable_ptr( static_cast< uint8_t* >( t ) + prism::offsets::trailer_physics_joint ) ||
                 joint( t ) == nullptr )
            {
                continue;
            }
            void* s = prism::offsets::field< void* >( t, prism::offsets::trailer_slave );
            if ( s != nullptr && readable_ptr( s ) &&
                 readable_ptr( static_cast< uint8_t* >( s ) + prism::offsets::trailer_physics_joint ) &&
                 joint( s ) == nullptr )
            {
                hidden_from[ hidden ] = t;
                hidden_link[ hidden ] = s;
                ++hidden;
                prism::offsets::field< void* >( t, prism::offsets::trailer_slave ) = nullptr;
            }
        }
        apply_cables_hook->get_original< vehicle_cables_fn >()( truck, trailer, unused, mode, zero );
        for ( uint32_t i = 0; i < hidden; ++i )
        {
            prism::offsets::field< void* >( hidden_from[ i ], prism::offsets::trailer_slave ) = hidden_link[ i ];
        }
    }

    // the native line-state setter (exe+0x875410): (vehicle, state, force) —
    // state=0 calls vfunc+0x10 (hide) on every +0xE98 line entity, state=1
    // calls vfunc+0x08 (show) gated by vehicle+0xE8E/+0x766. A trailer owns
    // the hoses of its FRONT edge, so a trailer whose front joint is alive
    // physically still has its hoses connected — a state=0 aimed at it is
    // the native detach cascade tearing down lines on trailers BEHIND the
    // released one. Refuse the hide while the joint lives; a real detach
    // drops the joint first, so legit teardown still lands a frame later
    void __fastcall hk_set_lines( void* vehicle, uint8_t state, uint8_t force )
    {
        static int sl_log = 0;
        const int vi = vehicle != nullptr ? canon_index_of( vehicle ) : -1;
        void* j = vehicle != nullptr &&
                  readable_ptr( static_cast< uint8_t* >( vehicle ) + prism::offsets::trailer_physics_joint )
                      ? joint( vehicle ) : nullptr;
        const bool is_canon = vi >= 0;
        if ( sl_log < 160 && is_canon )
        {
            ++sl_log;
            CCore::g_instance->info( "set_lines(c{}={:p} state={} force={}) joint={:p}",
                vi, vehicle, state, force, j );
        }
        if ( state == 0 && is_canon && j != nullptr )
        {
            if ( sl_log < 160 )
            {
                ++sl_log;
                CCore::g_instance->info( "set_lines: refused hide on jointed c{}", vi );
            }
            return;
        }
        set_lines_hook->get_original< line_state_fn >()( vehicle, state, force );
    }

    // rebuild_lines draws the air/electric lines between a front vehicle and
    // a trailer — the resolver walks the +0xC8 chain, and with the root
    // pinned on the canonical parked piece it draws hoses to (and between)
    // trailers that have no joint at all. Physical rule: a jointless
    // trailer carries no lines (they are literally disconnected), and a
    // jointed trailer's front is whatever it is really joined to — a stale
    // parent resolving to a parked trailer means the truck
    //
    // NOTE: this hook actually sits on pair_vehicles (exe+0x8785A0), whose
    // prologue DESTROYS the front vehicle's cable objects (+0xEF0/+0xF20
    // unregistered, the +0xEC0 entity vector released) and then recurses
    // down trailer+0x1088. The native detach calls it once on the broken
    // edge and the recursion tears down every section behind it — the hose
    // cascade. A jointless canon trailer as `front` only ever gets here via
    // that recursion (or the decoupled-event handler's (t, null) call):
    // while it still takes part in a live coupling — a jointed trailer arg
    // or a jointed slave of its own — its cable objects serve an INTACT
    // edge and the call must be refused. When the rear edge really breaks
    // (slave jointless too) the nullptr path below clears it natively.
    void __fastcall hk_rebuild_lines( void* front, void* trailer )
    {
        void* truck = nullptr;
        void* towed = nullptr;
        if ( auto* game_actor = CCore::g_instance->get_game_actor() )
        {
            truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
            towed = truck != nullptr && readable_ptr( truck )
                ? prism::offsets::field< void* >( truck, 0x15A8 ) : nullptr;
        }
        // the detach cascade: pair_vehicles destroys the FRONT's cable
        // objects (prologue) and also runs per-vehicle (t, null) sweeps over
        // parked sections. A front vehicle owns the cables of its REAR edge,
        // so any call on a parked-section member that still takes part in a
        // real coupling would tear down hoses whose edge never broke.
        // Refuse those; a real edge break always arrives with a jointless
        // trailer arg and falls through to the native nullptr cleanup
        const int fi = canon_index_of( front );
        if ( fi >= 0 && readable_ptr( front ) )
        {
            // live chain = towed head + everything reachable through
            // jointed slave links; parked section members are off it
            bool live = false;
            for ( void* p = towed; p != nullptr && readable_ptr( p ); )
            {
                if ( p == front ) { live = true; break; }
                if ( !readable_ptr( static_cast< uint8_t* >( p ) + prism::offsets::trailer_slave ) ) break;
                void* s = prism::offsets::field< void* >( p, prism::offsets::trailer_slave );
                if ( s == nullptr || !readable_ptr( s ) ||
                     !readable_ptr( static_cast< uint8_t* >( s ) + prism::offsets::trailer_physics_joint ) ||
                     joint( s ) == nullptr )
                {
                    break;
                }
                p = s;
            }
            if ( !live )
            {
                const bool front_coupled =
                    ( readable_ptr( static_cast< uint8_t* >( front ) + prism::offsets::trailer_physics_joint ) &&
                      joint( front ) != nullptr );
                void* fslave =
                    readable_ptr( static_cast< uint8_t* >( front ) + prism::offsets::trailer_slave )
                        ? prism::offsets::field< void* >( front, prism::offsets::trailer_slave ) : nullptr;
                const bool slave_intact = fslave != nullptr && readable_ptr( fslave ) &&
                    readable_ptr( static_cast< uint8_t* >( fslave ) + prism::offsets::trailer_physics_joint ) &&
                    joint( fslave ) != nullptr;
                const bool arg_live_edge = trailer != nullptr && readable_ptr( trailer ) &&
                    readable_ptr( static_cast< uint8_t* >( trailer ) + prism::offsets::trailer_physics_joint ) &&
                    joint( trailer ) != nullptr;
                if ( ( front_coupled || slave_intact ) && ( trailer == nullptr || arg_live_edge ) )
                {
                    return;
                }
            }
        }
        const bool t_jointed = trailer != nullptr && readable_ptr( trailer ) && joint( trailer ) != nullptr;
        static int rl_log = 0;
        if ( rl_log++ < 400 )
        {
            CCore::g_instance->info( "rebuild_lines(front={:p} trailer={:p} towed={:p}) t_jointed={} f_is_truck={}",
                front, trailer, towed, t_jointed, front == truck );
        }
        if ( trailer != nullptr && !t_jointed )
        {
            // the walk recurses down trailer+0x1088 — rebuilding on a parked
            // piece would draw hoses across the whole claimed section. null
            // is the native "nothing attached" input: it clears the front's
            // outgoing lines without drawing any — the phantom section
            // hoses (and stale coupled lines) die instead of persisting
            trailer = ( front == truck && towed != nullptr && joint( towed ) != nullptr )
                ? towed : nullptr;
        }
        if ( trailer == nullptr )
        {
            rebuild_lines_hook->get_original< prism::vehicle_rebuild_lines_fn >()( front, nullptr );
            return;
        }
        void* fixed_front = front;
        if ( front != nullptr && front != truck && readable_ptr( front ) && joint( front ) == nullptr )
        {
            fixed_front = ( trailer == towed ) ? truck : front;
        }
        rebuild_lines_hook->get_original< prism::vehicle_rebuild_lines_fn >()( fixed_front, trailer );
    }

    // the trailer-side joint release the native detach runs. The game's own
    // teardown also calls it on trailers whose physics joint is already gone
    // — it dereferences trailer+0xFE0->vtable without a check and dies on a
    // detached trailer left sitting at quit
    void __fastcall hk_pre_detach( void* self, void* trailer )
    {
        if ( trailer == nullptr ||
            prism::offsets::field< void* >( trailer, prism::offsets::trailer_physics_joint ) == nullptr )
        {
            return;
        }
        pre_detach_hook->get_original< prism::vehicle_pre_detach_fn >()( self, trailer );
    }

    // the native detach dispatcher (the T key's detach path). It releases the
    // joint, but the parent's per-frame connect_slave would re-couple the
    // trailer the very next frame because it still sits under the hook — the
    // same problem plugin detaches needed the detached registry for. Mark the
    // head trailer after every native detach that actually killed its joint
    void __fastcall hk_detach_dispatch_raw( void* self, uint8_t dl, uint64_t r8 );
    void __fastcall hk_detach_dispatch( void* self, uint8_t dl, uint64_t r8 )
    {
        // a fault on a respawned-away trailer (workshop/garage rebuilds kill
        // canon pointers mid-frame) must never reach the game — reset and
        // let the next pass rebuild from live state
        __try { hk_detach_dispatch_raw( self, dl, r8 ); }
        __except ( EXCEPTION_EXECUTE_HANDLER )
        {
            CCore::g_instance->warning( "detach dispatch faulted — trailer state reset" );
            reset_trailer_state();
        }
    }

    void __fastcall hk_detach_dispatch_raw( void* self, uint8_t dl, uint64_t r8 )
    {
        // a save load frees every trailer we cached — wipe before any mark
        // or dump touches a dead pointer
        if ( canon_dead() ) reset_trailer_state();
        // the trailer-of-interest slot may have drifted onto a nearby parked
        // trailer (proximity resolver, our own re-root). While the truck tows
        // something the ONLY sane detach candidate is the towed trailer —
        // force it so T always releases what's on the wheel
        void* truck = self != nullptr
            ? prism::offsets::field< void* >( self, prism::offsets::game_actor_truck ) : nullptr;
        void* towed = truck != nullptr && readable_ptr( truck )
            ? prism::offsets::field< void* >( truck, 0x15A8 ) : nullptr;
        if ( towed != nullptr && joint( towed ) != nullptr )
        {
            prism::offsets::field< void* >( self, prism::offsets::game_actor_trailer ) = towed;
        }
        dump_state( "pre-detach(T)" );
        snap_trailers();
        detach_dispatch_hook->get_original< native_detach_dispatch_fn >()( self, dl, r8 );
        dump_state( "post-detach(T)" );
        diff_trailers( "post-detach(T)" );
        diff_probe_frames = 90;
        if ( self == nullptr ) return;
        void* t = prism::offsets::field< void* >( self, prism::offsets::game_actor_trailer );
        if ( t != nullptr && joint( t ) == nullptr )
        {
            mark_detached( t );
            // NO slave re-link: parked trailers are independent pieces — each
            // stays visible and OFF through its own parked record. The slave
            // link between parked pieces only draws phantom hoses and makes
            // the parked machine treat them as one departing-prone section
        }
        // truck tows nothing → the canonical first parked trailer becomes
        // the trailer-of-interest again, so the T key attaches it like the
        // game's own head
        if ( truck != nullptr && readable_ptr( truck ) &&
            prism::offsets::field< void* >( truck, 0x15A8 ) == nullptr &&
            canon[ 0 ] != nullptr && readable_ptr( canon[ 0 ] ) && joint( canon[ 0 ] ) == nullptr )
        {
            prism::offsets::field< void* >( self, prism::offsets::game_actor_trailer ) = canon[ 0 ];
        }
    }

    // the native attach dispatcher (0x1405DCC20) — every native re-attach
    // funnels through it: the T key, the cargo load/unload set reassembly,
    // garage transfers, save-load restores. A trailer the plugin detached
    // is refused: wherever the player left it, it stays — the load/unload
    // sequence may not pull it back onto the fifth wheel or the chain.
    // Natively detached trailers are unaffected: T re-attach still works
    void __fastcall hk_attach_dispatch_raw( void* self, uint8_t dl, uint64_t r8, uint64_t r9,
        uint64_t arg5, const float* settle_override );
    void __fastcall hk_attach_dispatch( void* self, uint8_t dl, uint64_t r8, uint64_t r9,
        uint64_t arg5, const float* settle_override )
    {
        __try { hk_attach_dispatch_raw( self, dl, r8, r9, arg5, settle_override ); }
        __except ( EXCEPTION_EXECUTE_HANDLER )
        {
            CCore::g_instance->warning( "attach dispatch faulted — trailer state reset" );
            reset_trailer_state();
        }
    }

    void __fastcall hk_attach_dispatch_raw( void* self, uint8_t dl, uint64_t r8, uint64_t r9,
        uint64_t arg5, const float* settle_override )
    {
        if ( canon_dead() ) reset_trailer_state();
        // the user's chosen attach target beats the game's proximity pick:
        // whatever parked trailer they selected is the one T attaches —
        // but ONLY while that trailer is the jointless piece closest to
        // the truck. Forcing the slot when the player backed under a
        // DIFFERENT trailer aims the native attach at a piece that is
        // not under the hook, which no-ops forever (the stale-target
        // lockout). Closest wins = the piece you are actually under
        if ( attach_target != nullptr && self != nullptr )
        {
            if ( readable_ptr( attach_target ) && joint( attach_target ) == nullptr )
            {
                bool closest = true;
                hook_xform_t txf{};
                if ( truck_xform( self, reinterpret_cast< uint8_t* >( &txf ) ) )
                {
                    hook_xform_t ttf{};
                    float d2t = 1e30f;
                    if ( hook_world( attach_target, ttf ) )
                    {
                        const float dx = ttf.pos.x - txf.pos.x, dz = ttf.pos.z - txf.pos.z;
                        d2t = dx * dx + dz * dz;
                    }
                    for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
                    {
                        void* o = canon[ i ];
                        if ( o == attach_target || !readable_ptr( o ) || joint( o ) != nullptr )
                        {
                            continue;
                        }
                        hook_xform_t oxf{};
                        if ( !hook_world( o, oxf ) ) continue;
                        const float ddx = oxf.pos.x - txf.pos.x, ddz = oxf.pos.z - txf.pos.z;
                        if ( ddx * ddx + ddz * ddz < d2t )
                        {
                            closest = false;
                            break;
                        }
                    }
                }
                if ( closest )
                {
                    prism::offsets::field< void* >( self, prism::offsets::game_actor_trailer ) = attach_target;
                }
            }
            else
            {
                attach_target = nullptr;
            }
        }
        void* head = self != nullptr
            ? prism::offsets::field< void* >( self, prism::offsets::game_actor_trailer ) : nullptr;
        if ( head != nullptr && is_plugin_detached( head ) )
        {
            CCore::g_instance->info( "attach dispatcher skipped: trailer {:p} is plugin-detached", head );
            return;
        }
        // last-resort guard: a limbo trailer (no parked record AND no actor
        // in the game_actor slots) kills the game silently inside the native
        // attach — refuse the call instead. The maintenance re-registers
        // limbo pieces via vfunc +0x328, so this should never fire
        if ( attach_target != nullptr && head == attach_target &&
            readable_ptr( attach_target ) &&
            prism::offsets::field< void* >( attach_target, prism::offsets::trailer_parked_state ) == nullptr &&
            find_trailer_actor( attach_target, self ) == nullptr )
        {
            CCore::g_instance->warning( "attach refused: target {:p} has no record/actor (limbo)", attach_target );
            return;
        }
        dump_state( "pre-attach(T)" );
        attach_dispatch_hook->get_original< native_attach_dispatch_fn >()( self, dl, r8, r9, arg5, settle_override );
        dump_state( "post-attach(T)" );
        if ( attach_target != nullptr && joint( attach_target ) != nullptr )
        {
            CCore::g_instance->info( "attach target {:p} coupled natively", attach_target );
            attach_target = nullptr;
        }
        // the game attached a parked trailer through its own path (T, cargo
        // restore): clear its marks. The chain links are rebuilt every frame
        // by the refresh maintenance — the attached piece stays reachable
        // through the parked chain's bridge link, parked pieces stay first
        if ( head != nullptr && joint( head ) != nullptr )
        {
            unmark_detached( head );
            const int hi = canon_index_of( head );
            if ( hi >= 0 ) detached_idx[ hi ] = false;
            attach_target = nullptr;
            unhide_detached();
        }
    }

    // the trailer-state record apply (0x1405E14D1 on game_actor+0x10): copies
    // a recorded transform/velocity into the head trailer's state record and
    // reconciles its joint through the attach/detach dispatchers — the path
    // the cargo sequence uses to teleport the set into dock slots. A
    // plugin-detached trailer keeps its current record untouched: it stays
    // parked where the player left it
    using trailer_state_apply_fn = void __fastcall( void* /*record*/ );
    void __fastcall hk_trailer_state_apply( void* record )
    {
        void* trailer = record != nullptr
            ? prism::offsets::field< void* >( record, 0xB8 ) : nullptr;
        if ( trailer != nullptr && is_plugin_detached( trailer ) )
        {
            CCore::g_instance->info( "trailer state apply skipped: {:p} is plugin-detached", trailer );
            return;
        }
        trailer_state_apply_hook->get_original< trailer_state_apply_fn >()( record );
    }

    // the per-trailer teleport (0x140786320): the actor-teleport walks the
    // trailer list and calls this on each one to move it into its dock slot.
    // Trailer objects are NOT recreated — this is the call that actually
    // drags a detached trailer to the dock during cargo load/unload, where
    // it lands under its parent's hook and gets re-jointed. A
    // plugin-detached trailer is a world object: it is never teleported,
    // by any caller — dock alignment, garage, ferry, job start
    // per-sequence slot log: the actor teleport computes each member's
    // destination itself — but a member whose state record is missing
    // (prec=0, the slot the reconcile never gave it) falls back to the
    // ANCHOR slot, landing jointed trailers on top of each other. Snapshot
    // every canon trailer's position at sequence start, then re-seat any
    // member the game stacked onto its parent at its real pre-teleport
    // offset — preserving the whole formation, not guessing a distance
    struct tp_slot_t { void* t; float3_t dst; float3_t orig; };
    tp_slot_t tp_log[ 16 ] = {};
    int tp_n = 0;
    bool tp_active = false;
    float3_t tp_snap[ max_trailers ] = {};
    bool tp_snap_ok[ max_trailers ] = {};
    // frames the parked-anchor emulation stays off after an actor teleport —
    // the dock re-joint sequence leaves trailers jointless for a few frames
    // mid-move and the anchor would drag them back to the pre-teleport spot
    int tp_cooldown = 0;

    void __fastcall hk_trailer_teleport( void* self, void* transform )
    {
        if ( self != nullptr && is_plugin_detached( self ) )
        {
            CCore::g_instance->info( "trailer teleport skipped: {:p} is plugin-detached", self );
            return;
        }
        const float* dst = static_cast< const float* >( transform );
        hook_xform_t cur{};
        const bool have_cur = self != nullptr && hook_world( self, cur );
        // a jointed trailer can never occupy its parent's slot — same dst
        // means the game lost its member offset (missing state record).
        // NOTE: never write the game's transform buffer — the reconcile
        // reads it back to compute the next member's slot (a write once
        // dragged the head onto the slave's corrected slot). Run the
        // original first, then re-teleport through our OWN buffer at the
        // parent's final slot plus the trailer's snapshot offset — the
        // whole formation lands translated, exactly as it was assembled
        float3_t corrected{};
        bool do_fix = false;
        void* parent = self != nullptr
            ? prism::offsets::field< void* >( self, prism::offsets::trailer_parent ) : nullptr;
        if ( dst != nullptr && self != nullptr && joint( self ) != nullptr &&
             tp_active )
        {
            const int ti = canon_index_of( self );
            const int pi = canon_index_of( parent );
            // latest recorded slot for the parent (search back — the
            // reconcile calls each member more than once). Stacking is
            // detected against the parent's ORIGINAL destination — the
            // point the game actually sent it to — while the corrected
            // landing slot is what the child's own offset anchors on
            for ( int i = tp_n; i-- > 0; )
            {
                if ( tp_log[ i ].t != parent ) continue;
                const float ddx = dst[ 0 ] - tp_log[ i ].orig.x;
                const float ddz = dst[ 2 ] - tp_log[ i ].orig.z;
                // a later reconcile pass can land the parent on its real
                // slot while a member still arrives at the shared anchor —
                // matching the first logged destination catches that too
                const float hdx = dst[ 0 ] - tp_log[ 0 ].orig.x;
                const float hdz = dst[ 2 ] - tp_log[ 0 ].orig.z;
                if ( ( ddx * ddx + ddz * ddz < 1.f || hdx * hdx + hdz * hdz < 1.f ) &&
                     ti >= 0 && pi >= 0 && tp_snap_ok[ ti ] && tp_snap_ok[ pi ] )
                {
                    // Y comes from the game's own destination, never the
                    // origin formation delta — preserving the old snapshot's
                    // height difference bakes the ORIGIN terrain slope into
                    // the landing, and a member seated below the destination
                    // grade gets ejected by physics (the F9 bounce)
                    corrected = {
                        tp_log[ i ].dst.x + ( tp_snap[ ti ].x - tp_snap[ pi ].x ),
                        dst[ 1 ],
                        tp_log[ i ].dst.z + ( tp_snap[ ti ].z - tp_snap[ pi ].z ) };
                    do_fix = true;
                }
                break;
            }
        }
        if ( dst != nullptr )
        {
            CCore::g_instance->info(
                "trailer teleport {:p} ci={} joint={:p} parent={:p} slave={:p} "
                "cur=({:.1f},{:.1f},{:.1f}) dst=({:.1f},{:.1f},{:.1f}){}",
                self, canon_index_of( self ),
                self != nullptr ? joint( self ) : nullptr, parent,
                self != nullptr ? prism::offsets::field< void* >( self, prism::offsets::trailer_slave ) : nullptr,
                have_cur ? cur.pos.x : 0.f, have_cur ? cur.pos.y : 0.f, have_cur ? cur.pos.z : 0.f,
                dst[ 0 ], dst[ 1 ], dst[ 2 ],
                do_fix ? " [STACKED]" : "" );
            if ( tp_active && tp_n < 16 )
            {
                tp_log[ tp_n ].t = self;
                tp_log[ tp_n ].orig = { dst[ 0 ], dst[ 1 ], dst[ 2 ] };
                tp_log[ tp_n ].dst = do_fix ? corrected : tp_log[ tp_n ].orig;
                ++tp_n;
            }
        }
        // substitute OUR buffer for the game's transform — the trailer goes
        // straight to its real slot and never spends a frame overlapping
        // the parent at the anchor (that instant of penetration is the
        // residual bounce on landing). The reconcile re-reads the arg, and
        // the corrected slot is the right value for it to see anyway
        if ( do_fix )
        {
            hook_xform_t fix = *static_cast< const hook_xform_t* >( transform );
            fix.pos = corrected;
            trailer_teleport_hook->get_original< trailer_teleport_fn >()( self, &fix );
            CCore::g_instance->info( "trailer teleport {:p}: re-seated at corrected slot "
                "({:.1f},{:.1f},{:.1f})", self, corrected.x, corrected.y, corrected.z );
            return;
        }
        trailer_teleport_hook->get_original< trailer_teleport_fn >()( self, transform );
    }

    // the actor teleport (0x1405DE1E0): rotates/moves truck + attached set
    // into the dock slots for the cargo cinematic — the same call travel
    // teleports use. Snapshot the rig on the first call of a sequence, then
    // arm the restore only when the move was short (a dock shuffle, not a
    // ferry/garage/job hop across the map)
    using actor_teleport_fn = void __fastcall( void* /*actor*/, void* /*arg*/, uint8_t, uint8_t );
    void __fastcall hk_actor_teleport( void* self, void* arg, uint8_t a3, uint8_t a4 )
    {
        CCore::g_instance->info( "actor teleport enter a3={} a4={}", a3, a4 );
        tp_n = 0;
        tp_active = true;
        // snapshot every canon trailer's position — the correction math
        // needs the pre-teleport formation, and members get re-teleported
        // mid-sequence so a live read drifts between reconcile passes
        for ( int i = 0; i < max_trailers; ++i )
        {
            tp_snap_ok[ i ] = false;
            if ( canon[ i ] == nullptr || !readable_ptr( canon[ i ] ) ) continue;
            hook_xform_t xf{};
            if ( hook_world( canon[ i ], xf ) ) { tp_snap[ i ] = xf.pos; tp_snap_ok[ i ] = true; }
        }
        const auto settle = [&]()
        {
            // final formation pass — the game sends every member to the head's
            // anchor point and the reconcile can then move the head AGAIN,
            // stranding members at offsets measured from the slot the head
            // no longer occupies (F9 log: head re-seated 6 m after members
            // landed — the joint then drags them through the air). Seat each
            // jointed member at its parent's FINAL live position + the
            // pre-teleport formation offset — the whole formation follows
            // wherever the head actually ended. Y tracks the parent's live
            // height: the terrain at the destination is not the origin's
            for ( int i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
            {
                void* t = canon[ i ];
                // only members that rode along with the rig — a trailer
                // jointed to a PARKED parent never got teleported and must
                // keep its parked flags
                if ( !readable_ptr( t ) || joint( t ) == nullptr || !trailer_is_live( t ) ) continue;
                // a teleported member must not keep an armed parked record —
                // the re-assert would yank it back to the pre-teleport pose
                // every frame while physics holds it at the new spot
                prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked ) = 0;
                prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked_registered ) = 0;
                void* parent = prism::offsets::field< void* >( t, prism::offsets::trailer_parent );
                const int pi = canon_index_of( parent );
                if ( pi < 0 || !tp_snap_ok[ i ] || !tp_snap_ok[ pi ] ) continue;
                hook_xform_t pt{}, tt{};
                if ( !hook_world( parent, pt ) || !hook_world( t, tt ) ) continue;
                const float wx = pt.pos.x + ( tp_snap[ i ].x - tp_snap[ pi ].x );
                const float wz = pt.pos.z + ( tp_snap[ i ].z - tp_snap[ pi ].z );
                const float ex = tt.pos.x - wx;
                const float ez = tt.pos.z - wz;
                if ( ex * ex + ez * ez < 0.5f * 0.5f ) continue;
                hook_xform_t fix = tt;
                fix.pos.x = wx;
                fix.pos.y = pt.pos.y;
                fix.pos.z = wz;
                trailer_teleport_hook->get_original< trailer_teleport_fn >()( t, &fix );
                CCore::g_instance->info(
                    "teleport settle: {:p} re-seated at formation slot ({:.1f}m off)",
                    t, std::sqrt( ex * ex + ez * ez ) );
            }
        };
        if ( !settings::g_settings.cargo_restore )
        {
            rig_armed = false;
            actor_teleport_hook->get_original< actor_teleport_fn >()( self, arg, a3, a4 );
            settle();
            tp_active = false;
            tp_cooldown = 90;
            return;
        }
        const bool fresh = !rig_armed;
        if ( fresh )
        {
            snapshot_rig();
        }
        actor_teleport_hook->get_original< actor_teleport_fn >()( self, arg, a3, a4 );
        settle();
        tp_active = false;
        tp_cooldown = 90;
        if ( !fresh || !rig_truck_valid )
        {
            return;
        }
        if ( auto* game_actor = CCore::g_instance->get_game_actor() )
        {
            void* truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
            hook_xform_t now{};
            if ( truck != nullptr && hook_world( truck, now ) )
            {
                const float dx = now.pos.x - rig_truck_xform.pos.x;
                const float dz = now.pos.z - rig_truck_xform.pos.z;
                // the restore exists to re-park plugin-detached trailers the
                // dock sequence respawned into the set. With nothing detached
                // the arm would only replay the rig teleport ~60 frames later,
                // yanking a settled jointed combination mid-solve — camera-0/F9
                // launches were exactly this. No detached trailer → nothing
                // to restore → never arm
                bool any_detached = false;
                bool any_displaced = false;
                for ( int i = 0; i < max_trailers; ++i )
                {
                    if ( !detached_idx[ i ] ) continue;
                    any_detached = true;
                    // the dock sequence is the only caller that needs the
                    // replay — it respawns detached trailers INTO the set or
                    // scatters them off their parked marks. A travel/F9 hop
                    // leaves them parked exactly where they were, and arming
                    // here would replay the actor teleport ~60 frames later,
                    // yanking the whole rig back to the PRE-teleport spot
                    void* d = canon[ i ];
                    if ( d == nullptr || !readable_ptr( d ) ) continue;
                    if ( joint( d ) != nullptr ) { any_displaced = true; break; }
                    if ( parked_xform_valid[ i ] )
                    {
                        hook_xform_t dx2{};
                        if ( hook_world( d, dx2 ) )
                        {
                            const float mx = dx2.pos.x - parked_xform[ i ].pos.x;
                            const float mz = dx2.pos.z - parked_xform[ i ].pos.z;
                            if ( mx * mx + mz * mz > 2.f * 2.f ) { any_displaced = true; break; }
                        }
                    }
                }
                if ( any_detached && any_displaced && dx * dx + dz * dz < 200.f * 200.f )
                {
                    rig_armed = true;
                    rig_restore_delay = 60;
                    rig_replay_a3 = a3;
                    rig_replay_a4 = a4;
                    CCore::g_instance->info( "actor teleported ~{:.1f}m — rig restore armed",
                        std::sqrt( dx * dx + dz * dz ) );
                }
            }
        }
    }


    // resolves every pattern scanned / fixed-offset native helper used by the
    // coupling flow replicas; returns false when a hard requirement is missing
    bool resolve_native_fns()
    {
        if ( !expected_build() ) return false;
        const auto set_towed_address = memory::get_address_for_pattern( "48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 18 57 48 81 ec 80 00 00 00 48 8b f1 49 8b d8 48 8b 89 a8 15" );
        const auto clear_detached_address = memory::get_address_for_pattern( "40 57 41 56 48 83 ec 58 44 0f b6 f2 48 8b f9 3a 91 8d 0e 00 00 75" );
        const auto rebuild_lines_address = memory::get_address_for_pattern( "48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 40 48 8b fa 48 8b f1 e8 ? ? ? ? 48 8b ce e8 ? ? ? ? 48 8d 8e c0 0e 00 00 e8" );
        const auto distribute_mass_address = memory::get_address_for_pattern( "48 89 6c 24 18 57 48 83 ec 50 0f 29 74 24 40 48 8b f9 0f 57" );
        const auto refresh_lift_address = memory::get_address_for_pattern( "48 8b 91 c8 00 00 00 45 32 c0 48 85 d2 74 2c 4c 8b 8a c8 06" );
        const auto exec_line_address = memory::get_address_for_pattern( "40 55 41 54 41 55 48 8d ac 24 00 f4 ff ff 48 81 ec 00 0d 00 00 44 8b ea 4c 8b e1" );
        // the decoupled-state component notify + the truck-side joint release,
        // both from the native T detach flow
        const auto notify_decoupled_address = memory::get_address_for_pattern( "48 83 ec 48 80 b9 8e 0e 00 00 00 0f 84" );
        const auto pre_detach_address = memory::get_address_for_pattern( "48 85 d2 0f 84 ? ? ? ? 48 89 5c 24 08 57 48 83 ec 20 48 8b d9 48 8b fa 48 8b 8a e0 0f 00 00" );
        // accessory event refresh the native attach/detach fire on the trailer:
        // without it the parked trailer's traffic-obstacle anchor stays broken
        // and the traffic lane-modifier sweep dereferences a null owner
        const auto refresh_accessories_address = memory::get_address_for_pattern(
            "48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 40 48 8b fa 48 8b f1 e8 ? ? ? ? 48 8b ce e8 ? ? ? ? 48 8d 8e c0 0e 00 00 e8" );
        if ( notify_decoupled_address != 0 ) notify_decoupled_fn = reinterpret_cast< prism::vehicle_notify_decoupled_fn* >( notify_decoupled_address );
        if ( pre_detach_address != 0 )
        {
            pre_detach_fn = reinterpret_cast< prism::vehicle_pre_detach_fn* >( pre_detach_address );
            pre_detach_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
                "vehicle_pre_detach", pre_detach_address,
                reinterpret_cast< uint64_t >( &hk_pre_detach ) );
            if ( pre_detach_hook != nullptr )
            {
                pre_detach_hook->hook();
            }
        }
        if ( refresh_accessories_address != 0 ) refresh_accessories_fn = reinterpret_cast< prism::vehicle_refresh_accessories_fn* >( refresh_accessories_address );

        // the world/vehicle manager singleton + the parked-obstacle anchor
        // registration + the in-game state flag, all read out of the native
        // detach body (fixed image offsets for this build)
        get_cvar_float_fn = reinterpret_cast< prism::cvar_get_float_fn* >( memory::get_address_from_offset( 0x1401DC850 ) );
        g_traffic_cvar = reinterpret_cast< void* >( memory::get_address_from_offset( 0x142D50AB0 ) );
        traffic_manager_global = reinterpret_cast< void** >( memory::get_address_from_offset( 0x1436BAD90 ) );
        register_anchor_fn = reinterpret_cast< prism::traffic_register_anchor_fn* >( memory::get_address_from_offset( 0x140285540 ) );
        relink_anchors_fn = reinterpret_cast< anchor_relink_fn* >( memory::get_address_from_offset( 0x140625090 ) );
        anchor_reg_guard = reinterpret_cast< uint32_t* >( memory::get_address_from_offset( 0x14305CDE4 ) );
        traffic_game_state = reinterpret_cast< uint32_t* >( memory::get_address_from_offset( 0x14305CDE0 ) );

        // the remaining native attach/detach calls (see declarations above)
        trailer_settle_fn_ptr = reinterpret_cast< trailer_settle_fn* >( memory::get_address_from_offset( 0x1409FCB00 ) );
        pair_vehicles_fn = reinterpret_cast< vehicle_pair_fn* >( memory::get_address_from_offset( 0x1408785A0 ) );
        apply_cables_fn = reinterpret_cast< vehicle_cables_fn* >( memory::get_address_from_offset( 0x1408776E0 ) );
        set_lines_fn = reinterpret_cast< line_state_fn* >( memory::get_address_from_offset( 0x140875410 ) );
        parked_record_factory = reinterpret_cast< parked_record_factory_fn* >( memory::get_address_from_offset( 0x1407B7DE0 ) );
        parked_record_mgr = reinterpret_cast< void** >( memory::get_address_from_offset( 0x1436CADF0 ) );
        apply_cables_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "apply_cables", memory::get_address_from_offset( 0x1408776E0 ),
            reinterpret_cast< uint64_t >( &hk_apply_cables ) );
        if ( apply_cables_hook != nullptr )
        {
            apply_cables_hook->hook();
        }
        // the line-state setter — refuse a hide aimed at a trailer whose
        // front joint is still bolted (the detach cascade)
        set_lines_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "set_lines", memory::get_address_from_offset( 0x140875410 ),
            reinterpret_cast< uint64_t >( &hk_set_lines ) );
        if ( set_lines_hook != nullptr )
        {
            set_lines_hook->hook();
        }
        actor_flag_fixup_fn = reinterpret_cast< actor_flag_fixup* >( memory::get_address_from_offset( 0x1405DDFC0 ) );
        native_attach_fn = reinterpret_cast< native_attach_dispatch_fn* >( memory::get_address_from_offset( 0x1405DE020 ) );
        detach_dispatch_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "native_detach_dispatch", memory::get_address_from_offset( 0x1405DE810 ),
            reinterpret_cast< uint64_t >( &hk_detach_dispatch ) );
        if ( detach_dispatch_hook != nullptr )
        {
            detach_dispatch_hook->hook();
        }
        // the attach dispatcher + the trailer-state apply: the two funnels
        // every native re-join/reposition of a trailer goes through — hooked
        // so a plugin-detached trailer survives cargo load/unload untouched
        attach_dispatch_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "native_attach_dispatch", memory::get_address_from_offset( 0x1405DE020 ),
            reinterpret_cast< uint64_t >( &hk_attach_dispatch ) );
        if ( attach_dispatch_hook != nullptr )
        {
            attach_dispatch_hook->hook();
        }
        trailer_state_apply_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "trailer_state_apply", memory::get_address_from_offset( 0x1405E28D1 ),
            reinterpret_cast< uint64_t >( &hk_trailer_state_apply ) );
        if ( trailer_state_apply_hook != nullptr )
        {
            trailer_state_apply_hook->hook();
        }
        trailer_teleport_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "trailer_teleport", memory::get_address_from_offset( 0x140787720 ),
            reinterpret_cast< uint64_t >( &hk_trailer_teleport ) );
        if ( trailer_teleport_hook != nullptr )
        {
            trailer_teleport_hook->hook();
        }
        actor_teleport_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "actor_teleport", memory::get_address_from_offset( 0x1405DF5E0 ),
            reinterpret_cast< uint64_t >( &hk_actor_teleport ) );
        if ( actor_teleport_hook != nullptr )
        {
            actor_teleport_hook->hook();
        }
        // chase-camera anchor setter (base vfunc 31): the resolver picks the
        // chain-end trailer, jointless ones included — redirect inside the
        // setter so the native flow anchors the last JOINTED trailer
        cam_set_target_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "cam_set_target", memory::get_address_from_offset( 0x14063D6D0 ),
            reinterpret_cast< uint64_t >( &hk_cam_set_target ) );
        if ( cam_set_target_hook != nullptr )
        {
            cam_set_target_hook->hook();
        }
        // chase-camera update (vfunc 32): hides dead-joint slaves from its
        // chain walk for the duration of the call only — a permanent cut
        // despawns the parked trailer
        cam_update_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "cam_update", memory::get_address_from_offset( 0x140AC89A0 ),
            reinterpret_cast< uint64_t >( &hk_cam_update ) );
        if ( cam_update_hook != nullptr )
        {
            cam_update_hook->hook();
        }
        // lane-obstacle insert: dead array entries (owner-node cleared without
        // removal) crash the traffic worker — sanitize before the original
        lane_obstacle_insert_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "lane_obstacle_insert", memory::get_address_from_offset( 0x14094F3C0 ),
            reinterpret_cast< uint64_t >( &hk_lane_obstacle_insert ) );
        if ( lane_obstacle_insert_hook != nullptr )
        {
            lane_obstacle_insert_hook->hook();
        }
        // purecall handler: dead listener nodes on detached trailers call
        // through pure vfunc slots — swallow every one of them, process-wide
        purecall_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "purecall", memory::get_address_from_offset( 0x141D24910 ),
            reinterpret_cast< uint64_t >( &hk_purecall ) );
        if ( purecall_hook != nullptr )
        {
            purecall_hook->hook();
        }
        // chain-length accumulator: the reimplementation stops the walk at
        // the first jointless (detached) trailer so every consumer — camera 2
        // included — measures only the live combination. The per-link hook
        // query it calls is resolved separately; if it fails to resolve the
        // hook falls back to the untouched original.
        combo_hook_pos_fn_ptr = reinterpret_cast< combo_hook_pos_fn* >(
            memory::get_address_from_offset( 0x14078AC80 ) );
        combo_length_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "combo_length", memory::get_address_from_offset( 0x14078AEB0 ),
            reinterpret_cast< uint64_t >( &hk_combo_length ) );
        if ( combo_length_hook != nullptr )
        {
            combo_length_hook->hook();
        }
        // lamp-mask setter: keeps parked trailers electrically dark while
        // they stay claimed in the attach tree for visibility
        lamp_mask_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "lamp_mask", memory::get_address_from_offset( 0x1409DC8B0 ),
            reinterpret_cast< uint64_t >( &hk_lamp_mask ) );
        if ( lamp_mask_hook != nullptr )
        {
            lamp_mask_hook->hook();
        }
        // joint-tree recursion guard: the accumulator walks node+0x68 chains
        // via a self-recursive vfunc — a cyclic graph overflows the stack
        // instantly (0xC00000FD). The detour returns early on a revisit
        joint_walk_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "joint_walk", memory::get_address_from_offset( 0x1409569F0 ),
            reinterpret_cast< uint64_t >( &hk_joint_walk ) );
        if ( joint_walk_hook != nullptr )
        {
            joint_walk_hook->hook();
        }
        // bidirectional joint link — refuses only the edge that would
        // close a +0x60/+0x68 cycle (find_root has no guard and hangs)
        phys_link_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "phys_link", memory::get_address_from_offset( 0x14091AA80 ),
            reinterpret_cast< uint64_t >( &hk_phys_link ) );
        if ( phys_link_hook != nullptr )
        {
            phys_link_hook->hook();
        }
        get_cvar_int_fn = reinterpret_cast< cvar_get_int_fn* >( memory::get_address_from_offset( 0x1401CD100 ) );
        cables_mode_cvar = reinterpret_cast< void* >( memory::get_address_from_offset( 0x142D53B70 ) );
        attach_settle_cvar = reinterpret_cast< void* >( memory::get_address_from_offset( 0x142D537B0 ) );

        // the coupling/decoupling event poster + its const param block and the
        // two token strings, all fixed image offsets read out of the native
        // attach/detach call sites
        post_event_fn_ptr = reinterpret_cast< post_event_fn* >( memory::get_address_from_offset( 0x1406251A0 ) );
        event_param_block = reinterpret_cast< void* >( memory::get_address_from_offset( 0x14273E748 ) );
        event_name_coupling = reinterpret_cast< const char* >( memory::get_address_from_offset( 0x142270C50 ) );
        event_name_decoupling = reinterpret_cast< const char* >( memory::get_address_from_offset( 0x1422706C0 ) );
        brace_anim_fn = reinterpret_cast< trailer_brace_anim_fn* >( memory::get_address_from_offset( 0x140638350 ) );

        if ( exec_line_address != 0 )
        {
            execute_line_fn = reinterpret_cast< prism::console_execute_line_fn* >( exec_line_address );
        }

        if ( set_towed_address != 0 && clear_detached_address != 0 && rebuild_lines_address != 0 &&
            distribute_mass_address != 0 && refresh_lift_address != 0 )
        {
            set_towed_trailer_fn = reinterpret_cast< prism::vehicle_set_towed_trailer_fn* >( set_towed_address );
            clear_detached_flag_fn = reinterpret_cast< prism::vehicle_clear_detached_flag_fn* >( clear_detached_address );
            rebuild_lines_fn = reinterpret_cast< prism::vehicle_rebuild_lines_fn* >( rebuild_lines_address );
            rebuild_lines_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
                "rebuild_lines", rebuild_lines_address,
                reinterpret_cast< uint64_t >( &hk_rebuild_lines ) );
            if ( rebuild_lines_hook != nullptr ) rebuild_lines_hook->hook();
            distribute_towed_mass_fn = reinterpret_cast< prism::vehicle_distribute_towed_mass_fn* >( distribute_mass_address );
            refresh_axle_lift_fn = reinterpret_cast< prism::game_actor_refresh_axle_lift_fn* >( refresh_lift_address );
            return true;
        }
        CCore::g_instance->warning( "Native attach functions not found; the attached trailer may stay braked" );
        return false;
    }

    //========================================================================
    // Per-frame update — chain refresh, convoy state, attach hotkeys
    //========================================================================

    // rebuilds trailers[] from game_actor and merges in truck_towed: a spliced
    // trailer no longer appears in the chain walk, but keeps being listed so
    // the UI and hotkeys can still reach it while it sits on the truck
    void CTrailerManipulation::refresh_trailer_chain()
    {
        // world reload: every cached trailer pointer is dead — start clean
        if ( canon_dead() ) reset_trailer_state();

        auto* game_actor = CCore::g_instance->get_game_actor();
        void* truck = game_actor != nullptr
            ? prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck )
            : nullptr;

        // two roots carry trailers: the live towed chain (truck+0x15A8) and
        // the trailer-of-interest chain (+0xC8) which also covers parked
        // pieces. Merge both walks, deduped, then append every canon member
        // that fell outside both — a parked trailer must stay in the list or
        // the attach keys can never reach it
        trailer_count = 0;
        const auto walk = [ & ]( void* head )
        {
            for ( void* t = head; t != nullptr && trailer_count < max_trailers; )
            {
                bool dup = false;
                for ( uint32_t i = 0; i < trailer_count; ++i ) dup |= trailers[ i ] == t;
                if ( dup ) return;
                trailers[ trailer_count++ ] = t;
                track_canon( t );
                void* next = prism::offsets::field< void* >( t, prism::offsets::trailer_slave );
                t = ( next != t ) ? next : nullptr;
            }
        };
        walk( truck != nullptr && readable_ptr( truck )
            ? prism::offsets::field< void* >( truck, 0x15A8 ) : nullptr );
        walk( game_actor != nullptr
            ? prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_trailer )
            : nullptr );
        for ( void* s : canon )
        {
            if ( s == nullptr || !readable_ptr( s ) ) continue;
            bool listed = false;
            for ( uint32_t i = 0; i < trailer_count; ++i ) listed |= trailers[ i ] == s;
            if ( !listed && trailer_count < max_trailers ) trailers[ trailer_count++ ] = s;
        }

        // the trailer parked on the truck through "connect to truck" is free
        // again once its joint is gone — release the coupling-state pin and
        // the towed slot too or the machine stays stuck at "coupled" and
        // refuses new attaches. But don't release while a connect op is still
        // queued: truck_towed is assigned at queue time, before the joint
        // exists
        if ( truck_towed != nullptr && joint( truck_towed ) == nullptr &&
            !( pending.kind == pending_kind::connect && pending.trailer == truck_towed ) )
        {
            if ( auto* game_actor = CCore::g_instance->get_game_actor() )
            {
                void* truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
                if ( truck != nullptr )
                {
                    if ( prism::offsets::field< void* >( truck, 0x15A8 ) == truck_towed &&
                        set_towed_trailer_fn != nullptr )
                    {
                        set_towed_trailer_fn( truck, true, nullptr );
                    }
                    prism::offsets::field< uint32_t >( truck, prism::offsets::truck_coupling_state ) = 0;
                }
            }
            truck_towed = nullptr;
        }

        if ( truck_towed != nullptr )
        {
            bool listed = false;
            for ( uint32_t i = 0; i < trailer_count; ++i ) listed |= trailers[ i ] == truck_towed;
            if ( !listed && trailer_count < max_trailers ) trailers[ trailer_count++ ] = truck_towed;
        }

        // promotion ordering: the trailer on the fifth wheel leads the list
        // and the rest follow in canonical order — attach trailer 1 and the
        // list becomes [1, 2, 0]; detach and it falls back to canonical
        // [0, 1, 2] because nothing is towed
        if ( trailer_count > 1 && truck != nullptr && readable_ptr( truck ) )
        {
            void* head = prism::offsets::field< void* >( truck, 0x15A8 );
            int hp = -1, cn = 0;
            for ( void* s : canon ) { if ( s == nullptr ) break; if ( s == head ) hp = cn; ++cn; }
            if ( head != nullptr && hp >= 0 && cn > 1 )
            {
                void* rot[ max_trailers ] = {};
                uint32_t rc = 0;
                for ( int k = 0; k < cn && rc < trailer_count; ++k )
                {
                    void* s = canon[ ( hp + k ) % cn ];
                    for ( uint32_t i = 0; i < trailer_count; ++i )
                    {
                        if ( trailers[ i ] == s ) { rot[ rc++ ] = s; break; }
                    }
                }
                for ( uint32_t i = 0; i < trailer_count && rc < max_trailers; ++i )
                {
                    bool in = false;
                    for ( uint32_t k = 0; k < rc; ++k ) in |= rot[ k ] == trailers[ i ];
                    if ( !in ) rot[ rc++ ] = trailers[ i ];
                }
                for ( uint32_t i = 0; i < rc; ++i ) trailers[ i ] = rot[ i ];
            }
        }

        if ( attach_target != nullptr &&
            ( !readable_ptr( attach_target ) || joint( attach_target ) != nullptr ) )
        {
            attach_target = nullptr;
        }
        // +0xC8 chain maintenance — a trailer is only visible when it is
        // reachable through trailer_slave links from the +0xC8 root; the
        // parked record keeps the pose, not the actor. ONE shape covers
        // every state — the canonical forward chain rooted at canon[0].
        // Forward links are the only direction the parked machine itself
        // ever claims, so our writes can never contradict a native writer,
        // and a chain that only points forward is acyclic by construction:
        // the fatal solver recursion (0xC00000FD at exe+0x956A87 — a
        // self-recursive vfunc walking the node+0x68 joint tree) needed a
        // BACKWARD leaf (towed -> canonically-earlier parked) to meet the
        // machine's forward claim and close a two-node loop
        {
            void* live[ max_trailers ] = {}; uint32_t ln = 0;
            bool jl[ max_trailers ] = {}, parkedish[ max_trailers ] = {};
            {
                void* truck = nullptr;
                if ( auto* game_actor = CCore::g_instance->get_game_actor() )
                {
                    truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
                }
                for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
                {
                    void* t = canon[ i ];
                    if ( t == nullptr || !plausible_object( t ) ) continue;
                    live[ ln ] = t;
                    jl[ ln ] = joint( t ) == nullptr;
                    // "live" means the joint chain bottoms out at the truck —
                    // a trailer still bolted to a PARKED parent belongs to the
                    // parked set, and treating it as live makes the tail
                    // append write a backward claim into its own parent
                    bool live_to_truck = false;
                    for ( void* cur = t; cur != nullptr && joint( cur ) != nullptr; )
                    {
                        void* p = prism::offsets::field< void* >( cur, prism::offsets::trailer_parent );
                        if ( p == truck ) { live_to_truck = true; break; }
                        if ( p == nullptr || p == cur || !readable_ptr( p ) ) break;
                        cur = p;
                    }
                    // every piece that is not live is a parked piece of ours
                    // and must stay reachable — a trailer parked by a plain
                    // T-detach carries no marks and only the head gets a
                    // parked record, so gating on marks/records drops the
                    // rest of the parked set off the chain and it vanishes
                    parkedish[ ln ] = !live_to_truck;
                    ++ln;
                }
            }
            // stale lines on jointless pieces — kill them through the
            // NATIVE line-state setter (exe+0x875410): fn(vehicle, state,
            // force). state=0 tears down every line entity in the
            // trailer's +0xE98..+0xEA0 list via the real destroy vfunc and
            // clears the +0xE8D "lines connected" flag — the parked piece
            // kept both from its last coupling and no field write kills
            // the recreated hose, only this transition does. Gate the call
            // on actual stale state so it is a no-op once clean
            for ( uint32_t i = 0; i < ln; ++i )
            {
                if ( !jl[ i ] ) continue;
                prism::offsets::field< uint32_t >( live[ i ], 0xEE8 ) = 0;
                if ( set_lines_fn == nullptr ) continue;
                char* const tl = static_cast< char* >( live[ i ] );
                const bool flagged = *reinterpret_cast< uint8_t* >( tl + 0xE8D ) != 0;
                const bool entities = *reinterpret_cast< void** >( tl + 0xE98 ) !=
                    *reinterpret_cast< void** >( tl + 0xEA0 );
                if ( flagged || entities ) set_lines_fn( live[ i ], 0, 1 );
            }
            bool any_parked = false;
            for ( uint32_t i = 0; i < ln; ++i ) any_parked |= parkedish[ i ];
            if ( any_parked )
            {
                void* truck = nullptr;
                void* towed = nullptr;
                if ( auto* game_actor = CCore::g_instance->get_game_actor() )
                {
                    truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
                    towed = truck != nullptr && readable_ptr( truck )
                        ? prism::offsets::field< void* >( truck, 0x15A8 ) : nullptr;
                }
                const bool towing = towed != nullptr && joint( towed ) != nullptr;
                static uint64_t maint_sig = 0;
                uint64_t sig = ln | ( towing ? 0x100 : 0 ) |
                    ( attach_target != nullptr ? 0x200 : 0 );
                for ( uint32_t i = 0; i < ln; ++i ) sig = sig * 131 + ( jl[ i ] ? 1 : 2 ) + ( parkedish[ i ] ? 4 : 0 );
                const bool edge = sig != maint_sig;
                if ( edge )
                {
                    CCore::g_instance->info( "maint: ln={} towing={} target={:p} jl={}{}",
                        ln, towing, static_cast< void* >( attach_target ), jl[ 0 ], ln > 1 ? jl[ 1 ] : false );
                    maint_sig = sig;
                }
                // the writes: while the truck tows nothing, every piece's
                // slave = its canonical successor — the parked machine's
                // own claim shape, which keeps every parked piece
                // reachable and visible. Written tail-to-head so no
                // intermediate state passes through a cycle either.
                // WHILE TOWING the model changes: the trailer on the
                // fifth wheel IS the head — parked pieces claim nothing
                // (a claim is what made the game treat the parked piece
                // as "the truck's trailer": hoses, markers, the wrong-T
                // detach — all of it followed the claim root). Jointed
                // pieces keep whatever slave the native chain gave them
                for ( uint32_t i = ln; i-- > 0; )
                {
                    void** slave = &prism::offsets::field< void* >( live[ i ], prism::offsets::trailer_slave );
                    if ( towing )
                    {
                        // parked pieces stay reachable by hanging at the
                        // TAIL of the live chain, in canonical order —
                        // the only root that renders them is +0xC8, which
                        // now sits on the towed trailer, so the chain
                        // must run live-first, parked-after. A parked
                        // piece itself claims nothing past its canonical
                        // neighbour — the leaf direction can never point
                        // back into the live chain
                        continue;
                    }
                    void* want = ( i + 1 < ln ) ? live[ i + 1 ] : nullptr;
                    if ( *slave != want ) *slave = want;
                }
                if ( towing )
                {
                    // parked pieces hang after the last piece whose joint
                    // chain reaches the truck — a member bolted to a parked
                    // parent is parked too and appending after it writes a
                    // backward claim into its own parent
                    void* last_live = nullptr;
                    for ( uint32_t i = 0; i < ln; ++i )
                        if ( !parkedish[ i ] ) last_live = live[ i ];
                    void* prev = last_live != nullptr ? last_live : towed;
                    bool appended = false;
                    for ( uint32_t i = 0; i < ln; ++i )
                    {
                        if ( !parkedish[ i ] ) continue;
                        void** sl = &prism::offsets::field< void* >( prev, prism::offsets::trailer_slave );
                        if ( *sl != live[ i ] ) *sl = live[ i ];
                        prev = live[ i ];
                        appended = true;
                    }
                    if ( appended )
                    {
                        void** tail = &prism::offsets::field< void* >( prev, prism::offsets::trailer_slave );
                        if ( *tail != nullptr ) *tail = nullptr;
                    }
                }
                // +0xC8 = the real head: the trailer physically on the
                // fifth wheel while towing, the canonical parked root
                // otherwise (that is what the attach marker/T-attach
                // resolver needs when nothing is coupled)
                if ( auto* game_actor = CCore::g_instance->get_game_actor() )
                {
                    prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_trailer ) =
                        towing ? towed : live[ 0 ];
                }
                if ( edge ) CCore::g_instance->info( "maint: canonical chain kept" );
                // cycle guard — whatever wrote the links, a slave chain
                // that revisits a piece makes a concurrent native walker
                // spin forever; sever the link that closes the loop
                for ( uint32_t i = 0; i < ln; ++i )
                {
                    void* seen[ max_trailers ] = {}; uint32_t sn = 0;
                    void* prev = nullptr;
                    for ( void* t = live[ i ]; t != nullptr; )
                    {
                        bool dup = false;
                        for ( uint32_t k = 0; k < sn; ++k ) dup |= seen[ k ] == t;
                        if ( dup )
                        {
                            if ( prev != nullptr && readable_ptr( prev ) )
                            {
                                CCore::g_instance->warning( "maint: severed cyclic slave link {:p}->{:p}", prev, t );
                                prism::offsets::field< void* >( prev, prism::offsets::trailer_slave ) = nullptr;
                            }
                            break;
                        }
                        seen[ sn++ ] = t;
                        prev = t;
                        if ( !readable_ptr( t ) ||
                            !readable_ptr( static_cast< uint8_t* >( t ) + prism::offsets::trailer_slave ) )
                        {
                            break;
                        }
                        t = prism::offsets::field< void* >( t, prism::offsets::trailer_slave );
                    }
                }
                // keep parked pieces in their true detached state and the
                // parked registration asserted — the parked machine tears
                // it down whenever a section member departs
                for ( uint32_t i = 0; i < ln; ++i )
                {
                    if ( !parkedish[ i ] ) continue;
                    void* t = live[ i ];
                    if ( prism::offsets::field< void* >( t, prism::offsets::trailer_parked_state ) != nullptr )
                    {
                        prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked ) = 1;
                        prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked_registered ) = 1;
                    }
                    else if ( plausible_vfunc( t, 0x328 / 8 ) )
                    {
                        // limbo piece: marked/target but no parked record —
                        // the section teardown destroyed it (the stale
                        // canonical claim made the trailer a "member" of the
                        // head's section at detach time). Re-run the native
                        // park registration — it recreates the record and
                        // the section actor; a limbo trailer has neither
                        // and kills the game on the next native attach
                        vcall< prism::physics_trailer_u_set_trailer_brace_fn >( t, 0x328 / 8, true );
                        static void* reg_logged[ 16 ] = {};
                        bool seen = false;
                        for ( auto* s : reg_logged ) seen |= s == t;
                        if ( !seen )
                        {
                            for ( auto& s : reg_logged )
                                if ( s == nullptr ) { s = t; break; }
                            CCore::g_instance->info( "maint: re-registered limbo trailer {:p}", t );
                        }
                    }
                }
            }
        }

        // cache each trailer's actor while it is still registered: the slots
        // only hold convoy members, so the pointer must be captured before a
        // detach or the parked trailer's traffic anchor can never be updated
        if ( auto* game_actor = CCore::g_instance->get_game_actor() )
        {
            for ( uint32_t i = 0; i < trailer_count; ++i )
            {
                cache_trailer_actor( trailers[ i ], game_actor );
            }
        }
    }

    // the chase camera object lives in the camera-manager slot array; find it
    // by its vehicle_behind_rotation_camera vtable (exe+0x2314B60 on ATS
    // 1.61.3.18). The object is NOT stable across cinematics — the delivery
    // cutscene rebuilds the camera list, and a pointer cached mid-cutscene
    // is freed memory a second later — so a cached hit still re-validates
    // the vtable before use and falls through to a fresh scan on mismatch.
    void* chase_camera_obj()
    {
        if ( !expected_build() ) return nullptr;
        const auto base = reinterpret_cast< uint64_t >( GetModuleHandle( nullptr ) );
        const auto chase_vtbl = reinterpret_cast< void* >( base + 0x2314B60 );
        auto is_chase = [ & ]( void* p )
        {
            return p != nullptr && readable_ptr( p ) &&
                *reinterpret_cast< void** >( p ) == chase_vtbl;
        };
        if ( chase_camera != nullptr )
        {
            if ( is_chase( chase_camera ) ) return chase_camera;
            chase_camera = nullptr;   // stale — object was destroyed/reused
        }
        auto* manager = *reinterpret_cast< void** >( base + 0x36BAE00 );
        if ( !readable_ptr( manager ) ) return nullptr;   // plain struct, no vtable
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
    // TRUCK — +0x3F0 = null is the native "no trailer" state (same as
    // driving solo), so the camera ignores the whole combination. The store
    // is a raw write with NO refcounting: the native resolver writes this
    // field directly too, and releasing the previous value is what
    // underflowed refcounts and froze the camera after recoupling. With the
    // full set coupled the chain head (first trailer — the native pick) is
    // restored through the real setter once and everything returns to native.
    void enforce_chase_camera_anchor()
    {
        static bool override_active = false;
        static float saved_zoom_min = 0.0f;   // camera +0x480 as shipped by the game
        static float zoom_dist = -1.0f;       // wheel-driven distance, <0 = inactive
        static uint32_t last_det_recs = 0;    // detached records at last recompute
        static uint32_t last_jointless = 0;   // jointless trailers at last recompute
        static bool was_detached = false;     // detach edge tracker
        static void* last_forced_anchor = reinterpret_cast< void* >( ~0ull );

        bool any_detached = false;
        for ( uint32_t i = 0; i < trailer_count; ++i )
        {
            void* t = trailers[ i ];
            if ( t != nullptr && joint( t ) == nullptr )
            {
                any_detached = true;
                break;
            }
        }
        for ( void* d : detached_trailers )
        {
            if ( d != nullptr ) any_detached = true;
        }

        const int wheel = CCore::g_instance->consume_in_game_wheel();
        static int last_det_state = -1;
        if ( static_cast< int >( any_detached ) != last_det_state )
        {
            last_det_state = any_detached;
            void* chase_dbg = chase_camera_obj();
            void* anch_dbg = nullptr;
            if ( chase_dbg != nullptr && readable_ptr( chase_dbg ) )
                anch_dbg = prism::offsets::field< void* >( chase_dbg, 0x3F0 );
            uint32_t det_recs = 0;
            for ( const void* d : detached_trailers ) if ( d != nullptr ) ++det_recs;
            CCore::g_instance->info(
                "cam: detached={} chase={:p} anchor={:p} trailers={} detached_recs={}",
                any_detached, chase_dbg, anch_dbg, trailer_count, det_recs );
        }
        if ( !any_detached && !override_active ) return;   // pure native behaviour
        void* chase = chase_camera_obj();
        // vfunc 14 (recompute) is called on transitions — a camera mid-
        // rebuild (delivery cinematic) would carry a dead slot target
        if ( chase == nullptr || !plausible_vfunc( chase, 14 ) )
        {
            static bool miss_logged = false;
            if ( !miss_logged )
            {
                miss_logged = true;
                CCore::g_instance->info( "cam: enforce skipped — chase={:p} vfunc14={}",
                    chase, chase != nullptr && plausible_vfunc( chase, 14 ) );
            }
            chase_camera = nullptr;
            return;
        }

        void*& anchor = prism::offsets::field< void* >( chase, 0x3F0 );
        // desired anchor = the trailer JOINTED TO THE TRUCK — the head of
        // the live chain. A trailer still jointed to a PARKED parent (the
        // pair stayed linked after a full detach) must not count: walk
        // parents up to the truck instead of testing joint() alone.
        // The camera measures the combination by walking trailer_slave
        // FROM the anchor and hk_combo_length stops at the first dead
        // joint, so it parks behind the last attached trailer.
        // Null = native solo state.
        void* chain_head = nullptr;
        {
            auto* game_actor = CCore::g_instance->get_game_actor();
            void* truck = game_actor != nullptr && readable_ptr( game_actor )
                ? prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck )
                : nullptr;
            if ( truck != nullptr && readable_ptr( truck ) )
            {
                // truck+0x15A8 is the trailer physically coupled to the
                // truck — authoritative chain head (the dump's truck_towed)
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
            // the set detaches — trailer 0 stays the head — so a pure
            // anchor-change check never fires and the camera keeps the
            // distance it measured for the full combination. Any change in
            // the live set (detach edge or another trailer leaving) needs a
            // recompute: vfunc 14 re-measures the combination through
            // hk_combo_length, which now stops at the first dead joint.
            uint32_t det_recs_now = 0;
            for ( const void* d : detached_trailers ) if ( d != nullptr ) ++det_recs_now;
            uint32_t jointless_now = 0;
            for ( uint32_t i = 0; i < trailer_count; ++i )
                if ( trailers[ i ] != nullptr && joint( trailers[ i ] ) == nullptr ) ++jointless_now;
            const bool set_changed = !was_detached || det_recs_now != last_det_recs ||
                jointless_now != last_jointless;
            // the native resolver re-writes the anchor to a parked trailer
            // every frame while anything sits detached — we must keep
            // re-storing chain_head, but vfunc 14 (recompute) RESETS the
            // orbit state: calling it per frame makes camera 2 snap back
            // behind the truck the moment the user tries to orbit. Recompute
            // only when the desired anchor itself changes
            if ( anchor != chain_head )
            {
                anchor = chain_head;   // raw store, no release — see comment above
                if ( last_forced_anchor != chain_head )
                {
                    // recompute (vfunc 14) re-reads the anchor's joint state —
                    // solo set when null, jointed set for a live chain head
                    vcall< cam_recompute_fn >( chase, 14, uint8_t( 1 ) );
                    last_forced_anchor = chain_head;
                    CCore::g_instance->info( "cam: anchor -> {:p} (live chain head)", chain_head );
                }
            }
            else if ( set_changed )
            {
                vcall< cam_recompute_fn >( chase, 14, uint8_t( 1 ) );
                CCore::g_instance->info(
                    "cam: recompute on set change (anchor {:p} kept, det_recs {} jointless {})",
                    anchor, det_recs_now, jointless_now );
            }
            was_detached = true;
            last_det_recs = det_recs_now;
            last_jointless = jointless_now;

            // zoom distance: the native update clamps the target (+0x4D8) to
            // [+0x480, base], so raising +0x480 above the base distance pins
            // the camera at our value — pure field write, the native lerp at
            // +0x4DC still smooths every change. Wheel forward = closer,
            // backward = farther, no upper limit.
            // Only while the truck tows NOTHING: with a live trailer on the
            // fifth wheel the camera must keep measuring the live
            // combination — pinning +0x4D8 would freeze it at the distance
            // the full parked set was measured at
            if ( chain_head == nullptr )
            {
                if ( zoom_dist < 0.0f )
                {
                    saved_zoom_min = prism::offsets::field< float >( chase, 0x480 );
                    zoom_dist = prism::offsets::field< float >( chase, 0x4DC );
                    if ( zoom_dist < 0.5f ) zoom_dist = prism::offsets::field< float >( chase, 0x4D8 );
                }
                zoom_dist -= static_cast< float >( wheel ) * ( 2.0f / 120.0f );   // ~2m per notch
                if ( zoom_dist < 0.5f ) zoom_dist = 0.5f;
                prism::offsets::field< float >( chase, 0x480 ) = zoom_dist;
                // keep the target strictly below our min so the clamp picks it
                prism::offsets::field< float >( chase, 0x4D8 ) = zoom_dist - 0.01f;
            }
            else if ( zoom_dist >= 0.0f )
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
            // trailer), not the last one — the setter hook log shows the
            // native resolver calls set_target(t0). The camera measures
            // the combination length by walking trailer_slave links
            // STARTING AT the anchor; anchoring on the last trailer
            // measures only that trailer and the camera parks just
            // behind the truck — exactly the "stuck on the horse" bug.
            if ( anchor != chain_head && cam_set_target_hook != nullptr &&
                plausible_vfunc( chase, 31 ) )
            {
                // go through the real setter (vfunc 31) — refcounted
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
            last_det_recs = 0;
            last_jointless = 0;
            was_detached = false;
            last_forced_anchor = reinterpret_cast< void* >( ~0ull );
            // recompute (vfunc 14) re-reads the anchor's joint and switches
            // the camera back to the jointed distance set (full SCS range)
            // — skipped while detached because it resets the orbit state
            vcall< cam_recompute_fn >( chase, 14, uint8_t( 1 ) );
        }
        override_active = any_detached;
    }

    // trailer hose/cable visuals + the detach/restore traffic suppression.
    // rebuilds every hose/cable edge to match the REAL coupling state: a
    // trailer's outgoing lines go to its slave only while that slave is
    // jointed — parked links keep their slave pointer for bookkeeping, which
    // is how phantom hoses to a parked trailer get drawn. Parked pairs keep
    // their hose (the member's joint to its parked parent is still live).
    // Edge-driven: runs only when the coupling signature changes
    // per-edge line rebuild, exactly one live connection per call: the
    // resolver recurses down trailer_slave, so the target's own slave link
    // is hidden for the duration of the call — the walk can only draw the
    // (front, trailer) pair, never hoses into a parked section
    void rebuild_one_edge( void* front, void* trailer )
    {
        if ( trailer != nullptr )
        {
            void* const rear = prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave );
            prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = nullptr;
            rebuild_lines_fn( front, trailer );
            prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = rear;
        }
        else
        {
            rebuild_lines_fn( front, nullptr );
        }
    }

    void refresh_all_lines()
    {
        if ( rebuild_lines_fn == nullptr ) return;
        void* truck = nullptr;
        if ( auto* game_actor = CCore::g_instance->get_game_actor() )
        {
            truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
        }
        void* towed = nullptr;
        bool towed_live = false;
        if ( truck != nullptr && readable_ptr( truck ) )
        {
            towed = prism::offsets::field< void* >( truck, 0x15A8 );
            towed_live = towed != nullptr && readable_ptr( towed ) &&
                readable_ptr( static_cast< uint8_t* >( towed ) + prism::offsets::trailer_physics_joint ) &&
                joint( towed ) != nullptr;
            rebuild_one_edge( truck, towed_live ? towed : nullptr );
        }
        for ( uint32_t i = 0; i < trailer_count; ++i )
        {
            void* t = trailers[ i ];
            if ( t == nullptr || !readable_ptr( t ) ||
                 !readable_ptr( static_cast< uint8_t* >( t ) + prism::offsets::trailer_slave ) )
            {
                continue;
            }
            void* slave = prism::offsets::field< void* >( t, prism::offsets::trailer_slave );
            const bool slave_live = slave != nullptr && readable_ptr( slave ) &&
                readable_ptr( static_cast< uint8_t* >( slave ) + prism::offsets::trailer_physics_joint ) &&
                joint( slave ) != nullptr;
            rebuild_one_edge( t, slave_live ? slave : nullptr );
        }
        // apply LAST — the native order is every rebuild first and
        // apply_cables once at the end; any rebuild after it tears the
        // visuals it just raised back down
        if ( apply_cables_fn != nullptr && cables_mode_cvar != nullptr &&
             get_cvar_int_fn != nullptr && truck != nullptr && readable_ptr( truck ) )
        {
            const int mode = get_cvar_int_fn( cables_mode_cvar );
            apply_cables_fn( truck, towed_live ? towed : nullptr, 0,
                             mode > 4 ? 4 : mode < 0 ? 0 : mode, 0 );
        }
    }

    // g_trailer_cables_mode: 0 nothing coupled, 1 first trailer (or partial
    // set) coupled, 2 whole original set coupled — written only on change, so
    // the console stays free for the user. A trailer counts only while
    // actually coupled to the truck's convoy — trailer 1 through the fifth
    // wheel, the rest through the chain or straight on the truck.
    void CTrailerManipulation::update_convoy_state()
    {
        if ( diff_probe_frames > 0 && --diff_probe_frames == 0 )
        {
            diff_trailers( "probe-end" );
            diff_probe_frames = -1;
        }
        bool in_convoy[ max_trailers ] = {};
        bool parent_in_convoy = true;
        for ( uint32_t i = 0; i < trailer_count; ++i )
        {
            const bool attached = joint( trailers[ i ] ) != nullptr;
            in_convoy[ i ] = trailers[ i ] == truck_towed ? attached : attached && parent_in_convoy;
            parent_in_convoy = in_convoy[ i ];
        }
        // the truck broadcasts its brake channels down the towed chain only
        // while the coupling state machine sits at 5 (0x14064B760), and it
        // only reaches the trailer registered at truck+0x15A8 — a plugin
        // attach can leave either short of that, which silently drops service
        // and park braking on the whole chain. Pin both while our trailer is
        // on the truck; natively coupled links run their own progression
        void* truck = nullptr;
        if ( auto* game_actor = CCore::g_instance->get_game_actor() )
        {
            truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
        }
        if ( truck_towed != nullptr && truck != nullptr && joint( truck_towed ) != nullptr )
        {
            if ( prism::offsets::field< void* >( truck, 0x15A8 ) != truck_towed &&
                set_towed_trailer_fn != nullptr )
            {
                set_towed_trailer_fn( truck, true, truck_towed );
            }
            prism::offsets::field< uint32_t >( truck, prism::offsets::truck_coupling_state ) = 5;
        }

        // a T-detach that bypassed the dispatcher can clear the towed slot
        // while leaving the fifth-wheel joint alive — the truck stays
        // physically locked to a trailer the game thinks it no longer tows
        // (gear sound played, truck can't move). Release it through the
        // same path the plugin detach uses; trailer->trailer joints in a
        // detached section have another trailer as parent and stay
        if ( truck != nullptr && readable_ptr( truck ) &&
            prism::offsets::field< void* >( truck, 0x15A8 ) == nullptr )
        {
            for ( uint32_t i = 0; i < trailer_count; ++i )
            {
                void* t = trailers[ i ];
                if ( t == nullptr || !readable_ptr( t ) || joint( t ) == nullptr ) continue;
                if ( prism::offsets::field< void* >( t, prism::offsets::trailer_parent ) == truck )
                {
                    CCore::g_instance->info( "stuck tow joint on trailer {} — releasing", i );
                    disconnect_now( t, truck, true );
                    break;
                }
            }
        }

        // mirror the truck's brake channels straight onto every trailer in the
        // convoy: the native service path needs the pneumatic connector flags
        // pair_vehicles_fn sets, which our replica can't fully rebuild — so a
        // plugin-attached trailer rolls unbraked and pushes the truck. The
        // physics consumes +0x730 directly (the spring-brake loop writes it
        // too), so landing the truck's own values there brakes the trailer.
        // Broadcast writes carry the same numbers when the native path works
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

        // a detached trailer must STAY spring-braked: the detach path writes
        // trailer_brake (+0x760) = 1 once, but the native per-frame loop that
        // keeps jointless trailers braked is gated by the truck's coupling
        // state — while another trailer stays coupled (state 5) nothing
        // re-applies it. Pin the spring factor + zeroed service demand every
        // frame the same way the convoy mirror pins the truck's values on
        // jointed trailers; the attach flow releases it through the same
        // fields when the joint comes back.
        // The test is live-reachability, NOT "joint == nullptr": a trailer
        // still jointed to a PARKED parent (dolly bolted behind a parked
        // trailer) is part of a parked section and must stay braked too —
        // only trailers reachable through the truck's live towed chain are
        // skipped
        // while a piece is still settling (anchor_settling — it is tipping
        // onto its support and rolling into equilibrium) the wheels must NOT
        // be locked: a single-axle dolly whose CG sits behind the axle falls
        // backward and needs to roll a few cm to find its rest pose. Locked
        // wheels leave it teetering on one contact patch — the reported
        // vibration/spin. Light brake damps a runaway slide; full brake
        // returns once the anchor locks it
        for ( uint32_t i = 0; i < trailer_count; ++i )
        {
            void* t = trailers[ i ];
            if ( t == nullptr || !readable_ptr( t ) ||
                 !readable_ptr( static_cast< uint8_t* >( t ) + prism::offsets::trailer_physics_joint ) ||
                 trailer_is_live( t ) )
            {
                continue;
            }
            const int bi = canon_index_of( t );
            const float brake = bi >= 0 && anchor_settling[ bi ] ? 0.25f : 1.f;
            prism::offsets::field< float >( t, prism::offsets::trailer_brake ) = brake;
            prism::offsets::field< float >( t, prism::offsets::trailer_service_brake ) = 0.f;
            prism::offsets::field< uint8_t >( t, prism::offsets::trailer_brake_forced ) = brake >= 1.f ? 1 : 0;
        }
        for ( void* d : detached_trailers )
        {
            if ( d == nullptr || !readable_ptr( d ) ||
                 !readable_ptr( static_cast< uint8_t* >( d ) + prism::offsets::trailer_physics_joint ) ||
                 trailer_is_live( d ) )
            {
                continue;
            }
            const int bi = canon_index_of( d );
            const float brake = bi >= 0 && anchor_settling[ bi ] ? 0.25f : 1.f;
            prism::offsets::field< float >( d, prism::offsets::trailer_brake ) = brake;
            prism::offsets::field< float >( d, prism::offsets::trailer_service_brake ) = 0.f;
            prism::offsets::field< uint8_t >( d, prism::offsets::trailer_brake_forced ) = brake >= 1.f ? 1 : 0;
        }

        // a jointless trailer without a parked-state record (+0x1188) has no
        // anchor: the record is what re-asserts the parked pose every frame,
        // and the record factory purecalls on mid-chain trailers so they
        // never get one — wheel brakes alone just slide. Emulate the lock:
        // snapshot the pose once when the joint drops and teleport back
        // whenever the trailer drifts (the restore teleports clear the mark
        // so our own moves never get fought). Only the horizontal drift is
        // corrected: the stored pose's Y is the coupled height, and forcing
        // it back makes the trailer bounce while physics settles it.
        // Suspended right after an actor teleport: the dock re-joint
        // sequence leaves trailers jointless mid-move and the anchor would
        // fight the game's own member placement
        if ( tp_cooldown > 0 ) --tp_cooldown;
        for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
        {
            if ( tp_cooldown > 0 )
            {
                anchor_valid[ i ] = false;
                anchor_settling[ i ] = false;
                anchor_delay[ i ] = 0;
                continue;
            }
            void* t = canon[ i ];
            if ( !readable_ptr( t ) || joint( t ) != nullptr )
            {
                anchor_valid[ i ] = false;
                anchor_settling[ i ] = false;
                anchor_delay[ i ] = 0;
                continue;
            }
            // a jointless trailer without a parked-state record is not parked
            // as far as the game is concerned: wheels roll free, no obstacle
            // behaviour, the electrical feed stays live. Run the native record
            // factory once — its mid-chain blocker was a component purecall
            // and hk_purecall swallows it now — then link the registered
            // record back into trailer+0x1188 and raise the parked flags the
            // dispatcher sets on the same condition
            if ( prism::offsets::field< void* >( t, prism::offsets::trailer_parked_state ) == nullptr &&
                !record_tried[ i ] &&
                ( is_detached( t ) || is_plugin_detached( t ) ) &&
                parked_record_factory != nullptr && parked_record_mgr != nullptr )
            {
                record_tried[ i ] = true;
                __try
                {
                    parked_record_factory( t );
                    void* mgr = readable_ptr( parked_record_mgr ) ? *parked_record_mgr : nullptr;
                    void** it = mgr != nullptr ? prism::offsets::field< void** >( mgr, 0x740 ) : nullptr;
                    void** end = mgr != nullptr ? prism::offsets::field< void** >( mgr, 0x748 ) : nullptr;
                    void* rec = nullptr;
                    if ( it != nullptr && end != nullptr && end >= it && end - it < 0x1000 )
                    {
                        for ( ; it != end; ++it )
                        {
                            void* e = *it;
                            if ( e != nullptr && readable_ptr( e ) &&
                                prism::offsets::field< void* >( e, 0x178 ) == t )
                            {
                                rec = e;
                                break;
                            }
                        }
                    }
                    if ( rec != nullptr )
                    {
                        prism::offsets::field< void* >( t, prism::offsets::trailer_parked_state ) = rec;
                        prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked ) = 1;
                        prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked_registered ) = 1;
                        CCore::g_instance->info( "parked record {:p} created for c{}", rec, i );
                    }
                    else
                    {
                        CCore::g_instance->warning( "parked record factory produced no record for c{}", i );
                    }
                }
                __except ( EXCEPTION_EXECUTE_HANDLER )
                {
                    CCore::g_instance->warning( "parked record factory faulted for c{}", i );
                }
            }
            // a record pointer alone does not lock the trailer: the native
            // pose re-assertion (vfunc +0x8B8) only runs while the trailer's
            // per-frame update arms +0x1258 — a mid-chain-detached trailer
            // keeps its stale +0x1188 record but the update never re-arms the
            // flag, so the record is INERT and the trailer rolls free. Only a
            // live registration (+0x1258 != 0) means the game owns the lock;
            // anything else gets the emulated anchor — when the native lock
            // is working the trailer never drifts and the teleport stays idle
            if ( prism::offsets::field< void* >( t, prism::offsets::trailer_parked_state ) != nullptr &&
                !record_tried[ i ] &&
                prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked_registered ) != 0 )
            {
                anchor_valid[ i ] = false;
                anchor_settling[ i ] = false;
                anchor_delay[ i ] = 0;
                continue;
            }
            // jointless grace — the anchor must not capture during the first
            // frames after joint-drop: load/assembly holds trailers jointless
            // while the game repositions them through staging poses like
            // (0,0,-512), and anchoring one of those teleports the trailer
            // into the void on the next frame (buried-underground load bug)
            if ( anchor_delay[ i ] < 40 )
            {
                ++anchor_delay[ i ];
                continue;
            }
            if ( !anchor_valid[ i ] )
            {
                anchor_valid[ i ] = hook_world( t, anchor_xform[ i ] );
                anchor_home[ i ] = anchor_xform[ i ];
                anchor_settle[ i ] = 0;
                anchor_age[ i ] = 0;
                anchor_settling[ i ] = true;
                continue;
            }
            hook_xform_t now{};
            if ( !hook_world( t, now ) || trailer_teleport_hook == nullptr ) continue;
            ++anchor_age[ i ];
            const float mx = now.pos.x - anchor_xform[ i ].pos.x;
            const float my = now.pos.y - anchor_xform[ i ].pos.y;
            const float mz = now.pos.z - anchor_xform[ i ].pos.z;
            const quat_t& aq = anchor_xform[ i ].quat;
            const float qdot = aq.w * now.quat.w + aq.x * now.quat.x +
                               aq.y * now.quat.y + aq.z * now.quat.z;
            const float rotd = 1.f - std::fabs( qdot );   // 0 = same rotation
            const bool moving = mx * mx + my * my + mz * mz > 0.02f * 0.02f || rotd > 0.001f;
            if ( anchor_settle[ i ] < 30 && anchor_age[ i ] < 240 )
            {
                // settle phase: a freshly detached trailer is still tipping
                // onto its support (the dolly drops the nose onto the static
                // leg) — follow the pose instead of locking, or the stored
                // level pose fights the settle every frame. That fight is
                // the bounce/tremble: teleport to level, tip, teleport, tip.
                // Stillness counts rotation too — a rocking trailer holds its
                // origin almost still while pivoting on the contact patch
                anchor_settling[ i ] = true;
                const float hx = now.pos.x - anchor_home[ i ].pos.x;
                const float hz = now.pos.z - anchor_home[ i ].pos.z;
                if ( hx * hx + hz * hz > 1.5f * 1.5f && trailer_teleport_hook != nullptr )
                {
                    // wandered too far while free-rolling — pull it back to
                    // the detach pose and let it settle from there again
                    using teleport_fn_t = void __fastcall( void* /*trailer*/, void* /*transform*/ );
                    trailer_teleport_hook->get_original< teleport_fn_t >()( t, &anchor_home[ i ] );
                    anchor_xform[ i ] = anchor_home[ i ];
                    anchor_settle[ i ] = 0;
                    continue;
                }
                if ( moving )
                {
                    anchor_xform[ i ] = now;
                    anchor_settle[ i ] = 0;
                }
                else
                {
                    ++anchor_settle[ i ];
                }
                continue;
            }
            anchor_settling[ i ] = false;
            if ( mx * mx + my * my + mz * mz > 0.01f * 0.01f || rotd > 0.0004f )
            {
                // locked — emulate the native parked record: re-assert the
                // WHOLE stored pose (position AND rotation) on any real
                // drift. A position-only pin can never see a rocking trailer:
                // it pivots on the contact patch without moving the origin,
                // which is exactly the dolly's perpetual wobble/spin
                using teleport_fn_t = void __fastcall( void* /*trailer*/, void* /*transform*/ );
                trailer_teleport_hook->get_original< teleport_fn_t >()( t, &anchor_xform[ i ] );
            }
        }

        // hose sanity: every attach/detach path rebuilds only the pair it
        // touched, but parked trailers keep live slave links and stray
        // native calls re-pair connectors through them — leaving phantom
        // hoses on dead edges or missing ones on live edges. Whenever the
        // coupling signature changes, rebuild EVERY edge to match reality
        uint64_t lines_sig = trailer_count;
        for ( uint32_t i = 0; i < trailer_count && i < 20; ++i )
        {
            void* t = trailers[ i ];
            const bool live = t != nullptr && readable_ptr( t ) && joint( t ) != nullptr;
            void* slave = t != nullptr && readable_ptr( t ) &&
                readable_ptr( static_cast< uint8_t* >( t ) + prism::offsets::trailer_slave )
                ? prism::offsets::field< void* >( t, prism::offsets::trailer_slave ) : nullptr;
            const bool edge = slave != nullptr && readable_ptr( slave ) && joint( slave ) != nullptr;
            lines_sig |= ( live ? 1ull : 0 ) << i;
            lines_sig |= ( edge ? 1ull : 0 ) << ( i + 20 );
        }
        void* towed_sig = truck != nullptr && readable_ptr( truck )
            ? prism::offsets::field< void* >( truck, 0x15A8 ) : nullptr;
        lines_sig |= ( towed_sig != nullptr && readable_ptr( towed_sig ) &&
            readable_ptr( static_cast< uint8_t* >( towed_sig ) + prism::offsets::trailer_physics_joint ) &&
            joint( towed_sig ) != nullptr ? 1ull : 0 ) << 40;
        static uint64_t last_lines_sig = ~0ull;
        if ( lines_sig != last_lines_sig )
        {
            last_lines_sig = lines_sig;
            refresh_all_lines();
        }

        // mode = cables on how much of the LIVE convoy: 1 draws them only
        // on the first trailer, 2+ on the whole coupled set. Counting live
        // members — not "is the canonical last trailer coupled" — keeps the
        // dolly's hoses when the tail of a rodotrain detaches while the
        // dolly itself stays coupled to trailer 1
        // memory watch: diff the trailer's parked/brace block every frame so
        // whatever really moves during a detach/attach shows up in the log —
        // the actual landing-gear field is unknown, this finds it empirically
        for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
        {
            static uint8_t snap[ max_trailers ][ 0x240 ] = {};
            static void* snap_for[ max_trailers ] = {};
            void* t = canon[ i ];
            if ( !readable_ptr( t ) ||
                 !readable_ptr( reinterpret_cast< uint8_t* >( t ) + 0x1100 + 0x240 ) )
            {
                snap_for[ i ] = nullptr;
                continue;
            }
            const uint8_t* cur = reinterpret_cast< const uint8_t* >( t ) + 0x1100;
            if ( snap_for[ i ] != t )
            {
                memcpy( snap[ i ], cur, sizeof( snap[ i ] ) );
                snap_for[ i ] = t;
                continue;
            }
            for ( uint32_t o = 0; o < sizeof( snap[ i ] ); o += 4 )
            {
                uint32_t nv, ov;
                memcpy( &nv, cur + o, 4 );
                memcpy( &ov, snap[ i ] + o, 4 );
                if ( nv != ov )
                {
                    float fo, fn;
                    memcpy( &fo, &ov, 4 );
                    memcpy( &fn, &nv, 4 );
                    CCore::g_instance->info(
                        "mw c{} +0x{:03X} {:08X} -> {:08X} (f {:.3g} -> {:.3g})",
                        i, 0x1100 + o, ov, nv, fo, fn );
                }
            }
            memcpy( snap[ i ], cur, sizeof( snap[ i ] ) );
        }
        uint32_t convoy_n = 0;
        for ( uint32_t i = 0; i < trailer_count; ++i )
        {
            if ( in_convoy[ i ] ) ++convoy_n;
        }
        const uint32_t mode = convoy_n >= 2 ? 2 : convoy_n;
        static uint32_t sent_cables_mode = UINT32_MAX;
        if ( mode != sent_cables_mode && execute_line_fn != nullptr &&
            !pending_cmd_queued.load( std::memory_order_acquire ) )
        {
            sent_cables_mode = mode;
            snprintf( pending_cmd, sizeof( pending_cmd ), "g_trailer_cables_mode %u", mode );
            pending_cmd_queued.store( true, std::memory_order_release );
            CCore::g_instance->info( "g_trailer_cables_mode = {}", mode );
        }

        // a trailer back on the convoy must be fully un-parked — but the
        // parked-state record (+0x1188) must stay linked: the traffic-obstacle
        // registration references it, and orphaning it leaves a dead lane
        // entry the traffic sweep crashes on (the invariant connect_now
        // already honours). What actually re-asserts the parked pose is the
        // per-frame update gated by the +0x1258 registration flag — so the
        // parked byte and that flag are dropped, the record is kept.
        // JOINTED trailers only: a jointless piece stays in the chain on
        // purpose (chain reachability = visibility), and clearing its parked
        // flags fights the chain maintenance that re-asserts them
        for ( uint32_t i = 0; i < trailer_count; ++i )
        {
            if ( !in_convoy[ i ] ) continue;
            void* t = trailers[ i ];
            if ( joint( t ) == nullptr ) continue;
            const bool parked_flag = prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked ) != 0;
            const bool parked_rec = prism::offsets::field< void* >( t, prism::offsets::trailer_parked_state ) != nullptr;
            // only a live parked state needs clearing — the record (+0x1188)
            // stays linked on purpose for the traffic registration, so its
            // mere presence must not re-trigger this (it fires every frame
            // and zeroes the coupled trailer's brake channels)
            const bool parked_reg = prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked_registered ) != 0;
            if ( parked_flag || parked_reg )
            {
                // ONCE per joint: the parked machine re-asserts the flags
                // every frame while the record stays linked, so an
                // unconditional clear just fights it forever — and zeroes
                // the coupled trailer's brake channels on every pass
                static void* unparked_for[ max_trailers ] = {};
                if ( unparked_for[ i ] != joint( t ) )
                {
                    unparked_for[ i ] = joint( t );
                    prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked ) = 0;
                    prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked_registered ) = 0;
                    prism::offsets::field< float >( t, prism::offsets::trailer_brake ) = 0.f;
                    prism::offsets::field< uint8_t >( t, prism::offsets::trailer_brake_forced ) = 0;
                    CCore::g_instance->info( "un-parked trailer {} (flag {} record {})", i, parked_flag, parked_rec );
                }
            }
        }

        // landing gear follows the trailer's OWN front joint, not the convoy:
        // the legs carry the front end, so jointed (to anything — live or
        // parked) means raised and jointless (head of a parked piece) means
        // down. The native detach propagates legs-down to the whole parked
        // set but only restores the directly-attached trailer on re-attach
        // (mw log), and skips the head entirely when its parked record
        // already exists — both fixed here by enforcing +0x126C = jointed.
        // arg semantics proven against the +0x328 disasm: 0 -> +0x126C = 1
        // (raised), 1 -> 0 (deployed); brace_anim mirrors them and builds the
        // crank-down anim player on models that have the markers, so a
        // legitimately detached head lowers like a native detach while a
        // wrongly-stamped jointed trailer snaps back instantly
        // the native T-detach sets truck+0x14E0 = 1 (decoupling) at the very
        // start of the sequence, plays the crank sound, and only drops the
        // joint at the end — so firing on joint-drop runs the whole gear
        // anim AFTER the sound finished. Trailers skipped by the native
        // +0x1170 gate (parked record) get their crank started here instead,
        // synced with the sound; the stamped-restore stays off for that
        // trailer while the sequence runs or it fights the lowering
        void* coupling_trailer = nullptr;
        bool decoupling = false;
        if ( auto* ga = CCore::g_instance->get_game_actor() )
        {
            void* tr = readable_ptr( ga )
                ? prism::offsets::field< void* >( ga, prism::offsets::game_actor_truck ) : nullptr;
            if ( tr != nullptr && readable_ptr( tr ) )
            {
                decoupling = prism::offsets::field< uint32_t >( tr, prism::offsets::truck_coupling_state ) == 1;
                coupling_trailer = prism::offsets::field< void* >( tr, 0x15A8 );
            }
        }
        for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
        {
            void* t = canon[ i ];
            if ( !readable_ptr( t ) ||
                 !readable_ptr( reinterpret_cast< uint8_t* >( t ) + 0x1274 ) )
                continue;
            const bool jointed = joint( t ) != nullptr;
            const float gear = prism::offsets::field< float >( t, 0x126C );
            const bool detaching_head = decoupling && coupling_trailer != nullptr && t == coupling_trailer;
            if ( jointed && gear > 0.5f && detaching_head && brace_anim_fn != nullptr )
            {
                CCore::g_instance->info( "gear: detach sequence on {} — cranking legs down", i );
                brace_anim_fn( t, 1, 0.3333333f );
            }
            else if ( jointed && gear < 0.5f && !detaching_head && plausible_vfunc( t, 0x328 / 8 ) )
            {
                CCore::g_instance->info( "gear: jointed trailer {} stamped legs-down — restoring", i );
                vcall< prism::physics_trailer_u_set_trailer_brace_fn >( t, 0x328 / 8, false );
            }
            else if ( !jointed && gear > 0.5f && brace_anim_fn != nullptr )
            {
                CCore::g_instance->info( "gear: parked head {} legs still up — lowering", i );
                brace_anim_fn( t, 1, 0.3333333f );
            }
        }

        // chase camera: while any detached trailer exists the orbit anchor
        // (camera+0x3F0) is forced onto the truck — the native resolver would
        // still pick the parked trailer through the live slave link
        enforce_chase_camera_anchor();

        // traffic was zeroed for a detach: bring the saved level back only once
        // the WHOLE original set is coupled again AND a detach has genuinely
        // completed — during the post-command delay every joint is still alive,
        // so the set reads as coupled while traffic would bounce straight back
        // before the joint drops. A pending console line defers the write by a
        // frame instead of being overwritten
        if ( saved_traffic >= 0.f && !pending_cmd_queued.load( std::memory_order_acquire ) )
        {
            bool all_coupled = expected_trailers > 0 && trailer_count >= expected_trailers &&
                traffic_detach_done.load( std::memory_order_acquire );
            for ( uint32_t i = 0; all_coupled && i < trailer_count; ++i ) all_coupled &= in_convoy[ i ];
            // a parked detached trailer that fell out of the chain must also
            // be back before traffic returns
            for ( void* d : detached_trailers )
                if ( d != nullptr && joint( d ) == nullptr ) all_coupled = false;
            if ( all_coupled )
            {
                snprintf( pending_cmd, sizeof( pending_cmd ), "g_traffic %g", saved_traffic );
                pending_cmd_queued.store( true, std::memory_order_release );
                CCore::g_instance->info( "restoring g_traffic = {:.2f}", saved_traffic );
                saved_traffic = -1.f;
                expected_trailers = 0;
                traffic_detach_done.store( false, std::memory_order_release );
            }
            // until the whole set is back, traffic stays pinned at zero — even
            // if it was raised by hand through the console in between
            else if ( get_cvar_float_fn != nullptr && g_traffic_cvar != nullptr &&
                get_cvar_float_fn( g_traffic_cvar ) > 0.f )
            {
                queue_cmd( "g_traffic 0" );
            }
        }
    }


    // a detach mark only protects a trailer while it is jointless or sitting
    // on the truck; back on its chain parent the game may manage it again.
    // Entries no longer in the chain (respawned trailers) are dropped without
    // touching them
    void CTrailerManipulation::gc_detached_marks()
    {
        for ( void*& d : detached_trailers )
        {
            if ( d == nullptr || d == truck_towed ) continue;
            bool live = false;
            for ( uint32_t i = 0; i < trailer_count; ++i ) live |= trailers[ i ] == d;
            if ( !live || joint( d ) != nullptr ) d = nullptr;
        }
        for ( void*& d : plugin_detached )
        {
            if ( d == nullptr || d == truck_towed ) continue;
            bool live = false;
            for ( uint32_t i = 0; i < trailer_count; ++i ) live |= trailers[ i ] == d;
            if ( !live || joint( d ) != nullptr ) d = nullptr;
        }
    }

    // attach/detach hotkeys, ignored while the game is in the background or a
    // key is being rebound
    void CTrailerManipulation::update_hotkeys()
    {
        if ( !connect_valid_ || !game_has_focus() || settings::is_capturing() ) return;

        // one key toggles a trailer against its chain parent, the truck
        // variants connect that trailer straight to the fifth wheel — one
        // bind pair per trailer slot. Trailer 0 (index 0) is left to the
        // game's own T key
        for ( uint32_t i = 1; i < trailer_count; ++i )
        {
            bool toggle = false, to_truck = false;
            trailer_key_down( static_cast< int >( i ), false, &toggle );
            trailer_key_down( static_cast< int >( i ), true, &to_truck );
            if ( toggle ) this->toggle_attach( i );
            if ( to_truck ) this->connect_to_truck( i );
        }
    }

    // steering hotkeys + lock state for steerable-axle (oversize) trailers;

    //========================================================================
    // Feature entry points (called by the window shell)
    //========================================================================
    void crash_probes_install()
    {
        veh_dump_event = CreateEventA( nullptr, FALSE, FALSE, nullptr );
        AddVectoredExceptionHandler( 1, veh_crash_probe );
        CloseHandle( CreateThread( nullptr, 0, freeze_watchdog, nullptr, 0, nullptr ) );
        // process-death probes: a silent exit leaves no exception and no
        // crash log — hooking the kill entry points reveals the caller
        if ( const HMODULE kb = GetModuleHandleA( "kernelbase.dll" ) )
        {
            exit_process_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
                "exit_process", reinterpret_cast< uint64_t >( GetProcAddress( kb, "ExitProcess" ) ),
                reinterpret_cast< uint64_t >( &hk_ExitProcess ) );
            if ( exit_process_hook != nullptr ) exit_process_hook->hook();
            terminate_process_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
                "terminate_process", reinterpret_cast< uint64_t >( GetProcAddress( kb, "TerminateProcess" ) ),
                reinterpret_cast< uint64_t >( &hk_TerminateProcess ) );
            if ( terminate_process_hook != nullptr ) terminate_process_hook->hook();
        }
    }

    // Individually detachable trailers. Two extra guards are needed: a null-joint
    // check the game crashes on while a trailer is detached, and connect_slave,
    // which would otherwise re-attach the trailers behind the one being connected.
    bool coupling_install()
    {
        const auto crash_address = memory::get_address_for_pattern( "48 85 d2 0f 84 ? ? ? ? 48 89 74 24 18 57 48 83 ec 40" );
        const auto connect_slave_address = memory::get_address_for_pattern( "40 53 48 83 ec 60 48 83 b9 ? ? ? ? 00 48 8b d9 0f 84 ? ? ? ? 48 8d 54 24 ? e8" );

        if ( crash_address == 0 || connect_slave_address == 0 )
        {
            CCore::g_instance->warning( "Trailer attach/detach unavailable on this game version" );
            return false;
        }

        // get_slave_hook_position is the tail call inside connect_slave
        get_slave_hook_position_fn = reinterpret_cast< prism::physics_trailer_u_get_slave_hook_position_fn* >(
            connect_slave_address + 29 + *reinterpret_cast< int32_t* >( connect_slave_address + 29 ) + 4 );

        resolve_native_fns();
        brake_register_hooks();

        crashes_when_disconnected_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "crashes_when_disconnected",
            crash_address,
            reinterpret_cast< uint64_t >( &hk_crashes_when_disconnected ) );

        connect_slave_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "connect_slave",
            connect_slave_address,
            reinterpret_cast< uint64_t >( &hk_connect_slave ) );

        if ( !CCore::g_instance->is_truckersmp() &&
            crashes_when_disconnected_hook->hook() == CHook::HOOKED &&
            connect_slave_hook->hook() == CHook::HOOKED )
        {
            return true;
        }
        CCore::g_instance->warning( "Could not install the attach/detach hooks" );
        return false;
    }

    // Cargo respawn watch: while a detached section sits parked, keep its
    // slots' transforms fresh (head and jointed members alike). When a
    // marked slot's object shows up jointed without ever being unmarked,
    // the game respawned the set attached — queue the restore. ~30 frames
    // of settled state first so the cinematic's own writes finish
    void coupling_parked_update()
    {
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
                    rejoined_frames[ i ] = 0;
                    hook_xform_t xf{};
                    if ( hook_world( t, xf ) ) { parked_xform[ i ] = xf; parked_xform_valid[ i ] = true; }
                }
                else
                {
                    in_section = false;
                    if ( !settings::g_settings.cargo_restore )
                    {
                        // restore off (camera-0 teleports etc.) — a jointed
                        // marked slot is left alone and the count restarts
                        rejoined_frames[ i ] = 0;
                    }
                    else if ( ++rejoined_frames[ i ] >= 30 && pending.kind == pending_kind::none )
                    {
                        // sim suspended by the cinematic → run inline now so
                        // the respawned set never visibly sits assembled at
                        // the dock; sim alive → queue for the sim thread
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
                // parked section member still riding its detached head —
                // keep its spot fresh too; the respawned case lands in the
                // head's branch above and never reaches here
                hook_xform_t xf{};
                if ( hook_world( t, xf ) ) { parked_xform[ i ] = xf; parked_xform_valid[ i ] = true; }
            }
            else
            {
                in_section = false;
            }
        }

        // rig restore countdown — the same queued op the respawn watch fires;
        // restore_parked undoes the dock alignment and re-parks the detached
        // sections in one pass
        if ( rig_armed )
        {
            if ( !settings::g_settings.cargo_restore )
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
    }
}
