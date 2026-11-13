#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
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

#include <winver.h>
#pragma comment( lib, "version.lib" )

namespace br_carretas
{
    constexpr uint32_t max_trailers = 20;

    struct va_sentinel { uint64_t old_va; uint64_t va; const uint8_t* sig; uint32_t len; };

    inline const va_sentinel va_sentinels[] = {
        // 001 - leitora de cvar inteira
        { 0x1401CBE10, 0x1401d3180, ( const uint8_t* )"\x40\x57\x48\x83\xec\x20\x48\x83\xb9\x20\x01\x00\x00\x00\x48\x8b\xf9\x74\x39\x0f", 20 },
        // 002 - leitora de cvar float
        { 0x1401DB560, 0x1401e2820, ( const uint8_t* )"\x40\x53\x48\x83\xec\x20\x48\x83\xb9\x20\x01\x00\x00\x00\x48\x8b\xd9\x74\x39\x0f", 20 },
        // 003 - despachante de engate nativo
        { 0x1405DCC20, 0x1405db530, ( const uint8_t* )"\x48\x89\x5c\x24\x18\x48\x89\x74\x24\x20\x55\x48\x8d\xac\x24\xd0\xfd\xff\xff\x48", 20 },
        // 004 - religa âncoras de tráfego
        { 0x140623C90, 0x140622f60, ( const uint8_t* )"\x4c\x6b\x41\x20\x70\x48\x8b\x41\x18\x4c\x03\xc0\x49\x3b\xc0\x74\x2e\x80\x78\x04", 20 },
        // 005 - post_event - dispara evento no jogo
        { 0x140623DA0, 0x140623070, ( const uint8_t* )"\x44\x89\x4c\x24\x20\x4c\x89\x44\x24\x18\x48\x89\x54\x24\x10\x55\x57\x41\x56\x48", 20 },
        // 006 - destrutor de carreta
        { 0x1406359E0, 0x14062a650, ( const uint8_t* )"\x48\x89\x5c\x24\x08\x57\x48\x83\xec\x20\x8b\xda\x48\x8b\xf9\xe8\x6c\xfe\xff\xff", 20 },
        // 007 - engate vfunc98 impl A
        { 0x140639090, 0x14062dd20, ( const uint8_t* )"\x40\x53\x48\x83\xec\x30\x0f\xb6\x44\x24\x68\x48\x8b\xd9\x88\x44\x24\x28\x0f\xb6", 20 },
        // 008 - engate vfunc98 impl B
        { 0x1407890A0, 0x140774710, ( const uint8_t* )"\x48\x8b\xc4\x48\x89\x58\x18\x55\x56\x57\x41\x54\x41\x55\x41\x56\x41\x57\x48\x8d", 20 },
        // 009 - desengate vfunc99 impl
        { 0x140789C20, 0x140775290, ( const uint8_t* )"\x48\x89\x5c\x24\x08\x57\x48\x83\xec\x30\x48\x8b\x05\x0f\xfd\xf9\x02\x48\x8b\xd9", 20 },
        // 010 - set_lines - liga/desliga linhas
        { 0x140874010, 0x1408632b0, ( const uint8_t* )"\x40\x57\x41\x56\x48\x83\xec\x58\x44\x0f\xb6\xf2\x48\x8b\xf9\x3a\x91\x5d\x0e\x00", 20 },
        // 011 - apply_cables - aplica cabos
        { 0x1408762E0, 0x140865580, ( const uint8_t* )"\x48\x89\x5c\x24\x08\x48\x89\x74\x24\x10\x57\x48\x83\xec\x20\x8b\x7c\x24\x50\x41", 20 },
        // 012 - settle da carreta no engate
        { 0x1409FB700, 0x14096df40, ( const uint8_t* )"\x48\x8b\xc4\x53\x48\x81\xec\xa0\x00\x00\x00\x0f\x29\x70\xe8\x48\x8d\x50\x88\xf3", 20 },
        // 013 - set_towed - marca carreta rebocada
        { 0x140642080, 0x1406420b0, ( const uint8_t* )"\x48\x89\x5c\x24\x08\x48\x89\x6c\x24\x10\x48\x89\x74\x24\x18\x57\x48\x81\xec\x80", 20 },
        // 014 - rebuild_lines - reconstrói linhas
        { 0x1408663F0, 0x140866440, ( const uint8_t* )"\x48\x89\x5c\x24\x08\x48\x89\x74\x24\x10\x57\x48\x83\xec\x40\x48\x8b\xfa\x48\x8b", 20 },
        // 015 - notify_decoupled - aviso de desengate
        { 0x1408633D0, 0x140863420, ( const uint8_t* )"\x48\x83\xec\x48\x80\xb9\x5e\x0e\x00\x00\x00\x0f\x84\xc5\x00\x00\x00\x4c\x8b\x05", 20 },
        // 016 - pre_detach - solta a junta no cavalo
        { 0x140864160, 0x1408641b0, ( const uint8_t* )"\x48\x85\xd2\x0f\x84\x9e\x00\x00\x00\x48\x89\x5c\x24\x08\x57\x48\x83\xec\x20\x48", 20 },
        // 017 - pair/unpair de conectores
        { 0x1408771A0, 0x140865600, ( const uint8_t* )"\x48\x89\x4c\x24\x08\x55\x53\x41\x55\x41\x56\x48\x8d\xac\x24\x78\xfe\xff\xff\x48", 20 },
        // 018 - animação do apoio (brace)
        { 0x140636F50, 0x14062bbe0, ( const uint8_t* )"\x40\x55\x41\x56\x41\x57\x48\x83\xec\x30\x4c\x8d\xb9\x30\x12\x00\x00\xf3\x0f\x11", 20 },
        // 019 - setter da máscara de luz
        { 0x1409DB4B0, 0x14094c9d0, ( const uint8_t* )"\x40\x53\x48\x83\xec\x20\x44\x0f\xb6\xc2\x48\x8b\xd9\x38\x91\xa0\x02\x00\x00\x74", 20 },
        // 020 - propagador de luzes
        { 0x140790DC0, 0x14077c3b0, ( const uint8_t* )"\x48\x8b\x81\x58\x10\x00\x00\x89\x91\xa0\x02\x00\x00\x48\x85\xc0\x74\x20\x0f\x1f", 20 },
        // 021 - execute_line do console
        { 0x1401E7F80, 0x1401e7fc0, ( const uint8_t* )"\x48\x89\x4c\x24\x08\x55\x41\x54\x41\x55\x48\x8d\xac\x24\x00\xf4\xff\xff\x48\x81", 20 },
        // 022 - geometria de mangueira (por frame)
        { 0x1408653A0, 0x1408653f0, ( const uint8_t* )"\x48\x89\x5c\x24\x08\x48\x89\x6c\x24\x10\x48\x89\x74\x24\x18\x57\x48\x83\xec\x60", 20 },
        // 023 - gerenciador de tráfego (dado)
        { 0x1436AF7C8, 0x143714FB0, nullptr, 0 },
        // 024 - estado do jogo (dado: 2 = no mundo)
        { 0x143051820, 0x14273B008, nullptr, 0 },
        // 025 - cvar g_trailer_cables_mode (dado)
        { 0x142D485C0, 0x142DA9FA0, nullptr, 0 },
        // 026 - cvar de settle do engate (dado)
        { 0x142D48200, 0x142DA9BE0, nullptr, 0 },
        // 027 - bloco de parâmetros de evento (dado)
        { 0x142733198, 0x1427949E8, nullptr, 0 },
        // 028 - string "@@trailer_coupling@@"
        { 0x14226E368, 0x1422bc6c8, ( const uint8_t* )"\x40\x40\x74\x72\x61\x69\x6c\x65\x72\x5f\x63\x6f\x75\x70\x6c\x69\x6e\x67\x40\x40\x00", 21 },
        // 029 - string "@@trailer_decoupling@@"
        { 0x14226E4B0, 0x1422bc990, ( const uint8_t* )"\x40\x40\x74\x72\x61\x69\x6c\x65\x72\x5f\x64\x65\x63\x6f\x75\x70\x6c\x69\x6e\x67\x40\x40", 22 },
        // 240 - cam_set_target - setter real da âncora (vfunc 31)
        { 0x14063C2D0, 0x140630d80, ( const uint8_t* )"\x4c\x8b\xc1\x48\x8b\x89\xf0\x03\x00\x00\x48\x85\xd2\x74\x18\x8b\x42\x08\xc1\xe8", 20 },
        // 241 - cam_update - update por frame da câmera 2 (vfunc 32)
        { 0x140AC75A0, 0x140a41210, ( const uint8_t* )"\x48\x8b\xc4\x55\x53\x41\x57\x48\x8d\x6c\x24\x80\x48\x81\xec\x80\x01\x00\x00\x0f", 20 },
        // 242 - combo_length - comprimento do comboio (anda cadeia slave)
        { 0x140789AB0, 0x140775120, ( const uint8_t* )"\x48\x89\x5c\x24\x08\x57\x48\x83\xec\x70\x48\x8b\x99\x58\x10\x00\x00\x48\x8b\xf9", 20 },
        // 243 - posição de engate usada pelo combo_length
        { 0x140789880, 0x140774ef0, ( const uint8_t* )"\x4c\x8b\xdc\x49\x89\x5b\x10\x49\x89\x73\x18\x57\x48\x81\xec\xb0\x00\x00\x00\x0f", 20 },
        // 244 - gerenciador de câmeras (dado)
        { 0x1436AF830, 0x143714FB8, nullptr, 0 },
        // 245 - vtable da câmera 2 vehicle_behind_rotation_camera (dado)
        { 0x1423126F0, 0x142355138, nullptr, 0 },
        // 246 - despachante de desengate nativo
        { 0x1405DD410, 0x1405dbd20, ( const uint8_t* )"\x48\x89\x6c\x24\x18\x48\x89\x74\x24\x20\x57\x48\x81\xec\xa0\x00\x00\x00\x41\x0f", 20 },
        // 247 - correção de flags do ator (anda cadeia, eixo levantado)
        { 0x1405DCBC0, 0x1405db4d0, ( const uint8_t* )"\x48\x8b\x91\xc8\x00\x00\x00\x45\x32\xc0\x48\x85\xd2\x74\x2c\x4c\x8b\x8a\xc8\x06", 20 },
        // 248 - teleporte do ator (hook: bloqueia com carreta solta)
        { 0x1405DE1E0, 0x1405dcaf0, ( const uint8_t* )"\x44\x88\x4c\x24\x20\x44\x88\x44\x24\x18\x55\x53\x41\x54\x41\x55\x41\x56\x48\x8d", 20 },
        // 249 - teleporte por carreta
        { 0x140786320, 0x1407864d0, ( const uint8_t* )"\x48\x89\x5c\x24\x08\x48\x89\x6c\x24\x10\x48\x89\x74\x24\x18\x48\x89\x7c\x24\x20", 20 },
        // 250 - trailer_state_apply - reaplica estado salvo no load
        { 0x1405E14D1, 0x1405dfe20, ( const uint8_t* )"\x48\x89\x74\x24\x18\x48\x89\x7c\x24\x20\x55\x48\x8d\x6c\x24\xa9\x48\x81\xec\x90", 20 },
        // 251 - fábrica de registro de carreta estacionada
        { 0x1407B69E0, 0x14070a160, ( const uint8_t* )"\x48\x89\x5c\x24\x08\x48\x89\x74\x24\x18\x57\x48\x83\xec\x20\x48\x8b\x35\x9a\xae", 20 },
        // 252 - gerenciador de registros estacionados (dado)
        { 0x1436AF828, 0x143714FB0, nullptr, 0 },
        // 253 - purecall do CRT - alvo do crash tracer
        { 0x141D23610, 0x141d57200, ( const uint8_t* )"\x48\x83\xec\x28\x48\x8b\x05\xed\xf1\x33\x01\x48\x85\xc0\x74\x06\xff\x15\xa2\xc6", 20 },
        // 254 - RIP da leitura de curva de navegação (emula xmm5=0)
        { 0x141BEA1C5, 0x141a95a0b, ( const uint8_t* )"\xf3\x0f\x10\x2c\x8f\xf3\x0f\x10\x74\x8f\x08\x41\x0f\xb6\x04\x00\xf3\x0f\x59\xdd", 20 },
        // 255 - retorno do broadcast de freio real (+0x180)
        { 0x14064B7EF, 0x1406403af, ( const uint8_t* )"\x48\x8b\x8b\x80\x15\x00\x00\x0f\xb6\x93\x64\x07\x00\x00\x48\x8b\x01\xff\x90\x88", 20 },
        // 256 - retorno do broadcast de freio forçado (+0x188)
        { 0x14064B806, 0x1406403c6, ( const uint8_t* )"\x0f\x28\x7c\x24\x50\x44\x0f\x28\x44\x24\x40\x48\x83\xc4\x70\x5b\xc3\xcc\xcc\xcc", 20 },
        // 257 - cvar g_traffic (dado - candidato, sem nome confirmado)
        { 0x142D45500, 0x142DA6EE0, nullptr, 0 },
        // 266 - phys_link - link físico (recusa ciclo)
        { 0x140919680, 0x1412be360, ( const uint8_t* )"\x48\x89\x5c\x24\x10\x57\x48\x83\xec\x40\x48\x8b\xfa\x48\x8b\xd9\x48\x85\xd2\x74", 20 },
        // 267 - joint_walk - caminhada de juntas (auto-recursiva)
        { 0x1409555F0, 0x1412bf010, ( const uint8_t* )"\x48\x8b\xc4\x57\x48\x81\xec\xe0\x00\x00\x00\x48\x89\x58\x08\x48\x8b\xf9\x48\x89\x70", 20 },
        // 269 - lane_obstacle_insert - inserção ordenada de obstáculo
        { 0x14094DFC0, 0x1412b7dd0, ( const uint8_t* )"\x48\x89\x5c\x24\x20\x56\x48\x83\xec\x60\x8b\x41\x10\x48\x8b\xd9\xc1\xe8\x15\x24", 20 },
    };

