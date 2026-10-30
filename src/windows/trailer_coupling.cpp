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

    // console execution (sim thread only - see pending_cmd)
    prism::console_execute_line_fn* execute_line_fn = nullptr;

    // traffic-obstacle machinery read out of the native detach body:
    // - the world/vehicle manager singleton (rbx in the native detach); its
    //   +0x2B30 subsystem gets vfunc+0x138 (pre) and +0x1E0 (post) notifies
    // - the type-2 anchor re-link the native detach runs between the notifies
    // - the game-state dword (== 2 while in game) gating the update
    void** traffic_manager_global = nullptr;
    uint32_t* traffic_game_state = nullptr;
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
    //   pneumatic/electric links - a trailer attached without it keeps its
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
    // trailer_brace_down/up_sound - i.e. the gear-crank noise. dl=1 lowers
    // (detach), dl=0 raises (attach), xmm2 is the anim speed (native: 1/3)
    using trailer_brace_anim_fn = void __fastcall( void* /*trailer*/, uint8_t /*down*/, float /*speed*/ );
    trailer_brace_anim_fn* brace_anim_fn = nullptr;

    // the native attach dispatcher itself (0x1405DCC20, a game_actor method):
    // the same function the T key calls - it runs the complete native attach
    // sequence (hook settle, connect vfunc, towed registration, connector
    // pairing, cables, anchors) which our manual replica could never fully
    // reproduce. It takes the trailer to attach from game_actor+0xC8
    using native_attach_dispatch_fn = void __fastcall( void* /*game_actor*/, uint8_t /*dl*/,
        uint64_t /*r8*/, void* /*settle_value_override*/ );
    native_attach_dispatch_fn* native_attach_fn = nullptr;

    // line/hose teardown + lamp-mask setter + the camera update vfunc,
    // resolved alongside the other native helpers (all signature-verified
    // for this build - same functions the ATS build hooked at the shared
    // build hash)
    using line_state_fn = void __fastcall( void* /*vehicle*/, uint8_t /*state*/, uint8_t /*force*/ );
    line_state_fn* set_lines_fn = nullptr;
    using lamp_mask_setter_fn = uint8_t __fastcall( void* /*vehicle*/, uint8_t /*mask*/ );
    using cam_update_fn = void( void* /*camera*/, float /*dt*/ );   // vfunc 32, 0x140AC75A0

    //========================================================================
    // Estado de engate mantido aqui - estado de estacionamento foi para
    // trailer_loadstate.cpp, rastreamento de canon para
    // trailer_manipulation.cpp
    //========================================================================
    // the trailer the "connect to truck" action wants the native attach
    // dispatcher to take - forced into game_actor+0xC8 inside
    // hk_attach_dispatch only while it is the closest jointless piece
    void* attach_target = nullptr;

    // a free canon slot only auto-claims within this distance of the
    // mother's rear hook - farther means parked scenery, not a coupling
    // candidate (the spontaneous re-join fix)
    const float attach_gap_limit = 12.f;

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

    // attach offset in the parent's frame - the same vec passed to the
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
    // vfunc teleports a far trailer onto the hook in one step - an instant
    // jump that, over a live traffic lane, leaves a component half-initialized
    // (purecall). Gate on real hook-to-hook distance: trailer front hook vs
    // parent rear hook in world space. The locator frame's sign convention
    // isn't fully nailed down, so both signed offsets are measured and the
    // nearer wins - attached-or-not, one of them is the true rear hook.
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
    // would allow - no teleporting the trailer across the map onto the hook
    bool hook_far( void* trailer, void* parent, const bool parent_is_truck )
    {
        const float gap = hook_gap( trailer, parent, parent_is_truck );
        if ( gap < 0.f ) return false; // cannot measure: keep the old fail-open
        CCore::g_instance->info( "Hook gap trailer {:p} to {:p}: {:.2f}m", trailer, parent, gap );
        // bitrem chains measure ~6.9 m at touching distance (log: 6.85-7.03 m
        // with the trailers backed right up to each other) and a hair more
        // when the player stops a little short - 7.0 m refused a correct
        // coupling. One limit for every layout, with room up to 8.0 m
        return gap > 8.f;
    }

    //========================================================================
    // Native flow replicas - sim thread only
    //========================================================================

    // drive the game's own detach dispatcher (the T path) at a chosen trailer
    // by pointing the actor's trailer-of-interest slot at it - identical to
    // what pressing T on the head does. Only used while the trailer sits on
    // the truck (or straight on the fifth wheel): it is the head-detach
    // path - run on a mid-chain trailer it tears down the whole towed set's
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
        // the dispatcher's internal disconnect walks the chain directly -
        // every link must be a live trailer or the teardown faults on it
        sanitize_slave_walk( trailer );
        sanitize_slave_walk( prev );
        // o dispatcher interno anda a cadeia bypassando o hook de vfunc -
        // saneia o grafo físico também, senão uma aresta pra slot morto mata
        // o detach no meio e deixa a carreta meio-desatrelada em limbo
        sanitize_phys_walk( trailer );
        if ( prev != nullptr ) sanitize_phys_walk( prev );
        dump_chain( "pre-detach(slot)" );
        native_chain_call_active.store( true, std::memory_order_release );
        detach_dispatch_hook->get_original< detach_fn_t >()( game_actor, 1, 0 );
        native_chain_call_active.store( false, std::memory_order_release );
        dump_chain( "post-detach(slot)" );
        // the dispatcher unconditionally clears the truck's towed bookkeeping
        // - put it back when the trailer we dropped was mid-chain and
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
        // o dispatcher registra a cadeia slave INTEIRA atrás do trailer -
        // corta todas as claims estacionadas, não só a dele, senão uma peça
        // estacionada mais fundo vira membro fantasma e afunda sem trava
        sever_parked_claims();
        native_chain_call_active.store( true, std::memory_order_release );
        attach_dispatch_hook->get_original< native_attach_dispatch_fn >()( game_actor, 1, 0, nullptr );
        native_chain_call_active.store( false, std::memory_order_release );
        if ( severed && readable_ptr( trailer ) )
        {
            prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = claim;
        }
        restore_parked_claims();
        prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_trailer ) = prev;
        return joint( trailer ) != nullptr;
    }

    // parked-trailer lamp pin: the lamp update reads the SOURCE vehicle's
    // +0x2A0 mask through [[veh+0x110]+0x18], and the propagator keeps
    // repointing a parked trailer's ctx at the chain head - so it mirrors
    // the truck's brake lights forever. Pin its own mask byte at 0 AND
    // repoint its source at itself: it reads its own (zeroed) mask and goes
    // natively dark, while the live set keeps the real feed. The native
    // attach flow rewrites the source on recouple. Non-live is the test
    // (not joint==null): a trailer bolted to a parked parent is parked too
    // the lamp update reads the SOURCE vehicle's +0x2A0 through [ctx+0x18]
    // for the mask channels, but most channels (reverse, blinkers, aux)
    // read scalar fields straight off the light ctx itself - every offset
    // below was verified against the channel bodies in the exe lamp update
    // (bytes/words/floats only, no pointers)

    // parked trailers keep their slave links for visibility, so the game
    // still registers/feeds straggler lamp components inside a live
    // vehicle's ctx buckets. The comps carry no reliable trailer
    // back-pointer, so ownership is proven the other way: a comp is a parked
    // trailer's if the trailer's own fields reference it. Matched entries
    // are pushed lamp-off through their own +0xD8 vfunc and unlinked - the
    // native attach re-registers them on recouple


    void disconnect_now( void* trailer, void* parent, const bool parent_is_truck )
    {
        // a trailer sitting on the truck (or straight on the fifth wheel)
        // gets the NATIVE T-detach: the dispatcher drives the whole proper
        // sequence - joint release, parked-state record, traffic obstacle,
        // electrical cut - producing a REAL parked trailer instead of a
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
                // the parked machine hid it the moment the joint died - the
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
                // reescreve as claims já na topologia pós-detach - senão a
                // seção solta fica inalcançável até o próximo frame e a
                // máquina de visibilidade pisca ela
                rebuild_parked_claim_links();
                traffic_detach_done.store( true, std::memory_order_release );
                return;
            }
            CCore::g_instance->info( "disconnect {:p}: native detach declined - manual fallback", trailer );
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
        // the brace_model_anim node - proven silent in testing)
        play_brace_sound( trailer, false );
        vcall< prism::physics_trailer_u_set_trailer_brace_fn >( trailer, prism::offsets::vfunc_set_brace, true );

        // the native detach pushes the parked trailer's position into its
        // traffic-obstacle anchor BEFORE the joint drops: actor slot at
        // game_actor+0x1D38 -> provider +0x1F0 -> anchor update. Skipping it
        // leaves the lane modifier owner stale and the traffic sweep hits it
        // whenever the trailer sits on a road. Unlike the head trailer - which
        // parks right under the truck - a mid-chain detach can sit anywhere,
        // so the trailer's own transform is the one that goes to the anchor
        // the actor->provider->anchor path is head-trailer-only in the native
        // code; the parked vfunc +0x328 below already registers the detached
        // trailer as a traffic obstacle, and pushing a mid-chain trailer's
        // transform through the head trailer's anchor corrupts it (AV in this
        // plugin). Intentionally left out.
        // hide the slave chain for the call: the disconnect walker tears
        // down every member it reaches and faults on a stale interior link
        // at exe+0x788CB7 - every fault resets canon, which disables every
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


        // tell every component on both vehicles the coupling is gone - the
        // native detach notifies the vehicle component list (+0xE98) on both
        // sides; a loaded trailer's cargo constraints corrupt the physics
        // sweep if they are never told. A section head only broke its FRONT
        // edge though - its rear connector to the slave is still a live
        // coupling, so the trailer-level "you are decoupled" notifications
        // (component notify, decoupled flag, connector unpair) would tear
        // the T1->T2 pair down too - exactly the missing hoses the user
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
        // it. The whole detached section is marked, not just the head - a slave
        // that stays jointed behind it (dolly + rear trailer) moves as a rigid
        // unit, and an unmarked member would be teleported to a dock slot and
        // drag the section back through its live joint
        for ( void* t = trailer; t != nullptr; )
        {
            mark_detached( t );
            mark_plugin_detached( t );
            // keep walking only while the slave is still jointed - i.e. part of
            // this rigid section. A slave link to an already-parked trailer is
            // not followed, or its own mark would be lifted on re-attach
            void* next = prism::offsets::field< void* >( t, prism::offsets::trailer_slave );
            t = ( next != t && next != nullptr && joint( next ) != nullptr ) ? next : nullptr;
        }
        // slot-level mark too: pointer marks die when a cargo op destroys the
        // object - the canon index is what lets the restore spot the fresh
        // jointed replacement and put it back
        const int di = canon_index_of( trailer );
        if ( di >= 0 ) detached_idx[ di ] = true;

        // the trailer's own line entities die with the joint: set_lines is the
        // native teardown the detach body calls on the now-free piece -
        // without it the hoses on the detached trailer stay frozen visible,
        // drawn between the trailer and nothing
        if ( set_lines_fn != nullptr )
        {
            set_lines_fn( trailer, 0, 1 );
        }

        // rest of the native detach sequence: flag the trailer as parked, drop the
        // parent's wheel load, and rebuild the lines so the hoses between the
        // pair come down
        if ( prism::offsets::field< void* >( trailer, prism::offsets::trailer_parked_state ) != nullptr )
        {
            prism::offsets::field< uint8_t >( trailer, prism::offsets::trailer_parked ) = 1;
        }
        // native detach calls trailer vfunc +0x328(trailer, true) - the real
        // "set parked" that registers the detached trailer as a traffic
        // obstacle with a live owner. Setting the flag alone leaves the
        // obstacle unregistered and the traffic sweep hits a dead entry
        if ( plausible_vfunc( trailer, 0x328 / 8 ) )
        {
            vcall< prism::physics_trailer_u_set_trailer_brace_fn >( trailer, 0x328 / 8, true );
        }
        // the game applies spring brakes the same way on a jointless trailer -
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
            // native detach fires coupling events at every accessory on the
            // trailer (cargo, traffic-obstacle anchor) - without it the parked
            // trailer registers a lane modifier with a null owner and the
            // traffic sweep crashes on it near roads/docks
            if ( refresh_accessories_fn != nullptr )
            {
                refresh_accessories_fn( parent, trailer );
            }

            // native detach tail: notify the traffic subsystem, re-link the
            // type-2 obstacle anchors (rebinds the parked trailer's lane
            // modifier to its new owner), then the post notify
            traffic_detach_tail();
        }
        // native detach also unpairs the two vehicles' connectors and
        // re-applies the cables cvar with a null trailer (cables drop to the
        // remaining set). The truck clears its coupling progress scalar too.
        // unpairs the connector LISTS of the pair - on a section head that
        // reaches the still-paired rear connector too, so skip it when a
        // live section is behind: the broken edge's hoses are already gone
        // with the front's outgoing-lines clear
        if ( parent != nullptr && pair_vehicles_fn != nullptr && !section_behind )
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
                // the towed registration - leave them pinned and the game
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
        // the native detach lowers the landing gear with its crank anim -
        // the replica plays the sound already; the anim drives the physical
        // gear state on models that have the brace_model_anim node. Rewind
        // the progress field first (+0x1270) so the anim player actually runs
        // the wind-down instead of snapping
        if ( brace_anim_fn != nullptr )
        {
            prism::offsets::field< float >( trailer, 0x1270 ) = 1.f;
            brace_anim_fn( trailer, 1, 1.f / 3.f );
        }
        // give the freshly detached trailer the NATIVE parked registration -
        // the dispatcher gives it a real parked-state record like the head
        // gets on a T-detach, so the trailer is never left in limbo (limbo
        // pieces vanish when the head's dispatcher sweeps)
        if ( joint( trailer ) == nullptr )
        {
            native_detach_via_slot( trailer );
        }
        // the parked machine hides a trailer it can no longer reach through
        // the live chain - re-assert it before the frame ends
        unhide_detached();
        // reescreve as claims já na topologia pós-detach - senão a seção
        // solta fica inalcançável até o próximo frame e a máquina de
        // visibilidade pisca ela
        rebuild_parked_claim_links();
        // joint is dead now - a detach genuinely completed, so a later "whole
        // set coupled" state means the user re-assembled it and traffic may
        // be restored
        traffic_detach_done.store( true, std::memory_order_release );
    }

    void connect_now( void* trailer, void* parent, const bool parent_is_truck )
    {

        // re-check hook proximity at execution time: the trailer may have been
        // nudged between queueing and this sim tick - never teleport it
        if ( hook_far( trailer, parent, parent_is_truck ) )
        {
            CCore::g_instance->warning( "Carreta muito longe - aproxime dando ré para engatar" );
            return;
        }

        // a trailer still bolted to a parked parent would take a second live
        // joint - two constraints pull it different ways and the solver
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
        // sequence through the trailer-of-interest slot - settle, joint,
        // towed registration, connector pairing, cables, anchors - native
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

        // the trailer is coming back under a live joint by explicit request -
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
        // mesma proteção do run_native_connect: o join anda trailer_slave
        // por recursão e jointa o que encontrar - uma claim estacionada
        // viva vira membro fantasma: perde a trava de estacionada, a âncora
        // desarma (joint != nullptr) e ela afunda sem proteção
        sever_parked_claims();
        native_chain_call_active.store( true, std::memory_order_release );
        vcall< prism::physics_trailer_u_connect_fn >( trailer, prism::offsets::vfunc_connect,
            parent, vec, 0, parent_is_truck || parent_coupling != 0, slave_hook );
        native_chain_call_active.store( false, std::memory_order_release );
        restore_parked_claims();

        // the +0xC8 visibility walk only reaches a trailer through slave
        // links: the native chain writes the claim during its own connect,
        // the replica must do it too or a trailer-parent attach leaves the
        // piece unreachable and the parked machine hides it. Never clobber a
        // jointed slave - hk_connect_slave refuses double-joining anyway
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
        // The truck-side bookkeeping (towed pointer, wheel load) is truck-only - the
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
        // to "coupled" and forces the connector virtuals - the air-hose
        // connect sound the native progression plays. r8=1 forces the notify
        // even when the state byte already matches, so the sound always fires
        if ( clear_detached_flag_fn != nullptr )
        {
            clear_detached_flag_fn( parent, 1, 1 );
            clear_detached_flag_fn( trailer, 1, 1 );
        }
        // un-park: native attach only clears the flag byte (when a parked-state
        // record exists) and never nulls the backpointer - the traffic obstacle
        // registration may still reference it, and orphaning it leaves a dead
        // entry the traffic sweep crashes on even after the trailer re-couples
        if ( prism::offsets::field< void* >( trailer, prism::offsets::trailer_parked_state ) != nullptr )
        {
            prism::offsets::field< uint8_t >( trailer, prism::offsets::trailer_parked ) = 0;
        }
        // native attach calls trailer vfunc +0x328(trailer, false) - un-registers
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
        // the piece was torn down when it parked (line entities hidden with
        // state=0) - a rebuilt edge stays invisible until the lists are raised
        if ( set_lines_fn != nullptr )
        {
            set_lines_fn( trailer, 1, 1 );
            if ( !parent_is_truck ) set_lines_fn( parent, 1, 1 );
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
        // pneumatic/electric connectors - without it the trailer keeps its
        // standalone spring brake and stays braked with a live joint - then
        // re-apply the cables cvar and refresh the actor's trailer flags.
        // The native calls pair(tower, towed), which for a chain attach is
        // the parent trailer - the connector pairing is between neighbours
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
        // The second anchor the native updates sits under [game_actor+0xC8] -
        // replicating it kept resolving wrong-typed objects whose vfuncs jumped
        // into space, so it stays out: the truck-side anchor gets refreshed by
        // the game's own per-tick updates anyway
        // no anchor push here either: the only actor slot is the head
        // trailer's, and +0x328 already un-registers the parked obstacle -
        // writing through a foreign anchor corrupts it
        // the T attach raises the landing gear with its crank sound -
        // play the registered brace-up sound event on the trailer
        play_brace_sound( trailer, true );
        if ( brace_anim_fn != nullptr )
        {
            brace_anim_fn( trailer, 0, 1.f / 3.f );
        }
        // pieces that stayed jointed behind the reattached head kept their
        // parked half-state - re-assert them too
        unhide_detached();
        dump_chain( "post-connect" );
    }

    // puts back what a cargo load/unload respawn destroyed: every marked
    // chain slot whose fresh object came back jointed is disconnected again
    // and teleported to the transform it was parked at. Runs on the sim
    // thread (queued op), descending index order so a tail trailer parked
    // on its own leaves before the section it was taken with - each marked
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
                // the op bodies run native flows start to finish - a fault
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
                    CCore::g_instance->warning( "pending op faulted - canon {}",
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

    // Cauda de notificação do tráfego que o detach nativo roda depois de
    // soltar uma peça: notify 0x138 no sub-manager, re-link das âncoras
    // tipo-2 (re-liga o lane modifier da carreta estacionada ao novo dono)
    // e notify 0x1E0 final. Sem isso a carreta registra um obstáculo com
    // dono nulo e a varredura do tráfego quebra nele perto de estradas.
    void traffic_detach_tail()
    {
        void* mgr = traffic_manager_global != nullptr ? *traffic_manager_global : nullptr;
        void* sub = mgr != nullptr ? prism::offsets::field< void* >( mgr, 0x2B30 ) : nullptr;
        if ( sub != nullptr ) vcall< prism::vehicle_notify_decoupled_fn >( sub, 0x138 / 8 );
        if ( relink_anchors_fn != nullptr )
        {
            void* game_actor = CCore::g_instance->get_game_actor();
            void* reg = game_actor != nullptr ? prism::offsets::field< void* >( game_actor, 0x30 ) : nullptr;
            if ( reg != nullptr && readable_ptr( reinterpret_cast< uint8_t* >( reg ) + 0xD8 ) )
            {
                relink_anchors_fn( reinterpret_cast< uint8_t* >( reg ) + 0xD8, 2 );
            }
        }
        if ( sub != nullptr ) vcall< prism::vehicle_notify_decoupled_fn >( sub, 0x1E0 / 8 );
    }

    combo_hook_pos_fn* combo_hook_pos_fn_ptr = nullptr;

    // the game calls this to auto-attach a parent's slave trailer; refuse
    // every claim that isn't a real coupling: a detached slave, a slave
    // whose mother has no physics joint (a parked trailer must never pull
    // its neighbours through the parked machine), an already-jointed slave
    // (stale claim), a backward canon claim (leaf links point forward only),
    // or a jointless piece farther than the attach gap - a real coupling
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
            // coupling - refuse it or the head's attach teleports the
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
    // linked at node+0x68, so a cyclic body graph - the visibility leaf plus
    // the parked machine's canonical back-claim landing between maintenance
    // passes - recurses until the stack overflows (0xC00000FD). Track the
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
                        "joint walk: cyclic node {:p} - skipped subtree", self );
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
    // iterates node+0x60 with NO cycle guard - a cyclic pair hangs it
    // forever (the detach freeze: the visibility leaf and the canonical
    // back-claim both physicalized, two nodes each other's parent). A link
    // that would close a cycle can never be legitimate - refuse just that
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

    // vfunc_disconnect implementation (vfunc 99, exe+0x789C20 - shared by
    // every trailer-class vtable): releases the joint, feeds the brake
    // channel, then walks trailer_slave writing the +0x2A0 lamp mask per
    // member. Every caller lands here - the T dispatcher, the queued
    // section teardown on the sim thread, our own vcall - so this is the
    // one point that can sever a link that is not a real departing member:
    // a jointless trailer behind the section is a parked claim we keep for
    // visibility, and a recycled trailer slot reads as a bogus pointer -
    // walk the slave chain a native dispatcher is about to traverse: repair
    // every member's lamp-cut src (not only canon - parked claims chain
    // non-canon pieces too) and sever the first link that is not a trailer
    // object. The dispatcher's internal disconnect bypasses the vtable hook
    // and walks whatever the links point at - a recycled slot (observed as a
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
    // o teardown também pode andar o grafo físico (+0x60 sobe pro pai,
    // +0x68 desce pro filho): um membro alcançado por aí com o lamp-src
    // reciclado - ou uma aresta apontando pra slot morto - mata o walk em
    // exe+0x789CB7 ANTES de limpar +0x1080, e a carreta fica meio-desatrelada
    // em limbo (piscada + balanço). Saneia os dois sentidos: conserta o ctx
    // das carretas e corta arestas pra objetos que não são veículo nenhum
    void sanitize_phys_walk( void* trailer )
    {
        if ( trailer == nullptr || !readable_ptr( trailer ) ) return;
        void* truck = nullptr;
        if ( auto* game_actor = CCore::g_instance->get_game_actor() )
        {
            truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
        }
        for ( int dir = 0; dir < 2; ++dir )
        {
            const uint32_t edge = dir == 0 ? 0x60 : 0x68;
            uint32_t pd = 0;
            for ( void* cur = trailer;
                  cur != nullptr && readable_ptr( cur ) && pd++ < max_trailers * 4; )
            {
                if ( is_trailer_obj( cur ) ) repair_lamp_ctx( cur );
                void* next = prism::offsets::field< void* >( cur, edge );
                if ( next == nullptr || next == cur ) break;
                if ( !is_trailer_obj( next ) && next != truck &&
                     !plausible_vfunc( next, 99 ) )
                {
                    break;
                }
                cur = next;
            }
        }
    }
    void __fastcall hk_trailer_disconnect( void* trailer )
    {
        if ( trailer == nullptr || !is_trailer_obj( trailer ) ) return;
        // the walker's lamp cut writes [src+0x2A0] where src comes out of each
        // member's light ctx ([m+0x110]+0x18) - a src pointing at a recycled
        // vehicle slot kills the teardown mid-write on a foreign page
        // (exe+0x788CB7, rcx = a system-DLL address). Repoint a dead src at
        // the member itself - the same self-source the lamp pin uses on
        // parked pieces - so the write lands on a live object
        const auto repair_ctx = []( void* m ) { repair_lamp_ctx( m ); };
        repair_ctx( trailer );
        // during an actor-teleport reconcile the game may try to disconnect a
        // member mid-remount - the native member walk is what faults and its
        // failure resets canon, leaving every gate dead for 90 frames (the
        // F9 dive: truck through the floor, trailers launched). Minimal
        // release instead: joint + parent cleared, members left untouched -
        // the reconcile owns the remount
        if ( tp_cooldown > 0 )
        {
            prism::offsets::field< void* >( trailer, prism::offsets::trailer_physics_joint ) = nullptr;
            prism::offsets::field< void* >( trailer, prism::offsets::trailer_parent ) = nullptr;
            return;
        }
        // snapshot the validated members - the fault repair below uses it to
        // keep the pieces behind the head reachable instead of orphaning them
        void* chain_members[ max_trailers * 2 ] = {};
        uint32_t chain_n = 0;
        // a cyclic chain (leaf claim + real link both physicalized) would
        // walk forever - a real section is never deeper than the canon count
        uint32_t depth = 0;
        for ( void* t = trailer; t != nullptr && readable_ptr( t ); )
        {
            void* slave = prism::offsets::field< void* >( t, prism::offsets::trailer_slave );
            if ( slave == nullptr ) break;
            // sever EVERY jointless slave edge - a live section member always
            // carries a live joint, so a jointless target is a visibility
            // claim (ours OR the parked machine's canonical back-claim).
            // Gating on claim_edge_ours let the native walker step into a
            // game-written claim and die at exe+0x789CB7 - the recurring
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
        sanitize_phys_walk( trailer );
        native_chain_call_active.store( true, std::memory_order_release );
        __try
        {
            trailer_disconnect_hook->get_original< prism::physics_trailer_u_disconnect_fn >()( trailer );
        }
        __except ( EXCEPTION_EXECUTE_HANDLER )
        {
            // the fault is a teardown-internal write on one member - the
            // trailer objects themselves stay alive. Resetting canon here
            // re-enumerates it in walk order: a piece the sweep already hid
            // never comes back (the vanishing T0) and the UI buttons get
            // relabelled onto different physical trailers. Only a dead canon
            // (world unload) justifies wiping it
            const bool canon_alive = !canon_dead();
            CCore::g_instance->warning( "disconnect walk faulted on {:p} - canon {}", trailer,
                                        canon_alive ? "kept" : "reset" );
            if ( !canon_alive ) reset_trailer_state();
            if ( readable_ptr( trailer ) )
            {
                // keep the members behind the head reachable - the parked
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

    // claims estacionadas cortadas por sever_parked_claims - restauradas no
    // fim da MESMA chamada nativa. Antes a manutenção só religava no frame
    // seguinte, deixando a peça inalcançável um frame inteiro: a máquina de
    // visibilidade a escondia e re-registrava (a piscada no engate) e o
    // registro da peça ligava/desligava o suporte físico (as tremidas)
    struct severed_edge_t { void* parent; void* child; };
    severed_edge_t severed_edges[ max_trailers ];
    int severed_edge_count = 0;

    // vfunc_connect impls (vfunc 98 - exe+0x639090 / exe+0x7890A0, shared by
    // the trailer-class vtables): the native join processes the slave chain
    // through SELF-RECURSIVE walkers (exe+0x6390C0 / exe+0x639220 call
    // themselves on trailer_slave) - a parked visibility claim pointing at
    // a limbo trailer (no record/actor) aborts it mid-join, and a cyclic
    // chain overflows the stack - both die with no crash dump. Hide every
    // slave edge whose target is jointless for the duration of the call:
    // claims are our fiction, the maintenance rebuilds them next frame,
    // and the original then sees only real section members
    void sever_parked_claims()
    {
        severed_edge_count = 0;
        // not while the world assembles: trailers spawn before their joints
        // form, so every staged link reads jointless - cutting them mid-
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
            // a jointless slave link is ALWAYS a visibility claim - ours or
            // the game's own parked-machine back-claim (which our registry
            // never tagged, so claim_edge_ours misses it). A real member of
            // a departing section is always jointed, so cutting every
            // jointless edge for the duration of a native walk is safe - and
            // missing the game's back-claims is what let the dispatcher
            // wander into the parked set and destroy trailers 1/2 on the
            // head detach
            if ( joint( slave ) == nullptr )
            {
                prism::offsets::field< void* >( t, prism::offsets::trailer_slave ) = nullptr;
                clear_claim_edge( t );
                if ( severed_edge_count < static_cast< int >( max_trailers ) )
                    severed_edges[ severed_edge_count++ ] = { t, slave };
            }
        }
    }

    // religa as claims cortadas - roda no fim de cada chamada nativa. Só
    // religa se o join não escreveu nada no lugar (uma aresta real criada
    // pelo attach vence) e a peça continua sem junta
    void restore_parked_claims()
    {
        for ( int i = 0; i < severed_edge_count; ++i )
        {
            void* p = severed_edges[ i ].parent;
            void* c = severed_edges[ i ].child;
            if ( readable_ptr( p ) && readable_ptr( c ) && joint( c ) == nullptr &&
                 prism::offsets::field< void* >( p, prism::offsets::trailer_slave ) == nullptr )
            {
                prism::offsets::field< void* >( p, prism::offsets::trailer_slave ) = c;
                set_claim_edge( p, c );
            }
        }
        severed_edge_count = 0;
    }
    // Corpo comum dos dois hooks de connect - duas vtables de classes
    // diferentes de carreta chegam aqui, e cada uma precisa chamar o original
    // do seu próprio hook. A cadeia parked é cortada antes da chamada porque
    // o join nativo anda trailer_slave por recursão e aborta (ou entra em
    // loop infinito) se encontrar um link nosso no meio.
    void run_native_connect( void* trailer, void* parent, float3_t vec,
                             int state, bool slave_hook, bool is_slave,
                             prism::physics_trailer_u_connect_fn* orig )
    {
        if ( trailer == nullptr || !is_trailer_obj( trailer ) || orig == nullptr ) return;
        native_chain_call_active.store( true, std::memory_order_release );
        sever_parked_claims();
        __try
        {
            orig( trailer, parent, vec, state, slave_hook, is_slave );
        }
        __except ( EXCEPTION_EXECUTE_HANDLER )
        {
            // mesma regra do fault de disconnect: os objetos da carreta
            // sobrevivem a uma falha do walk - só canon morto justifica
            // zerar o estado (senão os botões trocam de carreta)
            const bool canon_alive = !canon_dead();
            CCore::g_instance->warning( "connect walk faulted on {:p} - canon {}", trailer,
                                        canon_alive ? "kept" : "reset" );
            if ( readable_ptr( trailer ) )
                prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = nullptr;
            clear_claim_edge( trailer );
            if ( !canon_alive ) reset_trailer_state();
        }
        restore_parked_claims();
        native_chain_call_active.store( false, std::memory_order_release );
    }
    void __fastcall hk_trailer_connect_a( void* trailer, void* parent, float3_t vec,
                                          int state, bool slave_hook, bool is_slave )
    {
        run_native_connect( trailer, parent, vec, state, slave_hook, is_slave,
            trailer_connect_hook->get_original< prism::physics_trailer_u_connect_fn >() );
    }
    void __fastcall hk_trailer_connect_b( void* trailer, void* parent, float3_t vec,
                                          int state, bool slave_hook, bool is_slave )
    {
        run_native_connect( trailer, parent, vec, state, slave_hook, is_slave,
            trailer_connect_hook2->get_original< prism::physics_trailer_u_connect_fn >() );
    }

    // the trailer-side joint release the native detach runs. The game's own
    // teardown also calls it on trailers whose physics joint is already gone
    // - it dereferences trailer+0xFE0->vtable without a check and dies on a
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
    // trailer the very next frame because it still sits under the hook - the
    // same problem plugin detaches needed the detached registry for. Mark the
    // head trailer after every native detach that actually killed its joint
    using native_detach_dispatch_fn = void __fastcall( void* /*game_actor*/, uint8_t /*dl*/, uint64_t /*r8*/ );
    void* __fastcall hk_trailer_dtor( void* self, uint64_t flags );
    void __fastcall hk_detach_dispatch_raw( void* self, uint8_t dl, uint64_t r8 );
    void __fastcall hk_detach_dispatch( void* self, uint8_t dl, uint64_t r8 )
    {
        // only the player's T key (caller exe+0x9F7DAE, seen in the logs) is
        // bound by the handbrake rule - the game's own detach (delivery,
        // world teardown on load/quit, caller exe+0x5DBED6) must always run
        // or the old world is torn down half-way
        const uint64_t caller = reinterpret_cast< uint64_t >( _ReturnAddress() ) -
            reinterpret_cast< uint64_t >( GetModuleHandle( nullptr ) );
        const bool allowed = caller != 0x9F7DAE || canon_age < 90 || handbrake_ok();
        CCore::g_instance->info( "detach dispatch from exe+0x{:X}: handbrake {} -> {}",
            caller,
            parking_brake_on.load( std::memory_order_relaxed ) ? "on" : "off",
            allowed ? "allowed" : "REFUSED" );
        if ( !allowed )
        {
            CCore::g_instance->warning( "Puxe o freio de mão para desengatar" );
            return;
        }
        // a fault on a respawned-away trailer (workshop/garage rebuilds kill
        // canon pointers mid-frame) must never reach the game - reset and
        // let the next pass rebuild from live state
        __try { hk_detach_dispatch_raw( self, dl, r8 ); }
        __except ( EXCEPTION_EXECUTE_HANDLER )
        {
            const bool canon_alive = !canon_dead();
            CCore::g_instance->warning( "detach dispatch faulted - canon {}",
                                        canon_alive ? "kept" : "reset" );
            if ( !canon_alive ) reset_trailer_state();
        }
    }

    void __fastcall hk_detach_dispatch_raw( void* self, uint8_t dl, uint64_t r8 )
    {
        // a save load frees every trailer we cached - wipe before any mark
        // touches a dead pointer
        if ( canon_dead() ) reset_trailer_state();
        // the trailer-of-interest slot may have drifted onto a nearby parked
        // trailer (proximity resolver, our own re-root). While the truck tows
        // something the ONLY sane detach candidate is the towed trailer -
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
        // canonical claim chain visible - parked pieces sit at the tail of
        // the departing head's slave chain and the dispatcher's parked-
        // section registration keeps them. Cutting the link makes them
        // unreachable exactly when the sweep runs (vanishing 1/2)
        repair_lamp_ctx_all();
        // the dispatcher's internal disconnect walks the chain directly -
        // every link must be a live trailer or the teardown faults on it
        sanitize_slave_walk( prism::offsets::field< void* >( self, prism::offsets::game_actor_trailer ) );
        if ( towed != nullptr ) sanitize_slave_walk( towed );
        // idem pro grafo físico - o dispatcher anda ele internamente e uma
        // aresta pra slot morto mata o teardown em exe+0x789CB7
        sanitize_phys_walk( prism::offsets::field< void* >( self, prism::offsets::game_actor_trailer ) );
        if ( towed != nullptr ) sanitize_phys_walk( towed );
        dump_chain( "pre-detach(T)" );
        native_chain_call_active.store( true, std::memory_order_release );
        detach_dispatch_hook->get_original< native_detach_dispatch_fn >()( self, dl, r8 );
        native_chain_call_active.store( false, std::memory_order_release );
        dump_chain( "post-detach(T)" );
        // the native detach cascade re-raises the line entities of pieces
        // behind the released head - the once-per-mark teardown of the loose
        // pieces must run again or their old hoses hang in the air (T3's
        // cable still tied to T2 from 100 m away)
        for ( auto& torn : lines_torn ) torn = false;
        if ( self == nullptr ) return;
        void* t = prism::offsets::field< void* >( self, prism::offsets::game_actor_trailer );
        if ( t != nullptr && joint( t ) == nullptr )
        {
            pending_native_detach = nullptr;
            mark_detached( t );
            // cut the truck<->head hose pair the dispatcher can leave half-
            // registered when the tow slot was re-pinned by maintenance -
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
            // NO slave re-link: parked trailers are independent pieces - each
            // stays visible and OFF through its own parked record. The slave
            // link between parked pieces only draws phantom hoses and makes
            // the parked machine treat them as one departing-prone section
        }
        else if ( t != nullptr )
        {
            // joint still alive after dispatch = the async crank animation
            // runs before the drop - the lines refresh must not re-draw the
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
        // reescreve as claims já na topologia pós-detach - senão a seção
        // solta fica inalcançável até o próximo frame e a máquina de
        // visibilidade pisca ela
        rebuild_parked_claim_links();
    }

    // the native attach dispatcher (0x1405DCC20) - every native re-attach
    // funnels through it: the T key, the cargo load/unload set reassembly,
    // garage transfers, save-load restores. A trailer the plugin detached
    // is refused: wherever the player left it, it stays - the load/unload
    // sequence may not pull it back onto the fifth wheel or the chain.
    // Natively detached trailers are unaffected: T re-attach still works
    // set by the wrapper: the game itself (not the player's T key) called the attach
    bool attach_from_game = false;
    void __fastcall hk_attach_dispatch_raw( void* self, uint8_t dl, uint64_t r8, void* settle_override );
    void __fastcall hk_attach_dispatch( void* self, uint8_t dl, uint64_t r8, void* settle_override )
    {
        // the delivery/dock sequence never needs an attach - a game-driven
        // re-attach there is what yanks the parked pieces around
        // the handbrake rule binds the player's T key only (caller
        // exe+0x9F7DD6); the game's own attach (world assembly, caller
        // exe+0x5DC731) is left alone - except inside the cargo window
        const uint64_t caller = reinterpret_cast< uint64_t >( _ReturnAddress() ) -
            reinterpret_cast< uint64_t >( GetModuleHandle( nullptr ) );
        const bool allowed = !in_cargo_window() &&
            ( caller != 0x9F7DD6 || canon_age < 90 || handbrake_ok( false ) );
        CCore::g_instance->info( "attach dispatch from exe+0x{:X}: handbrake {} -> {}",
            caller,
            parking_brake_on.load( std::memory_order_relaxed ) ? "on" : "off",
            allowed ? "allowed" : "REFUSED" );
        if ( !allowed )
        {
            CCore::g_instance->warning( "Puxe o freio de mão para engatar" );
            return;
        }
        attach_from_game = caller != 0x9F7DD6;
        __try { hk_attach_dispatch_raw( self, dl, r8, settle_override ); }
        __except ( EXCEPTION_EXECUTE_HANDLER )
        {
            const bool canon_alive = !canon_dead();
            CCore::g_instance->warning( "attach dispatch faulted - canon {}",
                                        canon_alive ? "kept" : "reset" );
            if ( !canon_alive ) reset_trailer_state();
        }
        for ( auto& torn : lines_torn ) torn = false;
    }

    void __fastcall hk_attach_dispatch_raw( void* self, uint8_t dl, uint64_t r8, void* settle_override )
    {
        if ( canon_dead() ) reset_trailer_state();
        // the user's chosen attach target beats the game's proximity pick -
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
        // actually aiming at it - released here, right before the native
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
            // - the intent is explicit, release the marks and attach
            void* truck = readable_ptr( self )
                ? prism::offsets::field< void* >( self, prism::offsets::game_actor_truck ) : nullptr;
            hook_xform_t hxf{};
            const bool under_hook = truck != nullptr && hook_world( head, hxf ) &&
                hook_gap( head, truck, true ) < attach_gap_limit;
            // checkbox off = the game's own reassembly (teleport to the trailer,
            // garage, cargo) works like stock: it may couple a parked piece
            const bool free_mode = !settings::g_settings.cargo_restore && attach_from_game;
            if ( !under_hook && !free_mode )
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
        // attach - refuse the call instead
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
        // parked piece behind it (mid-chain trailers never get a record -
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
        // o dispatcher registra a cadeia slave INTEIRA - corta todas as
        // claims estacionadas além da do head, senão uma peça mais fundo é
        // jointada pela metade e afunda sem trava
        sever_parked_claims();
        // the dispatcher walks the chain for registration - every link must
        // be a live trailer object or the internal teardown faults on it
        sanitize_slave_walk( head );
        native_chain_call_active.store( true, std::memory_order_release );
        attach_dispatch_hook->get_original< native_attach_dispatch_fn >()( self, dl, r8, settle_override );
        native_chain_call_active.store( false, std::memory_order_release );
        if ( severed_slave != nullptr && readable_ptr( head ) )
        {
            prism::offsets::field< void* >( head, prism::offsets::trailer_slave ) = severed_slave;
        }
        restore_parked_claims();
        if ( released_marks && joint( head ) == nullptr )
        {
            // the dispatcher ran but nothing coupled - re-protect the
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
        // restore): clear its marks - the chain links are rebuilt every
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
    // reconciles its joint through the attach/detach dispatchers - the path
    // the cargo sequence uses to teleport the set into dock slots. A
    // plugin-detached trailer keeps its current record untouched: it stays
    // parked where the player left it
    using trailer_state_apply_fn = void __fastcall( void* /*record*/ );

    // the per-trailer teleport (0x140786320): the actor-teleport walks the
    // trailer list and calls this on each one to move it into its dock slot.
    // Trailer objects are NOT recreated - this is the call that actually
    // drags a detached trailer to the dock during cargo load/unload, where
    // it lands under its parent's hook and gets re-jointed. A
    // plugin-detached trailer is a world object: it is never teleported,
    // by any caller - dock alignment, garage, ferry, job start
    using trailer_teleport_fn = void __fastcall( void* /*trailer*/, void* /*transform*/ );

    // the actor teleport (0x1405DE1E0): rotates/moves truck + attached set
    // into the dock slots for the cargo cinematic - the same call travel
    // teleports use. Snapshot the rig on the first call of a sequence, then
    // arm the restore only when the move was short (a dock shuffle, not a
    // ferry/garage/job hop across the map)
    using actor_teleport_fn = void __fastcall( void* /*actor*/, void* /*arg*/, uint8_t, uint8_t );

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
        get_cvar_float_fn = reinterpret_cast< prism::cvar_get_float_fn* >( memory::get_address_from_offset( 0x1401DB560 ) );
        g_traffic_cvar = reinterpret_cast< void* >( memory::get_address_from_offset( 0x142D45500 ) );
        traffic_manager_global = reinterpret_cast< void** >( memory::get_address_from_offset( 0x1436AF7C8 ) );
        relink_anchors_fn = reinterpret_cast< anchor_relink_fn* >( memory::get_address_from_offset( 0x140623C90 ) );
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
        // the line-state setter - refuse a hide aimed at a trailer whose
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
        // every native re-join/reposition of a trailer goes through - hooked
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
        // chain-end trailer, jointless ones included - redirect inside the
        // setter so the native flow anchors the last JOINTED trailer
        cam_set_target_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "cam_set_target", memory::get_address_from_offset( 0x14063C2D0 ),
            reinterpret_cast< uint64_t >( &hk_cam_set_target ) );
        if ( cam_set_target_hook != nullptr )
        {
            cam_set_target_hook->hook();
        }
        // lane-obstacle insert: dead array entries (owner-node cleared without
        // removal) crash the traffic worker - sanitize before the original
        lane_obstacle_insert_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "lane_obstacle_insert", memory::get_address_from_offset( 0x14094DFC0 ),
            reinterpret_cast< uint64_t >( &hk_lane_obstacle_insert ) );
        if ( lane_obstacle_insert_hook != nullptr )
        {
            lane_obstacle_insert_hook->hook();
        }
        // chain-length accumulator: the reimplementation stops the walk at
        // the first jointless (detached) trailer so every consumer - camera 2
        // included - measures only the live combination. The per-link hook
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
        // through pure vfunc slots - swallow every one of them, process-wide
        init_purecall_fake();
        purecall_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "purecall", memory::get_address_from_offset( 0x141D23610 ),
            reinterpret_cast< uint64_t >( &hk_purecall ) );
        if ( purecall_hook != nullptr )
        {
            purecall_hook->hook();
            install_crash_tracer();
        }
        // trailer vfunc_disconnect impl - the single funnel every teardown
        // (T dispatcher, sim-thread section teardown, our vcall) passes
        // through; the detour sanitizes the slave chain it is about to walk
        trailer_disconnect_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "trailer_disconnect", memory::get_address_from_offset( 0x140789C20 ),
            reinterpret_cast< uint64_t >( &hk_trailer_disconnect ) );
        if ( trailer_disconnect_hook != nullptr )
        {
            trailer_disconnect_hook->hook();
        }
        // vfunc_connect has TWO impls across the trailer-class vtables -
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
        trailer_dtor_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "trailer_dtor", memory::get_address_from_offset( 0x1406359E0 ),
            reinterpret_cast< uint64_t >( &hk_trailer_dtor ) );
        if ( trailer_dtor_hook != nullptr )
        {
            trailer_dtor_hook->hook();
        }
        // the propagator's raw +0x2A0 stores bypass the mask setter - the
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
        // via a self-recursive vfunc - a cyclic graph overflows the stack
        // instantly (0xC00000FD). The detour returns early on a revisit
        joint_walk_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "joint_walk", memory::get_address_from_offset( 0x1409555F0 ),
            reinterpret_cast< uint64_t >( &hk_joint_walk ) );
        if ( joint_walk_hook != nullptr )
        {
            joint_walk_hook->hook();
        }
        // bidirectional joint link - refuses only the edge that would
        // close a +0x60/+0x68 cycle (find_root has no guard and hangs)
        phys_link_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "phys_link", memory::get_address_from_offset( 0x140919680 ),
            reinterpret_cast< uint64_t >( &hk_phys_link ) );
        if ( phys_link_hook != nullptr )
        {
            phys_link_hook->hook();
        }
        // chase-camera update (vfunc 32): the camera's own chain walk follows
        // trailer_slave without checking joints - hide live->dead links for
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
    // trailer deleting destructor (0x1406359E0, vtable slot 0): the game
    // destroys the job trailer when the job is dismissed/finished. Its base
    // destructor follows trailer_slave and treats whatever it finds as a real
    // coupled member - our visibility claims (jointless slave links between
    // parked pieces) make it run teardown on pieces that were never coupled
    // and it jumps through a bad vtable (log: AV with RIP in the heap, stack
    // exe+0x6359F4 -> exe+0x7845E5). Cut every claim that touches the dying
    // trailer before the original runs; maintenance rebuilds the rest
    using trailer_dtor_fn = void* __fastcall( void* /*self*/, uint64_t /*flags*/ );
    void* __fastcall hk_trailer_dtor( void* self, uint64_t flags )
    {
        if ( self != nullptr && canon_age >= 90 && canon_index_of( self ) >= 0 && is_trailer_obj( self ) )
        {
            int cut = 0;
            for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
            {
                void* c = canon[ i ];
                if ( c == self || !readable_ptr( c ) || !is_trailer_obj( c ) ) continue;
                if ( prism::offsets::field< void* >( c, prism::offsets::trailer_slave ) == self &&
                     joint( self ) == nullptr )
                {
                    prism::offsets::field< void* >( c, prism::offsets::trailer_slave ) = nullptr;
                    clear_claim_edge( c );
                    ++cut;
                }
            }
            void* sl = prism::offsets::field< void* >( self, prism::offsets::trailer_slave );
            if ( sl != nullptr && ( !is_trailer_obj( sl ) || joint( sl ) == nullptr ) )
            {
                prism::offsets::field< void* >( self, prism::offsets::trailer_slave ) = nullptr;
                ++cut;
            }
            // the per-frame sim update calls a vfunc on game_actor+0xC8 - if the
            // chain head still points at the trailer being freed it reads a
            // dead object (log: AV at exe+0x4CFBF8 right after the destructors)
            if ( auto* game_actor = CCore::g_instance->get_game_actor() )
            {
                if ( prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_trailer ) == self )
                {
                    prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_trailer ) = nullptr;
                    ++cut;
                }
            }
            CCore::g_instance->info( "trailer destructor on c{}: {} claim link(s) cut", canon_index_of( self ), cut );
        }
        return trailer_dtor_hook->get_original< trailer_dtor_fn >()( self, flags );
    }

    SCSAPI_VOID on_parking_brake( const scs_string_t, const scs_u32_t, const scs_value_t* const value, const scs_context_t )
    {
        parking_brake_on.store( value != nullptr && value->value_bool.value != 0, std::memory_order_relaxed );
        parking_brake_known.store( value != nullptr, std::memory_order_relaxed );
    }

    void coupling_register_channels()
    {
        const auto* init_params = CCore::g_instance->get_init_params();
        if ( init_params->register_for_channel( SCS_TELEMETRY_TRUCK_CHANNEL_parking_brake,
                SCS_U32_NIL, SCS_VALUE_TYPE_bool, SCS_TELEMETRY_CHANNEL_FLAG_none,
                &on_parking_brake, nullptr ) != SCS_RESULT_ok )
        {
            CCore::g_instance->warning( "Could not register 'truck.brake.parking'" );
        }
    }

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

}
