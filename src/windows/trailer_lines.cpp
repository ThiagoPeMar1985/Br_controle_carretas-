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
    using sound_event_play_fn = void __fastcall( void* /*event*/, uint8_t /*flag*/ );

    // 148 - toca o som do apoio subindo/descendo
    void play_brace_sound( void* trailer, const bool up )
    {
        if ( !expected_build() ) return;
        const auto sound_vtbl = validated_va( 0x142220EE60 );
        void* audio = readable_ptr( trailer ) ? prism::offsets::field< void* >( trailer, 0x12A8 ) : nullptr;
        if ( !readable_ptr( audio ) || !readable_ptr( reinterpret_cast< uint8_t* >( audio ) + 0x60 ) )
        {
            return;
        }
        void* channel = prism::offsets::field< void* >( audio, up ? 0x50 : 0x58 );
        void* event = nullptr;
        if ( readable_ptr( channel ) && sound_vtbl != 0 &&
            *reinterpret_cast< uint64_t* >( channel ) == sound_vtbl )
        {
            event = channel;
        }
        else if ( readable_ptr( channel ) &&
            readable_ptr( reinterpret_cast< uint8_t* >( channel ) + 0x10 ) )
        {
            void* inner = prism::offsets::field< void* >( channel, 0x8 );
            if ( readable_ptr( inner ) && sound_vtbl != 0 &&
                *reinterpret_cast< uint64_t* >( inner ) == sound_vtbl )
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

    // 149 - por tick: apoio das carretas (levanta no engate, desce na estacionada)
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
                coupling_trailer = prism::offsets::field< void* >( tr, 0x1580 );
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
                CCore::g_instance->info( "gear: detach sequence on {} - cranking legs down", i );
                brace_anim_fn( t, 1, 0.3333333f );
            }
            else if ( jointed && gear < 0.5f && !detaching_head && plausible_vfunc( t, 0x328 / 8 ) )
            {
                CCore::g_instance->info( "gear: jointed trailer {} stamped legs-down - restoring", i );
                vcall< prism::physics_trailer_u_set_trailer_brace_fn >( t, 0x328 / 8, false );
            }
            else if ( !jointed && gear > 0.5f && brace_anim_fn != nullptr )
            {
                CCore::g_instance->info( "gear: parked head {} legs still up - lowering", i );
                brace_anim_fn( t, 1, 0.3333333f );
            }
        }
    }

    // 150 - acha o contexto de luzes do veículo
    static void* light_ctx_of( void* veh )
    {
        if ( veh == nullptr || !readable_ptr( veh ) ) return nullptr;
        static constexpr uint32_t cand[] =
            { 0x110, 0xE8, 0xE0, 0xF0, 0xF8, 0x100, 0x108, 0x118, 0x120, 0x128 };
        for ( const uint32_t off : cand )
        {
            if ( !readable_ptr( static_cast< uint8_t* >( veh ) + off ) ) continue;
            void* p = prism::offsets::field< void* >( veh, off );
            if ( p == nullptr || !plausible_object( p ) ) continue;
            if ( readable_span( p, 0x2A8 ) < 0x2A8 ) continue;
            if ( prism::offsets::field< uint64_t >( p, 0x10 ) > 0x80 ) continue;
            void* src = prism::offsets::field< void* >( p, 0x18 );
            if ( src == nullptr || !plausible_object( src ) ) continue;
            return p;
        }
        return nullptr;
    }
    // 151 - acha a fonte de luzes do veículo
    void* light_source_of( void* veh )
    {
        if ( veh == nullptr || !readable_ptr( veh ) ) return nullptr;
        void* ctx = light_ctx_of( veh );
        if ( ctx == nullptr || !readable_ptr( ctx ) ) return nullptr;
        return prism::offsets::field< void* >( ctx, 0x18 );
    }

    // 152 - reconstrói as mangueiras de uma aresta (link escondido na chamada)
    void rebuild_one_edge( void* front, void* trailer )
    {
        if ( rebuild_lines_fn == nullptr || front == nullptr || trailer == nullptr ) return;
        CCore::g_instance->info( "fn152 cabo: rebuild_lines({:p},{:p})", front, trailer );
        void* slave = prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave );
        prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = nullptr;
        rebuild_lines_fn( front, trailer );
        prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = slave;
    }

    // 153 - redesenha a aresta real atrás de uma cabeça estacionada
    void rebuild_parked_edge( void* front, void* trailer )
    {
        if ( rebuild_lines_hook == nullptr || front == nullptr || trailer == nullptr ) return;
        CCore::g_instance->info( "fn153 cabo: rebuild_parked({:p},{:p})", front, trailer );
        void* slave = prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave );
        prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = nullptr;
        rebuild_lines_hook->get_original< prism::vehicle_rebuild_lines_fn >()( front, trailer );
        prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave ) = slave;

        if ( set_lines_fn != nullptr )
        {
            set_lines_fn( trailer, 1, 1 );
            set_lines_fn( front, 1, 1 );
        }

        if ( apply_cables_hook != nullptr )
        {
            apply_cables_hook->get_original< vehicle_cables_fn >()( front, trailer, 0, 2, 0 );
        }
    }

    // 154 - reescreve todas as linhas vivas
    void refresh_all_lines()
    {
        auto* game_actor = CCore::g_instance->get_game_actor();
        void* truck = game_actor != nullptr
            ? prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck ) : nullptr;
        void* towed = truck != nullptr && readable_ptr( truck )
            ? prism::offsets::field< void* >( truck, 0x1580 ) : nullptr;

        if ( pending_native_detach != nullptr &&
             ( joint( pending_native_detach ) == nullptr || ++pending_native_age > 600 ) )
        {
            pending_native_detach = nullptr;
        }
        if ( truck != nullptr && is_trailer_obj( towed ) && joint( towed ) != nullptr &&
             towed != pending_native_detach )
        {
            CCore::g_instance->info( "fn154 cabo: aresta cavalo->towed {:p}", towed );
            rebuild_one_edge( truck, towed );
        }
        for ( uint32_t i = 0; i < trailer_count; ++i )
        {
            void* t = trailers[ i ];
            if ( !is_trailer_obj( t ) ) continue;
            void* slave = prism::offsets::field< void* >( t, prism::offsets::trailer_slave );
            if ( slave == nullptr || !is_trailer_obj( slave ) || joint( slave ) == nullptr ) continue;

            if ( joint( t ) != nullptr )
            {
                rebuild_one_edge( t, slave );
            }
            else
            {
                rebuild_parked_edge( t, slave );
            }
        }

        for ( auto& torn : lines_torn ) torn = false;
        if ( apply_cables_fn != nullptr && cables_mode_cvar != nullptr &&
            get_cvar_int_fn != nullptr && truck != nullptr )
        {
            const bool tows = towed != nullptr && joint( towed ) != nullptr;
            const int mode = tows ? get_cvar_int_fn( cables_mode_cvar ) : 0;
            CCore::g_instance->info( "fn154 cabo: apply_cables(cavalo,{:p}) mode={}",
                                     tows ? towed : nullptr, mode );
            apply_cables_fn( truck, tows ? towed : nullptr,
                0, mode > 4 ? 4 : mode < 0 ? 0 : mode, 0 );
        }
    }

    // 155 - repina o src de luz reciclado de volta pro membro
    void repair_lamp_ctx( void* m )
    {
        if ( m == nullptr || !readable_ptr( m ) ) return;
        void* ctx = light_ctx_of( m );
        if ( ctx == nullptr || !readable_ptr( ctx ) ||
             !readable_ptr( static_cast< uint8_t* >( ctx ) + 0x18 ) ) return;
        void* src = prism::offsets::field< void* >( ctx, 0x18 );
        if ( src == nullptr || src == m ) return;

        if ( auto* game_actor = CCore::g_instance->get_game_actor() )
        {
            void* truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
            if ( src == truck ) return;
        }
        if ( canon_index_of( src ) >= 0 ) return;
        CCore::g_instance->warning(
            "lamp ctx: {:p} tinha src {:p} reciclado - repinado em self", m, src );
        prism::offsets::field< void* >( ctx, 0x18 ) = m;
    }
    // 156 - repina os contextos de luz de todas as carretas
    void repair_lamp_ctx_all()
    {
        for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
        {
            repair_lamp_ctx( canon[ i ] );
        }
    }

    // 157 - prende o estado de luz num contexto
    void pin_ctx_lamp_state( void* ctx )
    {
        static const uint32_t byte_fields[] =
            { 0x188, 0x1BC, 0x1C4, 0x1CD, 0x1CE, 0x1F4, 0x204, 0x248, 0x249 };
        static const uint32_t dword_fields[] =
            { 0x17C, 0x200, 0x208, 0x230, 0x2A0 };

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

    // 158 - varre os contextos de luz do dono
    void lamp_ctx_sweep( void* reg_owner )
    {
        void* ctx = light_ctx_of( reg_owner );
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
    // 159 - vigia de lâmpadas por tick
    void light_watch_tick()
    {
        if ( canon_age < 90 ) return;
        for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
        {
            void* t = canon[ i ];

            if ( readable_span( t, 0x1200 ) < 0x1200 ) continue;
            const bool parked_piece = !trailer_is_live( t );
            if ( !parked_piece ) continue;

            if ( readable_ptr( reinterpret_cast< uint8_t* >( t ) +
                    prism::offsets::trailer_lamp_mask + 4 ) &&
                 prism::offsets::field< uint32_t >( t, prism::offsets::trailer_lamp_mask ) != 0 )
            {
                prism::offsets::field< uint32_t >( t, prism::offsets::trailer_lamp_mask ) = 0;
            }
            void* ctx = light_ctx_of( t );
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
        if ( sig != sweep_sig || ++sweep_idle >= 150 )
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

                if ( readable_ptr( t ) ) lamp_ctx_sweep( t );
            }
        }
    }

    // 160 - hook: máscara de lâmpadas
    uint8_t __fastcall hk_lamp_mask( void* self, uint8_t mask )
    {

        if ( !settings::g_settings.cargo_restore )
        {
            return lamp_mask_hook->get_original< lamp_mask_setter_fn >()( self, mask );
        }
        const int ci = canon_age >= 90 ? canon_index_of( self ) : -1;
        if ( mask != 0 && ci >= 0 && !trailer_is_live( self ) )
        {
            mask = 0;
        }
        return lamp_mask_hook->get_original< lamp_mask_setter_fn >()( self, mask );
    }

    using lamp_propagate_fn = void __fastcall( void* /*slave*/, uint32_t /*mask*/ );
    // 161 - hook: propagação de lâmpadas pela cadeia
    void __fastcall hk_lamp_propagate( void* self, uint32_t mask )
    {

        if ( !settings::g_settings.cargo_restore )
        {
            lamp_propagate_hook->get_original< lamp_propagate_fn >()( self, mask );
            return;
        }

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

    // 162 - hook: cabos/mangueiras do engate
    void __fastcall hk_apply_cables( void* truck, void* trailer, int unused, int mode, int zero )
    {

        if ( !settings::g_settings.cargo_restore )
        {
            apply_cables_hook->get_original< vehicle_cables_fn >()( truck, trailer, unused, mode, zero );
            return;
        }

        if ( canon_age < 90 )
        {
            apply_cables_hook->get_original< vehicle_cables_fn >()( truck, trailer, unused, mode, zero );
            return;
        }
        void* towed = truck != nullptr && readable_ptr( truck )
            ? prism::offsets::field< void* >( truck, 0x1580 ) : nullptr;

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

                void* fslave =
                    readable_ptr( static_cast< uint8_t* >( truck ) + prism::offsets::trailer_slave )
                        ? prism::offsets::field< void* >( truck, prism::offsets::trailer_slave ) : nullptr;
                const bool slave_intact = fslave != nullptr && readable_ptr( fslave ) &&
                    readable_ptr( static_cast< uint8_t* >( fslave ) + prism::offsets::trailer_physics_joint ) &&
                    joint( fslave ) != nullptr;
                const bool arg_live_edge = trailer != nullptr && readable_ptr( trailer ) &&
                    readable_ptr( static_cast< uint8_t* >( trailer ) + prism::offsets::trailer_physics_joint ) &&
                    joint( trailer ) != nullptr;
                if ( slave_intact && ( trailer == nullptr || arg_live_edge ) )
                {
                    return;
                }
            }
        }

        void* real_truck = nullptr;
        if ( auto* game_actor = CCore::g_instance->get_game_actor() )
        {
            real_truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
        }
        const bool front_is_truck = truck != nullptr && truck == real_truck;
        if ( trailer == nullptr && front_is_truck &&
             towed != nullptr && readable_ptr( towed ) && joint( towed ) != nullptr )
        {

            trailer = towed;
        }
        else if ( trailer != nullptr && readable_ptr( trailer ) && joint( trailer ) == nullptr )
        {

            trailer = ( front_is_truck && towed != nullptr && joint( towed ) != nullptr )
                ? towed : nullptr;
            if ( trailer == nullptr ) mode = 0;
        }

        void* hidden_from[ max_trailers ] = {};
        void* hidden_link[ max_trailers ] = {};
        uint32_t hidden = 0;
        for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr && hidden < max_trailers; ++i )
        {
            void* t = canon[ i ];
            if ( !readable_ptr( t ) ||
                 !readable_ptr( static_cast< uint8_t* >( t ) + prism::offsets::trailer_slave ) )
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

    // 163 - hook: liga/desliga as linhas do veículo
    void __fastcall hk_set_lines( void* vehicle, uint8_t state, uint8_t force )
    {

        if ( !settings::g_settings.cargo_restore )
        {
            set_lines_hook->get_original< line_state_fn >()( vehicle, state, force );
            return;
        }
        const int vi = canon_age >= 90 && vehicle != nullptr ? canon_index_of( vehicle ) : -1;
        void* j = vehicle != nullptr &&
                  readable_ptr( static_cast< uint8_t* >( vehicle ) + prism::offsets::trailer_physics_joint )
                      ? joint( vehicle ) : nullptr;
        if ( state == 0 && vi >= 0 && j != nullptr )
        {
            return;
        }

        if ( state != 0 && vi >= 0 && j == nullptr )
        {
            state = 0;
        }
        set_lines_hook->get_original< line_state_fn >()( vehicle, state, force );
    }

    using hose_update_fn = void __fastcall( void* /*front*/, void* /*trailer*/ );
    // 164 - hook: atualização de mangueiras por aresta
    void __fastcall hk_hose_update( void* front, void* trailer )
    {
        if ( !settings::g_settings.cargo_restore || canon_age < 90 )
        {
            hose_update_hook->get_original< hose_update_fn >()( front, trailer );
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

                void* behind = readable_ptr( static_cast< uint8_t* >( slave ) + prism::offsets::trailer_slave )
                    ? prism::offsets::field< void* >( slave, prism::offsets::trailer_slave ) : nullptr;
                const bool section_behind = behind != nullptr && readable_ptr( behind ) &&
                    readable_ptr( static_cast< uint8_t* >( behind ) + prism::offsets::trailer_physics_joint ) &&
                    joint( behind ) != nullptr;
                if ( !section_behind )
                {
                    hidden[ hidden_n++ ] = { t, slave };
                    prism::offsets::field< void* >( t, prism::offsets::trailer_slave ) = nullptr;
                }
            }
        }
        native_chain_call_active.store( true, std::memory_order_release );
        hose_update_hook->get_original< hose_update_fn >()( front, trailer );
        native_chain_call_active.store( false, std::memory_order_release );
        for ( uint32_t i = 0; i < hidden_n; ++i )
        {
            if ( readable_ptr( hidden[ i ].owner ) )
            {
                prism::offsets::field< void* >( hidden[ i ].owner, prism::offsets::trailer_slave ) =
                    hidden[ i ].slave;
            }
        }
    }

    // 165 - hook: cascata de desengate das linhas
    void __fastcall hk_rebuild_lines( void* front, void* trailer )
    {

        if ( !settings::g_settings.cargo_restore )
        {
            rebuild_lines_hook->get_original< prism::vehicle_rebuild_lines_fn >()( front, trailer );
            return;
        }

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
                ? prism::offsets::field< void* >( truck, 0x1580 ) : nullptr;
        }
        const int fi = canon_index_of( front );
        if ( fi >= 0 && readable_ptr( front ) )
        {

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

                void* fslave =
                    readable_ptr( static_cast< uint8_t* >( front ) + prism::offsets::trailer_slave )
                        ? prism::offsets::field< void* >( front, prism::offsets::trailer_slave ) : nullptr;
                const bool slave_intact = fslave != nullptr && readable_ptr( fslave ) &&
                    readable_ptr( static_cast< uint8_t* >( fslave ) + prism::offsets::trailer_physics_joint ) &&
                    joint( fslave ) != nullptr;
                const bool arg_live_edge = trailer != nullptr && readable_ptr( trailer ) &&
                    readable_ptr( static_cast< uint8_t* >( trailer ) + prism::offsets::trailer_physics_joint ) &&
                    joint( trailer ) != nullptr;
                if ( slave_intact && ( trailer == nullptr || arg_live_edge ) )
                {
                    return;
                }
            }
        }
        const bool t_jointed = trailer != nullptr && readable_ptr( trailer ) && joint( trailer ) != nullptr;
        if ( trailer != nullptr && !t_jointed )
        {

            trailer = ( front == truck && towed != nullptr && joint( towed ) != nullptr )
                ? towed : nullptr;
        }
        if ( trailer == nullptr )
        {

            void* hidden_link = nullptr;
            if ( front != nullptr && readable_ptr( front ) &&
                 readable_ptr( static_cast< uint8_t* >( front ) + prism::offsets::trailer_slave ) )
            {
                void* s = prism::offsets::field< void* >( front, prism::offsets::trailer_slave );
                if ( s != nullptr && readable_ptr( s ) &&
                     readable_ptr( static_cast< uint8_t* >( s ) + prism::offsets::trailer_physics_joint ) &&
                     joint( s ) == nullptr )
                {
                    hidden_link = s;
                    prism::offsets::field< void* >( front, prism::offsets::trailer_slave ) = nullptr;
                }
            }
            rebuild_lines_hook->get_original< prism::vehicle_rebuild_lines_fn >()( front, nullptr );
            if ( hidden_link != nullptr )
            {
                prism::offsets::field< void* >( front, prism::offsets::trailer_slave ) = hidden_link;
            }
            return;
        }
        void* fixed_front = front;
        if ( front != nullptr && front != truck && readable_ptr( front ) && joint( front ) == nullptr )
        {
            fixed_front = ( trailer == towed ) ? truck : front;
        }
        rebuild_lines_hook->get_original< prism::vehicle_rebuild_lines_fn >()( fixed_front, trailer );
    }
    namespace
    {
        const bool fn_idx_reg = []()
        {
            fn_index_add( fn_addr_of( &play_brace_sound ), 148, "play_brace_sound" );
            fn_index_add( fn_addr_of( &update_landing_gear ), 149, "update_landing_gear" );
            fn_index_add( fn_addr_of( &light_ctx_of ), 150, "light_ctx_of" );
            fn_index_add( fn_addr_of( &light_source_of ), 151, "light_source_of" );
            fn_index_add( fn_addr_of( &rebuild_one_edge ), 152, "rebuild_one_edge" );
            fn_index_add( fn_addr_of( &rebuild_parked_edge ), 153, "rebuild_parked_edge" );
            fn_index_add( fn_addr_of( &refresh_all_lines ), 154, "refresh_all_lines" );
            fn_index_add( fn_addr_of( &repair_lamp_ctx ), 155, "repair_lamp_ctx" );
            fn_index_add( fn_addr_of( &repair_lamp_ctx_all ), 156, "repair_lamp_ctx_all" );
            fn_index_add( fn_addr_of( &pin_ctx_lamp_state ), 157, "pin_ctx_lamp_state" );
            fn_index_add( fn_addr_of( &lamp_ctx_sweep ), 158, "lamp_ctx_sweep" );
            fn_index_add( fn_addr_of( &light_watch_tick ), 159, "light_watch_tick" );
            fn_index_add( fn_addr_of( &hk_lamp_mask ), 160, "hk_lamp_mask" );
            fn_index_add( fn_addr_of( &hk_lamp_propagate ), 161, "hk_lamp_propagate" );
            fn_index_add( fn_addr_of( &hk_apply_cables ), 162, "hk_apply_cables" );
            fn_index_add( fn_addr_of( &hk_set_lines ), 163, "hk_set_lines" );
            fn_index_add( fn_addr_of( &hk_hose_update ), 164, "hk_hose_update" );
            fn_index_add( fn_addr_of( &hk_rebuild_lines ), 165, "hk_rebuild_lines" );
            return true;
        }();
    }
}