    // 030 - confere o endereço fixo contra a assinatura (cache por chamada)
    inline uint64_t validated_va( const uint64_t old_va )
    {
        static constexpr uint32_t cache_n = 80;
        static uint64_t cache_old[ cache_n ] = {};
        static uint64_t cache_val[ cache_n ] = {};
        static uint32_t cache_used = 0;
        for ( uint32_t i = 0; i < cache_used; ++i )
        {
            if ( cache_old[ i ] == old_va ) return cache_val[ i ];
        }
        uint64_t v = 0;
        for ( const auto& s : va_sentinels )
        {
            if ( s.old_va == old_va )
            {

                v = memory::offset_bytes_match( s.va, s.sig, s.len )
                    ? memory::get_address_from_offset( s.va ) : 0;
                break;
            }
        }
        if ( cache_used < cache_n )
        {
            cache_old[ cache_used ] = old_va;
            cache_val[ cache_used ] = v;
            ++cache_used;
        }
        if ( v == 0 && CCore::g_instance != nullptr )
        {
            CCore::g_instance->warning(
                "endereco fixo 0x{:X} nao confirmado neste exe - recurso desligado", old_va );
        }
        return v;
    }

    // 031 - portão do build: todas as assinaturas batem com o exe
    inline bool expected_build()
    {
        static const int8_t verdict = []() -> int8_t {
            for ( const auto& s : va_sentinels )
            {
                if ( !memory::offset_bytes_match( s.va, s.sig, s.len ) )
                {
                    if ( CCore::g_instance != nullptr )
                    {
                        CCore::g_instance->error(
                            "Build do jogo nao reconhecido (enderecos fixos divergiram) - recursos de endereco fixo desligados" );
                    }
                    return 0;
                }
            }
            return 1;
        }();
        return verdict == 1;
    }

