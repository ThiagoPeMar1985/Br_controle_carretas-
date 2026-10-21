#include "trailer_manipulation.hpp"

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
    bool readable_ptr( const void* p )
    {
        if ( p == nullptr ) return false;
        MEMORY_BASIC_INFORMATION mbi{};
        return VirtualQuery( p, &mbi, sizeof( mbi ) ) != 0 && mbi.State == MEM_COMMIT &&
            ( mbi.Protect & ( PAGE_NOACCESS | PAGE_GUARD ) ) == 0;
    }

    bool plausible_object( const void* p )
    {
        if ( !readable_ptr( p ) ) return false;
        const auto base = reinterpret_cast< uint64_t >( GetModuleHandle( nullptr ) );
        const auto vtbl = *reinterpret_cast< const uint64_t* >( p );
        return vtbl >= base && vtbl < base + 0x4000000;
    }

    bool plausible_vfunc( const void* obj, const uint32_t slot )
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

    //========================================================================
    // Game object accessors
    //========================================================================
    void* joint( void* trailer )
    {
        return prism::offsets::field< void* >( trailer, prism::offsets::trailer_physics_joint );
    }

    // non-null only on trailers with steerable wheels (heavy-haul / oversize)
    prism::vehicle_wheel_steering_data_t* steering_data( void* trailer )
    {
        return trailer == nullptr ? nullptr
            : prism::offsets::field< prism::vehicle_wheel_steering_data_t* >(
                trailer, prism::offsets::trailer_steering_data );
    }

    //========================================================================
    // Resolved native functions and globals (filled by init())
    //========================================================================

    // hooks installed by this file
    std::shared_ptr< CFunctionHook > crashes_when_disconnected_hook = nullptr;
    std::shared_ptr< CFunctionHook > connect_slave_hook = nullptr;
    std::shared_ptr< CFunctionHook > brake_chain_setter_hook = nullptr;
    std::shared_ptr< CFunctionHook > brake_setter_hook = nullptr;
    std::shared_ptr< CFunctionHook > svc_brake_chain_hook = nullptr; // unused: +0x730 is a dead demand mirror
    std::shared_ptr< CFunctionHook > svc_brake_hook = nullptr;
    std::shared_ptr< CFunctionHook > forced_chain_setter_hook = nullptr;
    std::shared_ptr< CFunctionHook > forced_setter_hook = nullptr;
    std::shared_ptr< CFunctionHook > wheel_brake_hook = nullptr;
    std::shared_ptr< CFunctionHook > pre_detach_hook = nullptr;
    std::shared_ptr< CFunctionHook > detach_dispatch_hook = nullptr;
    std::shared_ptr< CFunctionHook > attach_dispatch_hook = nullptr;
    std::shared_ptr< CFunctionHook > trailer_state_apply_hook = nullptr;
    std::shared_ptr< CFunctionHook > trailer_teleport_hook = nullptr;
    std::shared_ptr< CFunctionHook > actor_teleport_hook = nullptr;
    std::shared_ptr< CFunctionHook > lane_obstacle_insert_hook = nullptr;
    std::shared_ptr< CFunctionHook > combo_length_hook = nullptr;
    std::shared_ptr< CFunctionHook > cam_set_target_hook = nullptr;
    std::shared_ptr< CFunctionHook > set_individual_steering_hook = nullptr;

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
    void* chase_camera = nullptr;
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

    //========================================================================
    // Runtime state
    //========================================================================

    // the current trailer chain, refreshed by update() every frame on the
    // render thread
    void* trailers[ max_trailers ] = {};
    uint32_t trailer_count = 0;

    // trailers intentionally detached through this plugin. The game's own
    // attach/detach flows call connect_slave on the parent, which would
    // re-attach them right back — so the hook stays active and skips only
    // those trailers
    void* detached_trailers[ max_trailers ] = {};

    bool is_detached( const void* trailer )
    {
        for ( const void* d : detached_trailers ) if ( d == trailer ) return true;
        return false;
    }

    void mark_detached( void* trailer )
    {
        for ( void*& d : detached_trailers ) if ( d == nullptr ) { d = trailer; return; }
    }

    void unmark_detached( const void* trailer )
    {
        for ( void*& d : detached_trailers ) if ( d == trailer ) d = nullptr;
    }

    // trailers detached through the PLUGIN's own controls — a stricter set
    // than detached_trailers (which also holds natively detached heads so
    // connect_slave won't slam them back). These must survive cargo
    // load/unload untouched: no teleporting to the dock, no re-joining —
    // wherever the player left them is where they stay
    void* plugin_detached[ max_trailers ] = {};

    bool is_plugin_detached( const void* trailer )
    {
        for ( const void* d : plugin_detached ) if ( d == trailer ) return true;
        return false;
    }

    void mark_plugin_detached( void* trailer )
    {
        for ( void*& d : plugin_detached ) if ( d == nullptr ) { d = trailer; return; }
    }

    void unmark_plugin_detached( const void* trailer )
    {
        for ( void*& d : plugin_detached ) if ( d == trailer ) d = nullptr;
    }

    // any trailer object this plugin is tracking — needed before touching
    // trailer-specific fields like the physics joint on an unknown pointer
    bool is_tracked_trailer( const void* p )
    {
        for ( uint32_t i = 0; i < trailer_count; ++i )
            if ( trailers[ i ] == p ) return true;
        return is_detached( p ) || is_plugin_detached( p );
    }

    // trailer currently jointed straight to the truck through "connect to
    // truck"; used to refuse a second trailer on the fifth wheel
    void* truck_towed = nullptr;

    // g_traffic value saved before a forced "g_traffic 0" detach; restored once
    // the whole trailer set is jointed again. < 0 means nothing saved
    float saved_traffic = -1.f;
    // trailers the set had when traffic suppression started. The chain shrinks
    // when links leave it, so "all coupled" must compare against this — a
    // partially rebuilt set (e.g. just the dolly on the truck) must not count
    uint32_t expected_trailers = 0;
    // set only after disconnect_now actually dropped a joint. The detach waits
    // ~30 sim ticks after g_traffic 0, and during that window every trailer
    // still reads as coupled — without this flag the restore fires mid-delay
    // and the joint lands with live traffic again
    std::atomic< bool > traffic_detach_done{ false };

    //========================================================================
    // Liftable trailer axles (ported from ts-liftable-axles)
    //
    // The native trailer lift key raises every axle whose flag byte is set —
    // the controller mask at +0x38 gates which of those actually animate.
    // Rewriting both every frame lets the UI pick which natively liftable
    // axles respond, per trailer. No plugin key is needed: the game's own
    // lift binding stays the trigger.
    //
    // Safety rules (learned the hard way in ts-liftable-axles):
    //  - only natively liftable axles may be selected: forcing a flag on an
    //    axle the chassis never marked crashes the wheel/suspension update
    //  - nothing is written while the controller state is not "down" and a
    //    real mask is applied — flipping flags mid-air leaves a mixed
    //    progress state the game cannot resolve
    //  - a nonzero state with an empty applied mask is a desync (mask 0 can
    //    never animate): write state 0 to recover
    //========================================================================
    constexpr uint32_t max_axles = 16; // sane upper bound for the flag count sanity check

    // Per trailer chain position: the lift controller and axle flag array seen
    // last, plus the values the game gave them.
    struct LiftAxles
    {
        void* controller = nullptr; // trailer+0x6C8, detects a trailer swap/rebuild on change
        void* axle_data = nullptr; // trailer+0x200
        uint8_t* flags = nullptr; // per-axle liftable flags inside axle_data
        uint8_t orig_flags[ max_axles ]{}; // the flags the game set, restored on unload
        uint32_t orig_mask = 0; // the mask the game built, restored on unload
        uint32_t applied_mask = 0; // last mask we wrote; a different value means the game re-initialized
        uint32_t native_axles = 0; // orig_flags folded to axle bits, the default selection
        uint32_t axle_count = 0;
    };

    LiftAxles lift_axles[ max_trailers ];

    // Reverse-gear lift assist: with all axles on the ground, engaging reverse
    // keeps only the first liftable axle selected and raises it; leaving
    // reverse lowers it and restores the saved selection. Fed by telemetry
    // channels, read on the render thread.
    std::atomic< bool > reverse_light{ false };
    std::atomic< int32_t > engine_gear{ 0 };

    // While true the effective selection is the first liftable axle only; the
    // saved axle_masks are never touched.
    bool reverse_override = false;
    // True when the plugin commanded the raise itself — only then is lowering
    // on reverse exit ours to undo.
    bool reverse_auto_lift = false;
    bool was_reversing = false;

    SCSAPI_VOID on_light_reverse( const scs_string_t, const scs_u32_t, const scs_value_t* const value, const scs_context_t )
    {
        reverse_light.store( value != nullptr && value->value_bool.value != 0, std::memory_order_relaxed );
    }

    SCSAPI_VOID on_engine_gear( const scs_string_t, const scs_u32_t, const scs_value_t* const value, const scs_context_t )
    {
        engine_gear.store( value != nullptr ? value->value_s32.value : 0, std::memory_order_relaxed );
    }

    //========================================================================
    // Air compressor diagnostics (temporary — collecting the native refill
    // rate and the truck-object field holding the tank pressure)
    //========================================================================
    std::atomic< float > air_psi{ 0.f };
    std::atomic< float > engine_rpm{ 0.f };

    SCSAPI_VOID on_air_pressure( const scs_string_t, const scs_u32_t, const scs_value_t* const value, const scs_context_t )
    {
        air_psi.store( value != nullptr ? value->value_float.value : 0.f, std::memory_order_relaxed );
    }

    SCSAPI_VOID on_engine_rpm( const scs_string_t, const scs_u32_t, const scs_value_t* const value, const scs_context_t )
    {
        engine_rpm.store( value != nullptr ? value->value_float.value : 0.f, std::memory_order_relaxed );
    }

    //========================================================================
    // Air compressor boost ("Compressor" slider)
    //
    // Reverse-engineered from the telemetry channel registration (1.61):
    // the truck.brake.air.pressure getter callback at 0x14064B850 reads
    //   psi = [[truck+0x200] + 0x458] * [truck+0x1148] * 14.5
    // +0x458 on the air sub-object is the tank MAX pressure (~10 bar — a
    // constant, do NOT write); the live fill is the fraction at
    // truck+0x1148 (0..~0.93 cutout). The boost adds
    // `native_rate * air_boost` psi/s to that fraction.
    // (The +0xA8/+0xD0 float arrays with cached means belong to a different
    // subsystem — per-axle brake air, always 0 when unbraked — and the
    // singleton chain [[[exe+0x3046780]+0x18]+0xA0]+0x10 resolves a dead
    // template instance; neither is the player telemetry source.)
    //========================================================================
    constexpr uint32_t air_sub_ptr    = 0x200;  // truck -> air/brake object
    constexpr uint32_t air_press_off  = 0x458;  // tank MAX pressure (~10 bar)
    constexpr uint32_t air_scale_off  = 0x1148; // fill fraction 0..~0.93
    constexpr float    air_psi_const  = 14.5f;  // getter's bar->psi factor

    float air_win_t = 0.f;                 // rate-window accumulator (s)
    float air_win_last_psi = -1.f;         // last psi seen in the window
    float air_win_dpsi = 0.f;              // psi gained inside the window
    float air_win_ours = 0.f;              // psi we wrote inside the window
    float air_meas_rate = 0.f;             // achieved total fill rate psi/s
    float air_inject = 0.f;                // integral term: psi/s we inject
    float air_rate_est = 0.f;              // EMA of the native refill slope
    float air_units = 0.f;                 // psi per internal unit
    bool air_tanks_seen = false;
    bool air_diag_dumped = false;          // one-shot layout dump on failure
    float air_field_base = 0.f;            // locked tank max pressure (bar)
    void* air_obj = nullptr;               // resolved air/brake sub-object
    void* air_truck = nullptr;             // truck physics object (scale owner)
    float air_recheck_t = 0.f;             // 0.5 s verify throttle

    // each wheelset's native brake mask, captured before our first rewrite —
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
    // Wheels map to axles evenly — wheel i belongs to axle i*axles/n
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


    // guarded reads/writes — the arrays live on the truck object; kept apart
    // because __try cannot live in a function with C++ objects that need
    // unwinding
    bool air_safe_read_f( const void* addr, float& out )
    {
        __try
        {
            memcpy( &out, addr, 4 );
            return true;
        }
        __except ( EXCEPTION_EXECUTE_HANDLER ) {}
        return false;
    }

    bool air_safe_read_q( const void* addr, uint64_t& out )
    {
        __try
        {
            memcpy( &out, addr, 8 );
            return true;
        }
        __except ( EXCEPTION_EXECUTE_HANDLER ) {}
        return false;
    }

    bool air_safe_write_f( void* addr, const float v )
    {
        __try
        {
            *static_cast< float* >( addr ) = v;
            return true;
        }
        __except ( EXCEPTION_EXECUTE_HANDLER ) {}
        return false;
    }

    // the live pressure field is on the truck's air/brake sub-object:
    // game_actor -> +0x18 (truck physics) -> +0x200 (air object) -> +0x458,
    // exactly what the telemetry getter 0x14064B850 reads. The scale factor
    // the getter applies sits on the truck object itself at +0x1148.
    void* air_object()
    {
        auto* game_actor = CCore::g_instance->get_game_actor();
        if ( game_actor == nullptr ) return nullptr;
        void* const truck = prism::offsets::field< void* >( game_actor,
            prism::offsets::game_actor_truck );
        if ( truck == nullptr ) return nullptr;
        air_truck = truck;
        uint64_t sub = 0;
        if ( !air_safe_read_q( static_cast< uint8_t* >( truck ) + air_sub_ptr, sub ) || sub == 0 )
        {
            return nullptr;
        }
        return reinterpret_cast< void* >( sub );
    }

    void update_air( const float dt )
    {
        const float psi = air_psi.load();
        const float rpm = engine_rpm.load();
        const int boost = settings::g_settings.air_boost;

        // achieved fill rate over a 0.25 s window — the telemetry channel
        // is quantized so per-frame dpsi/dt is meaningless; native rate =
        // measured total minus what we injected ourselves
        if ( dt > 0.f )
        {
            air_win_t += dt;
            if ( air_win_last_psi >= 0.f )
            {
                air_win_dpsi += psi - air_win_last_psi;
            }
            air_win_last_psi = psi;
            if ( air_win_t >= 0.25f )
            {
                air_meas_rate = air_win_dpsi / air_win_t;
                const float native_obs = air_meas_rate - air_win_ours / air_win_t;
                if ( native_obs > 0.f && native_obs < 50.f )
                {
                    air_rate_est = air_rate_est == 0.f ? native_obs
                        : air_rate_est * 0.8f + native_obs * 0.2f;
                }
                // integral: nudge the injected rate toward the target — a
                // pure proportional push converges to only half the goal.
                // Same gates as production or the term winds up at the cap
                // while the tank is full and bursts on the next drain. The
                // frac check also covers the compressor cutout — past
                // ~0.93 the displayed psi saturates so the controller would
                // keep pumping a fraction the sim no longer uses
                float frac_now = 0.f;
                if ( boost > 0 && air_tanks_seen && rpm > 0.f && psi < 133.f &&
                     air_truck != nullptr &&
                     air_safe_read_f( static_cast< uint8_t* >( air_truck ) + air_scale_off, frac_now ) &&
                     frac_now < 0.95f )
                {
                    const float native = air_rate_est > 0.f ? air_rate_est
                        : 0.0003f * rpm;
                    air_inject += ( native * static_cast< float >( boost + 1 )
                        - air_meas_rate ) * 0.7f;
                    if ( air_inject < 0.f ) air_inject = 0.f;
                    if ( air_inject > 30.f ) air_inject = 30.f;
                }
                else
                {
                    air_inject = 0.f;
                }
                air_win_t = 0.f;
                air_win_dpsi = 0.f;
                air_win_ours = 0.f;
            }
        }

        // resolve + verify at ~2 Hz: the air object hangs right off the
        // truck; the sanity check confirms the getter contract still holds
        air_recheck_t -= dt;
        if ( air_recheck_t <= 0.f && psi > 5.f )
        {
            air_recheck_t = 0.5f;
            air_obj = air_object();
            if ( air_obj != nullptr && air_truck != nullptr )
            {
                float field = 0.f, frac = 0.f;
                if ( air_safe_read_f( static_cast< uint8_t* >( air_obj ) + air_press_off, field ) &&
                     air_safe_read_f( static_cast< uint8_t* >( air_truck ) + air_scale_off, frac ) &&
                     field > 0.f && frac > 0.f )
                {
                    // getter contract: psi = field(max bar) * frac * 14.5
                    const float predicted = field * frac * air_psi_const;
                    if ( predicted > psi * 0.7f && predicted < psi * 1.3f )
                    {
                        if ( !air_tanks_seen )
                        {
                            air_tanks_seen = true;
                            air_field_base = field;
                            air_units = field * air_psi_const; // psi per fraction
                            CCore::g_instance->info( "air: pressure locked max={:.3f} frac={:.4f} -> {:.1f} vs psi {:.1f}",
                                field, frac, predicted, psi );
                        }
                        // heal +0x458 — earlier builds wrote into it; it is
                        // the tank's MAX pressure, not the live value
                        if ( field < air_field_base * 0.9f || field > air_field_base * 1.1f )
                        {
                            air_safe_write_f( static_cast< uint8_t* >( air_obj ) + air_press_off,
                                air_field_base );
                        }
                    }
                    else if ( !air_diag_dumped )
                    {
                        air_diag_dumped = true;
                        CCore::g_instance->warning(
                            "Compressor indisponível: layout de ar divergiu ({:.3f} x {:.3f} -> {:.1f} vs {:.1f} psi)",
                            field, frac, predicted, psi );
                    }
                }
                else if ( !air_diag_dumped )
                {
                    air_diag_dumped = true;
                    CCore::g_instance->warning( "Compressor indisponível: campo de ar ilegível" );
                }
            }
        }

        // production: inject air_inject psi/s into the fill fraction — the
        // integral term has already sized it so the achieved rate lands on
        // native*(boost+1)
        if ( boost > 0 && air_tanks_seen && air_truck != nullptr && rpm > 0.f &&
             psi < 133.f && dt > 0.f && air_units > 0.f && air_inject > 0.f )
        {
            float frac = 0.f;
            if ( air_safe_read_f( static_cast< uint8_t* >( air_truck ) + air_scale_off, frac ) &&
                 frac < 0.95f )
            {
                const float add_psi = air_inject * dt;
                // hard cap at the physical fill range — the sim saturates
                // the effective pressure at the cutout, anything above just
                // accumulates an invisible overshoot in the fraction
                air_safe_write_f( static_cast< uint8_t* >( air_truck ) + air_scale_off,
                    ( std::min )( frac + add_psi / air_units, 0.95f ) );
                air_win_ours += add_psi;
            }
        }
    }

    void release_all_axles()
    {
        for ( auto& lift : lift_axles )
        {
            // give the game its own mask and flag bytes back; the objects may
            // already be gone if the plugin is unloaded after trailers were,
            // so guard the writes
            if ( lift.controller != nullptr || lift.flags != nullptr )
            {
                __try
                {
                    if ( lift.controller != nullptr )
                    {
                        prism::offsets::field< uint32_t >( lift.controller, prism::offsets::lift_controller_axle_mask ) = lift.orig_mask;
                    }
                    if ( lift.flags != nullptr )
                    {
                        for ( uint32_t a = 0; a < lift.axle_count && a < max_axles; ++a )
                        {
                            lift.flags[ a ] = lift.orig_flags[ a ];
                        }
                    }
                }
                __except ( EXCEPTION_EXECUTE_HANDLER )
                {
                }
                lift = LiftAxles{};
            }
        }
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

    pending_op pending;

    char pending_cmd[ 64 ] = {};
    std::atomic< bool > pending_cmd_queued{ false };

    void queue_cmd( const char* text )
    {
        std::strncpy( pending_cmd, text, sizeof( pending_cmd ) - 1 );
        pending_cmd[ sizeof( pending_cmd ) - 1 ] = '\0';
        pending_cmd_queued.store( true, std::memory_order_release );
    }

    bool queue_op( const pending_kind kind, void* trailer, void* parent,
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
    uint64_t sim_heartbeat = 0;
    uint64_t sim_heartbeat_seen = 0;
    uint32_t sim_dead_frames = 0;
    uint64_t sim_thread_id = 0;

    void pump_sim_heartbeat()
    {
        sim_thread_id = GetCurrentThreadId();
        ++sim_heartbeat;
    }

    void watch_sim_heartbeat()
    {
        const uint64_t hb = sim_heartbeat;
        sim_dead_frames = ( hb == sim_heartbeat_seen ) ? sim_dead_frames + 1 : 0;
        sim_heartbeat_seen = hb;
    }

    // true when the sim side is demonstrably not running — either update()
    // itself is the thread that calls the trailer tick (single-threaded
    // engine), or it has been silent for a sustained stretch of frames
    bool sim_is_quiet()
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
    bool hook_world( void* vehicle, hook_xform_t& out )
    {
        alignas( 16 ) uint8_t buf[ 0x20 ]{};
        void* p = vcall< prism::vehicle_get_transform_fn >( vehicle, 0xF8 / 8, buf );
        if ( p == nullptr ) return false;
        std::memcpy( &out, p, sizeof( out ) );
        return std::isfinite( out.pos.x ) && std::isfinite( out.pos.y ) && std::isfinite( out.pos.z ) &&
            std::fabs( out.pos.x ) < 1e6f && std::fabs( out.pos.z ) < 1e6f;
    }

    // the truck's transform, the way the native coupling paths read it
    bool truck_xform( void* game_actor, uint8_t* out32 )
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
    bool detached_idx[ max_trailers ] = {};
    hook_xform_t parked_xform[ max_trailers ] = {};
    bool parked_xform_valid[ max_trailers ] = {};
    // frames the marked slot has spent jointed — the restore only fires after
    // the cargo cinematic's own writes settle (~half a second)
    uint32_t rejoined_frames[ max_trailers ] = {};

    int index_of( const void* trailer )
    {
        for ( uint32_t i = 0; i < trailer_count; ++i )
            if ( trailers[ i ] == trailer ) return static_cast< int >( i );
        return -1;
    }

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

    // plays the trailer's registered brace sound event directly. The audio
    // object sits at trailer+0x12D8 with channel slots +0x50 (brace_up) and
    // +0x58 (brace_down); each slot points at a 16-byte channel whose +8 is
    // the pooled sound-event object (vtable base+0x2220EE60, vfunc+0x18 =
    // "start" — it early-returns unless the event is in ready state 2, so a
    // wrong guess fails silent, not fatal)
    using sound_event_play_fn = void __fastcall( void* /*event*/, uint8_t /*flag*/ );
    void play_brace_sound( void* trailer, const bool up )
    {
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

    void disconnect_now( void* trailer, void* parent, const bool parent_is_truck )
    {
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
        // coupling range in the native flow
        if ( clear_detached_flag_fn != nullptr )
        {
            if ( parent != nullptr ) clear_detached_flag_fn( parent, 0, 1 );
            clear_detached_flag_fn( trailer, 0, 1 );
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
        // object — the index is what lets the restore spot the fresh jointed
        // replacement and put it back
        const int di = index_of( trailer );
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
        // the game applies spring brakes the same way on a jointless trailer
        prism::offsets::field< float >( trailer, prism::offsets::trailer_brake ) = 1.f;
        prism::offsets::field< float >( trailer, prism::offsets::trailer_service_brake ) = 0.f;
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

        // NOTE: driving the native attach dispatcher (0x1405DCC20, the T-key
        // path) was tried and abandoned: it does give the trailer native
        // brakes/lights, but it also registers the trailer as the convoy's
        // fifth-wheel candidate in ways that corrupt the world state —
        // repeated runs produced scene-origin rebase storms, despawned
        // trailers and crashes. The manual replica below is the stable path.

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
            const int ti = index_of( t );
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
            if ( rig_truck_valid && actor_teleport_hook != nullptr )
            {
                if ( auto* game_actor = CCore::g_instance->get_game_actor() )
                {
                    using fn_t = void __fastcall( void*, void*, uint8_t, uint8_t );
                    actor_teleport_hook->get_original< fn_t >()(
                        game_actor, &rig_truck_xform, rig_replay_a3, rig_replay_a4 );
                }
            }
        }

        for ( int i = static_cast< int >( trailer_count ) - 1; i >= 0; --i )
        {
            if ( !detached_idx[ i ] ) continue;
            // a marked slot whose previous slot is also marked belongs to the
            // same parked section — the section head's walk below covers it;
            // disconnecting it here would split the parked combination apart
            if ( i > 0 && detached_idx[ i - 1 ] && trailers[ i - 1 ] != nullptr ) continue;
            void* t = trailers[ i ];
            if ( t == nullptr || joint( t ) == nullptr ) continue;

            // the parent it was respawned under — chain order is preserved
            void* parent = nullptr;
            bool parent_is_truck = false;
            if ( auto* game_actor = CCore::g_instance->get_game_actor() )
            {
                parent = ( i == 0 || t == truck_towed )
                    ? prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck )
                    : ( i - 1 < static_cast< int >( trailer_count ) ? trailers[ i - 1 ] : nullptr );
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
                const int mi = index_of( m );
                if ( mi >= 0 && parked_xform_valid[ mi ] && trailer_teleport_hook != nullptr )
                {
                    using teleport_fn_t = void __fastcall( void* /*trailer*/, void* /*transform*/ );
                    trailer_teleport_hook->get_original< teleport_fn_t >()( m, &parked_xform[ mi ] );
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
        for ( void* t = prism::offsets::field< void* >( self, prism::offsets::trailer_slave );
              t != nullptr;
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
        if ( slave != nullptr && is_detached( slave ) )
        {
            return;
        }
        connect_slave_hook->get_original< prism::physics_trailer_u_connect_slave_fn >()( self );
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
    void __fastcall hk_detach_dispatch( void* self, uint8_t dl, uint64_t r8 )
    {
        detach_dispatch_hook->get_original< native_detach_dispatch_fn >()( self, dl, r8 );
        if ( self == nullptr ) return;
        void* t = prism::offsets::field< void* >( self, prism::offsets::game_actor_trailer );
        if ( t != nullptr && joint( t ) == nullptr )
        {
            mark_detached( t );
        }
    }

    // the native attach dispatcher (0x1405DCC20) — every native re-attach
    // funnels through it: the T key, the cargo load/unload set reassembly,
    // garage transfers, save-load restores. A trailer the plugin detached
    // is refused: wherever the player left it, it stays — the load/unload
    // sequence may not pull it back onto the fifth wheel or the chain.
    // Natively detached trailers are unaffected: T re-attach still works
    void __fastcall hk_attach_dispatch( void* self, uint8_t dl, uint64_t r8, void* settle_override )
    {
        void* head = self != nullptr
            ? prism::offsets::field< void* >( self, prism::offsets::game_actor_trailer ) : nullptr;
        if ( head != nullptr && is_plugin_detached( head ) )
        {
            CCore::g_instance->info( "attach dispatcher skipped: trailer {:p} is plugin-detached", head );
            return;
        }
        attach_dispatch_hook->get_original< native_attach_dispatch_fn >()( self, dl, r8, settle_override );
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
    using trailer_teleport_fn = void __fastcall( void* /*trailer*/, void* /*transform*/ );
    void __fastcall hk_trailer_teleport( void* self, void* transform )
    {
        if ( self != nullptr && is_plugin_detached( self ) )
        {
            CCore::g_instance->info( "trailer teleport skipped: {:p} is plugin-detached", self );
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
        if ( !settings::g_settings.cargo_restore )
        {
            rig_armed = false;
            actor_teleport_hook->get_original< actor_teleport_fn >()( self, arg, a3, a4 );
            return;
        }
        const bool fresh = !rig_armed;
        if ( fresh )
        {
            snapshot_rig();
        }
        actor_teleport_hook->get_original< actor_teleport_fn >()( self, arg, a3, a4 );
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
                if ( dx * dx + dz * dz < 200.f * 200.f )
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

    //------------------------------------------------------------------------
    // Brake setter hooks
    //
    // A trailer that leaves a parked set stays linked under it, so the set's
    // per-frame spring-brake push keeps reaching it through the stale slave
    // link and re-locks the wheels — no matter what the truck broadcasts.
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
    // is the truck's own broadcast. Anything else — a stale parked set pushing
    // through the shared trailer_slave link, or the per-frame spring-brake
    // loop while the coupling state is mid-change — is swallowed: a trailer
    // with a live joint takes its brakes from the truck, period.
    bool foreign_brake_write( void* self, const void* ret )
    {
        const auto ra = reinterpret_cast< uint64_t >( ret );
        if ( ra == truck_brake_broadcast_ret || ra == truck_forced_broadcast_ret )
        {
            return false;
        }
        // only jointed trailers are candidates — a free (jointless) trailer must
        // keep receiving its spring brake or it rolls away
        if ( self == nullptr || joint( self ) == nullptr ) return false;
        // trailer_physics_joint is a trailer field — on the truck object that
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
    // folding into a jackknife). The tractor's own channel is never touched —
    // the truck check must run BEFORE joint(): +0xFE0 is a trailer field and
    // holds unrelated live data on the truck object.
    // The wheel solver (0x14090C570) applies the factor as
    // max(trailer_brake, per-wheel floor) * grip/load with NO upper clamp,
    // so the amplified value is allowed to exceed 1.0 — that is what makes
    // loaded axles actually lock once their wheels unload under braking.
    float biased_brake( void* self, const float v )
    {
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
    // is touched — after the original runs we zero the outputs of every wheel
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
    // but that field is a demand mirror — writing 10x to it produced zero
    // physical effect. The wheel solver only turns +0x760 into torque, so the
    // bias is applied there per-frame in update() instead
    //========================================================================
    // Input helpers
    //========================================================================

    //========================================================================
    // Steering lock state (oversize / steerable-axle trailers)
    //
    // Shared between the render thread (UI) and the game thread (the hook).
    // The hook only compares the pointer, never dereferences it, so a stale
    // entry is harmless.
    //========================================================================
    struct LockedSteering
    {
        std::atomic< prism::vehicle_wheel_steering_data_t* > data{ nullptr };
        std::atomic< float > angle{ 0.f };
    };

    LockedSteering locked_steering[ max_trailers ];

    // UI-only state, render thread
    bool locked_ui[ max_trailers ] = {};
    float angle_ui[ max_trailers ] = {};
    bool centering_ui[ max_trailers ] = {}; // easing back to 0 at steering_speed

    void set_locked( const uint32_t i, const bool lock )
    {
        if ( locked_ui[ i ] == lock ) return;
        locked_ui[ i ] = lock;
        if ( lock )
        {
            // start from where the game had the wheels so they do not jump
            angle_ui[ i ] = prism::offsets::field< float >( trailers[ i ], prism::offsets::trailer_steering );
            locked_steering[ i ].angle.store( angle_ui[ i ] );
        }
    }

    // set_individual_steering hook: the game calls this every steering advance
    // with the angle it computed from the joint. For a trailer the user took
    // control of, swap in their angle.
    void hk_set_individual_steering( prism::vehicle_wheel_steering_data_t* data, float angle )
    {
        for ( auto& locked : locked_steering )
        {
            if ( data != nullptr && locked.data.load( std::memory_order_relaxed ) == data )
            {
                angle = locked.angle.load( std::memory_order_relaxed );
                break;
            }
        }
        set_individual_steering_hook->get_original< prism::set_individual_steering_fn >()( data, angle );
    }

    // key state for edge detection, shared with key_down below
    bool held[ settings::action_count ] = {};

    bool game_has_focus()
    {
        DWORD pid = 0;
        GetWindowThreadProcessId( GetForegroundWindow(), &pid );
        return pid == GetCurrentProcessId();
    }

    // true while the bound key is down; first_press only on the frame it went down
    bool key_down( const settings::Action action, bool* first_press = nullptr )
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
    // CTrailerManipulation — lifecycle
    //========================================================================
    CTrailerManipulation::CTrailerManipulation() = default;

    CTrailerManipulation::~CTrailerManipulation()
    {
        for ( uint32_t i = 0; i < max_trailers; ++i )
        {
            locked_steering[ i ].data.store( nullptr );
            locked_ui[ i ] = false;
        }
        set_individual_steering_hook.reset();
        release_all_axles();
        crashes_when_disconnected_hook.reset();
        connect_slave_hook.reset();
        pre_detach_hook.reset();
        detach_dispatch_hook.reset();
        attach_dispatch_hook.reset();
        trailer_state_apply_hook.reset();
        trailer_teleport_hook.reset();
        actor_teleport_hook.reset();
        lane_obstacle_insert_hook.reset();
        combo_length_hook.reset();
        cam_set_target_hook.reset();
        svc_brake_chain_hook.reset();
        svc_brake_hook.reset();
        brake_chain_setter_hook.reset();
        brake_setter_hook.reset();
        forced_chain_setter_hook.reset();
        forced_setter_hook.reset();
        wheel_brake_hook.reset();
        chase_camera = nullptr;
    }

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
        anchor_reg_guard = reinterpret_cast< uint32_t* >( memory::get_address_from_offset( 0x143AF1824 ) );
        traffic_game_state = reinterpret_cast< uint32_t* >( memory::get_address_from_offset( 0x143AF1820 ) );

        // the remaining native attach/detach calls (see declarations above)
        trailer_settle_fn_ptr = reinterpret_cast< trailer_settle_fn* >( memory::get_address_from_offset( 0x1409FB700 ) );
        pair_vehicles_fn = reinterpret_cast< vehicle_pair_fn* >( memory::get_address_from_offset( 0x1408771A0 ) );
        apply_cables_fn = reinterpret_cast< vehicle_cables_fn* >( memory::get_address_from_offset( 0x1408762E0 ) );
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
        get_cvar_int_fn = reinterpret_cast< cvar_get_int_fn* >( memory::get_address_from_offset( 0x1401CBE10 ) );
        cables_mode_cvar = reinterpret_cast< void* >( memory::get_address_from_offset( 0x142D485C0 ) );
        attach_settle_cvar = reinterpret_cast< void* >( memory::get_address_from_offset( 0x142D48200 ) );

        // the coupling/decoupling event poster + its const param block and the
        // two token strings, all fixed image offsets read out of the native
        // attach/detach call sites
        post_event_fn_ptr = reinterpret_cast< post_event_fn* >( memory::get_address_from_offset( 0x140623DA0 ) );
        event_param_block = reinterpret_cast< void* >( memory::get_address_from_offset( 0x142D33198 ) );
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
            distribute_towed_mass_fn = reinterpret_cast< prism::vehicle_distribute_towed_mass_fn* >( distribute_mass_address );
            refresh_axle_lift_fn = reinterpret_cast< prism::game_actor_refresh_axle_lift_fn* >( refresh_lift_address );
            return true;
        }
        CCore::g_instance->warning( "Funções nativas de engate não encontradas; a carreta engatada pode ficar freiada" );
        return false;
    }

    // hooks on the trailer+0x760/+0x764 brake setters: they swallow positive
    // writes that did not come from the truck's own broadcast when aimed at a
    // coupled trailer still owned by a parked set, so a stale parked-set
    // chain cannot re-brake it through the shared trailer_slave link
    void register_brake_hooks()
    {
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
        // per-wheel brake solver — zeroing lifted-axle outputs must happen on
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

    bool CTrailerManipulation::init()
    {
        // Individually detachable trailers. Two extra guards are needed: a null-joint
        // check the game crashes on while a trailer is detached, and connect_slave,
        // which would otherwise re-attach the trailers behind the one being connected.
        const auto crash_address = memory::get_address_for_pattern( "48 85 d2 0f 84 ? ? ? ? 48 89 74 24 18 57 48 83 ec 40" );
        const auto connect_slave_address = memory::get_address_for_pattern( "40 53 48 83 ec 60 48 83 b9 ? ? ? ? 00 48 8b d9 0f 84 ? ? ? ? 48 8d 54 24 ? e8" );

        if ( crash_address == 0 || connect_slave_address == 0 )
        {
            CCore::g_instance->warning( "Engate/desengate de carretas indisponível nesta versão do jogo" );
        }
        else
        {
            // get_slave_hook_position is the tail call inside connect_slave
            get_slave_hook_position_fn = reinterpret_cast< prism::physics_trailer_u_get_slave_hook_position_fn* >(
                connect_slave_address + 29 + *reinterpret_cast< int32_t* >( connect_slave_address + 29 ) + 4 );

            resolve_native_fns();
            register_brake_hooks();

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
                this->connect_valid_ = true;
            }
            else
            {
                CCore::g_instance->warning( "Não foi possível instalar os hooks de engate/desengate" );
            }
        }

        // steerable axles (oversize trailers): the hook only swaps the angle,
        // so a miss just means no steering control — never fatal
        const auto steering_address = memory::get_address_for_pattern(
            "48 89 5c 24 08 48 89 74 24 10 57 48 83 ec ? 8b 41 ? 48 8b d9 0f 29 74" );
        if ( steering_address != 0 )
        {
            set_individual_steering_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
                "set_individual_steering", steering_address,
                reinterpret_cast< uint64_t >( &hk_set_individual_steering ) );
            if ( set_individual_steering_hook != nullptr )
            {
                set_individual_steering_hook->hook();
            }
        }
        else
        {
            CCore::g_instance->warning( "Direção de eixos indisponível nesta versão do jogo" );
        }

        // reverse-gear detection for the axle-lift assist; each channel
        // failing just narrows the fallback, neither is fatal
        const auto* init_params = CCore::g_instance->get_init_params();
        if ( init_params->register_for_channel( SCS_TELEMETRY_TRUCK_CHANNEL_light_reverse,
                SCS_U32_NIL, SCS_VALUE_TYPE_bool, SCS_TELEMETRY_CHANNEL_FLAG_none,
                &on_light_reverse, nullptr ) != SCS_RESULT_ok )
        {
            CCore::g_instance->warning( "Could not register 'truck.light.reverse'" );
        }
        if ( init_params->register_for_channel( SCS_TELEMETRY_TRUCK_CHANNEL_engine_gear,
                SCS_U32_NIL, SCS_VALUE_TYPE_s32, SCS_TELEMETRY_CHANNEL_FLAG_none,
                &on_engine_gear, nullptr ) != SCS_RESULT_ok )
        {
            CCore::g_instance->warning( "Could not register 'truck.engine.gear'" );
        }
        if ( init_params->register_for_channel( SCS_TELEMETRY_TRUCK_CHANNEL_brake_air_pressure,
                SCS_U32_NIL, SCS_VALUE_TYPE_float, SCS_TELEMETRY_CHANNEL_FLAG_none,
                &on_air_pressure, nullptr ) != SCS_RESULT_ok )
        {
            CCore::g_instance->warning( "Could not register 'truck.brake.air.pressure'" );
        }
        if ( init_params->register_for_channel( SCS_TELEMETRY_TRUCK_CHANNEL_engine_rpm,
                SCS_U32_NIL, SCS_VALUE_TYPE_float, SCS_TELEMETRY_CHANNEL_FLAG_none,
                &on_engine_rpm, nullptr ) != SCS_RESULT_ok )
        {
            CCore::g_instance->warning( "Could not register 'truck.engine.rpm'" );
        }

        return true;
    }

    //========================================================================
    // Per-frame update — chain refresh, convoy state, attach hotkeys
    //========================================================================

    // rebuilds trailers[] from game_actor and merges in truck_towed: a spliced
    // trailer no longer appears in the chain walk, but keeps being listed so
    // the UI and hotkeys can still reach it while it sits on the truck
    void CTrailerManipulation::refresh_trailer_chain()
    {
        auto* game_actor = CCore::g_instance->get_game_actor();
        void* trailer = game_actor != nullptr
            ? prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_trailer )
            : nullptr;

        trailer_count = 0;
        for ( ; trailer != nullptr && trailer_count < max_trailers; ++trailer_count )
        {
            trailers[ trailer_count ] = trailer;
            trailer = prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave );
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
    void* chase_camera_obj()
    {
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
        if ( !any_detached && !override_active ) return;   // pure native behaviour
        void* chase = chase_camera_obj();
        if ( chase == nullptr ) return;

        void*& anchor = prism::offsets::field< void* >( chase, 0x3F0 );
        if ( any_detached )
        {
            if ( anchor != nullptr )
            {
                anchor = nullptr;   // raw store, no release — see comment above
                // recompute (vfunc 14) picks the solo distance set for the
                // now-null anchor — without it the camera keeps the stale
                // jointed params
                vcall< cam_recompute_fn >( chase, 14, uint8_t( 1 ) );
                CCore::g_instance->info( "cam: anchor -> truck (set detached)" );
            }

            // zoom distance: the native update clamps the target (+0x4D8) to
            // [+0x480, base], so raising +0x480 above the base distance pins
            // the camera at our value — pure field write, the native lerp at
            // +0x4DC still smooths every change. Wheel forward = closer,
            // backward = farther, no upper limit.
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
        else
        {
            // set recoupled: the native pick is the CHAIN HEAD (first
            // trailer), not the last one — the setter hook log shows the
            // native resolver calls set_target(t0). The camera measures
            // the combination length by walking trailer_slave links
            // STARTING AT the anchor; anchoring on the last trailer
            // measures only that trailer and the camera parks just
            // behind the truck — exactly the "stuck on the horse" bug.
            void* chain_head = nullptr;
            for ( uint32_t i = 0; i < trailer_count; ++i )
            {
                void* t = trailers[ i ];
                if ( t != nullptr && joint( t ) != nullptr )
                {
                    chain_head = t;
                    break;
                }
            }
            if ( anchor != chain_head && cam_set_target_hook != nullptr )
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
            // recompute (vfunc 14) re-reads the anchor's joint and switches
            // the camera back to the jointed distance set (full SCS range)
            // — skipped while detached because it resets the orbit state
            vcall< cam_recompute_fn >( chase, 14, uint8_t( 1 ) );
        }
        override_active = any_detached;
    }

    // trailer hose/cable visuals + the detach/restore traffic suppression.
    // g_trailer_cables_mode: 0 nothing coupled, 1 first trailer (or partial
    // set) coupled, 2 whole original set coupled — written only on change, so
    // the console stays free for the user. A trailer counts only while
    // actually coupled to the truck's convoy — trailer 1 through the fifth
    // wheel, the rest through the chain or straight on the truck.
    void CTrailerManipulation::update_convoy_state()
    {
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

        uint32_t mode = 0;
        if ( trailer_count > 0 )
        {
            if ( in_convoy[ 0 ] ) mode = 1;
            if ( trailer_count > 1 && in_convoy[ trailer_count - 1 ] ) mode = 2;
        }
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
        // parked byte and that flag are dropped, the record is kept
        for ( uint32_t i = 0; i < trailer_count; ++i )
        {
            if ( !in_convoy[ i ] ) continue;
            void* t = trailers[ i ];
            const bool parked_flag = prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked ) != 0;
            const bool parked_rec = prism::offsets::field< void* >( t, prism::offsets::trailer_parked_state ) != nullptr;
            // only a live parked state needs clearing — the record (+0x1188)
            // stays linked on purpose for the traffic registration, so its
            // mere presence must not re-trigger this (it fires every frame
            // and zeroes the coupled trailer's brake channels)
            const bool parked_reg = prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked_registered ) != 0;
            if ( parked_flag || parked_reg )
            {
                prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked ) = 0;
                prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked_registered ) = 0;
                prism::offsets::field< float >( t, prism::offsets::trailer_brake ) = 0.f;
                prism::offsets::field< uint8_t >( t, prism::offsets::trailer_brake_forced ) = 0;
                CCore::g_instance->info( "un-parked trailer {} (flag {} record {})", i, parked_flag, parked_rec );
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
        // variants connect that trailer straight to the fifth wheel. Trailer 0
        // (index 0) is left to the game's own T key
        bool ad2 = false, ad3 = false, tk2 = false, tk3 = false;
        key_down( settings::Action::ATTACH_DETACH_2, &ad2 );
        key_down( settings::Action::ATTACH_DETACH_3, &ad3 );
        key_down( settings::Action::ATTACH_TRUCK_2, &tk2 );
        key_down( settings::Action::ATTACH_TRUCK_3, &tk3 );
        if ( ad2 ) this->toggle_attach( 1 );
        if ( ad3 ) this->toggle_attach( 2 );
        if ( tk2 ) this->connect_to_truck( 1 );
        if ( tk3 ) this->connect_to_truck( 2 );
    }

    // steering hotkeys + lock state for steerable-axle (oversize) trailers;
    // ignored while the game is in the background or a key is being rebound
    void CTrailerManipulation::update_steering( const float dt )
    {
        // nothing to steer on a trailer without steerable wheels, or one that is gone
        for ( uint32_t i = 0; i < max_trailers; ++i )
        {
            if ( i >= trailer_count || steering_data( trailers[ i ] ) == nullptr )
            {
                locked_ui[ i ] = false;
                centering_ui[ i ] = false;
            }
        }

        if ( game_has_focus() && !settings::is_capturing() )
        {
            bool lock_pressed = false, center_pressed = false;
            key_down( settings::Action::TOGGLE_STEERING_LOCK, &lock_pressed );
            key_down( settings::Action::STEER_CENTER, &center_pressed );
            const bool left = key_down( settings::Action::STEER_LEFT );
            const bool right = key_down( settings::Action::STEER_RIGHT );

            bool any_locked = false;
            for ( uint32_t i = 0; i < trailer_count; ++i ) any_locked |= locked_ui[ i ];

            for ( uint32_t i = 0; i < trailer_count; ++i )
            {
                if ( steering_data( trailers[ i ] ) == nullptr ) continue;

                if ( lock_pressed )
                {
                    set_locked( i, !any_locked );
                }
                else if ( left || right || center_pressed )
                {
                    set_locked( i, true ); // steering a trailer means taking control of it
                }

                if ( !locked_ui[ i ] ) continue;

                const float step = settings::g_settings.steering_speed * dt;
                if ( center_pressed ) centering_ui[ i ] = true;
                if ( left || right ) centering_ui[ i ] = false;
                if ( centering_ui[ i ] )
                {
                    // ease back to straight instead of snapping
                    if ( angle_ui[ i ] > step ) angle_ui[ i ] -= step;
                    else if ( angle_ui[ i ] < -step ) angle_ui[ i ] += step;
                    else
                    {
                        angle_ui[ i ] = 0.f;
                        centering_ui[ i ] = false;
                    }
                }
                if ( left ) angle_ui[ i ] = ( std::max )( angle_ui[ i ] - step, -1.f );
                if ( right ) angle_ui[ i ] = ( std::min )( angle_ui[ i ] + step, 1.f );
                locked_steering[ i ].angle.store( angle_ui[ i ] );
            }
        }

        // published every frame: the data pointer changes when trailers are swapped
        for ( uint32_t i = 0; i < max_trailers; ++i )
        {
            locked_steering[ i ].data.store( locked_ui[ i ] ? steering_data( trailers[ i ] ) : nullptr );
            if ( i < trailer_count && !locked_ui[ i ] )
            {
                angle_ui[ i ] = prism::offsets::field< float >( trailers[ i ], prism::offsets::trailer_steering );
            }
        }
    }

    void CTrailerManipulation::update()
    {
        // joystick buttons have no WM_KEYDOWN — poll them while a capture is open
        settings::poll_joystick_capture();

        refresh_trailer_chain();
        update_convoy_state();
        gc_detached_marks();
        watch_sim_heartbeat();

        auto* game_actor = CCore::g_instance->get_game_actor();

        // Reverse-gear lift assist: on the engage edge, with every trailer
        // axle on the ground, only the first liftable axle of each trailer
        // stays selected and the controllers are commanded to raise — as if
        // the lift key was pressed. On the disengage edge whatever we raised
        // is lowered again and the saved selection resumes. Axles already up
        // when reverse engages are left untouched.
        const bool reversing = settings::g_settings.reverse_lift
            && ( reverse_light.load( std::memory_order_relaxed )
                || engine_gear.load( std::memory_order_relaxed ) < 0 );

        bool reverse_just_engaged = false;
        if ( reversing && !was_reversing )
        {
            // all trailers must be down, and at least one must have a liftable axle
            bool all_down = trailer_count > 0, any_liftable = false;
            for ( uint32_t i = 0; i < trailer_count; ++i )
            {
                any_liftable |= lift_axles[ i ].native_axles != 0;
                auto* controller = lift_axles[ i ].controller;
                if ( controller != nullptr
                    && prism::offsets::field< uint8_t >( controller, prism::offsets::lift_controller_state ) != 0 )
                {
                    all_down = false;
                }
            }
            reverse_override = all_down && any_liftable;
            reverse_just_engaged = reverse_override;
        }
        else if ( !reversing && was_reversing )
        {
            reverse_override = false;
            if ( reverse_auto_lift )
            {
                reverse_auto_lift = false;
                for ( uint32_t i = 0; i < trailer_count; ++i )
                {
                    // fresh read — lift_axles[] may still point at a detached trailer
                    auto* controller = prism::offsets::field< void* >( trailers[ i ], prism::offsets::trailer_lift_controller );
                    if ( controller != nullptr
                        && prism::offsets::field< uint8_t >( controller, prism::offsets::lift_controller_state ) != 0 )
                    {
                        prism::offsets::field< uint8_t >( controller, prism::offsets::lift_controller_state ) = 1; // lowering
                    }
                }
                if ( game_actor != nullptr )
                {
                    prism::offsets::field< uint8_t >( game_actor, prism::offsets::game_actor_lift_state ) &= ~2u;
                }
            }
        }
        was_reversing = reversing;

        // Axle lift: rewrite each trailer's lift controller mask (+0x38) and
        // the per-axle liftable flags on the axle data so the game's lift key
        // raises exactly the natively liftable axles picked in the UI.
        for ( uint32_t i = 0; i < max_trailers; ++i )
        {
            auto& lift = lift_axles[ i ];

            void* axle_data = nullptr;
            void* controller = nullptr;
            if ( i < trailer_count )
            {
                axle_data = prism::offsets::field< void* >( trailers[ i ], prism::offsets::vehicle_chassis_data );
                controller = prism::offsets::field< void* >( trailers[ i ], prism::offsets::trailer_lift_controller );
            }

            const auto flag_count = axle_data != nullptr
                ? prism::offsets::field< uint64_t >( axle_data, prism::offsets::axle_data_flags_count )
                : 0;
            auto* flags = axle_data != nullptr
                ? prism::offsets::field< uint8_t* >( axle_data, prism::offsets::axle_data_flags )
                : nullptr;
            if ( flags == nullptr || flag_count == 0 || flag_count > max_axles )
            {
                lift = LiftAxles{}; // detached, or not set up yet
                continue;
            }

            if ( lift.axle_data != axle_data || lift.flags != flags
                || ( controller != nullptr && lift.controller != controller ) )
            {
                // first sight, or the game rebuilt the objects: snapshot its values
                lift.axle_data = axle_data;
                lift.flags = flags;
                lift.axle_count = static_cast< uint32_t >( flag_count );
                lift.controller = controller;
                lift.native_axles = 0;
                lift.orig_mask = controller != nullptr
                    ? prism::offsets::field< uint32_t >( controller, prism::offsets::lift_controller_axle_mask )
                    : 0;
                for ( uint32_t a = 0; a < lift.axle_count; ++a )
                {
                    lift.orig_flags[ a ] = flags[ a ];
                    if ( flags[ a ] != 0 ) lift.native_axles |= 1u << a;
                }
                lift.applied_mask = ~0u; // force a write this frame
            }
            else if ( controller != nullptr
                && prism::offsets::field< uint32_t >( controller, prism::offsets::lift_controller_axle_mask ) != lift.applied_mask )
            {
                // the game re-initialized the mask (trailer data reload): re-snapshot
                lift.orig_mask = prism::offsets::field< uint32_t >( controller, prism::offsets::lift_controller_axle_mask );
                lift.native_axles = 0;
                for ( uint32_t a = 0; a < lift.axle_count; ++a )
                {
                    lift.orig_flags[ a ] = flags[ a ];
                    if ( flags[ a ] != 0 ) lift.native_axles |= 1u << a;
                }
            }

            // an existing controller must agree with the flag array on the
            // axle count; a mismatch means the layout shifted in a game update
            if ( controller != nullptr
                && prism::offsets::field< uint32_t >( controller, prism::offsets::lift_controller_axle_count ) != lift.axle_count )
            {
                continue;
            }

            if ( controller == nullptr ) continue;

            const auto state = prism::offsets::field< uint8_t >( controller, prism::offsets::lift_controller_state );
            if ( state != 0 && lift.applied_mask == 0 )
            {
                // nothing can be up with an empty mask, but the native toggle
                // still flipped the state — a mask of 0 never animates, so the
                // controller would stay "raising" forever; bring it back to down
                prism::offsets::field< uint8_t >( controller, prism::offsets::lift_controller_state ) = 0;
            }
            else if ( state != 0 )
            {
                // raised or moving: leave the mask and flags untouched —
                // flipping them mid-air leaves a mixed progress state the
                // game cannot resolve
                continue;
            }

            const auto stored = settings::g_settings.axle_masks[ i ];
            // only natively liftable axles may be selected; mask off anything
            // else so a stale settings value can never reach the game
            uint32_t selection = ( stored < 0 ? lift.native_axles : static_cast< uint32_t >( stored ) )
                & lift.native_axles;
            if ( reverse_override )
            {
                selection = lift.native_axles & ( 0u - lift.native_axles ); // first liftable axle only
            }

            for ( uint32_t a = 0; a < lift.axle_count; ++a )
            {
                flags[ a ] = ( ( selection >> a ) & 1 ) != 0
                    ? lift.orig_flags[ a ]
                    : static_cast< uint8_t >( 0 );
            }

            prism::offsets::field< uint32_t >( controller, prism::offsets::lift_controller_axle_mask ) = selection;
            lift.applied_mask = selection;
        }

        // the override mask is in place now: command the raise like the native key
        if ( reverse_just_engaged )
        {
            if ( game_actor != nullptr )
            {
                prism::offsets::field< uint8_t >( game_actor, prism::offsets::game_actor_lift_state ) |= 2u;
            }
            for ( uint32_t i = 0; i < trailer_count; ++i )
            {
                auto* controller = prism::offsets::field< void* >( trailers[ i ], prism::offsets::trailer_lift_controller );
                if ( controller != nullptr && lift_axles[ i ].applied_mask != 0
                    && lift_axles[ i ].applied_mask != ~0u
                    && prism::offsets::field< uint8_t >( controller, prism::offsets::lift_controller_state ) == 0 )
                {
                    prism::offsets::field< uint8_t >( controller, prism::offsets::lift_controller_state ) = 2; // raising
                }
            }
            reverse_auto_lift = true;
        }

        // Cargo respawn watch: while a detached section sits parked, keep its
        // slots' transforms fresh (head and jointed members alike). When a
        // marked slot's object shows up jointed without ever being unmarked,
        // the game respawned the set attached — queue the restore. ~30 frames
        // of settled state first so the cinematic's own writes finish
        bool in_section = false;
        for ( uint32_t i = 0; i < trailer_count; ++i )
        {
            void* t = trailers[ i ];
            if ( t == nullptr ) { detached_idx[ i ] = false; in_section = false; continue; }
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
        // a slot beyond the current set can never be a parked trailer — drop
        // any stale mark so a future trailer landing there isn't restored
        for ( uint32_t i = trailer_count; i < max_trailers; ++i )
        {
            detached_idx[ i ] = false;
            parked_xform_valid[ i ] = false;
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

        update_hotkeys();
        update_steering( ImGui::GetIO().DeltaTime );
        update_air( ImGui::GetIO().DeltaTime );

        // trailer brake bias: +0x730 is only a demand mirror — the wheel
        // solver (0x14090C570) turns +0x760 into real torque via
        // max(factor, per-wheel min) * grip, no upper clamp, and ONLY for
        // wheels whose bit is set in the wheelset mask (+0x2BC). The native
        // mask marks just the braked def axles (e.g. 0xC = middle axle of a
        // vanderleia); we rewrite it to cover every wheel of every GROUNDED
        // axle — lifted axles lose their bits, which matches both the user's
        // request and reality (a wheel in the air brakes nothing)
        const float brake_mult = 1.f + settings::g_settings.trailer_brake_boost / 100.f;
        if ( brake_mult > 1.001f )
        {
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

    }

    //========================================================================
    // User actions (buttons + hotkeys — they only queue sim-thread ops)
    //========================================================================
    void CTrailerManipulation::disconnect_trailer( const uint32_t i ) const
    {
        void* trailer = trailers[ i ];
        const bool on_truck = trailer == truck_towed;
        void* parent = nullptr;
        if ( auto* game_actor = CCore::g_instance->get_game_actor() )
        {
            parent = i == 0 || on_truck
                ? prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck )
                : trailers[ i - 1 ];
        }

        // a middle link may leave with its slave still jointed — the detached
        // pair (dolly + rear trailer) parks as a unit; the detach mark keeps
        // the game from auto-re-attaching the freed link to its old parent
        CCore::g_instance->info( "Disconnecting trailer {}", i );

        // traffic stays live: the parked vfunc (+0x328) registers the detached
        // trailer as a proper lane obstacle, so the sweep handles it like any
        // parked trailer — no suppression or despawn wait needed
        if ( queue_op( pending_kind::disconnect, trailer, parent, i == 0 || on_truck, 0 ) )
        {
            traffic_detach_done.store( false, std::memory_order_release );
        }
    }

    void CTrailerManipulation::connect_trailer( const uint32_t i ) const
    {
        if ( i >= trailer_count || joint( trailers[ i ] ) != nullptr ) return;
        auto* game_actor = CCore::g_instance->get_game_actor();
        void* parent = i == 0
            ? ( game_actor != nullptr ? prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck ) : nullptr )
            : trailers[ i - 1 ];
        if ( parent == nullptr )
        {
            CCore::g_instance->warning( "Não foi possível engatar a carreta {}: veículo anterior não encontrado", i );
            return;
        }
        // the chain parent must be attached itself — the game only ever creates
        // a joint onto an already-jointed parent; hooking onto a free trailer
        // leaves the component half-initialized (purecall crash)
        if ( i > 0 && joint( parent ) == nullptr )
        {
            CCore::g_instance->warning( "Engate a carreta {} primeiro", i );
            return;
        }
        // proximity rule like the native T: hooks must nearly touch — no
        // teleporting the trailer onto the hook (< 7 m measured hook-to-hook)
        if ( hook_far( trailers[ i ], parent, i == 0 ) )
        {
            CCore::g_instance->warning( "A carreta {} está muito longe — aproxime dando ré para engatar", i + 1 );
            return;
        }
        // back on its chain parent: the game may manage this trailer again
        unmark_detached( trailers[ i ] );
        queue_op( pending_kind::connect, trailers[ i ], parent, i == 0 );
    }

    void CTrailerManipulation::connect_to_truck( const uint32_t i ) const
    {
        auto* game_actor = CCore::g_instance->get_game_actor();
        if ( game_actor == nullptr || i >= trailer_count ) return;
        if ( joint( trailers[ i ] ) != nullptr ) return; // already attached

        // the fifth wheel must be free: refuse when trailer 1 is still attached
        // to it or another trailer was connected to the truck through this feature
        if ( joint( trailers[ 0 ] ) != nullptr || truck_towed != nullptr )
        {
            CCore::g_instance->warning( "O cavalo já tem uma carreta engatada" );
            return;
        }

        // a trailer that still tows the next chain link can go on the fifth
        // wheel as a unit — truck → dolly → rear trailer is exactly the
        // rodotrem this feature exists for, and the native T attach does the
        // same when a whole set couples at once. Only refuse when the next
        // chain trailer is jointed onto a DIFFERENT parent
        void* next = i + 1 < trailer_count ? trailers[ i + 1 ] : nullptr;
        if ( next != nullptr && next != truck_towed && joint( next ) != nullptr &&
            prism::offsets::field< void* >( trailers[ i ], prism::offsets::trailer_slave ) != next )
        {
            CCore::g_instance->warning( "Desengate a carreta {} primeiro", i + 1 );
            return;
        }

        // same proximity rule: no teleporting a far trailer onto the fifth wheel
        void* truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
        if ( hook_far( trailers[ i ], truck, true ) )
        {
            CCore::g_instance->warning( "A carreta {} está muito longe — aproxime dando ré para engatar", i + 1 );
            return;
        }

        // stays marked as detached: its chain parent still claims it logically,
        // so connect_slave must keep refusing it while it sits on the fifth wheel
        if ( queue_op( pending_kind::connect, trailers[ i ],
            prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck ), true ) )
        {
            truck_towed = trailers[ i ];
        }
    }

    void CTrailerManipulation::toggle_attach( const uint32_t i ) const
    {
        // the first trailer stays on the game's own T key: its detach path is
        // not safe to replay manually
        if ( i == 0 || i >= trailer_count ) return;
        if ( joint( trailers[ i ] ) != nullptr )
        {
            this->disconnect_trailer( i );
        }
        else
        {
            this->connect_trailer( i );
        }
    }

    namespace ui
    {
        // The game's framebuffer is sRGB-encoded: a style color is treated as
        // linear by the pipeline and gamma-encoded on the way out, which washes
        // every color toward white. Feeding colors already linearized
        // (srgb^2.2) makes them land on screen at the intended hex value.
        ImVec4 pdv( const uint32_t hex )
        {
            const auto lin = []( const uint32_t c )
            {
                return std::pow( ( c & 0xFF ) / 255.f, 2.2f );
            };
            return ImVec4( lin( hex >> 16 ), lin( hex >> 8 ), lin( hex ), 1.f );
        }

        ImU32 pdv32( const uint32_t hex )
        {
            const auto lin = []( const uint32_t c )
            {
                return static_cast< int >( std::pow( ( c & 0xFF ) / 255.f, 2.2f ) * 255.f + 0.5f );
            };
            return IM_COL32( lin( hex >> 16 ), lin( hex >> 8 ), lin( hex ), 255 );
        }

        // On/off pill switch drawn with the draw list (ImGui has no native one).
        bool toggle_switch( const char* id, bool* v )
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
        void right_side( const float width )
        {
            ImGui::SameLine( 0.f, 0.f );
            ImGui::SetCursorPosX( ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - width );
        }
    }

    // keybind list: action name, the bound key as a clickable capture button,
    // and a small clear button — packed tight for the narrow side panel
    void CTrailerManipulation::render_keybind_row( const int i ) const
    {
        const auto action = static_cast< settings::Action >( i );
        const bool fixed = action == settings::Action::TOGGLE_UI
            || action == settings::Action::TOGGLE_CURSOR;
        ImGui::PushID( i );
        ImGui::TextWrapped( "%s", settings::action_name( action ) );
        if ( fixed )
        {
            // rebindable in settings.ini only — keeping them off the
            // capture path makes the plugin unreachable harder
            ImGui::TextDisabled( "%s (settings.ini)", settings::key_name( settings::key( action ) ).c_str() );
        }
        else
        {
            const auto label = settings::is_capturing( action )
                                   ? std::string( "Pressione uma tecla... (Esc cancela)" )
                                   : settings::key_name( settings::key( action ) );
            if ( ImGui::Button( fmt::format( "{}##bind", label ).c_str(), ImVec2( -ImGui::GetFrameHeight() * 1.8f, 0 ) ) )
            {
                settings::begin_capture( action );
            }
            ImGui::SameLine();
            ImGui::BeginDisabled( settings::key( action ) == 0 );
            if ( ImGui::SmallButton( "X##clear" ) )
            {
                settings::g_settings.keys[ i ] = 0;
                settings::save();
            }
            ImGui::EndDisabled();
        }
        ImGui::Spacing();
        ImGui::PopID();
    }

    void CTrailerManipulation::render_keybinds() const
    {
        for ( int i = 0; i < settings::action_count; ++i )
        {
            const auto action = static_cast< settings::Action >( i );
            // steering binds live in the Oversize panel
            if ( action == settings::Action::STEER_LEFT ||
                 action == settings::Action::STEER_RIGHT ||
                 action == settings::Action::STEER_CENTER ||
                 action == settings::Action::TOGGLE_STEERING_LOCK ) continue;
            this->render_keybind_row( i );
        }
    }

    // per-trailer axle selection for the game's own trailer lift key
    void CTrailerManipulation::render_trailer_lift( const uint32_t i ) const
    {
        const auto& lift = lift_axles[ i ];
        if ( lift.axle_data == nullptr )
        {
            ImGui::TextDisabled( "Sem dados de eixo" );
            return;
        }

        auto* controller = prism::offsets::field< void* >( trailers[ i ], prism::offsets::trailer_lift_controller );
        const auto state = controller != nullptr
            ? prism::offsets::field< uint8_t >( controller, prism::offsets::lift_controller_state )
            : 0;
        ImGui::Text( "Eixos: %s", state != 0 ? "levantados" : "no chão" );

        // axles moving or up with a real mask physically occupy the air — the
        // selection must stay put until they are back on the ground; a state
        // desynced from an empty mask (nothing lifted) stays editable so the
        // trailer is never soft-locked
        bool locked = state != 0 && lift.applied_mask != 0;
        if ( locked )
        {
            ImGui::TextDisabled( "Abaixe os eixos para alterar quais serão levantados." );
        }

        if ( lift.native_axles == 0 )
        {
            ImGui::TextDisabled( "Nenhum eixo elevável nesta carreta" );
            return;
        }

        auto& stored = settings::g_settings.axle_masks[ i ];
        // only natively liftable axles are selectable, matching update()
        uint32_t selection = ( stored < 0 ? lift.native_axles : static_cast< uint32_t >( stored ) )
            & lift.native_axles;
        if ( reverse_override )
        {
            selection = lift.native_axles & ( 0u - lift.native_axles );
            locked = true;
            ImGui::TextDisabled( "Ré engatada: somente o primeiro eixo elevável está levantado." );
        }

        ImGui::BeginDisabled( locked );
        uint32_t next = selection;
        const float switch_w = ImGui::GetFrameHeight() * 1.9f;
        for ( uint32_t a = 0; a < lift.axle_count; ++a )
        {
            if ( ( lift.native_axles >> a ) & 1 )
            {
                bool on = ( selection >> a ) & 1;
                ImGui::Text( "Eixo %u", a + 1 );
                ui::right_side( switch_w );
                const auto id = fmt::format( "##axle{}", a );
                if ( ui::toggle_switch( id.c_str(), &on ) )
                {
                    next = on ? next | ( 1u << a ) : next & ~( 1u << a );
                }
            }
            else
            {
                ImGui::TextDisabled( "Eixo %u", a + 1 );
                ui::right_side( switch_w );
                ImGui::TextDisabled( "fixo" );
            }
        }
        ImGui::EndDisabled();

        if ( next != selection )
        {
            stored = static_cast< int32_t>( next );
            settings::save();
        }
    }

    // steerable-axle controls for one oversize trailer: lock toggle, angle
    // slider, arrows and center — the angle is applied by the game's next
    // steering advance on its own thread
    void CTrailerManipulation::render_trailer_steering( const uint32_t i ) const
    {
        bool lock = locked_ui[ i ];
        if ( ui::toggle_switch( "##steer_lock", &lock ) )
        {
            set_locked( i, lock );
        }
        ImGui::SameLine();
        ImGui::Text( "Controle manual" );

        ImGui::BeginDisabled( !locked_ui[ i ] );
        bool changed = ImGui::SliderFloat( "Ângulo", &angle_ui[ i ], -1.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp );

        ImGui::PushItemFlag( ImGuiItemFlags_ButtonRepeat, true );
        if ( ImGui::ArrowButton( "esquerda", ImGuiDir_Left ) )
        {
            angle_ui[ i ] = ( std::max )( angle_ui[ i ] - 0.02f, -1.f );
            changed = true;
        }
        ImGui::SameLine();
        if ( ImGui::Button( "centro" ) )
        {
            centering_ui[ i ] = true; // eased to 0 in update() at steering_speed
        }
        ImGui::SameLine();
        if ( ImGui::ArrowButton( "direita", ImGuiDir_Right ) )
        {
            angle_ui[ i ] = ( std::min )( angle_ui[ i ] + 0.02f, 1.f );
            changed = true;
        }
        ImGui::PopItemFlag();
        ImGui::EndDisabled();

        if ( changed )
        {
            locked_steering[ i ].angle.store( angle_ui[ i ] );
        }
    }

    // the Oversize panel: steering controls for every steerable trailer in the
    // chain, the steering keybinds and the response-speed slider
    void CTrailerManipulation::render_oversize() const
    {
        bool any_steerable = false;
        for ( uint32_t i = 0; i < trailer_count; ++i )
        {
            if ( steering_data( trailers[ i ] ) == nullptr ) continue;
            any_steerable = true;
            ImGui::SeparatorText( fmt::format( "Carreta {}", i + 1 ).c_str() );
            ImGui::PushID( static_cast< int >( i ) );
            this->render_trailer_steering( i );
            ImGui::PopID();
        }
        if ( !any_steerable )
        {
            ImGui::TextWrapped( "Nenhuma carreta com eixos direcionais no conjunto." );
        }

        ImGui::SeparatorText( "Teclas" );
        for ( int i = 0; i < settings::action_count; ++i )
        {
            const auto action = static_cast< settings::Action >( i );
            if ( action != settings::Action::STEER_LEFT &&
                 action != settings::Action::STEER_RIGHT &&
                 action != settings::Action::STEER_CENTER &&
                 action != settings::Action::TOGGLE_STEERING_LOCK ) continue;
            this->render_keybind_row( i );
        }

        ImGui::SeparatorText( "Resposta" );
        float speed = settings::g_settings.steering_speed;
        if ( ImGui::SliderFloat( "Velocidade", &speed, 0.1f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp ) )
        {
            settings::g_settings.steering_speed = speed;
            settings::save();
        }
    }

    //========================================================================
    // UI
    //========================================================================
    void CTrailerManipulation::render_trailer_connect( const uint32_t i ) const
    {
        // the first trailer belongs to the game's own T key; the plugin only
        // manages the trailers behind it
        if ( i == 0 )
        {
            ImGui::TextDisabled( "Gerenciada pelo jogo (tecla T)" );
            return;
        }

        ImGui::SeparatorText( "Engatar / Desengatar" );
        if ( !this->connect_valid_ )
        {
            ImGui::TextDisabled( "Indisponível nesta versão do jogo" );
            return;
        }

        const bool attached = joint( trailers[ i ] ) != nullptr;
        ImGui::BeginDisabled( attached );
        if ( ImGui::Button( "Engatar##trailer" ) )
        {
            this->connect_trailer( i );
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled( !attached );
        if ( ImGui::Button( "Desengatar##trailer" ) )
        {
            this->disconnect_trailer( i );
        }
        ImGui::EndDisabled();

        // picking up a trailer behind the first one with the truck directly,
        // only possible while the fifth wheel is free (trailer 1 detached or
        // absent)
        if ( !attached )
        {
            const bool wheel_free = truck_towed == nullptr && ( trailer_count == 0 || joint( trailers[ 0 ] ) == nullptr );
            ImGui::BeginDisabled( !wheel_free );
            if ( ImGui::Button( "Engatar no cavalo##trailer" ) )
            {
                this->connect_to_truck( i );
            }
            ImGui::EndDisabled();
            if ( !wheel_free )
            {
                ImGui::SameLine();
                ImGui::TextDisabled( "(quinta roda ocupada)" );
            }
        }
    }

    void CTrailerManipulation::render()
    {
        // PDV-CONTROLE palette, pre-linearized (ui::pdv): #232359 primary blue,
        // #222888 sidebar/hover, #303030 window, #282828 panels, #020202
        // inputs, #444444 border, #4a6fa5 buttons, #27ae60 green, white text
        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 10.f, 10.f ) );
        ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 10.f, 8.f ) );
        ImGui::PushStyleVar( ImGuiStyleVar_FrameRounding, 3.f );
        ImGui::PushStyleVar( ImGuiStyleVar_ChildRounding, 3.f );
        ImGui::PushStyleVar( ImGuiStyleVar_FrameBorderSize, 1.f );
        ImGui::PushStyleColor( ImGuiCol_Text, ui::pdv( 0xFFFFFF ) );
        ImGui::PushStyleColor( ImGuiCol_TextDisabled, ui::pdv( 0x656565 ) );
        ImGui::PushStyleColor( ImGuiCol_WindowBg, ui::pdv( 0x303030 ) );
        ImGui::PushStyleColor( ImGuiCol_ChildBg, ui::pdv( 0x282828 ) );
        ImGui::PushStyleColor( ImGuiCol_Border, ui::pdv( 0x444444 ) );
        ImGui::PushStyleColor( ImGuiCol_FrameBg, ui::pdv( 0x020202 ) );
        ImGui::PushStyleColor( ImGuiCol_TitleBg, ui::pdv( 0x232359 ) );
        ImGui::PushStyleColor( ImGuiCol_TitleBgActive, ui::pdv( 0x232359 ) );
        ImGui::PushStyleColor( ImGuiCol_Button, ui::pdv( 0x4A6FA5 ) );
        ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ui::pdv( 0x222888 ) );
        ImGui::PushStyleColor( ImGuiCol_ButtonActive, ui::pdv( 0x232359 ) );
        ImGui::PushStyleColor( ImGuiCol_Header, ui::pdv( 0x222888 ) );
        ImGui::PushStyleColor( ImGuiCol_HeaderHovered, ui::pdv( 0x4A6FA5 ) );
        ImGui::PushStyleColor( ImGuiCol_HeaderActive, ui::pdv( 0x27AE60 ) );
        ImGui::PushStyleColor( ImGuiCol_CheckMark, ui::pdv( 0x27AE60 ) );
        ImGui::PushStyleColor( ImGuiCol_Separator, ui::pdv( 0x444444 ) );

        ImGui::SetNextWindowSize( ImVec2( 1150.f, 500.f ), ImGuiCond_FirstUseEver );
        ImGui::Begin( "Controle de Carretas" );

        // left pane: keybinds, resizable by dragging the right border — the
        // chosen width is kept in settings.ini
        const float left_w = settings::g_settings.keys_width;
        ImGui::BeginChild( "##keybinds", ImVec2( left_w, 0.f ),
            ImGuiChildFlags_Border | ImGuiChildFlags_ResizeX );
        ImGui::SeparatorText( "Teclas" );
        this->render_keybinds();
        ImGui::SeparatorText( "Assistente" );
        bool reverse_lift = settings::g_settings.reverse_lift;
        if ( ui::toggle_switch( "##reverse_lift", &reverse_lift ) )
        {
            settings::g_settings.reverse_lift = reverse_lift;
            settings::save();
        }
        ImGui::SameLine();
        ImGui::TextWrapped( "Eixo na ré (vanderleia/4º eixo)" );
        ImGui::TextDisabled( "engatou a ré - ergue o 1º eixo" );

        ImGui::SeparatorText( "Carga separada" );
        bool cargo_restore = settings::g_settings.cargo_restore;
        if ( ui::toggle_switch( "##cargo_restore", &cargo_restore ) )
        {
            settings::g_settings.cargo_restore = cargo_restore;
            settings::save();
        }
        ImGui::SameLine();
        ImGui::TextWrapped( "Carreta fica parada na doca" );
        ImGui::TextDisabled( "ao carregar/descarregar separado, ela continua separada depois" );

        ImGui::SeparatorText( "Freio" );
        float brake_boost = settings::g_settings.trailer_brake_boost;
        ImGui::SetNextItemWidth( -1.f );
        if ( ImGui::SliderFloat( "##brake_boost", &brake_boost, 0.f, 200.f, "+%.0f%%", ImGuiSliderFlags_AlwaysClamp ) )
        {
            settings::g_settings.trailer_brake_boost = brake_boost;
            settings::save();
        }
        ImGui::TextWrapped( "Carreta freia mais" );
        ImGui::TextDisabled( "ela segura primeiro; no piso escorregadio arrasta os eixos em vez do cavalo dobrar" );

        ImGui::SeparatorText( "Ar" );
        int air_boost = settings::g_settings.air_boost;
        ImGui::SetNextItemWidth( -1.f );
        if ( ImGui::SliderInt( "##air_boost", &air_boost, 0, 5,
             air_boost == 0 ? "original" : fmt::format( "+{}00%", air_boost ).c_str() ) )
        {
            settings::g_settings.air_boost = air_boost;
            settings::save();
        }
        ImGui::TextWrapped( "Compressor por rotação" );
        ImGui::TextDisabled( "multiplica a produção de ar (que já sobe com o giro) — sem mexer no consumo" );
        ImGui::EndChild();
        // the pane re-lays itself out automatically since every child sizes
        // from the available width; persist once the user lets go
        const float dragged_w = ImGui::GetItemRectSize().x;
        if ( dragged_w > 0.f && dragged_w != settings::g_settings.keys_width )
        {
            settings::g_settings.keys_width = dragged_w;
            if ( !ImGui::IsMouseDown( ImGuiMouseButton_Left ) ) settings::save();
        }

        ImGui::SameLine();

        // Oversize (steerable-axle) pane next to the keys — collapsible to a
        // thin strip so it stays out of the way when unused
        if ( settings::g_settings.oversize_collapsed )
        {
            ImGui::BeginChild( "##oversize_min", ImVec2( 34.f, 0.f ), ImGuiChildFlags_Border );
            if ( ImGui::Button( ">>", ImVec2( -1.f, ImGui::GetFrameHeight() ) ) )
            {
                settings::g_settings.oversize_collapsed = false;
                settings::save();
            }
            ImGui::EndChild();
        }
        else
        {
            ImGui::BeginChild( "##oversize", ImVec2( 290.f, 0.f ), ImGuiChildFlags_Border );
            if ( ImGui::Button( "<<", ImVec2( ImGui::GetFrameHeight() * 1.6f, 0.f ) ) )
            {
                settings::g_settings.oversize_collapsed = true;
                settings::save();
            }
            ImGui::SameLine();
            ImGui::SeparatorText( "Oversize" );
            this->render_oversize();
            ImGui::EndChild();
        }

        ImGui::SameLine();

        // right pane: one column per trailer, side by side
        ImGui::BeginChild( "##trailers", ImVec2( 0.f, 0.f ) );
        if ( trailer_count == 0 )
        {
            ImGui::Text( "Nenhuma carreta encontrada" );
        }
        else
        {
            const float col_w = ( ImGui::GetContentRegionAvail().x
                - ImGui::GetStyle().ItemSpacing.x * ( trailer_count - 1 ) ) / trailer_count;
            for ( uint32_t i = 0; i < trailer_count; ++i )
            {
                if ( i > 0 ) ImGui::SameLine();
                ImGui::PushID( static_cast< int >( i ) );
                ImGui::BeginChild( fmt::format( "##trailer{}", i ).c_str(), ImVec2( col_w, 0.f ), ImGuiChildFlags_Border );
                ImGui::SeparatorText( fmt::format( "Carreta {}", i + 1 ).c_str() );
                this->render_trailer_connect( i );
                ImGui::SeparatorText( "Eixos" );
                this->render_trailer_lift( i );
                ImGui::EndChild();
                ImGui::PopID();
            }
        }
        ImGui::EndChild();

        ImGui::End();
        ImGui::PopStyleColor( 16 );
        ImGui::PopStyleVar( 5 );
    }
}
