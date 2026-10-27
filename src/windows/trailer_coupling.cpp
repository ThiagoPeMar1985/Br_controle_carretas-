// Trailer attach/detach flows, native hooks and convoy-chain maintenance.

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
    //========================================================================
    // Resolved native functions and globals (filled by coupling_install())
    //========================================================================

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

    // chase_camera lives in trailer_shared.hpp (the window dtor clears it)
    using cam_set_target_fn = void( void* /*camera*/, void* /*target*/ );
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
    // (0x142D33198), r9d = 2, stack tail {param_block, 2, 0, 2, -1, 0, &opts}
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
    using native_attach_dispatch_fn = void __fastcall( void* /*game_actor*/, uint8_t /*dl*/,
        uint64_t /*r8*/, void* /*settle_value_override*/ );
    native_attach_dispatch_fn* native_attach_fn = nullptr;

    // line/hose teardown + lamp-mask setter + the camera update vfunc,
    // resolved alongside the other native helpers (all signature-verified
    // for this build — same functions the ATS build hooked at the shared
    // build hash)
    using line_state_fn = void __fastcall( void* /*vehicle*/, uint8_t /*state*/, uint8_t /*force*/ );
    line_state_fn* set_lines_fn = nullptr;
    using lamp_mask_setter_fn = uint8_t __fastcall( void* /*vehicle*/, uint8_t /*mask*/ );
    using cam_update_fn = void( void* /*camera*/, float /*dt*/ );   // vfunc 32, 0x140AC75A0

    //========================================================================
    // Canonical trailer order
    //
    // trailers[] is rebuilt every frame from whatever chain root the game
    // still hands us; a trailer that drops out of the +0xC8 walk (native
    // detach, workshop respawn) slides every slot behind it and any
    // index-keyed state points at the wrong object. canon[] keeps the
    // ORIGINAL canonical order — pieces are appended first-seen and never
    // moved, so canon_index_of(t) is the stable slot the anchor/snapshot
    // arrays key on.
    //========================================================================
    void* canon[ max_trailers ] = {};
    // frames since the canon set's CONTENT last changed — reset by
    // reset_trailer_state (world reload), by every new canon member and
    // while canon is empty. Every path that calls into native code on
    // trailers (line rebuilds, parked registration, chain rewrites, hook
    // refusals) gates on this: during save/world assembly trailers appear
    // jointless and half-built over many frames and calling vfuncs or
    // line/cable functions on them crashes the build (model_object array
    // assert). The gate stays shut until the set stops changing for ~1.5s
    uint32_t canon_age = 0;
    // the trailer the "connect to truck" action wants the native attach
    // dispatcher to take — forced into game_actor+0xC8 inside
    // hk_attach_dispatch only while it is the closest jointless piece
    void* attach_target = nullptr;
    bool lw_ctx_logged[ max_trailers ] = {};

    void track_canon( void* t )
    {
        if ( t == nullptr ) return;
        for ( auto& c : canon )
        {
            if ( c == t ) return;
            if ( c == nullptr ) { c = t; canon_age = 0; return; }
        }
    }

    int canon_index_of( const void* t )
    {
        for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
            if ( canon[ i ] == t ) return static_cast< int >( i );
        return -1;
    }

    struct synthetic_slave_edge
    {
        std::atomic< void* > owner{ nullptr };
        std::atomic< void* > child{ nullptr };
    };
    synthetic_slave_edge synthetic_edges[ max_trailers ];

    bool claim_edge_ours( const void* owner, const void* child )
    {
        if ( owner == nullptr || child == nullptr ) return false;
        for ( auto& edge : synthetic_edges )
        {
            if ( edge.owner.load( std::memory_order_acquire ) == owner &&
                 edge.child.load( std::memory_order_acquire ) == child )
                return true;
        }
        return false;
    }

    void clear_claim_edge( const void* owner )
    {
        if ( owner == nullptr ) return;
        for ( auto& edge : synthetic_edges )
        {
            if ( edge.owner.load( std::memory_order_acquire ) != owner ) continue;
            edge.owner.store( nullptr, std::memory_order_release );
            edge.child.store( nullptr, std::memory_order_release );
        }
    }

    void set_claim_edge( void* owner, void* child )
    {
        if ( owner == nullptr ) return;
        if ( child == nullptr )
        {
            clear_claim_edge( owner );
            return;
        }
        for ( auto& edge : synthetic_edges )
        {
            if ( edge.owner.load( std::memory_order_acquire ) == owner )
            {
                edge.child.store( child, std::memory_order_release );
                return;
            }
        }
        for ( auto& edge : synthetic_edges )
        {
            if ( edge.owner.load( std::memory_order_acquire ) != nullptr ) continue;
            edge.child.store( child, std::memory_order_relaxed );
            edge.owner.store( owner, std::memory_order_release );
            return;
        }
    }

    //========================================================================
    // Parked-lock emulation state
    //
    // A detached trailer only stays planted while its parked-state record
    // (+0x1188) re-asserts the pose every frame. Trailers detached mid-chain
    // never get a record (the factory purecalls on them — do NOT call it),
    // so update_convoy_state emulates it: snapshot the transform once at
    // joint-drop and re-teleport on drift. Canon-indexed so slot shifts
    // never steer the lock onto the wrong object.
    //========================================================================
    hook_xform_t anchor_xform[ max_trailers ]{};
    hook_xform_t anchor_home[ max_trailers ]{};
    bool anchor_valid[ max_trailers ] = {};
    bool anchor_settling[ max_trailers ] = {};
    int anchor_settle[ max_trailers ] = {};
    int anchor_age[ max_trailers ] = {};
    int anchor_delay[ max_trailers ] = {};
    bool record_tried[ max_trailers ] = {};
    // the native parked-record factory (0x1407B69E0, signature-verified):
    // builds the 0x198-byte parked-state object the pose lock lives in. Its
    // mid-chain blocker was a component purecall — hk_purecall swallows it.
    // The manager global is read out of the factory prologue itself
    // (mov rsi, [rip+...] -> 0x1436AF828); its slot array is +0x740 base +
    // +0x748 count (verified in the tail call 0x140C377C0)
    // permanent state dump — every canon trailer's link/joint/record state at
    // each native boundary. Lives in the log forever: the disappearing-
    // trailer bugs were invisible exactly because nothing recorded what the
    // world looked like before/after a dispatch
    void dump_chain( const char* tag )
    {
        CCore::g_instance->info( "=== {} ===", tag );
        for ( uint32_t i = 0; i < max_trailers; ++i )
        {
            void* t = canon[ i ];
            if ( t == nullptr ) continue;
            const bool live_obj = readable_ptr( t );
            char* const tl = live_obj ? static_cast< char* >( t ) : nullptr;
            const uint8_t e8d = tl != nullptr ? *reinterpret_cast< uint8_t* >( tl + 0xE8D ) : uint8_t( 0xFF );
            const uint32_t ent = tl != nullptr
                ? static_cast< uint32_t >( ( *reinterpret_cast< uint64_t* >( tl + 0xEA0 ) -
                      *reinterpret_cast< uint64_t* >( tl + 0xE98 ) ) / 8 ) : 0;
            CCore::g_instance->info(
                "  c{} {:p} joint={:p} parent={:p} slave={:p} rec={:p} "
                "det={} pdet={} parked={} e8d={} ent={} {}",
                i, t,
                live_obj ? joint( t ) : nullptr,
                live_obj ? prism::offsets::field< void* >( t, prism::offsets::trailer_parent ) : nullptr,
                live_obj ? prism::offsets::field< void* >( t, prism::offsets::trailer_slave ) : nullptr,
                live_obj ? prism::offsets::field< void* >( t, prism::offsets::trailer_parked_state ) : nullptr,
                is_detached( t ), is_plugin_detached( t ),
                live_obj ? static_cast< int >( prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked ) ) : -1,
                e8d, ent,
                live_obj ? "" : "DEAD" );
        }
    }
    using parked_record_factory_fn = void __fastcall( void* /*trailer*/ );
    parked_record_factory_fn* parked_record_factory = nullptr;
    // SEH filter that logs WHERE inside the factory the fault happened — the
    // "factory faulted" catch alone can't tell a dead component deref from a
    // stale caller frame; the RIP names it
    int factory_fault_filter( uint32_t i, EXCEPTION_POINTERS* ep )
    {
        const auto rip = ep != nullptr && ep->ContextRecord != nullptr
            ? ep->ContextRecord->Rip : 0;
        const auto base = reinterpret_cast< uint64_t >( GetModuleHandle( nullptr ) );
        CCore::g_instance->warning(
            "parked record factory faulted for c{} at exe+0x{:X} code=0x{:X}",
            i, rip >= base ? rip - base : rip,
            ep != nullptr && ep->ExceptionRecord != nullptr
                ? ep->ExceptionRecord->ExceptionCode : 0 );
        return EXCEPTION_EXECUTE_HANDLER;
    }
    void** parked_record_mgr = nullptr;

    // is the trailer's parked record actually inside the manager's scan
    // list? A record linked on +0x1188 but absent from mgr+0x740/+0x748 is
    // INERT — the pose re-assert only iterates that array, so a save-loaded
    // trailer can show record + armed flag and still physically fall
    bool parked_record_listed( void* t )
    {
        void* mgr = readable_ptr( parked_record_mgr ) ? *parked_record_mgr : nullptr;
        if ( mgr == nullptr ) return false;
        __try
        {
            void** it = readable_span( mgr, 0x750 ) >= 0x750
                ? prism::offsets::field< void** >( mgr, 0x740 ) : nullptr;
            const uint64_t n = it != nullptr
                ? prism::offsets::field< uint64_t >( mgr, 0x748 ) : 0;
            if ( it == nullptr || n == 0 || n >= 0x1000 ||
                 readable_span( it, n * sizeof( void* ) ) < n * sizeof( void* ) )
                return false;
            for ( uint64_t k = 0; k < n; ++k )
            {
                if ( it[ k ] != nullptr && readable_ptr( it[ k ] ) &&
                     prism::offsets::field< void* >( it[ k ], 0x178 ) == t )
                    return true;
            }
        }
        __except ( EXCEPTION_EXECUTE_HANDLER ) {}
        return false;
    }

    // drop every record of this trailer out of the manager's scan list —
    // while it stays listed the parked machine keeps re-asserting the
    // record's stored pose against the live trailer
    void parked_record_unlist( void* t )
    {
        __try
        {
            void* mgr = readable_ptr( parked_record_mgr ) ? *parked_record_mgr : nullptr;
            void** it = mgr != nullptr && readable_span( mgr, 0x750 ) >= 0x750
                ? prism::offsets::field< void** >( mgr, 0x740 ) : nullptr;
            auto* nrec = it != nullptr
                ? &prism::offsets::field< uint64_t >( mgr, 0x748 ) : nullptr;
            if ( nrec == nullptr || *nrec == 0 || *nrec >= 0x1000 ||
                 readable_span( it, *nrec * sizeof( void* ) ) < *nrec * sizeof( void* ) )
                return;
            for ( uint64_t k = 0; k < *nrec; ++k )
            {
                if ( it[ k ] != nullptr && readable_ptr( it[ k ] ) &&
                     prism::offsets::field< void* >( it[ k ], 0x178 ) == t )
                {
                    std::memmove( it + k, it + k + 1, ( *nrec - k - 1 ) * sizeof( void* ) );
                    --*nrec;
                    --k;
                }
            }
        }
        __except ( EXCEPTION_EXECUTE_HANDLER ) {}
    }

    //========================================================================
    // Teleport formation log — the stacked-member correction in
    // hk_trailer_teleport and the formation settle in hk_actor_teleport
    //========================================================================
    tp_slot_t tp_log[ 16 ] = {};
    int tp_n = 0;
    bool tp_active = false;
    float3_t tp_snap[ max_trailers ] = {};
    quat_t tp_snap_q[ max_trailers ] = {};
    bool tp_snap_ok[ max_trailers ] = {};
    // frames the parked-anchor emulation stays off after an actor teleport —
    // the dock re-joint sequence leaves trailers jointless for a few frames
    // mid-move and the anchor would drag them back to the pre-teleport spot
    int tp_cooldown = 0;

    // a free canon slot only auto-claims within this distance of the
    // mother's rear hook — farther means parked scenery, not a coupling
    // candidate (the spontaneous re-join fix)
    const float attach_gap_limit = 12.f;

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
        // bitrem chains measure ~6-7 m at touching distance — keep one limit
        // that works for every configuration instead of one per layout
        return gap > 7.f;
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
            for ( uint32_t bo = 8; bo < 0x400; bo += 8 )
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

    // any canon entry that stopped being a valid trailer means the world was
    // reloaded (save load, workshop respawn): every cached pointer/mark could
    // be stale, so the whole tracker resets and rebuilds from live state
    bool canon_dead()
    {
        for ( auto* c : canon )
        {
            // a trailer slot freed and reused by a foreign allocation is
            // still readable — only the vtable reveals it stopped being a
            // trailer (the KERNELBASE-pointer crashes)
            if ( c != nullptr && !is_trailer_obj( c ) ) return true;
        }
        return false;
    }

    // every non-null canon entry is a trailer-class object — the light
    // validity bar the visibility pin needs before writing slave links
    bool canon_all_trailer_objs()
    {
        for ( auto* c : canon )
            if ( c != nullptr && !is_trailer_obj( c ) ) return false;
        return true;
    }

    // wipe every cached pointer and mark — called from canon_dead and the
    // SEH wrappers so a fault never leaves a half-built tracker behind
    void reset_trailer_state()
    {
        for ( auto& c : canon ) c = nullptr;
        for ( auto& j : canon_was_jointed ) j = false;
        for ( auto& j : canon_jointless_frames ) j = 0;
        for ( auto& edge : synthetic_edges )
        {
            edge.owner.store( nullptr, std::memory_order_release );
            edge.child.store( nullptr, std::memory_order_release );
        }
        for ( auto& d : detached_trailers ) d = nullptr;
        for ( auto& d : plugin_detached ) d = nullptr;
        for ( auto& f : detached_idx ) f = false;
        for ( auto& v : parked_xform_valid ) v = false;
        for ( auto& v : anchor_valid ) v = false;
        for ( auto& v : anchor_settling ) v = false;
        for ( auto& v : record_tried ) v = false;
        anchor_delay[ 0 ] = 0;
        truck_towed = nullptr;
        attach_target = nullptr;
        trailer_count = 0;
        pending = {};
        canon_age = 0;
        for ( auto& v : lw_ctx_logged ) v = false;
        CCore::g_instance->info( "trailer state reset" );
    }

    // drops every detach mark of a trailer picked for the native T attach —
    // the dispatcher hook refuses a plugin-detached head
    void release_detach_marks( void* t )
    {
        unmark_detached( t );
        unmark_plugin_detached( t );
        const int ci = canon_index_of( t );
        if ( ci >= 0 ) detached_idx[ ci ] = false;
    }

    void* canon_coupled_predecessor( void* t )
    {
        const int ci = canon_index_of( t );
        if ( ci <= 0 ) return nullptr;
        void* p = canon[ ci - 1 ];
        return p != nullptr && readable_ptr( p ) && joint( p ) != nullptr ? p : nullptr;
    }

    // a trailer is "live" when the convoy actually reaches it — walk UP the
    // physical parent chain (+0x1080) until the truck. Parents/joints are
    // only written on attach/detach, so the read stays stable while our
    // maintenance rewrites the slave links mid-frame — walking DOWN the
    // slave chain misclassifies a jointed trailer as parked for one frame
    // and the lamp pin darkens it (the flicker). Canonical trailers whose
    // parent chain dies before the truck are parked pieces, NOT convoy
    // members: their lamps/brakes/lines belong to themselves again
    bool trailer_is_live( const void* t )
    {
        if ( t == nullptr || !readable_ptr( t ) ||
             joint( const_cast< void* >( t ) ) == nullptr ) return false;
        auto* game_actor = CCore::g_instance->get_game_actor();
        void* truck = game_actor != nullptr
            ? prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck ) : nullptr;
        if ( truck == nullptr || !readable_ptr( truck ) ) return false;
        uint32_t depth = 0;
        for ( const void* v = t; v != nullptr && readable_ptr( v ) && depth <= max_trailers; ++depth )
        {
            void* parent = prism::offsets::field< void* >(
                const_cast< void* >( v ), prism::offsets::trailer_parent );
            if ( parent == truck ) return true;
            if ( parent == nullptr || parent == v || !is_trailer_obj( parent ) ||
                 joint( parent ) == nullptr ) return false;
            v = parent;
        }
        return false;
    }

    // A jointless object is part of a real parked section only when a
    // detached mark is already known on it or one of its physical parents.
    // During save/load, the game stages native slave links before joints are
    // created; those unmarked objects must be left to the game's connect flow.
    bool trailer_is_known_parked( const void* t )
    {
        for ( uint32_t depth = 0; t != nullptr && depth <= max_trailers; ++depth )
        {
            if ( !is_trailer_obj( t ) ) return false;
            if ( is_detached( t ) || is_plugin_detached( t ) ) return true;
            void* parent = prism::offsets::field< void* >(
                const_cast< void* >( t ), prism::offsets::trailer_parent );
            if ( parent == nullptr || parent == t ) return false;
            auto* game_actor = CCore::g_instance->get_game_actor();
            void* truck = game_actor != nullptr
                ? prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck ) : nullptr;
            if ( parent == truck ) return false;
            t = parent;
        }
        return false;
    }

    bool trailer_maintenance_ready()
    {
        if ( canon_age < 90 ||
             ( traffic_game_state != nullptr && *traffic_game_state != 2 ) )
            return false;
        auto* game_actor = CCore::g_instance->get_game_actor();
        void* truck = game_actor != nullptr
            ? prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck ) : nullptr;
        if ( truck == nullptr || !readable_ptr( truck ) ) return false;

        bool any = false;
        for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
        {
            void* t = canon[ i ];
            if ( !is_trailer_obj( t ) ) return false;
            any = true;
            // a limbo piece (no record, not known-parked) must NOT gate the
            // whole pass — it is exactly what the chain maintenance exists
            // to keep reachable. Treating it as invalid stopped every claim
            // write and dropped the parked set off the +0xC8 walk
            if ( !trailer_is_live( t ) && !trailer_is_known_parked( t ) &&
                 !is_detached( t ) && !is_plugin_detached( t ) )
                return false;
        }
        return any;
    }

    // the light-source context a trailer renders lamps through — the native
    // propagator repoints ctx+0x18 to the chain head every tick, so a parked
    // piece must point back at itself or it freezes mirroring truck lamps

    // the game's parked-trailer machine HIDES a trailer it no longer reaches
    // through the live chain — the parked record's own visibility rule. A
    // plugin-detached trailer is supposed to stay in the world, so once per
    // detach we re-assert the parked flags + brace state; pieces without any
    // record get their transform re-written to their current spot (the same
    // "I am here" nudge the parked record performs)
    void unhide_detached()
    {
        // mid-teleport reconcile: joints read null for a few frames and a
        // pose nudge here fights the game's own member placement
        if ( tp_cooldown > 0 ) return;
        for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
        {
            void* t = canon[ i ];
            if ( !readable_ptr( t ) || joint( t ) != nullptr ) continue;
            if ( !is_detached( t ) && !is_plugin_detached( t ) ) continue;
            if ( prism::offsets::field< void* >( t, prism::offsets::trailer_parked_state ) != nullptr )
            {
                prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked ) = 1;
                prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked_registered ) = 1;
                prism::offsets::field< uint8_t >( t, prism::offsets::trailer_brace_state ) = 1;
            }
            else if ( trailer_teleport_hook != nullptr )
            {
                hook_xform_t xf{};
                if ( hook_world( t, xf ) )
                {
                    using teleport_fn_t = void __fastcall( void*, void* );
                    trailer_teleport_hook->get_original< teleport_fn_t >()( t, &xf );
                }
            }
        }
    }

    void repair_lamp_ctx_all();
    void sanitize_slave_walk( void* head );
    // drive the game's own detach dispatcher (the T path) at a chosen trailer
    // by pointing the actor's trailer-of-interest slot at it — identical to
    // what pressing T on the head does. Only used while the trailer sits on
    // the truck (or straight on the fifth wheel): it is the head-detach
    // path — run on a mid-chain trailer it tears down the whole towed set's
    // registration, so chain detaches stay on the manual replica below
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
        using detach_fn_t = void __fastcall( void*, uint8_t, uint64_t );
        repair_lamp_ctx_all();
        // the dispatcher's internal disconnect walks the chain directly —
        // every link must be a live trailer or the teardown faults on it
        sanitize_slave_walk( trailer );
        sanitize_slave_walk( prev );
        dump_chain( "pre-detach(slot)" );
        native_chain_call_active.store( true, std::memory_order_release );
        detach_dispatch_hook->get_original< detach_fn_t >()( game_actor, 1, 0 );
        native_chain_call_active.store( false, std::memory_order_release );
        dump_chain( "post-detach(slot)" );
        // the dispatcher unconditionally clears the truck's towed bookkeeping
        // — put it back when the trailer we dropped was mid-chain and
        // something is still on the wheel
        void* truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
        void* prev_towed = truck != nullptr && readable_ptr( truck )
            ? prism::offsets::field< void* >( truck, 0x15A8 ) : nullptr;
        if ( prev_towed != nullptr && prev_towed != trailer &&
             joint( prev_towed ) != nullptr && truck != nullptr &&
             set_towed_trailer_fn != nullptr &&
             prism::offsets::field< void* >( truck, 0x15A8 ) == nullptr )
        {
            set_towed_trailer_fn( truck, true, prev_towed );
        }
        if ( joint( trailer ) == nullptr ) return true;
        prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_trailer ) = prev;
        return false;
    }

    // same trick on the attach side: the dispatcher takes the trailer to
    // attach from game_actor+0xC8. Point it at our trailer, call it, put the
    // old value back when it declines. Native attach = native cables, native
    // brakes/lights and a full five-wheel registration the replica only
    // approximates
    bool native_attach_via_slot( void* trailer )
    {
        auto* game_actor = CCore::g_instance->get_game_actor();
        if ( game_actor == nullptr || attach_dispatch_hook == nullptr ) return false;
        void* prev = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_trailer );
        prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_trailer ) = trailer;
        // sever a parked-piece claim for the call: the dispatcher registers
        // the trailer's whole slave chain, and a limbo parked piece behind it
        // aborts the process silently (mid-chain record factory purecall)
        void* claim = readable_ptr( trailer )
            ? prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) : nullptr;
        const bool severed = claim != nullptr && joint( claim ) == nullptr &&
            claim_edge_ours( trailer, claim );
        if ( severed )
        {
            prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = nullptr;
        }
        native_chain_call_active.store( true, std::memory_order_release );
        attach_dispatch_hook->get_original< native_attach_dispatch_fn >()( game_actor, 1, 0, nullptr );
        native_chain_call_active.store( false, std::memory_order_release );
        if ( severed && readable_ptr( trailer ) )
        {
            prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = claim;
        }
        prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_trailer ) = prev;
        return joint( trailer ) != nullptr;
    }

    // rebuild one cable/hose edge: rebuild_lines walks the trailer's own
    // slave chain, so the link is nulled for the call or the rebuild draws
    // hoses through the whole parked section behind it

    // rebuild EVERY live cable edge then apply the cables cvar once: the
    // per-edge rebuilds create the line entities, the cvar pass draws them.
    // A dead edge gets (front, nullptr) — the native "nothing attached"
    // clear — instead of a phantom hose into a parked section

    // parked-trailer lamp pin: the lamp update reads the SOURCE vehicle's
    // +0x2A0 mask through [[veh+0x110]+0x18], and the propagator keeps
    // repointing a parked trailer's ctx at the chain head — so it mirrors
    // the truck's brake lights forever. Pin its own mask byte at 0 AND
    // repoint its source at itself: it reads its own (zeroed) mask and goes
    // natively dark, while the live set keeps the real feed. The native
    // attach flow rewrites the source on recouple. Non-live is the test
    // (not joint==null): a trailer bolted to a parked parent is parked too
    // the lamp update reads the SOURCE vehicle's +0x2A0 through [ctx+0x18]
    // for the mask channels, but most channels (reverse, blinkers, aux)
    // read scalar fields straight off the light ctx itself — every offset
    // below was verified against the channel bodies in the exe lamp update
    // (bytes/words/floats only, no pointers)

    // parked trailers keep their slave links for visibility, so the game
    // still registers/feeds straggler lamp components inside a live
    // vehicle's ctx buckets. The comps carry no reliable trailer
    // back-pointer, so ownership is proven the other way: a comp is a parked
    // trailer's if the trailer's own fields reference it. Matched entries
    // are pushed lamp-off through their own +0xD8 vfunc and unlinked — the
    // native attach re-registers them on recouple


    // diag: dump the section's line-entity lists — which step drops the
    // hoses behind the head, and which trailer's list actually owns them
    inline void dump_lines( const char* tag, void* head )
    {
        for ( void* t = head; t != nullptr && readable_ptr( t ); )
        {
            char* const c = static_cast< char* >( t );
            void* b = *reinterpret_cast< void** >( c + 0xE98 );
            void* e = *reinterpret_cast< void** >( c + 0xEA0 );
            const uintptr_t n = ( b != nullptr && e != nullptr && e >= b )
                ? static_cast< uintptr_t >( reinterpret_cast< char* >( e ) -
                    reinterpret_cast< char* >( b ) ) / 8 : 0;
            CCore::g_instance->info( "lines {} {:p} n={} f={} joint={:p}",
                tag, t, n, *reinterpret_cast< uint8_t* >( c + 0xE8D ), joint( t ) );
            void* s = prism::offsets::field< void* >( t, prism::offsets::trailer_slave );
            t = ( s != t && is_trailer_obj( s ) ) ? s : nullptr;
        }
    }

    void disconnect_now( void* trailer, void* parent, const bool parent_is_truck )
    {
        dump_lines( "pre-detach", trailer );
        // a trailer sitting on the truck (or straight on the fifth wheel)
        // gets the NATIVE T-detach: the dispatcher drives the whole proper
        // sequence — joint release, parked-state record, traffic obstacle,
        // electrical cut — producing a REAL parked trailer instead of a
        // replica's approximation
        if ( parent_is_truck && joint( trailer ) != nullptr && detach_dispatch_hook != nullptr )
        {
            CCore::g_instance->info( "disconnect {:p}: native detach via trailer-of-interest", trailer );
            if ( native_detach_via_slot( trailer ) )
            {
                for ( void* t = trailer; t != nullptr; )
                {
                    mark_detached( t );
                    mark_plugin_detached( t );
                    void* next = prism::offsets::field< void* >( t, prism::offsets::trailer_slave );
                    t = ( next != t && next != nullptr && joint( next ) != nullptr ) ? next : nullptr;
                }
                const int di = canon_index_of( trailer );
                if ( di >= 0 ) detached_idx[ di ] = true;
                if ( trailer == truck_towed ) truck_towed = nullptr;
                // the parked machine hid it the moment the joint died — the
                // plugin wants it to stay a world object
                unhide_detached();
                if ( trailer_teleport_hook != nullptr )
                {
                    hook_xform_t xf{};
                    if ( hook_world( trailer, xf ) )
                    {
                        using teleport_fn_t = void __fastcall( void*, void* );
                        trailer_teleport_hook->get_original< teleport_fn_t >()( trailer, &xf );
                    }
                }
                // the gear-crank noise the native path skips while the brace
                // model is missing
                play_brace_sound( trailer, false );
                traffic_detach_done.store( true, std::memory_order_release );
                return;
            }
            CCore::g_instance->info( "disconnect {:p}: native detach declined — manual fallback", trailer );
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
        vcall< prism::physics_trailer_u_set_trailer_brace_fn >( trailer, prism::offsets::vfunc_set_brace, true );

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
        // hide the slave chain for the call: the disconnect walker tears
        // down every member it reaches and faults on a stale interior link
        // at exe+0x788CB7 — every fault resets canon, which disables every
        // canon-gated protection for 90 frames (that is why the detached
        // section's lamps stayed lit). A member still bolted behind the
        // head is the parked section that STAYS, not part of the departing
        // edge: only the head's own teardown runs; the link goes back after
        void* kept_slave = readable_ptr( trailer )
            ? prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) : nullptr;
        if ( kept_slave != nullptr )
        {
            prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = nullptr;
        }
        native_chain_call_active.store( true, std::memory_order_release );
        vcall< prism::physics_trailer_u_disconnect_fn >( trailer, prism::offsets::vfunc_disconnect );
        native_chain_call_active.store( false, std::memory_order_release );
        const bool section_behind = kept_slave != nullptr && joint( kept_slave ) != nullptr;
        if ( kept_slave != nullptr && readable_ptr( trailer ) )
        {
            prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = kept_slave;
        }

        dump_lines( "post-vfunc", trailer );

        // tell every component on both vehicles the coupling is gone — the
        // native detach notifies the vehicle component list (+0xE98) on both
        // sides; a loaded trailer's cargo constraints corrupt the physics
        // sweep if they are never told. A section head only broke its FRONT
        // edge though — its rear connector to the slave is still a live
        // coupling, so the trailer-level "you are decoupled" notifications
        // (component notify, decoupled flag, connector unpair) would tear
        // the T1->T2 pair down too — exactly the missing hoses the user
        // reports. The pair hoses on the broken edge die through the
        // front's outgoing-lines clear instead
        if ( notify_decoupled_fn != nullptr )
        {
            if ( parent != nullptr ) notify_decoupled_fn( parent );
            if ( !section_behind ) notify_decoupled_fn( trailer );
        }
        if ( clear_detached_flag_fn != nullptr )
        {
            if ( parent != nullptr && parent_is_truck ) clear_detached_flag_fn( parent, 1, 1 );
            if ( !section_behind ) clear_detached_flag_fn( trailer, 1, 1 );
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

        // the trailer's own line entities die with the joint: set_lines is the
        // native teardown the detach body calls on the now-free piece —
        // without it the hoses on the detached trailer stay frozen visible,
        // drawn between the trailer and nothing
        if ( set_lines_fn != nullptr )
        {
            set_lines_fn( trailer, 0, 1 );
        }
        dump_lines( "post-setlines", trailer );

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
        // the game applies spring brakes the same way on a jointless trailer —
        // the forced byte is the other half of the native spring-brake state
        prism::offsets::field< float >( trailer, prism::offsets::trailer_brake ) = 1.f;
        prism::offsets::field< float >( trailer, prism::offsets::trailer_service_brake ) = 0.f;
        prism::offsets::field< uint8_t >( trailer, prism::offsets::trailer_brake_forced ) = 1;
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
            dump_lines( "post-rebuild", trailer );
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
        // unpairs the connector LISTS of the pair — on a section head that
        // reaches the still-paired rear connector too, so skip it when a
        // live section is behind: the broken edge's hoses are already gone
        // with the front's outgoing-lines clear
        if ( parent != nullptr && pair_vehicles_fn != nullptr && !section_behind )
        {
            pair_vehicles_fn( parent, trailer );
        }
        dump_lines( "post-pair", trailer );
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
            dump_lines( "post-cables", trailer );
        }
        if ( trailer == truck_towed ) truck_towed = nullptr;
        // the native detach lowers the landing gear with its crank anim —
        // the replica plays the sound already; the anim drives the physical
        // gear state on models that have the brace_model_anim node. Rewind
        // the progress field first (+0x1270) so the anim player actually runs
        // the wind-down instead of snapping
        if ( brace_anim_fn != nullptr )
        {
            prism::offsets::field< float >( trailer, 0x1270 ) = 1.f;
            brace_anim_fn( trailer, 1, 1.f / 3.f );
        }
        // give the freshly detached trailer the NATIVE parked registration —
        // the dispatcher gives it a real parked-state record like the head
        // gets on a T-detach, so the trailer is never left in limbo (limbo
        // pieces vanish when the head's dispatcher sweeps)
        if ( joint( trailer ) == nullptr )
        {
            native_detach_via_slot( trailer );
        }
        // the parked machine hides a trailer it can no longer reach through
        // the live chain — re-assert it before the frame ends
        unhide_detached();
        // joint is dead now — a detach genuinely completed, so a later "whole
        // set coupled" state means the user re-assembled it and traffic may
        // be restored
        traffic_detach_done.store( true, std::memory_order_release );
    }

    void connect_now( void* trailer, void* parent, const bool parent_is_truck )
    {

        // re-check hook proximity at execution time: the trailer may have been
        // nudged between queueing and this sim tick — never teleport it
        if ( hook_far( trailer, parent, parent_is_truck ) )
        {
            CCore::g_instance->warning( "Carreta muito longe — aproxime dando ré para engatar" );
            return;
        }

        // a trailer still bolted to a parked parent would take a second live
        // joint — two constraints pull it different ways and the solver
        // launches it (the 2m hop on attach). Release the old coupling first;
        // its slave link stays hidden during the walk so members behind it
        // keep their section state
        if ( joint( trailer ) != nullptr )
        {
            void* kept_slave = prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave );
            if ( kept_slave != nullptr )
            {
                prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = nullptr;
            }
            vcall< prism::physics_trailer_u_disconnect_fn >( trailer, prism::offsets::vfunc_disconnect );
            if ( kept_slave != nullptr && readable_ptr( trailer ) )
            {
                prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = kept_slave;
            }
        }

        // a trailer going on the truck (or straight on the fifth wheel) gets
        // the NATIVE T-attach first: the dispatcher drives the whole proper
        // sequence through the trailer-of-interest slot — settle, joint,
        // towed registration, connector pairing, cables, anchors — native
        // cables/brakes/lights the manual replica only approximates. It only
        // runs while the piece is under the hook (the proximity check above
        // already gated it); a decline falls through to the replica
        if ( parent_is_truck && joint( trailer ) == nullptr &&
             attach_dispatch_hook != nullptr && native_attach_via_slot( trailer ) )
        {
            CCore::g_instance->info( "connect {:p}: native attach via trailer-of-interest", trailer );
            for ( void* t = trailer; t != nullptr; )
            {
                unmark_detached( t );
                unmark_plugin_detached( t );
                const int ti = canon_index_of( t );
                if ( ti >= 0 ) detached_idx[ ti ] = false;
                void* next = prism::offsets::field< void* >( t, prism::offsets::trailer_slave );
                t = ( next != t && next != nullptr && joint( next ) != nullptr ) ? next : nullptr;
            }
            if ( truck_towed == trailer )
            {
                auto* game_actor = CCore::g_instance->get_game_actor();
                void* truck = game_actor != nullptr
                    ? prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck ) : nullptr;
                if ( truck != nullptr )
                {
                    if ( set_towed_trailer_fn != nullptr &&
                         prism::offsets::field< void* >( truck, 0x15A8 ) != trailer )
                    {
                        set_towed_trailer_fn( truck, true, trailer );
                    }
                    prism::offsets::field< uint32_t >( truck, prism::offsets::truck_coupling_state ) = 5;
                }
            }
            play_brace_sound( trailer, true );
            dump_lines( "post-connect-native", trailer );
            unhide_detached();
            return;
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
        native_chain_call_active.store( true, std::memory_order_release );
        vcall< prism::physics_trailer_u_connect_fn >( trailer, prism::offsets::vfunc_connect,
            parent, vec, 0, parent_is_truck || parent_coupling != 0, slave_hook );
        native_chain_call_active.store( false, std::memory_order_release );

        // the +0xC8 visibility walk only reaches a trailer through slave
        // links: the native chain writes the claim during its own connect,
        // the replica must do it too or a trailer-parent attach leaves the
        // piece unreachable and the parked machine hides it. Never clobber a
        // jointed slave — hk_connect_slave refuses double-joining anyway
        if ( !parent_is_truck && parent != nullptr )
        {
            void** pslave = &prism::offsets::field< void* >( parent, prism::offsets::trailer_slave );
            void* cur = *pslave;
            if ( cur != trailer &&
                 ( cur == nullptr || !readable_ptr( cur ) || joint( cur ) == nullptr ) )
            {
                *pslave = trailer;
                clear_claim_edge( parent );
            }
        }

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
            clear_detached_flag_fn( parent, 1, 1 );
            clear_detached_flag_fn( trailer, 1, 1 );
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
        if ( brace_anim_fn != nullptr )
        {
            brace_anim_fn( trailer, 0, 1.f / 3.f );
        }
        // pieces that stayed jointed behind the reattached head kept their
        // parked half-state — re-assert them too
        dump_lines( "post-connect", trailer );
        unhide_detached();
    }

    // puts back what a cargo load/unload respawn destroyed: every marked
    // chain slot whose fresh object came back jointed is disconnected again
    // and teleported to the transform it was parked at. Runs on the sim
    // thread (queued op), descending index order so a tail trailer parked
    // on its own leaves before the section it was taken with — each marked
    // slot is the head of what the player actually detached

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
                // the op bodies run native flows start to finish — a fault
                // anywhere inside (record factory, settle, join) must not
                // reach the game. Canon only resets when its pointers are
                // actually dead, or a fault would relabel the UI buttons
                __try
                {
                    if ( op.kind == pending_kind::connect )
                    {
                        connect_now( op.trailer, op.parent, op.parent_is_truck );
                    }
                    else if ( op.kind == pending_kind::restore )
                    {
                        restore_parked_if_enabled();
                    }
                    else
                    {
                        disconnect_now( op.trailer, op.parent, op.parent_is_truck );
                    }
                }
                __except ( EXCEPTION_EXECUTE_HANDLER )
                {
                    const bool canon_alive = !canon_dead();
                    CCore::g_instance->warning( "pending op faulted — canon {}",
                                                canon_alive ? "kept" : "reset" );
                    if ( !canon_alive ) reset_trailer_state();
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

    // every container seen by the insert hook gets cached here and pruned —
    // the position-find region (exe+0x94E3CB) is ALSO reached by callers
    // outside the insert (obstacle erase on un-park) and dereferences +0x88
    // unchecked (AV at exe+0x94E45C 48ms after a detach freed the node).
    // Orphans appear at EVENTS (a joint drop frees the obstacle's owner
    // node), so the dense prune runs only inside a post-detach window plus
    // a slow background cadence — a per-frame full scan was 2 VirtualQuery
    // syscalls per element and the cost grew with the traffic lists
    void* lane_containers[ 128 ] = {};
    uint32_t lane_prune_frames = 0;
    uint32_t lane_prune_idle = 0;

    // the native "park this trailer" sequence a detached piece needs to
    // physically stand — the same tail disconnect_now runs, minus the
    // parent bookkeeping a save-loaded/free trailer does not have:
    // brace vfunc (registers the obstacle + support), spring brake fields,
    // the crank anim that actually BUILDS the landing-gear legs (without
    // the anim the +0x126C state reads down but no physical support body
    // exists — the nose falls through the floor), the traffic notifies and
    // the type-2 obstacle-anchor relink. Once per park event — the anim
    // rewind replays the manivela, so retries use the bare vfunc only
    void native_park_detached( void* t )
    {
        if ( t == nullptr || !is_trailer_obj( t ) ) return;
        __try
        {
            if ( prism::offsets::field< void* >( t, prism::offsets::trailer_parked_state ) != nullptr )
            {
                prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked ) = 1;
                prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked_registered ) = 1;
            }
            if ( plausible_vfunc( t, 0x328 / 8 ) )
            {
                vcall< prism::physics_trailer_u_set_trailer_brace_fn >( t, 0x328 / 8, true );
            }
            prism::offsets::field< float >( t, prism::offsets::trailer_brake ) = 1.f;
            prism::offsets::field< float >( t, prism::offsets::trailer_service_brake ) = 0.f;
            prism::offsets::field< uint8_t >( t, prism::offsets::trailer_brake_forced ) = 1;
            if ( brace_anim_fn != nullptr )
            {
                prism::offsets::field< float >( t, 0x1270 ) = 1.f;
                brace_anim_fn( t, 1, 1.f / 3.f );
            }
            if ( notify_decoupled_fn != nullptr )
            {
                notify_decoupled_fn( t );
            }
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
            lane_prune_frames = 240;
        }
        __except ( EXCEPTION_EXECUTE_HANDLER )
        {
            CCore::g_instance->warning( "native park sequence faulted on {:p}", t );
        }
    }

    // does this element still have a live owner node? SEH reads the field
    // directly — VirtualQuery-per-element is the syscall cost that scaled
    // with the world's obstacle lists
    bool lane_element_dead( void* el )
    {
        if ( el == nullptr ) return true;
        void* owner = nullptr;
        __try
        {
            owner = *reinterpret_cast< void** >( static_cast< uint8_t* >( el ) + 0x88 );
        }
        __except ( EXCEPTION_EXECUTE_HANDLER )
        {
            return true;
        }
        return owner == nullptr;
    }

    void prune_lane_containers()
    {
        for ( auto& c : lane_containers )
        {
            if ( c == nullptr ) continue;
            if ( !readable_ptr( c ) ||
                 !readable_ptr( static_cast< uint8_t* >( c ) + 0x10 ) )
            {
                c = nullptr;
                continue;
            }
            __try
            {
                void** elements = *reinterpret_cast< void*** >( static_cast< uint8_t* >( c ) + 8 );
                auto* count = reinterpret_cast< uint64_t* >( static_cast< uint8_t* >( c ) + 0x10 );
                if ( elements == nullptr || !readable_ptr( elements ) || *count > 0x10000 )
                {
                    c = nullptr;
                    continue;
                }
                uint64_t n = *count;
                for ( uint64_t i = n; i-- > 0; )
                {
                    void* el = elements[ i ];
                    if ( lane_element_dead( el ) )
                    {
                        CCore::g_instance->info( "lane obstacle: pruned dead entry {:p}", el );
                        memmove( &elements[ i ], &elements[ i + 1 ], ( n - i - 1 ) * sizeof( void* ) );
                        --n;
                    }
                }
                if ( n != *count )
                {
                    // a worker mid-scan with a stale count dereferences
                    // elements[n] — leave the last LIVE entry there instead
                    // of the dead one the memmove exposed
                    if ( n > 0 ) elements[ n ] = elements[ n - 1 ];
                    *count = n;
                }
            }
            __except ( EXCEPTION_EXECUTE_HANDLER )
            {
                // freed or reallocated mid-prune — drop the cache entry
                c = nullptr;
            }
        }
    }

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
                    // remember the container — the per-frame prune keeps the
                    // erase/scan paths outside this insert crash-free too
                    for ( auto& c : lane_containers )
                    {
                        if ( c == container ) break;
                        if ( c == nullptr ) { c = container; break; }
                    }
                    void** elements = *reinterpret_cast< void*** >( static_cast< uint8_t* >( container ) + 8 );
                    auto* count = reinterpret_cast< uint64_t* >( static_cast< uint8_t* >( container ) + 0x10 );
                    if ( elements != nullptr && readable_ptr( elements ) && *count <= 0x10000 )
                    {
                        uint64_t n = *count;
                        for ( uint64_t i = n; i-- > 0; )
                        {
                            void* el = elements[ i ];
                            if ( lane_element_dead( el ) )
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

    // Chase-camera target setter (base vfunc 31, 0x14063C2D0): whenever the
    // native resolver tries to anchor the camera on a tracked trailer whose
    // physics joint is dead, swap the target for null — the same "no
    // trailer" state the game uses when driving solo, so camera 2 follows
    // the truck with all of its native orbit/zoom handling intact

    // Chase-camera update (vfunc 32, 0x140AC75A0): the camera's own chain
    // walk also follows trailer_slave without checking joints — the same
    // shape as combo_length. Hide every live->dead link for the duration of
    // the call so the camera measures the live combination only (restored
    // right after — the links still carry the parked pieces' visibility)

    // lamp-feed mask setter (0x1409DB4B0, signature-verified): the lamp
    // propagator fans the chain head's feed down the attach tree through
    // this setter — a canon trailer outside the live convoy must stay
    // electrically dark (its mask pins at 0 in light_watch_tick too, but
    // the propagator rewrites it every tick, so the write has to be
    // suppressed at the funnel)

    // the lamp-feed propagator (0x140790DC0): copies the head's +0x2A0 mask
    // DWORD down every trailer_slave link with raw stores — bypasses the
    // mask setter, so hk_lamp_mask never sees it. Called per tick on the
    // sim thread AND inside vfunc_disconnect's electrical cut, so a
    // once-per-frame pin can never win the race — a claimed parked trailer
    // shows brakes/blinkers/reverse as if coupled. Faithful port with one
    // change: a canon trailer outside the live convoy gets 0 instead of
    // the feed — the same value the native cut would leave it with
    using lamp_propagate_fn = void __fastcall( void* /*slave*/, uint32_t /*mask*/ );

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

    // apply_cables (0x1408762E0): draws/drops the cable entities for a
    // truck->trailer pair and walks trailer_slave unchecked. The native
    // detach uncables with (truck, nullptr) — a global clear that drops the
    // lines on the WHOLE live chain, not just the dead edge. While the
    // truck still tows, substitute the towed head so the call re-applies
    // the live set instead; a jointless trailer arg resolves to what's
    // actually on the wheel

    // the native line-state setter (0x140874010): (vehicle, state, force) —
    // state=0 calls vfunc+0x10 (hide) on every +0xE98 line entity, state=1
    // calls vfunc+0x08 (show). A trailer owns the hoses of its FRONT edge,
    // so a trailer whose front joint is alive still has its hoses
    // connected — a state=0 aimed at it is the native detach cascade
    // tearing down lines on trailers BEHIND the released one. Refuse the
    // hide while the joint lives; a real detach drops the joint first, so
    // legit teardown still lands a frame later

    // rebuild_lines draws the air/electric lines between a front vehicle
    // and a trailer — the resolver walks the +0xC8 chain, and with the
    // root pinned on a parked piece it draws hoses to (and between)
    // trailers that have no joint at all. Physical rule: a jointless
    // trailer carries no lines, and a jointed trailer's front is whatever
    // it is really joined to.
    // The native detach calls this once on the broken edge and the
    // recursion down trailer+0x1088 tears down every section behind it —
    // the hose cascade. A canon front outside the live chain that still
    // takes part in a real coupling keeps its cable objects: refuse those
    // calls; a real edge break always arrives with a jointless trailer arg
    // and falls through to the native nullptr cleanup

    // the game calls this to auto-attach a parent's slave trailer; refuse
    // every claim that isn't a real coupling: a detached slave, a slave
    // whose mother has no physics joint (a parked trailer must never pull
    // its neighbours through the parked machine), an already-jointed slave
    // (stale claim), a backward canon claim (leaf links point forward only),
    // or a jointless piece farther than the attach gap — a real coupling
    // candidate sits inside gap range, the visibility leaf does not
    void hk_connect_slave( prism::physics_trailer_u* self )
    {
        void* slave = self != nullptr
            ? prism::offsets::field< void* >( self, prism::offsets::trailer_slave ) : nullptr;
        const int si = canon_index_of( self ), ti = canon_index_of( slave );
        const bool mother_parked = si >= 0 && !trailer_is_live( self ) &&
            trailer_is_known_parked( self );
        // The native loader stages links before it creates every joint.
        // Leave those transitions to the game, but keep refusing claims that
        // belong to a trailer already known to be parked/detached.
        if ( !trailer_maintenance_ready() )
        {
            if ( is_detached( slave ) || is_plugin_detached( slave ) ||
                 mother_parked || claim_edge_ours( self, slave ) )
                return;
            connect_slave_hook->get_original< prism::physics_trailer_u_connect_slave_fn >()( self );
            return;
        }
        {
            const bool bad = is_detached( slave ) || is_plugin_detached( slave ) ||
                mother_parked || claim_edge_ours( self, slave ) ||
                ( ti >= 0 && joint( slave ) != nullptr ) ||
                ( si >= 0 && ti >= 0 && ti < si && claim_edge_ours( self, slave ) );
            if ( bad )
            {
                return;
            }
            // a jointless canon slave the machine wants to join while it
            // sits beyond real hook distance is the visibility leaf, not a
            // coupling — refuse it or the head's attach teleports the
            // separated piece onto its rear hook
            if ( ti >= 0 && joint( slave ) == nullptr &&
                 hook_gap( slave, self, false ) > attach_gap_limit )
            {
                return;
            }
        }
        connect_slave_hook->get_original< prism::physics_trailer_u_connect_slave_fn >()( self );
    }

    // physics joint-tree accumulator (0x1409555F0, signature-verified for
    // this build): every node calls the SAME vfunc (+0x18) on the child
    // linked at node+0x68, so a cyclic body graph — the visibility leaf plus
    // the parked machine's canonical back-claim landing between maintenance
    // passes — recurses until the stack overflows (0xC00000FD). Track the
    // nodes already on this thread's path: a revisit returns a zero
    // contribution instead of diving again
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

    // physical joint link (0x140919680): writes child+0x60 = parent and
    // parent+0x68 = child, then vfunc-walks the graph. The root finder
    // iterates node+0x60 with NO cycle guard — a cyclic pair hangs it
    // forever (the detach freeze: the visibility leaf and the canonical
    // back-claim both physicalized, two nodes each other's parent). A link
    // that would close a cycle can never be legitimate — refuse just that
    // edge and let every real link through
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
        }
        phys_link_hook->get_original< phys_link_fn >()( parent, child );
    }

    // CRT __purecall (0x141D23610): dead listener nodes on detached trailers
    // still get vfunc-called through pure slots by the parked machine —
    // the stock handler kills the process. Returning null is what the
    // caller's missing-implementation path tolerates; the parked record's
    // factory completes with a null member instead of dying
    // permanent crash tracer: any unhandled AV in the process logs the
    // faulting RIP and the registers the memory model needs — the silent
    // teardown aborts and the factory crash were both invisible without it.
    // First chance, pass-through (CONTINUE_SEARCH) so the game's own crash
    // handling still runs
    LONG WINAPI crash_tracer( EXCEPTION_POINTERS* ep )
    {
        if ( ep == nullptr || ep->ExceptionRecord == nullptr ) return EXCEPTION_CONTINUE_SEARCH;
        const auto code = ep->ExceptionRecord->ExceptionCode;
        if ( code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW &&
             code != 0xE24C4A02 /* purecall */ )
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        const auto rip = ep->ContextRecord != nullptr ? ep->ContextRecord->Rip : 0;
        const auto base = reinterpret_cast< uint64_t >( GetModuleHandle( nullptr ) );
        CCore::g_instance->warning(
            "AV 0x{:X} at exe+0x{:X} rax={:p} rbx={:p} rcx={:p} rdx={:p} rsi={:p} rdi={:p} r8={:p} addr={:p}",
            code, rip >= base ? rip - base : rip,
            ep->ContextRecord ? reinterpret_cast< void* >( ep->ContextRecord->Rax ) : nullptr,
            ep->ContextRecord ? reinterpret_cast< void* >( ep->ContextRecord->Rbx ) : nullptr,
            ep->ContextRecord ? reinterpret_cast< void* >( ep->ContextRecord->Rcx ) : nullptr,
            ep->ContextRecord ? reinterpret_cast< void* >( ep->ContextRecord->Rdx ) : nullptr,
            ep->ContextRecord ? reinterpret_cast< void* >( ep->ContextRecord->Rsi ) : nullptr,
            ep->ContextRecord ? reinterpret_cast< void* >( ep->ContextRecord->Rdi ) : nullptr,
            ep->ContextRecord ? reinterpret_cast< void* >( ep->ContextRecord->R8 ) : nullptr,
            ep->ExceptionRecord->NumberParameters > 1
                ? reinterpret_cast< void* >( ep->ExceptionRecord->ExceptionInformation[ 1 ] ) : nullptr );
        return EXCEPTION_CONTINUE_SEARCH;
    }
    void install_crash_tracer()
    {
        static PVOID h = AddVectoredExceptionHandler( 1, crash_tracer );
        if ( h != nullptr )
        {
            CCore::g_instance->info( "crash tracer installed" );
        }
    }
    // the component iterator at exe+0x927C60 dereferences the purecall result
    // unconditionally (call [rax+0x40] then mov rdx,[rax] + call [rdx+8] —
    // type switch on 0x3000/0x7000/0xF000/0x17000). Returning null crashes it;
    // hand it a benign component whose every vfunc returns null — eax=0 hits
    // no switch case so the iterator skips the dead component and moves on.
    // Only that function's range gets the fake: the record factory and the
    // 0x9277F4 compare-site rely on the null they already handle
    void* __fastcall purecall_null_vfunc( void* /*self*/ ) { return nullptr; }
    void* purecall_fake_vtbl[ 16 ] = {};
    struct purecall_fake_t { void* vtbl; uint8_t pad[ 248 ]; };
    purecall_fake_t purecall_fake = { purecall_fake_vtbl };

    void init_purecall_fake()
    {
        for ( auto& slot : purecall_fake_vtbl )
        {
            slot = reinterpret_cast< void* >( &purecall_null_vfunc );
        }
    }

    void* __fastcall hk_purecall( void* /*object*/ )
    {
        const auto ret = reinterpret_cast< uint64_t >( _ReturnAddress() );
        const auto base = reinterpret_cast< uint64_t >( GetModuleHandle( nullptr ) );
        const auto rva = ret - base;
        static uint64_t seen[ 32 ] = {};
        for ( auto& s : seen )
        {
            if ( s == ret ) break;
            if ( s == 0 )
            {
                s = ret;
                CCore::g_instance->warning( "purecall swallowed at exe+0x{:X}", rva );
                break;
            }
        }
        if ( rva >= 0x927C60 && rva < 0x927CBF ) return &purecall_fake;
        return nullptr;
    }

    // vfunc_disconnect implementation (vfunc 99, exe+0x789C20 — shared by
    // every trailer-class vtable): releases the joint, feeds the brake
    // channel, then walks trailer_slave writing the +0x2A0 lamp mask per
    // member. Every caller lands here — the T dispatcher, the queued
    // section teardown on the sim thread, our own vcall — so this is the
    // one point that can sever a link that is not a real departing member:
    // a jointless trailer behind the section is a parked claim we keep for
    // visibility, and a recycled trailer slot reads as a bogus pointer —
    // either one kills the chain walker mid-teardown
    // repoint a dead lamp-cut src at the member itself — the native teardown
    // writes [src+0x2A0] where src = [[m+0x110]+0x18]; a recycled slot pointing
    // into system DLL pages (KERNELBASE at exe+0x789CB7) puts that write onto
    // code. Must run on every trailer the dispatcher might walk — its internal
    // disconnect calls can bypass the vtable hook
    void repair_lamp_ctx( void* m )
    {
        if ( m == nullptr || !readable_ptr( m ) ) return;
        void* ctx = prism::offsets::field< void* >( m, prism::offsets::trailer_light_ctx );
        if ( ctx == nullptr || !readable_ptr( ctx ) ||
             !readable_ptr( static_cast< uint8_t* >( ctx ) + 0x18 ) ) return;
        void* src = prism::offsets::field< void* >( ctx, 0x18 );
        // the src must be a writable vehicle object — plausible_object alone
        // passes recycled slots whose vtable looks exe-like but whose
        // +0x2A0 page is read-only/freed (the KERNELBASE lamp-cut AV). The
        // teardown writes [src+0x2A0], so the target page must accept writes
        if ( src != nullptr && plausible_object( src ) )
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if ( VirtualQuery( static_cast< uint8_t* >( src ) + prism::offsets::trailer_lamp_mask,
                               &mbi, sizeof( mbi ) ) != 0 &&
                 mbi.State == MEM_COMMIT &&
                 ( mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_WRITECOPY ||
                   mbi.Protect == PAGE_EXECUTE_READWRITE || mbi.Protect == PAGE_EXECUTE_WRITECOPY ) )
            {
                return;
            }
        }
        prism::offsets::field< void* >( ctx, 0x18 ) = m;
    }
    void repair_lamp_ctx_all()
    {
        for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
        {
            repair_lamp_ctx( canon[ i ] );
        }
    }
    // walk the slave chain a native dispatcher is about to traverse: repair
    // every member's lamp-cut src (not only canon — parked claims chain
    // non-canon pieces too) and sever the first link that is not a trailer
    // object. The dispatcher's internal disconnect bypasses the vtable hook
    // and walks whatever the links point at — a recycled slot (observed as a
    // KERNELBASE page) faults the teardown at exe+0x789CB7 and the reset
    // wipes canon ordering, which is what relabels the UI buttons mid-game
    void sanitize_slave_walk( void* head )
    {
        for ( uint32_t depth = 0; head != nullptr && depth < max_trailers * 4; ++depth )
        {
            if ( !is_trailer_obj( head ) ) return;
            repair_lamp_ctx( head );
            void* next = readable_ptr( static_cast< uint8_t* >( head ) + prism::offsets::trailer_slave )
                ? prism::offsets::field< void* >( head, prism::offsets::trailer_slave ) : nullptr;
            if ( next == head ) return;
            if ( next != nullptr && !is_trailer_obj( next ) )
            {
                CCore::g_instance->warning( "sanitize: severed dead slave link {:p} -> {:p}", head, next );
                prism::offsets::field< void* >( head, prism::offsets::trailer_slave ) = nullptr;
                return;
            }
            head = next;
        }
    }
    void __fastcall hk_trailer_disconnect( void* trailer )
    {
        if ( trailer == nullptr || !is_trailer_obj( trailer ) ) return;
        // the walker's lamp cut writes [src+0x2A0] where src comes out of each
        // member's light ctx ([m+0x110]+0x18) — a src pointing at a recycled
        // vehicle slot kills the teardown mid-write on a foreign page
        // (exe+0x788CB7, rcx = a system-DLL address). Repoint a dead src at
        // the member itself — the same self-source the lamp pin uses on
        // parked pieces — so the write lands on a live object
        const auto repair_ctx = []( void* m ) { repair_lamp_ctx( m ); };
        repair_ctx( trailer );
        // during an actor-teleport reconcile the game may try to disconnect a
        // member mid-remount — the native member walk is what faults and its
        // failure resets canon, leaving every gate dead for 90 frames (the
        // F9 dive: truck through the floor, trailers launched). Minimal
        // release instead: joint + parent cleared, members left untouched —
        // the reconcile owns the remount
        if ( tp_cooldown > 0 )
        {
            prism::offsets::field< void* >( trailer, prism::offsets::trailer_physics_joint ) = nullptr;
            prism::offsets::field< void* >( trailer, prism::offsets::trailer_parent ) = nullptr;
            return;
        }
        // snapshot the validated members — the fault repair below uses it to
        // keep the pieces behind the head reachable instead of orphaning them
        void* chain_members[ max_trailers * 2 ] = {};
        uint32_t chain_n = 0;
        // a cyclic chain (leaf claim + real link both physicalized) would
        // walk forever — a real section is never deeper than the canon count
        uint32_t depth = 0;
        for ( void* t = trailer; t != nullptr && readable_ptr( t ); )
        {
            void* slave = prism::offsets::field< void* >( t, prism::offsets::trailer_slave );
            if ( slave == nullptr ) break;
            // sever EVERY jointless slave edge — a live section member always
            // carries a live joint, so a jointless target is a visibility
            // claim (ours OR the parked machine's canonical back-claim).
            // Gating on claim_edge_ours let the native walker step into a
            // game-written claim and die at exe+0x789CB7 — the recurring
            // "disconnect walk faulted" -> canon reset cascade
            const bool bad = slave == t || !is_trailer_obj( slave ) ||
                joint( slave ) == nullptr;
            if ( bad || ++depth > max_trailers )
            {
                prism::offsets::field< void* >( t, prism::offsets::trailer_slave ) = nullptr;
                clear_claim_edge( t );
                break;
            }
            repair_ctx( slave );
            if ( chain_n < max_trailers * 2 ) chain_members[ chain_n++ ] = slave;
            t = slave;
        }
        native_chain_call_active.store( true, std::memory_order_release );
        __try
        {
            trailer_disconnect_hook->get_original< prism::physics_trailer_u_disconnect_fn >()( trailer );
        }
        __except ( EXCEPTION_EXECUTE_HANDLER )
        {
            // the fault is a teardown-internal write on one member — the
            // trailer objects themselves stay alive. Resetting canon here
            // re-enumerates it in walk order: a piece the sweep already hid
            // never comes back (the vanishing T0) and the UI buttons get
            // relabelled onto different physical trailers. Only a dead canon
            // (world unload) justifies wiping it
            const bool canon_alive = !canon_dead();
            CCore::g_instance->warning( "disconnect walk faulted on {:p} — canon {}", trailer,
                                        canon_alive ? "kept" : "reset" );
            if ( !canon_alive ) reset_trailer_state();
            if ( readable_ptr( trailer ) )
            {
                // keep the members behind the head reachable — the parked
                // machine hides whatever the +0xC8 walk can no longer reach,
                // and nulling the root link here is what made trailer 2
                // vanish when the head's teardown faulted
                void* keep = nullptr;
                for ( uint32_t k = 0; k < chain_n; ++k )
                {
                    if ( is_trailer_obj( chain_members[ k ] ) )
                    {
                        keep = chain_members[ k ];
                        break;
                    }
                }
                prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = keep;
                // re-assert the detach marks or the per-frame connect
                // re-couples the trailer straight back (the fault ->
                // auto-reconnect loop from the log)
                mark_detached( trailer );
                mark_plugin_detached( trailer );
            }
            clear_claim_edge( trailer );
        }
        native_chain_call_active.store( false, std::memory_order_release );
    }

    // vfunc_connect impls (vfunc 98 — exe+0x639090 / exe+0x7890A0, shared by
    // the trailer-class vtables): the native join processes the slave chain
    // through SELF-RECURSIVE walkers (exe+0x6390C0 / exe+0x639220 call
    // themselves on trailer_slave) — a parked visibility claim pointing at
    // a limbo trailer (no record/actor) aborts it mid-join, and a cyclic
    // chain overflows the stack — both die with no crash dump. Hide every
    // slave edge whose target is jointless for the duration of the call:
    // claims are our fiction, the maintenance rebuilds them next frame,
    // and the original then sees only real section members
    void sever_parked_claims()
    {
        // not while the world assembles: trailers spawn before their joints
        // form, so every staged link reads jointless — cutting them mid-
        // reassembly strands the not-yet-connected members (Failed to init
        // update, trailer never spawns)
        if ( !trailer_maintenance_ready() ) return;
        for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
        {
            void* t = canon[ i ];
            if ( !readable_ptr( t ) ) continue;
            void* slave = prism::offsets::field< void* >( t, prism::offsets::trailer_slave );
            if ( slave == nullptr ) continue;
            if ( slave == t || !is_trailer_obj( slave ) )
            {
                prism::offsets::field< void* >( t, prism::offsets::trailer_slave ) = nullptr;
                clear_claim_edge( t );
                continue;
            }
            // a jointless slave link is ALWAYS a visibility claim — ours or
            // the game's own parked-machine back-claim (which our registry
            // never tagged, so claim_edge_ours misses it). A real member of
            // a departing section is always jointed, so cutting every
            // jointless edge for the duration of a native walk is safe — and
            // missing the game's back-claims is what let the dispatcher
            // wander into the parked set and destroy trailers 1/2 on the
            // head detach
            if ( joint( slave ) == nullptr )
            {
                prism::offsets::field< void* >( t, prism::offsets::trailer_slave ) = nullptr;
                clear_claim_edge( t );
            }
        }
    }
    void __fastcall hk_trailer_connect_a( void* trailer, void* parent, float3_t vec,
                                          int state, bool slave_hook, bool is_slave )
    {
        if ( trailer == nullptr || !is_trailer_obj( trailer ) ) return;
        native_chain_call_active.store( true, std::memory_order_release );
        sever_parked_claims();
        __try
        {
            trailer_connect_hook->get_original< prism::physics_trailer_u_connect_fn >()(
                trailer, parent, vec, state, slave_hook, is_slave );
        }
        __except ( EXCEPTION_EXECUTE_HANDLER )
        {
            // same canon-preserving rule as the disconnect fault: the trailer
            // objects stay alive through a walk fault, only a dead canon
            // justifies wiping it (relabelled buttons, vanished pieces)
            const bool canon_alive = !canon_dead();
            CCore::g_instance->warning( "connect walk faulted on {:p} — canon {}", trailer,
                                        canon_alive ? "kept" : "reset" );
            if ( readable_ptr( trailer ) )
                prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = nullptr;
            clear_claim_edge( trailer );
            if ( !canon_alive ) reset_trailer_state();
        }
        native_chain_call_active.store( false, std::memory_order_release );
    }
    void __fastcall hk_trailer_connect_b( void* trailer, void* parent, float3_t vec,
                                          int state, bool slave_hook, bool is_slave )
    {
        if ( trailer == nullptr || !is_trailer_obj( trailer ) ) return;
        native_chain_call_active.store( true, std::memory_order_release );
        sever_parked_claims();
        __try
        {
            trailer_connect_hook2->get_original< prism::physics_trailer_u_connect_fn >()(
                trailer, parent, vec, state, slave_hook, is_slave );
        }
        __except ( EXCEPTION_EXECUTE_HANDLER )
        {
            const bool canon_alive = !canon_dead();
            CCore::g_instance->warning( "connect walk faulted on {:p} — canon {}", trailer,
                                        canon_alive ? "kept" : "reset" );
            if ( readable_ptr( trailer ) )
                prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = nullptr;
            clear_claim_edge( trailer );
            if ( !canon_alive ) reset_trailer_state();
        }
        native_chain_call_active.store( false, std::memory_order_release );
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
    using native_detach_dispatch_fn = void __fastcall( void* /*game_actor*/, uint8_t /*dl*/, uint64_t /*r8*/ );
    void __fastcall hk_detach_dispatch_raw( void* self, uint8_t dl, uint64_t r8 );
    void __fastcall hk_detach_dispatch( void* self, uint8_t dl, uint64_t r8 )
    {
        // a fault on a respawned-away trailer (workshop/garage rebuilds kill
        // canon pointers mid-frame) must never reach the game — reset and
        // let the next pass rebuild from live state
        __try { hk_detach_dispatch_raw( self, dl, r8 ); }
        __except ( EXCEPTION_EXECUTE_HANDLER )
        {
            const bool canon_alive = !canon_dead();
            CCore::g_instance->warning( "detach dispatch faulted — canon {}",
                                        canon_alive ? "kept" : "reset" );
            if ( !canon_alive ) reset_trailer_state();
        }
    }

    void __fastcall hk_detach_dispatch_raw( void* self, uint8_t dl, uint64_t r8 )
    {
        // a save load frees every trailer we cached — wipe before any mark
        // touches a dead pointer
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
        // NO severing: the reference calls the dispatcher with the full
        // canonical claim chain visible — parked pieces sit at the tail of
        // the departing head's slave chain and the dispatcher's parked-
        // section registration keeps them. Cutting the link makes them
        // unreachable exactly when the sweep runs (vanishing 1/2)
        repair_lamp_ctx_all();
        // the dispatcher's internal disconnect walks the chain directly —
        // every link must be a live trailer or the teardown faults on it
        sanitize_slave_walk( prism::offsets::field< void* >( self, prism::offsets::game_actor_trailer ) );
        if ( towed != nullptr ) sanitize_slave_walk( towed );
        dump_chain( "pre-detach(T)" );
        native_chain_call_active.store( true, std::memory_order_release );
        detach_dispatch_hook->get_original< native_detach_dispatch_fn >()( self, dl, r8 );
        native_chain_call_active.store( false, std::memory_order_release );
        dump_chain( "post-detach(T)" );
        if ( self == nullptr ) return;
        void* t = prism::offsets::field< void* >( self, prism::offsets::game_actor_trailer );
        if ( t != nullptr && joint( t ) == nullptr )
        {
            pending_native_detach = nullptr;
            mark_detached( t );
            // cut the truck<->head hose pair the dispatcher can leave half-
            // registered when the tow slot was re-pinned by maintenance —
            // same teardown the manual detach tail runs on a parent detach:
            // lines clear, connector unpair (the +0x798..+0x7A0 record is
            // what keeps drawing hoses on the detached trailer), cvar reset
            if ( set_lines_fn != nullptr )
            {
                set_lines_fn( t, 0, 1 );
            }
            if ( truck != nullptr && pair_vehicles_fn != nullptr )
            {
                pair_vehicles_fn( truck, t );
            }
            if ( truck != nullptr && apply_cables_fn != nullptr )
            {
                // mode 0, not the stale cvar: the mode console command lands
                // frames later, and mode>0 with +0xC8 on the parked head
                // makes the applier redraw the truck hoses we just cut
                apply_cables_fn( truck, nullptr, 0, 0, 0 );
            }
            // NO slave re-link: parked trailers are independent pieces — each
            // stays visible and OFF through its own parked record. The slave
            // link between parked pieces only draws phantom hoses and makes
            // the parked machine treat them as one departing-prone section
        }
        else if ( t != nullptr )
        {
            // joint still alive after dispatch = the async crank animation
            // runs before the drop — the lines refresh must not re-draw the
            // truck hoses the native sequence is cutting right now
            pending_native_detach = t;
            pending_native_age = 0;
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
    void __fastcall hk_attach_dispatch_raw( void* self, uint8_t dl, uint64_t r8, void* settle_override );
    void __fastcall hk_attach_dispatch( void* self, uint8_t dl, uint64_t r8, void* settle_override )
    {
        __try { hk_attach_dispatch_raw( self, dl, r8, settle_override ); }
        __except ( EXCEPTION_EXECUTE_HANDLER )
        {
            const bool canon_alive = !canon_dead();
            CCore::g_instance->warning( "attach dispatch faulted — canon {}",
                                        canon_alive ? "kept" : "reset" );
            if ( !canon_alive ) reset_trailer_state();
        }
    }

    void __fastcall hk_attach_dispatch_raw( void* self, uint8_t dl, uint64_t r8, void* settle_override )
    {
        if ( canon_dead() ) reset_trailer_state();
        // the user's chosen attach target beats the game's proximity pick —
        // but ONLY while its own front hook is the jointless hook closest
        // to the fifth wheel: comparing body centers refuses the pick
        // whenever another parked piece sits closer to the truck body
        // (a dolly parked beside the truck blocks the slot forever). Hook
        // gap measures the real coupling geometry; a stale pick aimed at a
        // piece not under the hook would no-op every T press
        if ( attach_target != nullptr && self != nullptr )
        {
            if ( readable_ptr( attach_target ) && joint( attach_target ) == nullptr )
            {
                void* truck = prism::offsets::field< void* >( self, prism::offsets::game_actor_truck );
                const float tgap = truck != nullptr
                    ? hook_gap( attach_target, truck, true ) : -1.f;
                bool closest = tgap >= 0.f;
                for ( uint32_t i = 0; closest && i < max_trailers && canon[ i ] != nullptr; ++i )
                {
                    void* o = canon[ i ];
                    if ( o == attach_target || !readable_ptr( o ) || joint( o ) != nullptr )
                    {
                        continue;
                    }
                    const float g = hook_gap( o, truck, true );
                    if ( g >= 0.f && g < tgap )
                    {
                        closest = false;
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
        // a selected target keeps its detach marks until the dispatcher is
        // actually aiming at it — released here, right before the native
        // call, so a picked-but-still-parked piece stays refused by the
        // game's own slave auto-attach
        bool released_marks = false;
        if ( head != nullptr && head == attach_target && is_plugin_detached( head ) )
        {
            release_detach_marks( head );
            released_marks = true;
        }
        if ( head != nullptr && is_plugin_detached( head ) )
        {
            // refuse only the game's own reassembly (cargo restore and the
            // slave auto-attach never have the trailer-of-interest aimed at
            // a parked piece). A real T-press does: the resolver put the
            // marked trailer in the slot because the player backed up to it
            // — the intent is explicit, release the marks and attach
            void* truck = readable_ptr( self )
                ? prism::offsets::field< void* >( self, prism::offsets::game_actor_truck ) : nullptr;
            hook_xform_t hxf{};
            const bool under_hook = truck != nullptr && hook_world( head, hxf ) &&
                hook_gap( head, truck, true ) < attach_gap_limit;
            if ( !under_hook )
            {
                CCore::g_instance->info( "attach dispatcher skipped: trailer {:p} is plugin-detached", head );
                return;
            }
            release_detach_marks( head );
            released_marks = true;
            CCore::g_instance->info( "attach dispatcher: T-attach released marks on {:p}", head );
        }
        // last-resort guard: a limbo trailer (no parked record AND no actor
        // in the game_actor slots) kills the game silently inside the native
        // attach — refuse the call instead
        if ( attach_target != nullptr && head == attach_target &&
            readable_ptr( attach_target ) &&
            prism::offsets::field< void* >( attach_target, prism::offsets::trailer_parked_state ) == nullptr &&
            find_trailer_actor( attach_target, self ) == nullptr )
        {
            CCore::g_instance->warning( "attach refused: target {:p} has no record/actor (limbo)", attach_target );
            return;
        }
        // sever a parked-piece claim on the trailer being attached: the
        // dispatcher registers the trailer's whole slave chain, and a limbo
        // parked piece behind it (mid-chain trailers never get a record —
        // the factory aborts on them) kills the game silently inside the
        // attach. A jointed slave is a real section member and stays
        void* severed_slave = nullptr;
        if ( head != nullptr && readable_ptr( head ) )
        {
            void* s = prism::offsets::field< void* >( head, prism::offsets::trailer_slave );
            if ( s != nullptr && joint( s ) == nullptr && claim_edge_ours( head, s ) )
            {
                severed_slave = s;
                prism::offsets::field< void* >( head, prism::offsets::trailer_slave ) = nullptr;
            }
        }
        // the dispatcher walks the chain for registration — every link must
        // be a live trailer object or the internal teardown faults on it
        sanitize_slave_walk( head );
        native_chain_call_active.store( true, std::memory_order_release );
        attach_dispatch_hook->get_original< native_attach_dispatch_fn >()( self, dl, r8, settle_override );
        native_chain_call_active.store( false, std::memory_order_release );
        if ( severed_slave != nullptr && readable_ptr( head ) )
        {
            prism::offsets::field< void* >( head, prism::offsets::trailer_slave ) = severed_slave;
        }
        if ( released_marks && joint( head ) == nullptr )
        {
            // the dispatcher ran but nothing coupled — re-protect the
            // still-parked piece or the slave auto-attach may pull it in
            mark_detached( head );
            mark_plugin_detached( head );
            const int hi = canon_index_of( head );
            if ( hi >= 0 ) detached_idx[ hi ] = true;
        }
        if ( attach_target != nullptr && joint( attach_target ) != nullptr )
        {
            CCore::g_instance->info( "attach target {:p} coupled natively", attach_target );
            attach_target = nullptr;
        }
        // the game attached a parked trailer through its own path (T, cargo
        // restore): clear its marks — the chain links are rebuilt every
        // frame by the refresh maintenance
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

    // the per-trailer teleport (0x140786320): the actor-teleport walks the
    // trailer list and calls this on each one to move it into its dock slot.
    // Trailer objects are NOT recreated — this is the call that actually
    // drags a detached trailer to the dock during cargo load/unload, where
    // it lands under its parent's hook and gets re-jointed. A
    // plugin-detached trailer is a world object: it is never teleported,
    // by any caller — dock alignment, garage, ferry, job start
    using trailer_teleport_fn = void __fastcall( void* /*trailer*/, void* /*transform*/ );

    // the actor teleport (0x1405DE1E0): rotates/moves truck + attached set
    // into the dock slots for the cargo cinematic — the same call travel
    // teleports use. Snapshot the rig on the first call of a sequence, then
    // arm the restore only when the move was short (a dock shuffle, not a
    // ferry/garage/job hop across the map)
    using actor_teleport_fn = void __fastcall( void* /*actor*/, void* /*arg*/, uint8_t, uint8_t );

    // resolves every pattern scanned / fixed-offset native helper used by the
    // coupling flow replicas; returns false when a hard requirement is missing
    bool resolve_native_fns()
    {
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
        get_cvar_float_fn = reinterpret_cast< prism::cvar_get_float_fn* >( memory::get_address_from_offset( 0x1401DB560 ) );
        g_traffic_cvar = reinterpret_cast< void* >( memory::get_address_from_offset( 0x142D45500 ) );
        traffic_manager_global = reinterpret_cast< void** >( memory::get_address_from_offset( 0x1436AF7C8 ) );
        register_anchor_fn = reinterpret_cast< prism::traffic_register_anchor_fn* >( memory::get_address_from_offset( 0x140284250 ) );
        relink_anchors_fn = reinterpret_cast< anchor_relink_fn* >( memory::get_address_from_offset( 0x140623C90 ) );
        anchor_reg_guard = reinterpret_cast< uint32_t* >( memory::get_address_from_offset( 0x143051824 ) );
        traffic_game_state = reinterpret_cast< uint32_t* >( memory::get_address_from_offset( 0x143051820 ) );

        // the remaining native attach/detach calls (see declarations above)
        trailer_settle_fn_ptr = reinterpret_cast< trailer_settle_fn* >( memory::get_address_from_offset( 0x1409FB700 ) );
        pair_vehicles_fn = reinterpret_cast< vehicle_pair_fn* >( memory::get_address_from_offset( 0x1408771A0 ) );
        apply_cables_fn = reinterpret_cast< vehicle_cables_fn* >( memory::get_address_from_offset( 0x1408762E0 ) );
        // the native line-state setter + the parked-record factory/manager,
        // all signature-verified fixed offsets for this build
        set_lines_fn = reinterpret_cast< line_state_fn* >( memory::get_address_from_offset( 0x140874010 ) );
        parked_record_factory = reinterpret_cast< parked_record_factory_fn* >( memory::get_address_from_offset( 0x1407B69E0 ) );
        parked_record_mgr = reinterpret_cast< void** >( memory::get_address_from_offset( 0x1436AF828 ) );
        apply_cables_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "apply_cables", memory::get_address_from_offset( 0x1408762E0 ),
            reinterpret_cast< uint64_t >( &hk_apply_cables ) );
        if ( apply_cables_hook != nullptr )
        {
            apply_cables_hook->hook();
        }
        // the line-state setter — refuse a hide aimed at a trailer whose
        // front joint is still bolted (the detach cascade)
        set_lines_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "set_lines", memory::get_address_from_offset( 0x140874010 ),
            reinterpret_cast< uint64_t >( &hk_set_lines ) );
        if ( set_lines_hook != nullptr )
        {
            set_lines_hook->hook();
        }
        actor_flag_fixup_fn = reinterpret_cast< actor_flag_fixup* >( memory::get_address_from_offset( 0x1405DCBC0 ) );
        native_attach_fn = reinterpret_cast< native_attach_dispatch_fn* >( memory::get_address_from_offset( 0x1405DCC20 ) );
        detach_dispatch_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "native_detach_dispatch", memory::get_address_from_offset( 0x1405DD410 ),
            reinterpret_cast< uint64_t >( &hk_detach_dispatch ) );
        if ( detach_dispatch_hook != nullptr )
        {
            detach_dispatch_hook->hook();
        }
        // the attach dispatcher + the trailer-state apply: the two funnels
        // every native re-join/reposition of a trailer goes through — hooked
        // so a plugin-detached trailer survives cargo load/unload untouched
        attach_dispatch_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "native_attach_dispatch", memory::get_address_from_offset( 0x1405DCC20 ),
            reinterpret_cast< uint64_t >( &hk_attach_dispatch ) );
        if ( attach_dispatch_hook != nullptr )
        {
            attach_dispatch_hook->hook();
        }
        trailer_state_apply_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "trailer_state_apply", memory::get_address_from_offset( 0x1405E14D1 ),
            reinterpret_cast< uint64_t >( &hk_trailer_state_apply ) );
        if ( trailer_state_apply_hook != nullptr )
        {
            trailer_state_apply_hook->hook();
        }
        trailer_teleport_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "trailer_teleport", memory::get_address_from_offset( 0x140786320 ),
            reinterpret_cast< uint64_t >( &hk_trailer_teleport ) );
        if ( trailer_teleport_hook != nullptr )
        {
            trailer_teleport_hook->hook();
        }
        actor_teleport_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "actor_teleport", memory::get_address_from_offset( 0x1405DE1E0 ),
            reinterpret_cast< uint64_t >( &hk_actor_teleport ) );
        if ( actor_teleport_hook != nullptr )
        {
            actor_teleport_hook->hook();
        }
        // chase-camera anchor setter (base vfunc 31): the resolver picks the
        // chain-end trailer, jointless ones included — redirect inside the
        // setter so the native flow anchors the last JOINTED trailer
        cam_set_target_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "cam_set_target", memory::get_address_from_offset( 0x14063C2D0 ),
            reinterpret_cast< uint64_t >( &hk_cam_set_target ) );
        if ( cam_set_target_hook != nullptr )
        {
            cam_set_target_hook->hook();
        }
        // lane-obstacle insert: dead array entries (owner-node cleared without
        // removal) crash the traffic worker — sanitize before the original
        lane_obstacle_insert_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "lane_obstacle_insert", memory::get_address_from_offset( 0x14094DFC0 ),
            reinterpret_cast< uint64_t >( &hk_lane_obstacle_insert ) );
        if ( lane_obstacle_insert_hook != nullptr )
        {
            lane_obstacle_insert_hook->hook();
        }
        // chain-length accumulator: the reimplementation stops the walk at
        // the first jointless (detached) trailer so every consumer — camera 2
        // included — measures only the live combination. The per-link hook
        // query it calls is resolved separately; if it fails to resolve the
        // hook falls back to the untouched original.
        combo_hook_pos_fn_ptr = reinterpret_cast< combo_hook_pos_fn* >(
            memory::get_address_from_offset( 0x140789880 ) );
        combo_length_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "combo_length", memory::get_address_from_offset( 0x140789AB0 ),
            reinterpret_cast< uint64_t >( &hk_combo_length ) );
        if ( combo_length_hook != nullptr )
        {
            combo_length_hook->hook();
        }
        // purecall handler: dead listener nodes on detached trailers call
        // through pure vfunc slots — swallow every one of them, process-wide
        init_purecall_fake();
        purecall_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "purecall", memory::get_address_from_offset( 0x141D23610 ),
            reinterpret_cast< uint64_t >( &hk_purecall ) );
        if ( purecall_hook != nullptr )
        {
            purecall_hook->hook();
            install_crash_tracer();
        }
        // trailer vfunc_disconnect impl — the single funnel every teardown
        // (T dispatcher, sim-thread section teardown, our vcall) passes
        // through; the detour sanitizes the slave chain it is about to walk
        trailer_disconnect_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "trailer_disconnect", memory::get_address_from_offset( 0x140789C20 ),
            reinterpret_cast< uint64_t >( &hk_trailer_disconnect ) );
        if ( trailer_disconnect_hook != nullptr )
        {
            trailer_disconnect_hook->hook();
        }
        // vfunc_connect has TWO impls across the trailer-class vtables —
        // the dolly uses a different class than the box trailers, so both
        // get the same claim-severing detour
        trailer_connect_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "trailer_connect_a", memory::get_address_from_offset( 0x140639090 ),
            reinterpret_cast< uint64_t >( &hk_trailer_connect_a ) );
        if ( trailer_connect_hook != nullptr )
        {
            trailer_connect_hook->hook();
        }
        trailer_connect_hook2 = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "trailer_connect_b", memory::get_address_from_offset( 0x1407890A0 ),
            reinterpret_cast< uint64_t >( &hk_trailer_connect_b ) );
        if ( trailer_connect_hook2 != nullptr )
        {
            trailer_connect_hook2->hook();
        }
        // lamp-mask setter: keeps parked trailers electrically dark while
        // they stay claimed in the attach tree for visibility
        lamp_mask_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "lamp_mask", memory::get_address_from_offset( 0x1409DB4B0 ),
            reinterpret_cast< uint64_t >( &hk_lamp_mask ) );
        if ( lamp_mask_hook != nullptr )
        {
            lamp_mask_hook->hook();
        }
        // the propagator's raw +0x2A0 stores bypass the mask setter — the
        // detached-trailer cut has to live here too or the pin loses the
        // per-tick race
        lamp_propagate_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "lamp_propagate", memory::get_address_from_offset( 0x140790DC0 ),
            reinterpret_cast< uint64_t >( &hk_lamp_propagate ) );
        if ( lamp_propagate_hook != nullptr )
        {
            lamp_propagate_hook->hook();
        }
        // joint-tree recursion guard: the accumulator walks node+0x68 chains
        // via a self-recursive vfunc — a cyclic graph overflows the stack
        // instantly (0xC00000FD). The detour returns early on a revisit
        joint_walk_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "joint_walk", memory::get_address_from_offset( 0x1409555F0 ),
            reinterpret_cast< uint64_t >( &hk_joint_walk ) );
        if ( joint_walk_hook != nullptr )
        {
            joint_walk_hook->hook();
        }
        // bidirectional joint link — refuses only the edge that would
        // close a +0x60/+0x68 cycle (find_root has no guard and hangs)
        phys_link_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "phys_link", memory::get_address_from_offset( 0x140919680 ),
            reinterpret_cast< uint64_t >( &hk_phys_link ) );
        if ( phys_link_hook != nullptr )
        {
            phys_link_hook->hook();
        }
        // chase-camera update (vfunc 32): the camera's own chain walk follows
        // trailer_slave without checking joints — hide live->dead links for
        // the call so it measures the live combination only
        cam_update_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "cam_update", memory::get_address_from_offset( 0x140AC75A0 ),
            reinterpret_cast< uint64_t >( &hk_cam_update ) );
        if ( cam_update_hook != nullptr )
        {
            cam_update_hook->hook();
        }
        get_cvar_int_fn = reinterpret_cast< cvar_get_int_fn* >( memory::get_address_from_offset( 0x1401CBE10 ) );
        cables_mode_cvar = reinterpret_cast< void* >( memory::get_address_from_offset( 0x142D485C0 ) );
        attach_settle_cvar = reinterpret_cast< void* >( memory::get_address_from_offset( 0x142D48200 ) );

        // the coupling/decoupling event poster + its const param block and the
        // two token strings, all fixed image offsets read out of the native
        // attach/detach call sites
        post_event_fn_ptr = reinterpret_cast< post_event_fn* >( memory::get_address_from_offset( 0x140623DA0 ) );
        event_param_block = reinterpret_cast< void* >( memory::get_address_from_offset( 0x142733198 ) );
        event_name_coupling = reinterpret_cast< const char* >( memory::get_address_from_offset( 0x14226E368 ) );
        event_name_decoupling = reinterpret_cast< const char* >( memory::get_address_from_offset( 0x14226E4B0 ) );
        brace_anim_fn = reinterpret_cast< trailer_brace_anim_fn* >( memory::get_address_from_offset( 0x140636F50 ) );

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
        CCore::g_instance->warning( "Funções nativas de engate não encontradas; a carreta engatada pode ficar freiada" );
        return false;
    }


    // Individually detachable trailers. Two extra guards are needed: a null-joint
    // check the game crashes on while a trailer is detached, and connect_slave,
    // which would otherwise re-attach the trailers behind the one being connected.
    // Returns false when attach/detach is unavailable on this build.
    bool coupling_install()
    {
        const auto crash_address = memory::get_address_for_pattern( "48 85 d2 0f 84 ? ? ? ? 48 89 74 24 18 57 48 83 ec 40" );
        const auto connect_slave_address = memory::get_address_for_pattern( "40 53 48 83 ec 60 48 83 b9 ? ? ? ? 00 48 8b d9 0f 84 ? ? ? ? 48 8d 54 24 ? e8" );

        if ( crash_address == 0 || connect_slave_address == 0 )
        {
            CCore::g_instance->warning( "Engate/desengate de carretas indisponível nesta versão do jogo" );
            return false;
        }

        // get_slave_hook_position is the tail call inside connect_slave
        get_slave_hook_position_fn = reinterpret_cast< prism::physics_trailer_u_get_slave_hook_position_fn* >(
            connect_slave_address + 29 + *reinterpret_cast< int32_t* >( connect_slave_address + 29 ) + 4 );

        resolve_native_fns();
        brake_register_hooks(); // trailer_brake.cpp

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
        CCore::g_instance->warning( "Não foi possível instalar os hooks de engate/desengate" );
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

        // drop orphaned lane-obstacle entries before any scan/erase meets
        // them — dense during the post-detach window (the crash hit 48ms
        // after a joint drop), a slow sweep otherwise
        if ( lane_prune_frames > 0 )
        {
            --lane_prune_frames;
            prune_lane_containers();
        }
        else if ( ++lane_prune_idle >= 300 )
        {
            lane_prune_idle = 0;
            prune_lane_containers();
        }

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
            for ( void* t = head; t != nullptr && is_trailer_obj( t ) &&
                 trailer_count < max_trailers; )
            {
                bool dup = false;
                for ( uint32_t i = 0; i < trailer_count; ++i ) dup |= trailers[ i ] == t;
                if ( dup ) return;
                trailers[ trailer_count++ ] = t;
                track_canon( t );
                void* next = readable_ptr( static_cast< uint8_t* >( t ) + prism::offsets::trailer_slave )
                    ? prism::offsets::field< void* >( t, prism::offsets::trailer_slave ) : nullptr;
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

        // frames with a populated canon — the assembly window detector every
        // risky path gates on
        canon_age = canon[ 0 ] != nullptr ? canon_age + 1 : 0;

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
        // the fatal solver recursion needed a BACKWARD leaf (towed ->
        // canonically-earlier parked) to meet the machine's forward claim
        // and close a two-node loop
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
                    // a canon piece that stays jointless while not live to
                    // the truck IS a parked trailer — mark it so the record
                    // factory, parked pin and anchor lock keep it alive,
                    // registered and attachable. The 90-frame threshold
                    // keeps mid-attach pieces out (they joint within a few
                    // frames); the game-state check keeps world-load
                    // assembly out (a spawning trailer is jointless too)
                    if ( !live_to_truck && joint( t ) == nullptr )
                    {
                        if ( canon_jointless_frames[ i ] < 0xFFFF ) ++canon_jointless_frames[ i ];
                        if ( canon_jointless_frames[ i ] >= 90 &&
                             !is_detached( t ) && !is_plugin_detached( t ) &&
                             canon_age >= 90 && traffic_game_state != nullptr &&
                             *traffic_game_state == 2 )
                        {
                            mark_detached( t );
                            mark_plugin_detached( t );
                            detached_idx[ i ] = true;
                            CCore::g_instance->info( "maint: auto-marked settled parked trailer c{} {:p}", i, t );
                            // a trailer we find jointless was never parked
                            // by a detach flow — it spawned bare: no record,
                            // no legs, no obstacle. Run the real park
                            // sequence once so the game's own machinery
                            // holds it instead of our teleports
                            native_park_detached( t );
                        }
                    }
                    else
                    {
                        canon_jointless_frames[ i ] = 0;
                    }
                    // a trailer that really is coupled to the truck can not
                    // still carry detached marks — a native-path re-attach
                    // never clears them, and stale marks arm the cargo
                    // restore / sever_parked_claims on every actor teleport
                    // (the F9 floor dive: claims cut mid-reconcile made the
                    // game disconnect a member in mid-air)
                    if ( live_to_truck &&
                        ( is_detached( t ) || is_plugin_detached( t ) || detached_idx[ i ] ) )
                    {
                        unmark_detached( t );
                        unmark_plugin_detached( t );
                        detached_idx[ i ] = false;
                    }
                    ++ln;
                }
                // repair live links: a jointed member whose slave edge is
                // missing — severed mid-teleport or never written by a
                // plugin attach — is unreachable through the +0xC8 chain
                // and vanishes even though it sits bolted in the right
                // spot. parent.slave must point at the child physically
                // bolted to it; only null links and stale/parked claims are
                // taken over, never a link feeding another jointed member
                // hands off during an actor-teleport reconcile: joints read
                // null mid-remount and writing links then corrupts the game
                // chain (the F9 dive)
                if ( trailer_maintenance_ready() && tp_cooldown <= 0 )
                {
                    for ( uint32_t i = 0; i < ln; ++i )
                    {
                        if ( parkedish[ i ] ) continue;
                        void* p = prism::offsets::field< void* >( live[ i ], prism::offsets::trailer_parent );
                        if ( p == nullptr || p == truck || !is_trailer_obj( p ) ) continue;
                        void** psl = &prism::offsets::field< void* >( p, prism::offsets::trailer_slave );
                        void* cur = *psl;
                        if ( cur == live[ i ] )
                        {
                            clear_claim_edge( p );
                            continue;
                        }
                        if ( cur != nullptr && is_trailer_obj( cur ) && joint( cur ) != nullptr ) continue;
                        *psl = live[ i ];
                        clear_claim_edge( p );
                    }
                }
            }
            // stale lines on jointless pieces — kill them through the
            // NATIVE line-state setter (0x140874010): fn(vehicle, state,
            // force). state=0 tears down every line entity in the trailer's
            // +0xE98..+0xEA0 list via the real destroy vfunc and clears the
            // +0xE8D "lines connected" flag — the parked piece kept both
            // from its last coupling and no field write kills the recreated
            // hose, only this transition does. Gate the call on actual
            // stale state so it is a no-op once clean — and only touch
            // pieces WE parked: a trailer mid-spawn during world load is
            // jointless with half-built line entities and tearing them
            // down then crashes the build
            for ( uint32_t i = 0; i < ln && tp_cooldown <= 0; ++i )
            {
                if ( !jl[ i ] ) continue;
                // a parked SECTION HEAD still tows a jointed slave — its line
                // list legitimately holds the live edge's hoses (the front
                // vehicle owns them), and zeroing them every frame is what
                // ate the T1->T2 cables
                void* s = readable_ptr( static_cast< uint8_t* >( live[ i ] ) + prism::offsets::trailer_slave )
                    ? prism::offsets::field< void* >( live[ i ], prism::offsets::trailer_slave ) : nullptr;
                const bool tows_section = s != nullptr && s != live[ i ] && is_trailer_obj( s ) &&
                    readable_ptr( static_cast< uint8_t* >( s ) + prism::offsets::trailer_physics_joint ) &&
                    joint( s ) != nullptr;
                if ( tows_section ) continue;
                // the game's line broadcaster re-flags +0xE8D on the whole
                // +0xC8-reachable set every frame while mode>0 — a jointless
                // piece never carries "lines connected", and the slave-edge
                // hose draw reads the SLAVE's flag: pinned off on every
                // jointless canon piece, marked or not — no flag, no hose
                if ( joint( live[ i ] ) == nullptr &&
                    *reinterpret_cast< uint8_t* >( static_cast< char* >( live[ i ] ) + 0xE8D ) != 0 )
                {
                    *reinterpret_cast< uint8_t* >( static_cast< char* >( live[ i ] ) + 0xE8D ) = 0;
                }
                prism::offsets::field< uint32_t >( live[ i ], 0xEE8 ) = 0;
                if ( set_lines_fn == nullptr ) continue;
                if ( !is_detached( live[ i ] ) && !is_plugin_detached( live[ i ] ) &&
                    live[ i ] != attach_target )
                {
                    continue;
                }
                char* const tl = static_cast< char* >( live[ i ] );
                const bool entities = *reinterpret_cast< void** >( tl + 0xE98 ) !=
                    *reinterpret_cast< void** >( tl + 0xEA0 );
                if ( entities ) set_lines_fn( live[ i ], 0, 1 );
            }
            bool any_parked = false;
            bool any_det = false;
            for ( uint32_t i = 0; i < ln; ++i )
            {
                any_parked |= parkedish[ i ];
                any_det |= joint( live[ i ] ) == nullptr;
            }
            static uint32_t hose_dump = 0;
            if ( any_det && ++hose_dump >= 180 )
            {
                hose_dump = 0;
                dump_chain( "hose-state" );
            }
            // the +0xC8/slave pin is what keeps parked trailers VISIBLE —
            // it must run whenever the canon objects are valid, not only in
            // the narrow "ready" window: every frame a parked piece spends
            // unreachable is a frame the sweep can hide it. The real load-
            // time hazard is half-built objects, which is_trailer_obj covers
            if ( any_parked && canon_age >= 15 && canon_all_trailer_objs() &&
                 !native_chain_call_active.load( std::memory_order_acquire ) )
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
                // hook-break teardown: a canon piece whose joint just died
                // must lose every hose connection — the async crank sequence
                // drops the joint seconds after dispatch, so the teardown
                // has to ride the transition itself, not the dispatch tail
                for ( uint32_t i = 0; i < ln; ++i )
                {
                    const bool jointed = joint( live[ i ] ) != nullptr;
                    if ( canon_was_jointed[ i ] && !jointed )
                    {
                        CCore::g_instance->info( "hook break on c{} — hose teardown", i );
                        // the detached piece's lane-obstacle owner node dies
                        // around now — dense-prune the containers for a few
                        // seconds so no worker scan/erase meets the orphan
                        lane_prune_frames = 240;
                        if ( set_lines_fn != nullptr )
                        {
                            set_lines_fn( live[ i ], 0, 1 );
                        }
                        if ( truck != nullptr && pair_vehicles_fn != nullptr )
                        {
                            pair_vehicles_fn( truck, live[ i ] );
                        }
                        if ( truck != nullptr && apply_cables_fn != nullptr )
                        {
                            apply_cables_fn( truck, nullptr, 0, 0, 0 );
                        }
                    }
                    canon_was_jointed[ i ] = jointed;
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
                    if ( towing ) continue;   // parked pieces hang at the tail below
                    void** slave = &prism::offsets::field< void* >( live[ i ], prism::offsets::trailer_slave );
                    void* want = ( i + 1 < ln ) ? live[ i + 1 ] : nullptr;
                    if ( *slave != want ) *slave = want;
                    if ( parkedish[ i ] ) set_claim_edge( live[ i ], want );
                    else clear_claim_edge( live[ i ] );
                }
                if ( towing )
                {
                    // parked pieces hang after the last piece whose joint
                    // chain reaches the truck — a member bolted to a parked
                    // parent is parked too and appending after it writes a
                    // backward claim into its own parent. Written
                    // TAIL-TO-HEAD: a mid-pass write that points an upstream
                    // piece at a downstream one whose stale link still aims
                    // back (canon[1].slave=canon[0] while canon[0].slave is
                    // still canon[1]) forms a 2-cycle for a few cycles —
                    // a concurrent native chain walker spins on it forever
                    // and dies by stack overflow with no crash record
                    void* last_live = nullptr;
                    for ( uint32_t i = 0; i < ln; ++i )
                        if ( !parkedish[ i ] ) last_live = live[ i ];
                    void* next = nullptr;
                    for ( uint32_t i = ln; i-- > 0; )
                    {
                        if ( !parkedish[ i ] ) continue;
                        void** sl = &prism::offsets::field< void* >( live[ i ], prism::offsets::trailer_slave );
                        if ( *sl != next ) *sl = next;
                        set_claim_edge( live[ i ], next );
                        next = live[ i ];
                    }
                    void* anchor = last_live != nullptr ? last_live : towed;
                    // last_live is the deepest live piece in canon order,
                    // not necessarily the physical tail — a repaired or
                    // native link can leave a jointed member behind it.
                    // Walk jointed slaves to the real tail so the parked
                    // append never overwrites a live link
                    uint32_t ad = 0;
                    for ( void* s = readable_ptr( anchor )
                              ? prism::offsets::field< void* >( anchor, prism::offsets::trailer_slave )
                              : nullptr;
                          s != nullptr && s != anchor && is_trailer_obj( s ) &&
                          joint( s ) != nullptr && ++ad <= max_trailers;
                          s = prism::offsets::field< void* >( anchor, prism::offsets::trailer_slave ) )
                    {
                        anchor = s;
                    }
                    void** sl = &prism::offsets::field< void* >( anchor, prism::offsets::trailer_slave );
                    if ( *sl != next ) *sl = next;
                    set_claim_edge( anchor, next );
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
                                clear_claim_edge( prev );
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
                // never-hide pass: the set must stay whole — walk +0xC8 ->
                // slave and append every canon piece the chain does not
                // reach. Any trailer outside the list is what the sweep
                // hides; reachability through the links is the show flag
                {
                    void* head = nullptr;
                    if ( auto* game_actor = CCore::g_instance->get_game_actor() )
                        head = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_trailer );
                    void* vis[ max_trailers ] = {}; uint32_t vn = 0;
                    void* tail = nullptr;
                    for ( void* t = head; t != nullptr && vn < max_trailers; )
                    {
                        bool dup = false;
                        for ( uint32_t k = 0; k < vn; ++k ) dup |= vis[ k ] == t;
                        if ( dup || !is_trailer_obj( t ) ) break;
                        vis[ vn++ ] = t;
                        tail = t;
                        t = prism::offsets::field< void* >( t, prism::offsets::trailer_slave );
                    }
                    if ( tail == nullptr )
                    {
                        // chain head is missing or not a trailer — root it on
                        // the first canon piece so the set stays enumerable
                        for ( uint32_t i = 0; i < ln && tail == nullptr; ++i )
                            if ( is_trailer_obj( live[ i ] ) ) tail = live[ i ];
                        if ( tail != nullptr )
                            vis[ vn++ ] = tail;
                    }
                    for ( uint32_t i = 0; i < ln && tail != nullptr; ++i )
                    {
                        bool seen = false;
                        for ( uint32_t k = 0; k < vn; ++k ) seen |= vis[ k ] == live[ i ];
                        if ( seen || !is_trailer_obj( live[ i ] ) ) continue;
                        CCore::g_instance->info( "visibility pin: re-chained unreachable trailer {:p}", live[ i ] );
                        prism::offsets::field< void* >( tail, prism::offsets::trailer_slave ) = live[ i ];
                        set_claim_edge( tail, live[ i ] );
                        tail = live[ i ];
                    }
                }
                // keep parked pieces in their true detached state and the
                // parked registration asserted — the parked machine tears
                // it down whenever a section member departs
                for ( uint32_t i = 0; i < ln; ++i )
                {
                    if ( !parkedish[ i ] ) continue;
                    void* t = live[ i ];
                    // only re-assert parked state on pieces WE detached —
                    // the game owns the flags on anything else, and arming
                    // +0x1258 on a trailer whose record holds a stale or
                    // staging pose drags it through the world (the parked
                    // machine re-asserts the stored pose every frame)
                    if ( !is_detached( t ) && !is_plugin_detached( t ) ) continue;
                    if ( prism::offsets::field< void* >( t, prism::offsets::trailer_parked_state ) != nullptr )
                    {
                        prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked ) = 1;
                        prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked_registered ) = 1;
                    }
                    else if ( ( is_detached( t ) || is_plugin_detached( t ) ||
                                t == attach_target ) &&
                              joint( t ) == nullptr &&
                              plausible_vfunc( t, 0x328 / 8 ) )
                    {
                        // the native brace call lowers the legs and asks the
                        // parked machine to register the piece — but running
                        // it EVERY frame re-arms the gear animation and
                        // re-notifies the machine each pass (the visible
                        // tremble). Once every ~2 s keeps the request alive
                        // without churning the trailer's physical state
                        static uint32_t brace_retry[ max_trailers ] = {};
                        const int ci = canon_index_of( t );
                        if ( ci >= 0 && brace_retry[ ci ]++ > 120 )
                        {
                            brace_retry[ ci ] = 0;
                            vcall< prism::physics_trailer_u_set_trailer_brace_fn >( t, 0x328 / 8, true );
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
    // by its vehicle_behind_rotation_camera vtable (exe+0x23126F0)

    // per-frame: while a trailer is detached the chase camera anchors on the
    // HEAD OF THE LIVE CHAIN — the trailer physically on the fifth wheel
    // (truck+0x15A8). Detaching a TAIL trailer does not change the head:
    // trailer 0 stays the anchor and the camera keeps measuring the live
    // combination through hk_combo_length (which stops at the first dead
    // joint). Only when the truck tows NOTHING does the anchor go null —
    // the native solo state that follows the truck. The store is a raw
    // write with NO refcounting: the native resolver writes this field
    // directly too, and releasing the previous value is what underflowed
    // refcounts and froze the camera after recoupling

    // trailer hose/cable visuals + the detach/restore traffic suppression.
    // g_trailer_cables_mode: 0 nothing coupled, 1 first trailer (or partial
    // set) coupled, 2 whole original set coupled — written only on change, so
    // the console stays free for the user. A trailer counts only while
    // actually coupled to the truck's convoy — trailer 1 through the fifth
    // wheel, the rest through the chain or straight on the truck.
    void CTrailerManipulation::update_convoy_state()
    {
        // not while the world is still being assembled: trailers exist
        // before their joints/models do, and this function calls native
        // vfuncs (line rebuilds, gear enforcement, parked registration)
        // that crash on half-built objects — the field writes are also
        // pointless before physics starts ticking. canon_age only counts
        // frames with a populated canon, so it restarts whenever the world
        // reload kills the tracked pointers
        if ( !trailer_maintenance_ready() ) return;
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
                    // a physically-jointed trailer with no detach marks is an
                    // attach the game forgot to register — re-pin the towed
                    // slot instead of releasing the joint. Releasing turned
                    // every "stuck" detection into a phantom head detach
                    if ( !is_detached( t ) && !is_plugin_detached( t ) &&
                         set_towed_trailer_fn != nullptr )
                    {
                        CCore::g_instance->info( "stuck tow joint on trailer {} — re-pinning towed slot", i );
                        set_towed_trailer_fn( truck, true, t );
                        // the full registration the normal pin writes — a
                        // half-registered tow leaves the detach dispatcher
                        // refusing the release (sound, no drop)
                        prism::offsets::field< uint32_t >( truck, prism::offsets::truck_coupling_state ) = 5;
                    }
                    else
                    {
                        CCore::g_instance->info( "stuck tow joint on trailer {} — releasing", i );
                        disconnect_now( t, truck, true );
                    }
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
        // frame. The test is live-reachability, NOT "joint == nullptr": a
        // trailer still jointed to a PARKED parent is part of a parked
        // section and must stay braked too. While a piece is settling
        // (anchor_settling — tipping onto its support, rolling into
        // equilibrium) the wheels must NOT be locked: a single-axle dolly
        // whose CG sits behind the axle needs to roll a few cm to find its
        // rest pose — locked wheels leave it teetering on one contact patch
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
        // anchor: the record is what re-asserts the parked pose every frame —
        // wheel brakes alone just slide. The native factory runs once per
        // slot (its mid-chain purecall is swallowed by hk_purecall); pieces
        // it never produces a record for get the emulated lock: snapshot the
        // pose once at joint-drop and teleport back whenever it drifts.
        // Only horizontal drift + rotation are corrected. Suspended right
        // after an actor teleport: the dock re-joint sequence leaves
        // trailers jointless mid-move and the anchor would fight the game's
        // own member placement
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
            // only OUR detached pieces get the emulated lock — an unmarked
            // jointless trailer (respawned by the game, parked by a plain
            // T-detach, still settling on its own) is the game's problem,
            // and pinning a mid-settle pose locks it mid-air
            if ( !readable_ptr( t ) || joint( t ) != nullptr ||
                 ( !is_detached( t ) && !is_plugin_detached( t ) ) )
            {
                anchor_valid[ i ] = false;
                anchor_settling[ i ] = false;
                anchor_delay[ i ] = 0;
                continue;
            }
            // run the native record factory once — its mid-chain blocker was
            // a component purecall and hk_purecall swallows it — then link
            // the registered record back into trailer+0x1188 and raise the
            // parked flags the dispatcher sets on the same condition
            // the factory derefs the parked-record MANAGER singleton at
            // +0x558 before anything else — it only exists after a trailer
            // was parked by the NATIVE flow (fault: mov r8,[rsi+0x558],
            // rsi=null, AV at exe+0x7B6A03). Skip while it is missing and
            // RETRY every frame: once any native park creates it, every
            // limbo trailer gets its record on the next pass — before the
            // parked-machine sweep collects unregistered pieces
            void* mgr = readable_ptr( parked_record_mgr ) ? *parked_record_mgr : nullptr;
            if ( prism::offsets::field< void* >( t, prism::offsets::trailer_parked_state ) == nullptr &&
                !record_tried[ i ] &&
                ( is_detached( t ) || is_plugin_detached( t ) ) &&
                parked_record_factory != nullptr && parked_record_mgr != nullptr &&
                mgr != nullptr )
            {
                record_tried[ i ] = true;
                __try
                {
                    parked_record_factory( t );
                    void** it = mgr != nullptr && readable_ptr(
                        reinterpret_cast< uint8_t* >( mgr ) + 0x748 )
                        ? prism::offsets::field< void** >( mgr, 0x740 ) : nullptr;
                    const uint64_t nrec = mgr != nullptr && readable_ptr(
                        reinterpret_cast< uint8_t* >( mgr ) + 0x748 )
                        ? prism::offsets::field< uint64_t >( mgr, 0x748 ) : 0;
                    void* rec = nullptr;
                    if ( it != nullptr && nrec > 0 && nrec < 0x1000 )
                    {
                        for ( uint64_t k = 0; k < nrec; ++k )
                        {
                            void* e = it[ k ];
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
                __except ( factory_fault_filter( i, GetExceptionInformation() ) )
                {
                }
            }
            // a record pointer alone does not lock the trailer: the native
            // pose re-assertion only runs while the record sits in the
            // manager's scan list AND +0x1258 stays armed — a save-loaded
            // trailer can carry record+flag with no list membership, which
            // leaves the record INERT and the trailer physically falling
            // while the anchor stays wrongly skipped
            if ( prism::offsets::field< void* >( t, prism::offsets::trailer_parked_state ) != nullptr &&
                !record_tried[ i ] &&
                prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked_registered ) != 0 &&
                parked_record_listed( t ) )
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
            // into the void on the next frame
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
                // onto its support — follow the pose instead of locking, or
                // the stored level pose fights the settle every frame
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
            // never lock a fallen pose: if the stored pose is visibly tipped
            // the native park still owes this trailer real support (legs
            // were never deployed) — recapture and keep following instead of
            // freezing it nose-down under the floor
            {
                const quat_t& q = anchor_xform[ i ].quat;
                const float up_y = 1.f - 2.f * ( q.x * q.x + q.z * q.z );
                if ( up_y < 0.85f )
                {
                    anchor_valid[ i ] = false;
                    anchor_settling[ i ] = true;
                    continue;
                }
            }
            if ( mx * mx + my * my + mz * mz > 0.01f * 0.01f || rotd > 0.0004f )
            {
                // locked — emulate the native parked record: re-assert the
                // WHOLE stored pose (position AND rotation, including Y) on
                // any real drift. A position-only pin can never see a rocking
                // trailer: it pivots on the contact patch without moving the
                // origin, and tolerating vertical drift lets the front sink
                // under the threshold forever
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
        // members — not "is the last chain trailer coupled" — keeps the
        // front set's hoses when the tail detaches
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
        // entry the traffic sweep crashes on. What actually re-asserts the
        // parked pose is the per-frame update gated by the +0x1258
        // registration flag — so the parked byte and that flag are dropped,
        // the record is kept. JOINTED trailers only: a jointless piece stays
        // in the chain on purpose, and clearing its parked flags fights the
        // chain maintenance that re-asserts them
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
            if ( !parked_flag && !parked_reg ) continue;
            // the flag bytes go down every frame — two stores that close the
            // gate the pose re-assert reads, even if the parked machine
            // re-arms them between passes. The once-per-joint work used to
            // carry the whole clear and was keyed on the trailers[] position,
            // which ROTATES while towing — a re-jointed trailer could inherit
            // another piece's "done" stamp and keep fighting the parked
            // machine forever (the per-frame pose re-assert against a live
            // joint is the physics war that drags the sim to 3-5 FPS)
            prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked ) = 0;
            prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked_registered ) = 0;
            static void* unparked_t[ max_trailers ] = {};
            static void* unparked_j[ max_trailers ] = {};
            uint32_t slot = max_trailers, free_slot = max_trailers;
            for ( uint32_t s = 0; s < max_trailers; ++s )
            {
                if ( unparked_t[ s ] == t ) { slot = s; break; }
                if ( unparked_t[ s ] == nullptr && free_slot == max_trailers ) free_slot = s;
            }
            if ( slot == max_trailers )
            {
                if ( free_slot == max_trailers ) continue;
                slot = free_slot;
                unparked_t[ slot ] = t;
                unparked_j[ slot ] = nullptr;
            }
            if ( unparked_j[ slot ] == joint( t ) ) continue;
            unparked_j[ slot ] = joint( t );
            prism::offsets::field< float >( t, prism::offsets::trailer_brake ) = 0.f;
            prism::offsets::field< uint8_t >( t, prism::offsets::trailer_brake_forced ) = 0;
            // drop the record out of the parked machine's scan list — while
            // it stays listed the manager re-arms the flags and re-asserts
            // the stored pose against the live joint every frame. The record
            // itself stays linked on the trailer (+0x1188) for the traffic
            // registration, exactly as before
            if ( parked_rec ) parked_record_unlist( t );
            CCore::g_instance->info( "un-parked trailer {} (flag {} record {})", i, parked_flag, parked_rec );
        }

        update_landing_gear();

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
        // variants connect that trailer straight to the fifth wheel. Trailer 0
        // (index 0) is left to the game's own T key
        bool ad2 = false, ad3 = false, tk2 = false, tk3 = false;
        key_down( settings::Action::ATTACH_DETACH_2, &ad2 );
        key_down( settings::Action::ATTACH_DETACH_3, &ad3 );
        key_down( settings::Action::ATTACH_TRUCK_2, &tk2 );
        key_down( settings::Action::ATTACH_TRUCK_3, &tk3 );
        if ( ad2 ) this->toggle_attach( 1 );
        if ( ad3 ) this->toggle_attach( 2 );
        // keys name the physical trailer (canon order), not the rotated
        // trailers[] slot — with trailer 1 on the wheel the list is [1,2,0]
        // and a raw index would select the wrong piece
        if ( tk2 ) this->connect_to_truck( canon[ 1 ] );
        if ( tk3 ) this->connect_to_truck( canon[ 2 ] );
    }

}