    // 032 - ponteiro é legível e comprometido
    inline bool readable_ptr( const void* p )
    {
        if ( p == nullptr ) return false;
        MEMORY_BASIC_INFORMATION mbi{};
        return VirtualQuery( p, &mbi, sizeof( mbi ) ) != 0 && mbi.State == MEM_COMMIT &&
            ( mbi.Protect & ( PAGE_NOACCESS | PAGE_GUARD ) ) == 0;
    }

    // 033 - quantos bytes legíveis seguem o ponteiro
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

    // 034 - ponteiro parece um objeto do jogo (vtable dentro do exe)
    inline bool plausible_object( const void* p )
    {
        if ( !readable_ptr( p ) ) return false;
        const auto base = reinterpret_cast< uint64_t >( GetModuleHandle( nullptr ) );
        const auto vtbl = *reinterpret_cast< const uint64_t* >( p );
        return vtbl >= base && vtbl < base + 0x4000000;
    }

    // 035 - o slot da vtable aponta pra código do exe
    inline bool plausible_vfunc( const void* obj, const uint32_t slot )
    {
        if ( !plausible_object( obj ) ) return false;
        const auto* vtbl = *reinterpret_cast< void* const* const* >( obj );
        const auto fn = reinterpret_cast< uint64_t >( vtbl[ slot ] );
        const auto base = reinterpret_cast< uint64_t >( GetModuleHandle( nullptr ) );
        return fn >= base && fn < base + 0x2000000;
    }

