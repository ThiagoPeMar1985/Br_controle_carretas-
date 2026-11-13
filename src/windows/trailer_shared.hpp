#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
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

#include <winver.h>
#pragma comment( lib, "version.lib" )

namespace br_carretas
{
    constexpr uint32_t max_trailers = 20;

    struct va_sentinel { uint64_t old_va; uint64_t va; const uint8_t* sig; uint32_t len; };

    inline const va_sentinel va_sentinels[] = {
        // 001 - leitora de cvar inteira
        { 0x1401CBE10, 0x1401D3140, ( const uint8_t* )"\x40\x57\x48\x83\xec\x20\x48\x83\xb9\x20\x01\x00\x00\x00\x48\x8b\xf9\x74\x39\x0f", 20 },
        // 002 - leitora de cvar float
        { 0x1401DB560, 0x1401E27E0, ( const uint8_t* )"\x40\x53\x48\x83\xec\x20\x48\x83\xb9\x20\x01\x00\x00\x00\x48\x8b\xd9\x74\x39\x0f", 20 },
        // 003 - despachante de engate nativo
        { 0x1405DCC20, 0x1405DB4F0, ( const uint8_t* )"\x48\x89\x5c\x24\x18\x48\x89\x74\x24\x20\x55\x48\x8d\xac\x24\xd0\xfd\xff\xff\x48", 20 },
        // 004 - religa âncoras de tráfego
        { 0x140623C90, 0x140622F30, ( const uint8_t* )"\x4c\x6b\x41\x20\x70\x48\x8b\x41\x18\x4c\x03\xc0\x49\x3b\xc0\x74\x2e\x80\x78\x04", 20 },
        // 005 - post_event - dispara evento no jogo
        { 0x140623DA0, 0x140623040, ( const uint8_t* )"\x44\x89\x4c\x24\x20\x4c\x89\x44\x24\x18\x48\x89\x54\x24\x10\x55\x57\x41\x56\x48", 20 },
        // 006 - destrutor de carreta
        { 0x1406359E0, 0x14062A620, ( const uint8_t* )"\x48\x89\x5c\x24\x08\x57\x48\x83\xec\x20\x8b\xda\x48\x8b\xf9\xe8\x6c\xfe\xff\xff", 20 },
        // 007 - engate vfunc98 impl A
        { 0x140639090, 0x14062DCF0, ( const uint8_t* )"\x40\x53\x48\x83\xec\x30\x0f\xb6\x44\x24\x68\x48\x8b\xd9\x88\x44\x24\x28\x0f\xb6", 20 },
        // 008 - engate vfunc98 impl B
        { 0x1407890A0, 0x1407746E0, ( const uint8_t* )"\x48\x8b\xc4\x48\x89\x58\x18\x55\x56\x57\x41\x54\x41\x55\x41\x56\x41\x57\x48\x8d", 20 },
        // 009 - desengate vfunc99 impl
        { 0x140789C20, 0x140775260, ( const uint8_t* )"\x48\x89\x5c\x24\x08\x57\x48\x83\xec\x30\x48\x8b\x05\xdf\xfc\xf9\x02\x48\x8b\xd9", 20 },
        // 010 - set_lines - liga/desliga linhas
        { 0x140874010, 0x140863280, ( const uint8_t* )"\x40\x57\x41\x56\x48\x83\xec\x58\x44\x0f\xb6\xf2\x48\x8b\xf9\x3a\x91\x5d\x0e\x00", 20 },
        // 011 - apply_cables - aplica cabos
        { 0x1408762E0, 0x140865550, ( const uint8_t* )"\x48\x89\x5c\x24\x08\x48\x89\x74\x24\x10\x57\x48\x83\xec\x20\x8b\x7c\x24\x50\x41", 20 },
        // 012 - settle da carreta no engate
        { 0x1409FB700, 0x14096DF10, ( const uint8_t* )"\x48\x8b\xc4\x53\x48\x81\xec\xa0\x00\x00\x00\x0f\x29\x70\xe8\x48\x8d\x50\x88\xf3", 20 },
        // 013 - set_towed - marca carreta rebocada
        { 0x140642080, 0x140642080, ( const uint8_t* )"\x48\x89\x5c\x24\x08\x48\x89\x6c\x24\x10\x48\x89\x74\x24\x18\x57\x48\x81\xec\x80", 20 },
        // 014 - rebuild_lines - reconstrói linhas
        { 0x1408663F0, 0x140866410, ( const uint8_t* )"\x48\x89\x5c\x24\x08\x48\x89\x74\x24\x10\x57\x48\x83\xec\x40\x48\x8b\xfa\x48\x8b", 20 },
        // 015 - notify_decoupled - aviso de desengate
        { 0x1408633D0, 0x1408633F0, ( const uint8_t* )"\x48\x83\xec\x48\x80\xb9\x5e\x0e\x00\x00\x00\x0f\x84\xc5\x00\x00\x00\x4c\x8b\x05", 20 },
        // 016 - pre_detach - solta a junta no cavalo
        { 0x140864160, 0x140864180, ( const uint8_t* )"\x48\x85\xd2\x0f\x84\x9e\x00\x00\x00\x48\x89\x5c\x24\x08\x57\x48\x83\xec\x20\x48", 20 },
        // 017 - pair/unpair de conectores
        { 0x1408771A0, 0x1408655D0, ( const uint8_t* )"\x48\x89\x4c\x24\x08\x55\x53\x41\x55\x41\x56\x48\x8d\xac\x24\x78\xfe\xff\xff\x48", 20 },
        // 018 - animação do apoio (brace)
        { 0x140636F50, 0x14062BBB0, ( const uint8_t* )"\x40\x55\x41\x56\x41\x57\x48\x83\xec\x30\x4c\x8d\xb9\x30\x12\x00\x00\xf3\x0f\x11", 20 },
        // 019 - setter da máscara de luz
        { 0x1409DB4B0, 0x14094C9A0, ( const uint8_t* )"\x40\x53\x48\x83\xec\x20\x44\x0f\xb6\xc2\x48\x8b\xd9\x38\x91\xa0\x02\x00\x00\x74", 20 },
        // 020 - propagador de luzes
        { 0x140790DC0, 0x14077C380, ( const uint8_t* )"\x48\x8b\x81\x58\x10\x00\x00\x89\x91\xa0\x02\x00\x00\x48\x85\xc0\x74\x20\x0f\x1f", 20 },
        // 021 - execute_line do console
        { 0x1401E7F80, 0x1401E7F80, ( const uint8_t* )"\x48\x89\x4c\x24\x08\x55\x41\x54\x41\x55\x48\x8d\xac\x24\x00\xf4\xff\xff\x48\x81", 20 },
        // 022 - geometria de mangueira (por frame)
        { 0x1408653A0, 0x1408653C0, ( const uint8_t* )"\x48\x89\x5c\x24\x08\x48\x89\x6c\x24\x10\x48\x89\x74\x24\x18\x57\x48\x83\xec\x60", 20 },
        // 023 - gerenciador de tráfego (dado)
        { 0x1436AF7C8, 0x143714F50, nullptr, 0 },
        // 024 - estado do jogo (dado: 2 = no mundo)
        { 0x143051820, 0x1430B5960, nullptr, 0 },
        // 025 - cvar g_trailer_cables_mode (dado)
        { 0x142D485C0, 0x142DA9FA0, nullptr, 0 },
        // 026 - cvar de settle do engate (dado)
        { 0x142D48200, 0x142DA9BE0, nullptr, 0 },
        // 027 - bloco de parâmetros de evento (dado)
        { 0x142733198, 0x1427949E8, nullptr, 0 },
        // 028 - string "@@trailer_coupling@@"
        { 0x14226E368, 0x1422BCCB8, ( const uint8_t* )"\x40\x40\x74\x72\x61\x69\x6c\x65\x72\x5f\x63\x6f\x75\x70\x6c\x69\x6e\x67\x40\x40\x00", 21 },
        // 029 - string "@@trailer_decoupling@@"
        { 0x14226E4B0, 0x1422BC710, ( const uint8_t* )"\x40\x40\x74\x72\x61\x69\x6c\x65\x72\x5f\x64\x65\x63\x6f\x75\x70\x6c\x69\x6e\x67\x40\x40", 22 },
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
    // 057 - regra do engate: freio de mão puxado
    inline bool handbrake_ok( const bool cargo_exempt = true )
    {
        if ( !parking_brake_known.load( std::memory_order_relaxed ) ||
             parking_brake_on.load( std::memory_order_relaxed ) ) return true;
        return cargo_exempt && in_cargo_window();
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
