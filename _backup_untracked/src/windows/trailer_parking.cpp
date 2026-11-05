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
    //========================================================================
    // Ator/provider de obstáculo de tráfego das carretas
    //========================================================================

    // cache trailer -> trailer-actor (o actor some dos slots depois do detach)
    std::pair< void*, void* > trailer_actors[ max_trailers ] = {};

    // slots de actor em game_actor+0x1D38 (entradas de 0x28); o backpointer pro
    // trailer é descoberto escaneando os campos do actor. A âncora de obstáculo
    // é um componente da própria carreta - vtable amostrada do actor da cabeça
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
        // fallback: posição na cadeia mapeia pro mesmo índice de slot
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
        // fallback: actor cacheado de quando ela ainda estava engatada
        for ( uint32_t i = 0; i < max_trailers; ++i )
        {
            if ( trailer_actors[ i ].first == trailer )
            {
                void* cached = trailer_actors[ i ].second;
                return plausible_object( cached ) ? cached : nullptr;
            }
        }
        // último recurso: objeto cujo +0x1F0 é um provider da vtable amostrada
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

    // acha o componente provider dentro da carreta (âncora de obstáculo dela)
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

    //========================================================================
    // Registro de estacionamento (parked-state record) e obstáculo de lane
    //========================================================================

    // filtro SEH que loga o RIP exato da falha dentro da fábrica de records
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

    // dump só-leitura: manager de parked-records + slots de ator em +0x1D38
    void dump_native_registry()
    {
        void* mgr = readable_ptr( parked_record_mgr ) ? *parked_record_mgr : nullptr;
        CCore::g_instance->info( "  registry: global at {:p} (exe+0x{:X}) readable={} raw={:p}",
            static_cast< void* >( parked_record_mgr ),
            reinterpret_cast< uint64_t >( parked_record_mgr ) -
                reinterpret_cast< uint64_t >( GetModuleHandle( nullptr ) ),
            readable_ptr( parked_record_mgr ), mgr );
        __try
        {
            if ( mgr == nullptr || readable_span( mgr, 0x750 ) < 0x750 )
            {
                CCore::g_instance->info( "  registry: parked-record manager {:p} (none)", mgr );
            }
            else
            {
                void** it = prism::offsets::field< void** >( mgr, 0x740 );
                const uint64_t n = prism::offsets::field< uint64_t >( mgr, 0x748 );
                CCore::g_instance->info( "  registry: manager {:p} records={}", mgr, n );
                if ( it != nullptr && n < 0x40 && readable_span( it, n * sizeof( void* ) ) >= n * sizeof( void* ) )
                {
                    for ( uint64_t k = 0; k < n; ++k )
                    {
                        void* rec = it[ k ];
                        void* owner = rec != nullptr && readable_ptr( rec )
                            ? prism::offsets::field< void* >( rec, 0x178 ) : nullptr;
                        CCore::g_instance->info( "    record {:p} -> trailer {:p} (c{})",
                            rec, owner, canon_index_of( owner ) );
                    }
                }
            }
            if ( auto* game_actor = CCore::g_instance->get_game_actor() )
            {
                for ( uint32_t s = 0; s < 8; ++s )
                {
                    void* entry = reinterpret_cast< uint8_t* >( game_actor ) + 0x1D38 + s * 0x28;
                    if ( !readable_ptr( reinterpret_cast< uint8_t* >( entry ) + 8 ) ) break;
                    void* actor = *reinterpret_cast< void** >( entry );
                    CCore::g_instance->info( "  actor slot {}: {:p}{}", s, actor,
                        actor != nullptr && plausible_object( actor ) ? "" : " (empty)" );
                }
            }
        }
        __except ( EXCEPTION_EXECUTE_HANDLER )
        {
            CCore::g_instance->warning( "registry dump faulted" );
        }
    }

    // o record está realmente na scan-list do manager? (linkado mas ausente = inerte)
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

    // tira o record da carreta da scan-list do manager
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

    // containers de lane-obstacle vistos pelo hook de insert - podados em
    // janela pós-detach + cadência lenta (órfãos quebram o scan das workers)
    void* lane_containers[ 128 ] = {};
    uint32_t lane_prune_frames = 0;
    uint32_t lane_prune_idle = 0;

    // owner-node do elemento (+0x88) ainda vivo?
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

    // remove entradas órfãs dos containers - fatiado a 64 inspeções por chamada
    // (a passada inteira era o stutter pós-detach)
    void prune_lane_containers()
    {
        static uint32_t ci = 0;
        static uint64_t ei = ~0ull;   // ~0 = próximo container, lê count dele
        uint32_t budget = 64;
        while ( budget > 0 && ci < 128 )
        {
            void*& c = lane_containers[ ci ];
            if ( c == nullptr || !readable_ptr( c ) ||
                 !readable_ptr( static_cast< uint8_t* >( c ) + 0x10 ) )
            {
                c = nullptr;
                ++ci;
                ei = ~0ull;
                continue;
            }
            void** elements = *reinterpret_cast< void*** >( static_cast< uint8_t* >( c ) + 8 );
            auto* count = reinterpret_cast< uint64_t* >( static_cast< uint8_t* >( c ) + 0x10 );
            if ( elements == nullptr || !readable_ptr( elements ) ||
                 *count == 0 || *count > 0x10000 )
            {
                c = nullptr;
                ++ci;
                ei = ~0ull;
                continue;
            }
            bool done = false;
            __try
            {
                uint64_t n = *count;
                if ( ei == ~0ull || ei >= n ) ei = n - 1;
                while ( budget > 0 )
                {
                    --budget;
                    void* el = elements[ ei ];
                    if ( lane_element_dead( el ) )
                    {
                        CCore::g_instance->info( "lane obstacle: pruned dead entry {:p}", el );
                        std::memmove( &elements[ ei ], &elements[ ei + 1 ],
                                      ( n - ei - 1 ) * sizeof( void* ) );
                        --n;
                        *count = n;
                        // worker com count velho lê elements[n]: última viva no rabo
                        if ( n > 0 ) elements[ n ] = elements[ n - 1 ];
                        if ( n == 0 || ei >= n ) { done = true; break; }
                    }
                    else if ( ei == 0 ) { done = true; break; }
                    else { --ei; }
                }
            }
            __except ( EXCEPTION_EXECUTE_HANDLER )
            {
                c = nullptr;
                ++ci;
                ei = ~0ull;
                continue;
            }
            if ( done )
            {
                ++ci;
                ei = ~0ull;
            }
        }
        if ( ci >= 128 ) { ci = 0; ei = ~0ull; }
    }

    // hook do insert de lane-obstacle (0x14094DFC0): sanitiza o container antes
    // do original (o scan dereferencia +0x88 sem checar - entrada morta quebra o job)
    using lane_obstacle_insert_fn = uint8_t __fastcall( void* /*node*/, void* /*link*/, float /*pos*/ );
    uint8_t __fastcall hk_lane_obstacle_insert( void* node, void* link, float pos )
    {
        if ( !settings::g_settings.cargo_restore )
        {
            return lane_obstacle_insert_hook->get_original< lane_obstacle_insert_fn >()( node, link, pos );
        }
        if ( node != nullptr && link != nullptr && plausible_vfunc( link, 1 ) )
        {
            using type_fn = uint32_t __fastcall( void* );
            const auto get_type = reinterpret_cast< type_fn* >( ( *reinterpret_cast< void** const* >( link ) )[ 1 ] );
            // link 0xFFF = a lista ordenada por posição (o caminho que lê +0x88)
            if ( get_type( link ) == 0xFFF && plausible_vfunc( node, 0xD0 / 8 ) )
            {
                using container_fn = void* __fastcall( void* );
                const auto get_container = reinterpret_cast< container_fn* >( ( *reinterpret_cast< void** const* >( node ) )[ 0xD0 / 8 ] );
                void* container = get_container( node );
                if ( readable_ptr( container ) && readable_ptr( static_cast< uint8_t* >( container ) + 0x18 ) )
                {
                    // lembra o container pra poda por frame
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

    // estaciona uma carreta já sem junta: brace + freio + animação da manivela
    // (sem a anim nenhum corpo de apoio existe) + registro de tráfego
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
            if ( notify_decoupled_fn != nullptr ) notify_decoupled_fn( t );
            traffic_detach_tail();
            lane_prune_frames = 40;
        }
        __except ( EXCEPTION_EXECUTE_HANDLER )
        {
            CCore::g_instance->warning( "native park sequence faulted on {:p}", t );
        }
    }

    //========================================================================
    // Proteção contra crashes: purecall fake + tracer de AV
    //========================================================================

    // tracer de AV: loga RIP + registradores (first chance, pass-through)
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
        // curva de nav com índice corrompido (exe+0x1BEA1C5): emula a carga como 0.0
        if ( code == EXCEPTION_ACCESS_VIOLATION && ep->ContextRecord != nullptr &&
             rip == base + 0x1BEA1C5 &&
             ep->ExceptionRecord->NumberParameters > 0 &&
             ep->ExceptionRecord->ExceptionInformation[ 0 ] == 0 /* leitura */ )
        {
            static int nav_curve_emulated = 0;
            if ( nav_curve_emulated < 4 )
            {
                ++nav_curve_emulated;
                CCore::g_instance->warning(
                    "nav curve read emulated (OOB index) at exe+0x1BEA1C5" );
            }
            ep->ContextRecord->Xmm5.Low = 0;
            ep->ContextRecord->Xmm5.High = 0;
            ep->ContextRecord->Rip += 5;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        // dumps de registrador/pilha só com debug ligado
        if ( !settings::g_settings.debug_logs ) return EXCEPTION_CONTINUE_SEARCH;
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
        // RIP fora do exe = vtable corrupta: dump dos endereços exe perto de RSP
        static int stack_dumps = 0;
        const auto* dos = reinterpret_cast< const IMAGE_DOS_HEADER* >( base );
        const auto* nt = reinterpret_cast< const IMAGE_NT_HEADERS64* >( base + dos->e_lfanew );
        const uint64_t image_end = base + nt->OptionalHeader.SizeOfImage;
        if ( ep->ContextRecord != nullptr && ( rip < base || rip >= image_end ) && stack_dumps < 4 )
        {
            ++stack_dumps;
            const auto* sp = reinterpret_cast< const uint64_t* >( ep->ContextRecord->Rsp );
            std::string found;
            int n = 0;
            for ( int k = 0; k < 96 && n < 14; ++k )
            {
                if ( !readable_ptr( sp + k ) ) break;
                const uint64_t v = sp[ k ];
                if ( v >= base && v < image_end )
                {
                    found += fmt::format( " [rsp+0x{:X}]=exe+0x{:X}", k * 8, v - base );
                    ++n;
                }
            }
            CCore::g_instance->warning( "AV stack (RIP outside exe):{}", found );
        }
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

    // iterador de componentes em exe+0x927C60 não checa o retorno do purecall:
    // pra esse range entrega um componente fake benigno; fora dele, null
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

    void* __fastcall hk_purecall( void* object )
    {
        if ( !settings::g_settings.cargo_restore )
        {
            return purecall_hook->get_original< void* __fastcall( void* ) >()( object );
        }
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

    //========================================================================
    // Trava de estacionamento por frame (roda dentro de update_convoy_state)
    //========================================================================

    // re-asserta flags parked/brace nas desatreladas (a máquina esconde o que
    // não alcança pela cadeia viva)
    void unhide_detached()
    {
        // reconcile de meio-teleporte: juntas leem null e nudge briga com o jogo
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



    // trava de estacionamento: roda a fábrica de record uma vez por slot;
    // peças sem record ganham âncora emulada (snapshot na queda da junta +
    // re-teleporte na deriva). Suspenso em tp_cooldown
    void update_parked_locks()
    {
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
            // uma validação de span cobre todos os campos lidos (<0x1200)
            if ( readable_span( t, 0x1200 ) < 0x1200 || joint( t ) != nullptr ||
                 ( !is_detached( t ) && !is_plugin_detached( t ) ) )
            {
                cargo_anchor_passive[ i ] = false;
                anchor_valid[ i ] = false;
                anchor_settling[ i ] = false;
                anchor_delay[ i ] = 0;
                continue;
            }
            if ( carry_park[ i ] > 0 || carry_park_pending[ i ] )
            {
                anchor_valid[ i ] = false;
                anchor_settling[ i ] = false;
                anchor_delay[ i ] = 0;
                continue;
            }
            if ( carry_anchor_delay[ i ] > 0 )
            {
                --carry_anchor_delay[ i ];
                anchor_valid[ i ] = false;
                anchor_settling[ i ] = false;
                anchor_delay[ i ] = 0;
                continue;
            }
            // fábrica de record: pula enquanto o singleton do manager (+0x558)
            // não existe - só nasce depois do primeiro park nativo
            void* mgr = readable_ptr( parked_record_mgr ) ? *parked_record_mgr : nullptr;
            if ( record_retry[ i ] > 0 ) --record_retry[ i ];
            const bool wants_record =
                prism::offsets::field< void* >( t, prism::offsets::trailer_parked_state ) == nullptr &&
                ( is_detached( t ) || is_plugin_detached( t ) );
            if ( wants_record && mgr != nullptr ) record_mgr_logged[ i ] = false;
            if ( wants_record && mgr == nullptr && !record_mgr_logged[ i ] )
            {
                record_mgr_logged[ i ] = true;
                CCore::g_instance->info(
                    "parked record manager ainda não existe - c{} segue na âncora emulada", i );
            }
            if ( wants_record &&
                record_retry[ i ] == 0 &&
                parked_record_factory != nullptr && parked_record_mgr != nullptr &&
                mgr != nullptr )
            {
                void* rec = nullptr;
                __try
                {
                    parked_record_factory( t );
                    void** it = mgr != nullptr && readable_ptr(
                        reinterpret_cast< uint8_t* >( mgr ) + 0x748 )
                        ? prism::offsets::field< void** >( mgr, 0x740 ) : nullptr;
                    const uint64_t nrec = mgr != nullptr && readable_ptr(
                        reinterpret_cast< uint8_t* >( mgr ) + 0x748 )
                        ? prism::offsets::field< uint64_t >( mgr, 0x748 ) : 0;
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
                }
                __except ( factory_fault_filter( i, GetExceptionInformation() ) )
                {
                }
                if ( rec != nullptr )
                {
                    prism::offsets::field< void* >( t, prism::offsets::trailer_parked_state ) = rec;
                    prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked ) = 1;
                    prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked_registered ) = 1;
                    record_tried[ i ] = true;
                    record_quiet[ i ] = 0;
                    CCore::g_instance->info( "parked record {:p} created for c{}", rec, i );
                }
                else
                {
                    // falhou: retenta em ~2s
                    record_retry[ i ] = 20;
                    if ( record_quiet[ i ] < 5 )
                    {
                        ++record_quiet[ i ];
                        CCore::g_instance->warning(
                            "parked record factory produced no record for c{} - retrying", i );
                    }
                }
            }
            // record + flag + na scan-list = o re-assert nativo segura a pose:
            // a âncora emulada fica fora pra não brigar com ele
            if ( prism::offsets::field< void* >( t, prism::offsets::trailer_parked_state ) != nullptr &&
                prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked_registered ) != 0 &&
                parked_record_listed( t ) )
            {
                anchor_valid[ i ] = false;
                anchor_settling[ i ] = false;
                anchor_delay[ i ] = 0;
                continue;
            }
            if ( cargo_anchor_passive[ i ] )
            {
                if ( !anchor_valid[ i ] )
                    cargo_trace_event( "anchor-passive", t, static_cast< int >( i ) );
                hook_xform_t current{};
                if ( hook_world( t, current ) )
                {
                    anchor_xform[ i ] = current;
                    anchor_home[ i ] = current;
                    anchor_valid[ i ] = true;
                    anchor_settling[ i ] = false;
                }
                continue;
            }
            // carência pós-queda da junta: load segura poses de staging
            // (0,0,-512) que ancoradas jogariam a carreta no vazio
            if ( anchor_delay[ i ] < 7 )
            {
                ++anchor_delay[ i ];
                continue;
            }
            if ( !anchor_valid[ i ] )
            {
                cargo_trace_event( "anchor-capture", t, static_cast< int >( i ) );
                anchor_valid[ i ] = hook_world( t, anchor_xform[ i ] );
                anchor_home[ i ] = anchor_xform[ i ];
                anchor_settle[ i ] = 0;
                anchor_age[ i ] = 0;
                anchor_settling[ i ] = true;
                anchor_fixes[ i ] = 0;
                continue;
            }
            hook_xform_t now{};
            if ( !hook_world( t, now ) || trailer_teleport_hook == nullptr ) continue;
            ++anchor_age[ i ];
            // reabre a janela de assentamento quando o elo traseiro muda
            {
                void* slave =
                    readable_ptr( static_cast< uint8_t* >( t ) + prism::offsets::trailer_slave )
                        ? prism::offsets::field< void* >( t, prism::offsets::trailer_slave )
                        : nullptr;
                const bool slave_j = slave != nullptr && readable_ptr( slave ) &&
                    readable_ptr( static_cast< uint8_t* >( slave ) + prism::offsets::trailer_physics_joint ) &&
                    joint( slave ) != nullptr;
                if ( slave != anchor_prev_slave[ i ] ||
                     slave_j != anchor_prev_slave_j[ i ] )
                {
                    anchor_prev_slave[ i ] = slave;
                    anchor_prev_slave_j[ i ] = slave_j;
                    anchor_settle[ i ] = 0;
                    anchor_age[ i ] = 0;
                    anchor_settling[ i ] = true;
                }
            }
            const float mx = now.pos.x - anchor_xform[ i ].pos.x;
            const float my = now.pos.y - anchor_xform[ i ].pos.y;
            const float mz = now.pos.z - anchor_xform[ i ].pos.z;
            const quat_t& aq = anchor_xform[ i ].quat;
            const float qdot = aq.w * now.quat.w + aq.x * now.quat.x +
                               aq.y * now.quat.y + aq.z * now.quat.z;
            const float rotd = 1.f - std::fabs( qdot );   // 0 = mesma rotação
            const bool moving = mx * mx + my * my + mz * mz > 0.02f * 0.02f || rotd > 0.001f;
            if ( anchor_settle[ i ] < 5 && anchor_age[ i ] < 40 )
            {
                // fase de assentamento: segue a pose enquanto ela tomba no apoio
                anchor_settling[ i ] = true;
                const float hx = now.pos.x - anchor_home[ i ].pos.x;
                const float hy = now.pos.y - anchor_home[ i ].pos.y;
                const float hz = now.pos.z - anchor_home[ i ].pos.z;
                // rolou demais ou afundou: puxa de volta e reassenta
                if ( ( hx * hx + hz * hz > 1.5f * 1.5f || hy < -0.5f ) &&
                     trailer_teleport_hook != nullptr )
                {
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
            // pose inclinada = apoio real nunca veio: recaptura em vez de congelar caída
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
            // correção só em deriva real (jitter de suspensão não teleporta)
            if ( mx * mx + my * my + mz * mz > 0.08f * 0.08f || rotd > 0.004f )
            {
                // trava emulada: re-asserta a pose guardada inteira (pos + rotação)
                using teleport_fn_t = void __fastcall( void* /*trailer*/, void* /*transform*/ );
                cargo_trace_event( "anchor-repin", t, static_cast< int >( i ) );
                trailer_teleport_hook->get_original< teleport_fn_t >()( t, &anchor_xform[ i ] );
                if ( ++anchor_fixes[ i ] >= 30 )
                {
                    // 30 re-pins seguidos = pose e física divergem: solta o pin
                    // e deixa o settle capturar a pose que a física segura
                    CCore::g_instance->info(
                        "anchor: c{} fought the pose {}x (drift {:.3f}m) - recapturing",
                        i, anchor_fixes[ i ],
                        std::sqrt( mx * mx + my * my + mz * mz ) );
                    anchor_valid[ i ] = false;
                }
            }
        }
    }

    // desestaciona carreta de volta ao convoy: flags caem, o record (+0x1188)
    // fica linkado (o registro de tráfego o referencia). Só peças engatadas
    void unpark_jointed( const bool* in_convoy )
    {
        for ( uint32_t i = 0; i < trailer_count; ++i )
        {
            if ( !in_convoy[ i ] ) continue;
            void* t = trailers[ i ];
            if ( joint( t ) == nullptr ) continue;
            const bool parked_flag = prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked ) != 0;
            const bool parked_rec = prism::offsets::field< void* >( t, prism::offsets::trailer_parked_state ) != nullptr;
            const bool parked_reg = prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked_registered ) != 0;
            if ( !parked_flag && !parked_reg ) continue;
            // flags caem todo frame (a máquina pode re-armar entre passadas);
            // trabalho por junta é chaveado pelo trailer, não pelo slot rotativo
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
            // tira o record da scan-list (listado = manager re-arma flags e
            // re-asserta pose contra a junta viva); o link +0x1188 fica
            if ( parked_rec ) parked_record_unlist( t );
            CCore::g_instance->info( "un-parked trailer {} (flag {} record {})", i, parked_flag, parked_rec );
        }
    }
    //========================================================================
    // Estado da trava de estacionamento (parked lock)
    //========================================================================
    hook_xform_t anchor_xform[ max_trailers ]{};
    hook_xform_t anchor_home[ max_trailers ]{};
    bool anchor_valid[ max_trailers ] = {};
    int anchor_fixes[ max_trailers ] = {};
    bool anchor_settling[ max_trailers ] = {};
    int anchor_settle[ max_trailers ] = {};
    int anchor_age[ max_trailers ] = {};
    int anchor_delay[ max_trailers ] = {};
    // último elo traseiro visto (a trava sai quando um elo da carreta muda)
    void* anchor_prev_slave[ max_trailers ] = {};
    bool anchor_prev_slave_j[ max_trailers ] = {};
    bool record_tried[ max_trailers ] = {};
    int record_retry[ max_trailers ] = {};      // frames até a próxima tentativa
    uint8_t record_quiet[ max_trailers ] = {};  // falhas já logadas (anti-spam)
    bool record_mgr_logged[ max_trailers ] = {}; // diag "mgr inexistente" já logado
    // fábrica nativa de parked-record (0x1407B69E0): constrói o record onde a
    // trava de pose mora; o manager global sai do prólogo dela (+0x740 base, +0x748 count)
    parked_record_factory_fn* parked_record_factory = nullptr;
    void** parked_record_mgr = nullptr;

}