    // 036 - o ponteiro é objeto-carreta de verdade (vfunc 99)
    inline bool is_trailer_obj( const void* p )
    {
        if ( !plausible_object( p ) ) return false;
        const auto* vtbl = *reinterpret_cast< const uint64_t* const* >( p );
        return vtbl[ 99 ] == validated_va( 0x140789C20 );
    }

    template < typename F, typename... A >
    // 037 - chama um slot da vtable
    auto vcall( void* object, const uint32_t slot, A&&... args )
    {
        auto* vtbl = *reinterpret_cast< void*** >( object );
        return reinterpret_cast< F* >( vtbl[ slot ] )( object, std::forward< A >( args )... );
    }

    // 038 - junta física da carreta (+0xFE0)
    inline void* joint( void* trailer )
    {
        return prism::offsets::field< void* >( trailer, prism::offsets::trailer_physics_joint );
    }

    // 270 - escreve o link slave só se a cadeia resultante não fechar ciclo
    inline bool slave_link_write( void* owner, void* child )
    {
        if ( !readable_ptr( owner ) || child == owner ) return false;
        if ( child != nullptr )
        {
            void* seen[ max_trailers * 4 ] = {};
            uint32_t n = 0;
            for ( void* t = child; t != nullptr; )
            {
                if ( t == owner ) return false;
                bool dup = false;
                for ( uint32_t k = 0; k < n; ++k ) dup |= seen[ k ] == t;
                if ( dup || n >= max_trailers * 4 ) return false;
                seen[ n++ ] = t;
                if ( !readable_ptr( t ) ||
                     !readable_ptr( static_cast< uint8_t* >( t ) + prism::offsets::trailer_slave ) )
                {
                    break;
                }
                t = prism::offsets::field< void* >( t, prism::offsets::trailer_slave );
            }
        }
        prism::offsets::field< void* >( owner, prism::offsets::trailer_slave ) = child;
        return true;
    }

    // 039 - dados de eixo direcional da carreta
    inline prism::vehicle_wheel_steering_data_t* steering_data( void* trailer )
    {
        return trailer == nullptr ? nullptr
            : prism::offsets::field< prism::vehicle_wheel_steering_data_t* >(
                trailer, prism::offsets::trailer_steering_data );
    }

