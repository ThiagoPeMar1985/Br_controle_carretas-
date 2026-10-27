// Cables, hoses, landing gear and lights — line hooks and parked-lamp upkeep.

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

    void update_landing_gear()
    {
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
    }

    // the light-source context a trailer renders lamps through — the native
    // propagator repoints ctx+0x18 to the chain head every tick, so a parked
    // piece must point back at itself or it freezes mirroring truck lamps
    void* light_source_of( void* veh )
    {
        if ( veh == nullptr || !readable_ptr( veh ) ) return nullptr;
        void* ctx = prism::offsets::field< void* >( veh, prism::offsets::trailer_light_ctx );
        if ( ctx == nullptr || !readable_ptr( ctx ) ) return nullptr;
        return prism::offsets::field< void* >( ctx, 0x18 );
    }
    // rebuild one cable/hose edge: rebuild_lines walks the trailer's own
    // slave chain, so the link is nulled for the call or the rebuild draws
    // hoses through the whole parked section behind it
    void rebuild_one_edge( void* front, void* trailer )
    {
        if ( rebuild_lines_fn == nullptr || front == nullptr || trailer == nullptr ) return;
        void* slave = prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave );
        prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = nullptr;
        rebuild_lines_fn( front, trailer );
        prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = slave;
    }
    // same edge rebuild through the ORIGINAL — hk_rebuild_lines refuses
    // fronts outside the live chain on purpose (the game's stray calls), but
    // a trailer still bolted behind a parked head is a real coupling and
    // keeps its hoses: detaching trailer 1 was dropping the 1->2 cables
    // because no path ever redrew that edge
    void rebuild_parked_edge( void* front, void* trailer )
    {
        if ( rebuild_lines_hook == nullptr || front == nullptr || trailer == nullptr ) return;
        void* slave = prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave );
        prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = nullptr;
        rebuild_lines_hook->get_original< prism::vehicle_rebuild_lines_fn >()( front, trailer );
        prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = slave;
        // the entities may live on EITHER side of the edge and the member
        // teardown can also leave them hidden — raise both lists through the
        // native setter (state=1 passes our hook), like the attach does
        if ( set_lines_fn != nullptr )
        {
            set_lines_fn( trailer, 1, 1 );
            set_lines_fn( front, 1, 1 );
        }
        // the cable draw for the pair itself — the live path calls
        // apply_cables on every edge; a parked section edge never gets one
        if ( apply_cables_hook != nullptr )
        {
            apply_cables_hook->get_original< vehicle_cables_fn >()( front, trailer, 0, 2, 0 );
        }
    }
    // rebuild EVERY live cable edge then apply the cables cvar once: the
    // per-edge rebuilds create the line entities, the cvar pass draws them.
    // A dead edge gets (front, nullptr) — the native "nothing attached"
    // clear — instead of a phantom hose into a parked section
    void refresh_all_lines()
    {
        auto* game_actor = CCore::g_instance->get_game_actor();
        void* truck = game_actor != nullptr
            ? prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck ) : nullptr;
        void* towed = truck != nullptr && readable_ptr( truck )
            ? prism::offsets::field< void* >( truck, 0x15A8 ) : nullptr;
        // the async detach sequence keeps the joint alive while the crank
        // anim runs — rebuilding the truck edge then re-draws the hoses the
        // game is cutting (the undo-then-redo the player sees)
        if ( pending_native_detach != nullptr &&
             ( joint( pending_native_detach ) == nullptr || ++pending_native_age > 600 ) )
        {
            pending_native_detach = nullptr;
        }
        if ( truck != nullptr && towed != nullptr && joint( towed ) != nullptr &&
             towed != pending_native_detach )
        {
            rebuild_one_edge( truck, towed );
        }
        for ( uint32_t i = 0; i < trailer_count; ++i )
        {
            void* t = trailers[ i ];
            if ( t == nullptr || !readable_ptr( t ) ) continue;
            void* slave = prism::offsets::field< void* >( t, prism::offsets::trailer_slave );
            if ( slave == nullptr || !readable_ptr( slave ) || joint( slave ) == nullptr ) continue;
            // the edge is defined by the SLAVE's joint: a piece bolted
            // behind a parked head is still a real coupling — redraw its
            // hoses through the bypass, a jointed front takes the live path
            if ( joint( t ) != nullptr )
            {
                rebuild_one_edge( t, slave );
            }
            else
            {
                rebuild_parked_edge( t, slave );
            }
        }
        if ( apply_cables_fn != nullptr && cables_mode_cvar != nullptr &&
            get_cvar_int_fn != nullptr && truck != nullptr )
        {
            const bool tows = towed != nullptr && joint( towed ) != nullptr;
            const int mode = tows ? get_cvar_int_fn( cables_mode_cvar ) : 0;
            apply_cables_fn( truck, tows ? towed : nullptr,
                0, mode > 4 ? 4 : mode < 0 ? 0 : mode, 0 );
        }
    }
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
    void pin_ctx_lamp_state( void* ctx )
    {
        static const uint32_t byte_fields[] =
            { 0x188, 0x1BC, 0x1C4, 0x1CD, 0x1CE, 0x1F4, 0x204, 0x248, 0x249 };
        static const uint32_t dword_fields[] =
            { 0x17C, 0x200, 0x208, 0x230, 0x2A0 };
        // one span check covers every field — the ctx is a single
        // allocation, so a VirtualQuery per field is a syscall per byte
        // wasted on the same page (this ran 14x per parked trailer per frame)
        if ( readable_span( ctx, 0x2A8 ) < 0x2A8 ) return;
        for ( const uint32_t o : byte_fields )
        {
            if ( prism::offsets::field< uint8_t >( ctx, o ) != 0 )
            {
                prism::offsets::field< uint8_t >( ctx, o ) = 0;
            }
        }
        for ( const uint32_t o : dword_fields )
        {
            if ( prism::offsets::field< uint32_t >( ctx, o ) != 0 )
            {
                prism::offsets::field< uint32_t >( ctx, o ) = 0;
            }
        }
    }
    // parked trailers keep their slave links for visibility, so the game
    // still registers/feeds straggler lamp components inside a live
    // vehicle's ctx buckets. The comps carry no reliable trailer
    // back-pointer, so ownership is proven the other way: a comp is a parked
    // trailer's if the trailer's own fields reference it. Matched entries
    // are pushed lamp-off through their own +0xD8 vfunc and unlinked — the
    // native attach re-registers them on recouple
    void lamp_ctx_sweep( void* reg_owner )
    {
        void* ctx = readable_ptr( reinterpret_cast< uint8_t* >( reg_owner ) +
                    prism::offsets::trailer_light_ctx )
            ? prism::offsets::field< void* >( reg_owner, prism::offsets::trailer_light_ctx )
            : nullptr;
        if ( ctx == nullptr || !readable_ptr( ctx ) ) return;
        void** buckets = prism::offsets::field< void** >( ctx, 0x08 );
        const uint64_t chn = prism::offsets::field< uint64_t >( ctx, 0x10 );
        if ( buckets == nullptr || !readable_ptr( buckets ) || chn > 0x80 ) return;
        void* parked[ max_trailers ] = {};
        uintptr_t parked_span[ max_trailers ] = {};
        uint32_t parked_n = 0;
        for ( uint32_t ti = 0; ti < max_trailers && canon[ ti ] != nullptr; ++ti )
        {
            void* t = canon[ ti ];
            if ( !readable_ptr( t ) || trailer_is_live( t ) ) continue;
            parked[ parked_n ] = t;
            parked_span[ parked_n ] = readable_span( t, 0x600 );
            ++parked_n;
        }
        if ( parked_n == 0 ) return;
        // ownership is proven forward: a bucket comp belongs to a parked
        // trailer when the trailer references it — directly (a field) or
        // through an object the trailer owns (model/chassis sub-object).
        // Collect the parked set's reachable pointers once per sweep
        static thread_local void* owned[ 8192 ];
        static thread_local void* owned_by[ 8192 ];
        uint32_t owned_n = 0;
        for ( uint32_t pi = 0; pi < parked_n; ++pi )
        {
            for ( uint32_t bo = 8; bo + 8 <= parked_span[ pi ] && owned_n < 8000; bo += 8 )
            {
                void* p = prism::offsets::field< void* >( parked[ pi ], bo );
                if ( p == nullptr ) continue;
                owned[ owned_n ] = p; owned_by[ owned_n ] = parked[ pi ]; ++owned_n;
                const uintptr_t hop = readable_span( p, 0x200 );
                for ( uint32_t ho = 8; ho + 8 <= hop && owned_n < 8192; ho += 8 )
                {
                    void* q = prism::offsets::field< void* >( p, ho );
                    if ( q != nullptr ) { owned[ owned_n ] = q; owned_by[ owned_n ] = parked[ pi ]; ++owned_n; }
                }
            }
        }
        for ( uint64_t c = 0; c < chn; ++c )
        {
            uint8_t* entry = reinterpret_cast< uint8_t* >( buckets ) + c * 0x38;
            if ( readable_span( entry, 0x18 ) < 0x18 ) continue;
            void** arr = *reinterpret_cast< void*** >( entry + 8 );
            uint64_t* cnt = reinterpret_cast< uint64_t* >( entry + 0x10 );
            if ( arr == nullptr || *cnt > 0x400 ||
                 readable_span( arr, *cnt * sizeof( void* ) ) < *cnt * sizeof( void* ) )
            {
                continue;
            }
            for ( uint64_t k = *cnt; k-- > 0; )
            {
                void* comp = arr[ k ];
                if ( comp == nullptr || !readable_ptr( comp ) ) continue;
                void* owner = nullptr;
                const uintptr_t cspan = readable_span( comp, 0x200 );
                for ( uint32_t pi = 0; pi < parked_n && owner == nullptr; ++pi )
                {
                    const auto tb = reinterpret_cast< uintptr_t >( parked[ pi ] );
                    for ( uint32_t bo = 8; bo + 8 <= cspan; bo += 8 )
                    {
                        const auto pv = reinterpret_cast< uintptr_t >(
                            prism::offsets::field< void* >( comp, bo ) );
                        if ( pv >= tb && pv < tb + 0x1400 )
                        {
                            owner = parked[ pi ];
                            break;
                        }
                    }
                }
                if ( owner == nullptr )
                {
                    for ( uint32_t oi = 0; oi < owned_n; ++oi )
                    {
                        if ( owned[ oi ] == comp ) { owner = owned_by[ oi ]; break; }
                    }
                }
                if ( owner == nullptr ) continue;
                if ( plausible_vfunc( comp, 0xD8 / 8 ) )
                {
                    __try { vcall< void __fastcall( void*, uint8_t ) >( comp, 0xD8 / 8, 0 ); }
                    __except ( EXCEPTION_EXECUTE_HANDLER ) {}
                }
                std::memmove( arr + k, arr + k + 1, ( *cnt - k - 1 ) * sizeof( void* ) );
                --*cnt;
                static int lr_unlinks = 0;
                if ( lr_unlinks++ < 16 )
                {
                    CCore::g_instance->info(
                        "lampreg: comp {:p} (owner {:p}) unlinked from {:p} ch=0x{:02X}",
                        comp, owner, reg_owner, c );
                }
            }
        }
    }
    void light_watch_tick()
    {
        if ( canon_age < 90 ) return;
        for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
        {
            void* t = canon[ i ];
            if ( !readable_ptr( t ) ) continue;
            const bool parked_piece = !trailer_is_live( t );
            if ( !parked_piece ) continue;
            // the native propagator copies the whole +0x2A0 light DWORD down
            // the slave chain (reverse/blinkers sit in the high bytes, brake/
            // tail in byte 0) — pinning only byte 0 leaves reverse alive
            if ( readable_ptr( reinterpret_cast< uint8_t* >( t ) +
                    prism::offsets::trailer_lamp_mask + 4 ) &&
                 prism::offsets::field< uint32_t >( t, prism::offsets::trailer_lamp_mask ) != 0 )
            {
                prism::offsets::field< uint32_t >( t, prism::offsets::trailer_lamp_mask ) = 0;
            }
            void* ctx = readable_ptr( reinterpret_cast< uint8_t* >( t ) +
                    prism::offsets::trailer_light_ctx )
                ? prism::offsets::field< void* >( t, prism::offsets::trailer_light_ctx ) : nullptr;
            if ( !lw_ctx_logged[ i ] )
            {
                lw_ctx_logged[ i ] = true;
                if ( ctx != nullptr && readable_span( ctx, 0x2A4 ) >= 0x2A4 )
                {
                    CCore::g_instance->info(
                        "lightwatch: c{} ctx={:p} src={:p} state={:08X} f248={:02X} f1F4={:02X}",
                        i, ctx, prism::offsets::field< void* >( ctx, 0x18 ),
                        prism::offsets::field< uint32_t >( ctx, 0x2A0 ),
                        prism::offsets::field< uint8_t >( ctx, 0x248 ),
                        prism::offsets::field< uint8_t >( ctx, 0x1F4 ) );
                }
                else
                {
                    CCore::g_instance->info( "lightwatch: c{} ctx=null trailer={:p}", i, t );
                }
            }
            if ( ctx != nullptr && readable_ptr( ctx ) &&
                 !native_chain_call_active.load( std::memory_order_acquire ) )
            {
                if ( prism::offsets::field< void* >( ctx, 0x18 ) != t )
                {
                    prism::offsets::field< void* >( ctx, 0x18 ) = t;
                }
                pin_ctx_lamp_state( ctx );
            }
        }
        // the reachability sweep rebuilds the parked set's pointer graph —
        // too heavy per frame, and registries only change on attach/detach:
        // run it when the parked/live signature actually changes instead of
        // on a fixed cadence, with a long fallback interval for strays
        static uint64_t sweep_sig = 0;
        static uint32_t sweep_idle = 0;
        uint64_t sig = trailer_count;
        for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
        {
            void* t = canon[ i ];
            sig = sig * 1315423911u ^ reinterpret_cast< uintptr_t >( t );
            sig = sig * 1315423911u ^
                  ( readable_ptr( t ) && trailer_is_live( t ) ? 1u : 0u );
        }
        if ( sig != sweep_sig || ++sweep_idle >= 900 )
        {
            sweep_sig = sig;
            sweep_idle = 0;
            if ( auto* game_actor = CCore::g_instance->get_game_actor() )
            {
                void* truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
                if ( truck != nullptr && readable_ptr( truck ) ) lamp_ctx_sweep( truck );
            }
            for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
            {
                void* t = canon[ i ];
                // a parked head's own registry can hold its section members'
                // lamp comps — a member only goes dark once its comps leave
                // EVERY registry that feeds them, so sweep parked ctx too
                if ( readable_ptr( t ) ) lamp_ctx_sweep( t );
            }
        }
    }
    // lamp-feed mask setter (0x1409DB4B0, signature-verified): the lamp
    // propagator fans the chain head's feed down the attach tree through
    // this setter — a canon trailer outside the live convoy must stay
    // electrically dark (its mask pins at 0 in light_watch_tick too, but
    // the propagator rewrites it every tick, so the write has to be
    // suppressed at the funnel)
    uint8_t __fastcall hk_lamp_mask( void* self, uint8_t mask )
    {
        const int ci = canon_age >= 90 ? canon_index_of( self ) : -1;
        if ( mask != 0 && ci >= 0 && !trailer_is_live( self ) )
        {
            mask = 0;
        }
        return lamp_mask_hook->get_original< lamp_mask_setter_fn >()( self, mask );
    }
    // the lamp-feed propagator (0x140790DC0): copies the head's +0x2A0 mask
    // DWORD down every trailer_slave link with raw stores — bypasses the
    // mask setter, so hk_lamp_mask never sees it. Called per tick on the
    // sim thread AND inside vfunc_disconnect's electrical cut, so a
    // once-per-frame pin can never win the race — a claimed parked trailer
    // shows brakes/blinkers/reverse as if coupled. Faithful port with one
    // change: a canon trailer outside the live convoy gets 0 instead of
    // the feed — the same value the native cut would leave it with
    using lamp_propagate_fn = void __fastcall( void* /*slave*/, uint32_t /*mask*/ );
    void __fastcall hk_lamp_propagate( void* self, uint32_t mask )
    {
        // a real convoy is never deeper than the canon set — bound the walk
        // so a transient slave cycle (claim edge meeting a native link
        // mid-maintenance) costs a few wasted writes instead of spinning
        // the sim thread until another thread breaks the ring
        uint32_t depth = 0;
        for ( void* p = self; p != nullptr && is_trailer_obj( p ) &&
              depth <= max_trailers * 2; ++depth )
        {
            void* next = prism::offsets::field< void* >( p, prism::offsets::trailer_slave );
            const bool dark = canon_age >= 90 && canon_index_of( p ) >= 0 &&
                              !trailer_is_live( p );
            prism::offsets::field< uint32_t >( p, prism::offsets::trailer_lamp_mask ) =
                dark ? 0u : mask;
            if ( next == p ) break;
            p = next;
        }
    }
    // apply_cables (0x1408762E0): draws/drops the cable entities for a
    // truck->trailer pair and walks trailer_slave unchecked. The native
    // detach uncables with (truck, nullptr) — a global clear that drops the
    // lines on the WHOLE live chain, not just the dead edge. While the
    // truck still tows, substitute the towed head so the call re-applies
    // the live set instead; a jointless trailer arg resolves to what's
    // actually on the wheel
    void __fastcall hk_apply_cables( void* truck, void* trailer, int unused, int mode, int zero )
    {
        // untouched passthrough during world assembly — redirects and
        // refusals based on half-built joint state corrupt the cable set
        // the game is still building
        if ( canon_age < 90 )
        {
            apply_cables_hook->get_original< vehicle_cables_fn >()( truck, trailer, unused, mode, zero );
            return;
        }
        void* towed = truck != nullptr && readable_ptr( truck )
            ? prism::offsets::field< void* >( truck, 0x15A8 ) : nullptr;
        // same cascade guard as hk_rebuild_lines: a canon trailer outside
        // the live (towed) chain that still takes part in a real coupling
        // keeps its cable state — refuse the teardown
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
            // (truck, nullptr) while still towing → re-apply on the towed
            // head instead of clearing the live chain's lines
            trailer = towed;
        }
        else if ( trailer != nullptr && readable_ptr( trailer ) && joint( trailer ) == nullptr )
        {
            // a jointless cable target: send the lines to what's on the
            // wheel, or drop them entirely when nothing is coupled. The
            // caller's mode travels with the arg — (truck, nullptr, 1)
            // still resolves the parked head through +0xC8 and redraws
            // the hoses we just cut, so a null target forces mode 0
            trailer = ( front_is_truck && towed != nullptr && joint( towed ) != nullptr )
                ? towed : nullptr;
            if ( trailer == nullptr ) mode = 0;
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
        native_chain_call_active.store( true, std::memory_order_release );
        apply_cables_hook->get_original< vehicle_cables_fn >()( truck, trailer, unused, mode, zero );
        native_chain_call_active.store( false, std::memory_order_release );
        for ( uint32_t i = 0; i < hidden; ++i )
        {
            prism::offsets::field< void* >( hidden_from[ i ], prism::offsets::trailer_slave ) = hidden_link[ i ];
        }
    }
    // the native line-state setter (0x140874010): (vehicle, state, force) —
    // state=0 calls vfunc+0x10 (hide) on every +0xE98 line entity, state=1
    // calls vfunc+0x08 (show). A trailer owns the hoses of its FRONT edge,
    // so a trailer whose front joint is alive still has its hoses
    // connected — a state=0 aimed at it is the native detach cascade
    // tearing down lines on trailers BEHIND the released one. Refuse the
    // hide while the joint lives; a real detach drops the joint first, so
    // legit teardown still lands a frame later
    void __fastcall hk_set_lines( void* vehicle, uint8_t state, uint8_t force )
    {
        const int vi = canon_age >= 90 && vehicle != nullptr ? canon_index_of( vehicle ) : -1;
        void* j = vehicle != nullptr &&
                  readable_ptr( static_cast< uint8_t* >( vehicle ) + prism::offsets::trailer_physics_joint )
                      ? joint( vehicle ) : nullptr;
        if ( vi >= 0 )
        {
            CCore::g_instance->info( "set_lines c{} {:p} state={} force={} joint={:p}",
                vi, vehicle, state, force, j );
        }
        if ( state == 0 && vi >= 0 && j != nullptr )
        {
            return;
        }
        set_lines_hook->get_original< line_state_fn >()( vehicle, state, force );
    }
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
    void __fastcall hk_rebuild_lines( void* front, void* trailer )
    {
        // untouched passthrough during world assembly — the refuse/redirect
        // logic reads joint state that is garbage on half-built trailers
        if ( canon_age < 90 )
        {
            rebuild_lines_hook->get_original< prism::vehicle_rebuild_lines_fn >()( front, trailer );
            return;
        }
        void* truck = nullptr;
        void* towed = nullptr;
        if ( auto* game_actor = CCore::g_instance->get_game_actor() )
        {
            truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
            towed = truck != nullptr && readable_ptr( truck )
                ? prism::offsets::field< void* >( truck, 0x15A8 ) : nullptr;
        }
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
        if ( trailer != nullptr && !t_jointed )
        {
            // the walk recurses down trailer+0x1088 — rebuilding on a
            // parked piece would draw hoses across the whole claimed
            // section. null is the native "nothing attached" input: it
            // clears the front's outgoing lines without drawing any
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
}