    inline std::shared_ptr< CFunctionHook > brake_chain_setter_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > brake_setter_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > svc_brake_chain_hook = nullptr;
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
    inline std::shared_ptr< CFunctionHook > trailer_dtor_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > set_lines_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > apply_cables_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > hose_update_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > rebuild_lines_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > joint_walk_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > phys_link_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > purecall_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > trigger_action_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > trailer_disconnect_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > trailer_connect_hook = nullptr;
    inline std::shared_ptr< CFunctionHook > trailer_connect_hook2 = nullptr;

    inline void* trailers[ max_trailers ] = {};
    inline uint32_t trailer_count = 0;

    // 040 - posição da carreta na lista atual
    inline int index_of( const void* trailer )
    {
        for ( uint32_t i = 0; i < trailer_count; ++i )
            if ( trailers[ i ] == trailer ) return static_cast< int >( i );
        return -1;
    }

    inline void* detached_trailers[ max_trailers ] = {};

    // 041 - carreta tem marca de desatrelada
    inline bool is_detached( const void* trailer )
    {
        for ( const void* d : detached_trailers ) if ( d == trailer ) return true;
        return false;
    }

    // 042 - marca a carreta como desatrelada
    inline void mark_detached( void* trailer )
    {
        for ( void*& d : detached_trailers ) if ( d == nullptr ) { d = trailer; return; }
    }

    // 043 - tira a marca de desatrelada
    inline void unmark_detached( const void* trailer )
    {
        for ( void*& d : detached_trailers ) if ( d == trailer ) d = nullptr;
    }

    inline void* plugin_detached[ max_trailers ] = {};

    // 044 - desatrelada pelos controles do plugin
    inline bool is_plugin_detached( const void* trailer )
    {
        for ( const void* d : plugin_detached ) if ( d == trailer ) return true;
        return false;
    }

    // 045 - marca desatrelada pelo plugin
    inline void mark_plugin_detached( void* trailer )
    {
        for ( void*& d : plugin_detached ) if ( d == nullptr ) { d = trailer; return; }
    }

    // 046 - tira a marca do plugin
    inline void unmark_plugin_detached( const void* trailer )
    {
        for ( void*& d : plugin_detached ) if ( d == trailer ) d = nullptr;
    }

    // 047 - o ponteiro é uma carreta rastreada
    inline bool is_tracked_trailer( const void* p )
    {
        for ( uint32_t i = 0; i < trailer_count; ++i )
            if ( trailers[ i ] == p ) return true;
        return is_detached( p ) || is_plugin_detached( p );
    }

    inline void* truck_towed = nullptr;

    inline void* pending_native_detach = nullptr;
    inline uint32_t pending_native_age = 0;

    inline bool canon_was_jointed[ max_trailers ] = {};

    inline bool lines_torn[ max_trailers ] = {};

    inline std::atomic< bool > native_chain_call_active{ false };

    inline std::recursive_mutex claim_write_mutex;

    inline std::atomic< bool > convoy_refresh_requested{ false };

    inline uint32_t canon_jointless_frames[ max_trailers ] = {};

    inline float saved_traffic = -1.f;

    inline uint32_t expected_trailers = 0;

    inline std::atomic< bool > traffic_detach_done{ false };

    enum class pending_kind { none, connect, disconnect, restore };

    struct pending_op
    {
        pending_kind kind = pending_kind::none;
        void* trailer = nullptr;
        void* parent = nullptr;
        bool parent_is_truck = false;

        uint32_t delay = 0;
    };

    inline pending_op pending;

    inline char pending_cmd[ 64 ] = {};
    inline std::atomic< bool > pending_cmd_queued{ false };

    // 048 - enfileira um comando de console
    inline void queue_cmd( const char* text )
    {
        std::strncpy( pending_cmd, text, sizeof( pending_cmd ) - 1 );
        pending_cmd[ sizeof( pending_cmd ) - 1 ] = '\0';
        pending_cmd_queued.store( true, std::memory_order_release );
    }

    // 049 - enfileira engate/desengate pro thread de física
    inline bool queue_op( const pending_kind kind, void* trailer, void* parent,
        const bool parent_is_truck, const uint32_t delay = 0 )
    {
        if ( pending.kind != pending_kind::none )
        {
            CCore::g_instance->warning( "Outro engate/desengate ainda está em andamento" );
            return false;
        }
        pending = { kind, trailer, parent, parent_is_truck, delay };
        CCore::g_instance->info( "fn049 fila+ kind={} trailer={:p} parent={:p} truck={} delay={}",
                                 static_cast< int >( kind ), trailer, parent,
                                 parent_is_truck ? 1 : 0, delay );
        return true;
    }

    inline uint64_t sim_heartbeat = 0;
    inline uint64_t sim_heartbeat_seen = 0;
    inline uint32_t sim_dead_frames = 0;
    inline uint64_t sim_thread_id = 0;

    // 050 - batimento do thread de física
    inline void pump_sim_heartbeat()
    {
        sim_thread_id = GetCurrentThreadId();
        ++sim_heartbeat;
    }

    // 051 - vigia se o sim continua rodando
    inline void watch_sim_heartbeat()
    {
        const uint64_t hb = sim_heartbeat;
        sim_dead_frames = ( hb == sim_heartbeat_seen ) ? sim_dead_frames + 1 : 0;
        sim_heartbeat_seen = hb;
    }

    // 052 - sim está parado - seguro executar inline
    inline bool sim_is_quiet()
    {
        return GetCurrentThreadId() == sim_thread_id || sim_dead_frames >= 30;
    }

    struct hook_xform_t
    {
        float3_t pos;
        float pad;
        quat_t quat;
    };

    // 053 - transform do veículo no mundo (vfunc 0xF8)
    inline bool hook_world( void* vehicle, hook_xform_t& out )
    {
        alignas( 16 ) uint8_t buf[ 0x20 ]{};

        if ( !plausible_vfunc( vehicle, 0xF8 / 8 ) ) return false;
        void* p = nullptr;
        __try { p = vcall< prism::vehicle_get_transform_fn >( vehicle, 0xF8 / 8, buf ); }
        __except ( EXCEPTION_EXECUTE_HANDLER ) { return false; }
        if ( p == nullptr || !readable_ptr( p ) ) return false;
        std::memcpy( &out, p, sizeof( out ) );
        return std::isfinite( out.pos.x ) && std::isfinite( out.pos.y ) && std::isfinite( out.pos.z ) &&
            std::fabs( out.pos.x ) < 1e6f && std::fabs( out.pos.z ) < 1e6f;
    }

    extern void* canon[ max_trailers ];

    inline void* tp_fault_trailer = nullptr;
    inline uint64_t tp_fault_ms = 0;

    // 262 - despeja os campos crus das listas internas quando o teleporte falha
    inline void dump_node_lists( void* trailer )
    {
        if ( !readable_ptr( trailer ) ) return;
        __try
        {
            const auto* vtbl = *reinterpret_cast< const uint64_t* const* >( trailer );
            CCore::g_instance->dlog( settings::logcat::teleport,
                "node dump {:p} trailer={} vtbl={:p} v99={:#x}", trailer,
                is_trailer_obj( trailer ), static_cast< const void* >( vtbl ), vtbl[ 99 ] );
            for ( int off = 0x28; off <= 0x78; off += 8 )
            {
                void* v = *reinterpret_cast< void** >( static_cast< uint8_t* >( trailer ) + off );
                CCore::g_instance->dlog( settings::logcat::teleport, "  +0x{:X} = {:p}", off, v );
            }
            for ( int li = 0; li < 2; ++li )
            {
                auto* desc = static_cast< uint8_t* >( trailer ) + 0x30 + li * 0x28;
                void** begin = *reinterpret_cast< void*** >( desc + 0x8 );
                if ( readable_span( begin, 0x80 ) < 0x80 ) continue;
                CCore::g_instance->dlog( settings::logcat::teleport,
                    "list {} begin={:p}:", li, static_cast< void* >( begin ) );
                for ( int r = 0; r < 16; ++r )
                {
                    CCore::g_instance->dlog( settings::logcat::teleport, "  [{}] {:p}", r, begin[ r ] );
                }
            }
            for ( uint32_t i = 0; i < max_trailers; ++i )
            {
                void* healthy = canon[ i ];
                if ( healthy == trailer || !is_trailer_obj( healthy ) ) continue;
                CCore::g_instance->dlog( settings::logcat::teleport,
                    "healthy c{} {:p}:", i, healthy );
                for ( int off = 0x30; off <= 0x70; off += 8 )
                {
                    void* v = *reinterpret_cast< void** >( static_cast< uint8_t* >( healthy ) + off );
                    CCore::g_instance->dlog( settings::logcat::teleport, "  +0x{:X} = {:p}", off, v );
                }
                break;
            }
        }
        __except ( EXCEPTION_EXECUTE_HANDLER )
        {
            CCore::g_instance->warning( "node list dump faulted on {:p}", trailer );
        }
    }

    // 261 - remove nós-filhos mortos das duas listas internas que o teleporte percorre (+0x30/+0x58)
    inline int repair_node_lists( void* trailer )
    {
        if ( !readable_ptr( trailer ) ) return 0;
        int removed = 0;
        __try
        {
            for ( int li = 0; li < 2; ++li )
            {
                auto* desc = static_cast< uint8_t* >( trailer ) + 0x30 + li * 0x28;
                void** begin = *reinterpret_cast< void*** >( desc + 0x8 );
                const uint64_t count = *reinterpret_cast< uint64_t* >( desc + 0x10 );
                if ( begin == nullptr || count == 0 || count > 0x1000 ) continue;
                if ( readable_span( begin, count * sizeof( void* ) ) < count * sizeof( void* ) ) continue;
                uint64_t w = 0;
                for ( uint64_t r = 0; r < count; ++r )
                {
                    void* node = begin[ r ];
                    bool ok = readable_ptr( node );
                    if ( ok )
                    {
                        void* sub = *reinterpret_cast< void** >( static_cast< uint8_t* >( node ) + 0x20 );
                        ok = readable_ptr( sub );
                    }
                    if ( ok ) begin[ w++ ] = node; else ++removed;
                }
                *reinterpret_cast< uint64_t* >( desc + 0x10 ) = w;
            }
        }
        __except ( EXCEPTION_EXECUTE_HANDLER ) {}
        return removed;
    }

    inline void* tp_wait_trailer = nullptr;
    inline uint64_t tp_wait_ms = 0;

    // 265 - confere se as listas de nós internas da carreta estão prontas pro teleporte
    inline bool node_lists_sane( void* trailer )
    {
        if ( !readable_ptr( trailer ) ) return false;
        __try
        {
            for ( int li = 0; li < 2; ++li )
            {
                auto* desc = static_cast< uint8_t* >( trailer ) + 0x30 + li * 0x28;
                void** begin = *reinterpret_cast< void*** >( desc + 0x8 );
                const uint64_t count = *reinterpret_cast< uint64_t* >( desc + 0x10 );
                if ( count > 0x1000 ) return false;
                if ( count == 0 ) continue;
                if ( readable_span( begin, count * sizeof( void* ) ) < count * sizeof( void* ) ) return false;
                for ( uint64_t r = 0; r < count; ++r )
                {
                    void* node = begin[ r ];
                    if ( !readable_ptr( node ) ) return false;
                    if ( !readable_ptr( *reinterpret_cast< void** >( static_cast< uint8_t* >( node ) + 0x20 ) ) )
                        return false;
                }
            }
            return true;
        }
        __except ( EXCEPTION_EXECUTE_HANDLER ) { return false; }
    }

    inline bool cargo_freeze();

    // 260 - teleporte por-carreta protegido: espera as listas internas estarem prontas, repara e tenta de novo se falhar
    inline bool teleport_safe( void* trailer, void* xform )
    {
        if ( trailer_teleport_hook == nullptr || !is_trailer_obj( trailer ) ||
             cargo_freeze() ) return false;
        if ( !node_lists_sane( trailer ) )
        {
            repair_node_lists( trailer );
        }
        if ( !node_lists_sane( trailer ) )
        {
            if ( tp_wait_trailer != trailer || tp_wait_ms == 0 || GetTickCount64() - tp_wait_ms > 5000 )
            {
                tp_wait_trailer = trailer;
                tp_wait_ms = GetTickCount64();
                dump_node_lists( trailer );
                CCore::g_instance->dlog( settings::logcat::teleport,
                    "trailer teleport deferred on {:p} - node lists not ready", trailer );
            }
            return false;
        }
        using teleport_fn_t = void __fastcall( void*, void* );
        __try
        {
            trailer_teleport_hook->get_original< teleport_fn_t >()( trailer, xform );
            return true;
        }
        __except ( EXCEPTION_EXECUTE_HANDLER ) {}
        const int removed = repair_node_lists( trailer );
        if ( removed > 0 )
        {
            __try
            {
                trailer_teleport_hook->get_original< teleport_fn_t >()( trailer, xform );
                CCore::g_instance->dlog( settings::logcat::teleport,
                    "trailer teleport {:p}: removed {} dead node(s) - re-seated", trailer, removed );
                return true;
            }
            __except ( EXCEPTION_EXECUTE_HANDLER ) {}
        }
        if ( tp_fault_trailer != trailer || tp_fault_ms == 0 || GetTickCount64() - tp_fault_ms > 5000 )
        {
            tp_fault_trailer = trailer;
            tp_fault_ms = GetTickCount64();
            dump_node_lists( trailer );
            CCore::g_instance->warning( "trailer teleport faulted on {:p} - skipped", trailer );
        }
        return false;
    }

    // 054 - transform do cavalo
    inline bool truck_xform( void* game_actor, uint8_t* out32 )
    {
        void* truck = game_actor != nullptr
            ? prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck ) : nullptr;
        if ( truck == nullptr ) return false;
        return vcall< prism::vehicle_get_transform_fn >( truck, 0xF8 / 8, out32 ) != nullptr;
    }

    inline bool detached_idx[ max_trailers ] = {};
    inline hook_xform_t parked_xform[ max_trailers ] = {};
    inline bool parked_xform_valid[ max_trailers ] = {};

    inline uint32_t rejoined_frames[ max_trailers ] = {};

    inline uint64_t carry_arm_tick = 0;
    inline bool carry_hold[ max_trailers ] = {};

    inline bool own_head[ max_trailers ] = {};
    inline uint32_t carry_brake_frames = 0;
    inline uint32_t carry_frames = 0;

    inline int carry_park[ max_trailers ] = {};

    inline std::atomic< bool > parking_brake_on{ false };
    inline std::atomic< bool > parking_brake_known{ false };
    inline uint64_t cargo_seq_ms = 0;
    inline bool cargo_respawn_done = false;

    inline uint64_t actor_tp_ms = 0;
    // 055 - relógio em milissegundos
    inline uint64_t steady_ms()
    {
        return static_cast< uint64_t >( std::chrono::duration_cast< std::chrono::milliseconds >(
            std::chrono::steady_clock::now().time_since_epoch() ).count() );
    }

    // 056 - janela de carga/descarga (20s após teleporte)
    inline bool in_cargo_window()
    {
        return cargo_seq_ms != 0 && steady_ms() - cargo_seq_ms < 20000;
    }
    // 272 - congela escritas do plugin enquanto o jogo refaz as carretas na carga
    inline bool cargo_freeze()
    {
        return in_cargo_window() && !cargo_respawn_done;
    }
    // 057 - regra do engate: freio de mão puxado (desativada - sempre libera)
    inline bool handbrake_ok( const bool cargo_exempt = true )
    {
        (void)cargo_exempt;
        return true;
    }

    inline void* chase_camera = nullptr;

    float hook_gap( void* trailer, void* parent, bool parent_is_truck );
    bool hook_far( void* trailer, void* parent, bool parent_is_truck );

    inline bool held[ settings::action_count ] = {};

    // 058 - o jogo está em primeiro plano
    inline bool game_has_focus()
    {
        DWORD pid = 0;
        GetWindowThreadProcessId( GetForegroundWindow(), &pid );
        return pid == GetCurrentProcessId();
    }

    // 059 - a tecla configurada está pressionada
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

    namespace ui
    {

        // 060 - cor hex linearizada pra UI
        inline ImVec4 pdv( const uint32_t hex )
        {
            const auto lin = []( const uint32_t c )
            {
                return std::pow( ( c & 0xFF ) / 255.f, 2.2f );
            };
            return ImVec4( lin( hex >> 16 ), lin( hex >> 8 ), lin( hex ), 1.f );
        }

        // 061 - cor hex convertida pra ImU32
        inline ImU32 pdv32( const uint32_t hex )
        {
            const auto lin = []( const uint32_t c )
            {
                return static_cast< int >( std::pow( ( c & 0xFF ) / 255.f, 2.2f ) * 255.f + 0.5f );
            };
            return IM_COL32( lin( hex >> 16 ), lin( hex >> 8 ), lin( hex ), 255 );
        }

        // 062 - botão liga/desliga da UI
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

        // 063 - alinha o próximo widget à direita
        inline void right_side( const float width )
        {
            ImGui::SameLine( 0.f, 0.f );
            ImGui::SetCursorPosX( ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - width );
        }

        // 064 - texto cinza com quebra de linha
        inline void text_disabled_wrapped( const char* text )
        {
            ImGui::PushStyleColor( ImGuiCol_Text,
                ImGui::GetStyleColorVec4( ImGuiCol_TextDisabled ) );
            ImGui::TextWrapped( "%s", text );
            ImGui::PopStyleColor();
        }
    }

    // declarações das funções que vivem nos outros arquivos
    void lift_register_channels();
    void lift_axles_update( void* game_actor );
    void release_all_axles();
    void brake_register_hooks();
    void brake_boost_update();
    void steering_install();
    void steering_reset();
    void air_register_channels();
    void update_air( float dt );
    bool coupling_install();
    void coupling_register_channels();
    void coupling_parked_update();
    void render_cargo_restore_option();
    bool block_camera_zero_teleport( void* arg );
    void workshop_respawn_update();
    void workshop_respawn_discard();
    bool load_game_active();
    void load_game_update();
    void garage_update();
    extern void* attach_target;
    extern void* canon[ max_trailers ];
    int canon_index_of( const void* t );
    void release_detach_marks( void* t );
    void* canon_coupled_predecessor( void* t);
    bool trailer_is_live( const void* t );
    void light_watch_tick();

    // 236 - tabela global função↔número (registro estático por arquivo)
    struct fn_index_entry { uintptr_t addr; uint16_t num; const char* name; };
    inline std::vector< fn_index_entry >& fn_index_table()
    {
        static std::vector< fn_index_entry > v;
        return v;
    }

    // 237 - endereço de qualquer função (livre ou membro)
    template< typename F >
    inline uintptr_t fn_addr_of( F f )
    {
        uintptr_t v = 0;
        static_assert( sizeof( v ) >= sizeof( f ) );
        std::memcpy( &v, &f, sizeof( f ) );
        return v;
    }

    // 238 - registra uma função no índice
    inline void fn_index_add( const uintptr_t a, const uint16_t n, const char* s )
    {
        fn_index_table().push_back( { a, n, s } );
    }

    // 239 - RIP -> função registrada mais próxima
    inline const fn_index_entry* fn_index_lookup( const uint64_t rip )
    {
        const fn_index_entry* best = nullptr;
        for ( const auto& e : fn_index_table() )
        {
            if ( e.addr <= rip && ( best == nullptr || e.addr > best->addr ) )
            {
                best = &e;
            }
        }
        return best != nullptr && rip - best->addr < 0x10000 ? best : nullptr;
    }
}
