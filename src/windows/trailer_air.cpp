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

#include "common/scssdk_telemetry_common_channels.h"
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

    std::atomic< float > air_psi{ 0.f };
    std::atomic< float > engine_rpm{ 0.f };
    std::atomic< uint32_t > game_minutes{ 0 };
    std::atomic< bool > engine_on{ true };
    std::atomic< bool > engine_known{ false };

    // 224 - callback de telemetria: pressão do ar
    SCSAPI_VOID on_air_pressure( const scs_string_t, const scs_u32_t, const scs_value_t* const value, const scs_context_t )
    {
        air_psi.store( value != nullptr ? value->value_float.value : 0.f, std::memory_order_relaxed );
    }

    // 225 - callback de telemetria: tempo do jogo
    SCSAPI_VOID on_game_time( const scs_string_t, const scs_u32_t, const scs_value_t* const value, const scs_context_t )
    {
        if ( value != nullptr ) game_minutes.store( value->value_u32.value, std::memory_order_relaxed );
    }

    // 226 - callback de telemetria: motor ligado
    SCSAPI_VOID on_engine_enabled( const scs_string_t, const scs_u32_t, const scs_value_t* const value, const scs_context_t )
    {
        engine_on.store( value != nullptr && value->value_bool.value != 0, std::memory_order_relaxed );
        engine_known.store( value != nullptr, std::memory_order_relaxed );
    }

    // 227 - callback de telemetria: rotação do motor
    SCSAPI_VOID on_engine_rpm( const scs_string_t, const scs_u32_t, const scs_value_t* const value, const scs_context_t )
    {
        engine_rpm.store( value != nullptr ? value->value_float.value : 0.f, std::memory_order_relaxed );
    }

    constexpr uint32_t air_sub_ptr    = 0x200;
    constexpr uint32_t air_press_off  = 0x458;
    constexpr uint32_t air_scale_off  = 0x1120;
    constexpr float    air_psi_const  = 14.5f;

    float air_win_t = 0.f;
    float air_win_last_psi = -1.f;
    float air_win_dpsi = 0.f;
    float air_win_ours = 0.f;
    float air_meas_rate = 0.f;
    float air_inject = 0.f;
    float air_rate_est = 0.f;
    float air_units = 0.f;
    bool air_tanks_seen = false;
    bool air_diag_dumped = false;
    float air_field_base = 0.f;
    void* air_obj = nullptr;
    void* air_truck = nullptr;
    float air_recheck_t = 0.f;

    // 228 - leitura segura de float (protegida)
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

    // 229 - leitura segura de uint64 (protegida)
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

    // 230 - escrita segura de float (protegida)
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

    // 231 - resolve o objeto de ar do cavalo
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

    // 232 - por frame: vazamento de ar com o motor desligado
    void update_air_leak()
    {
        static uint32_t last_min = 0;
        static bool last_valid = false;
        const uint32_t now = game_minutes.load( std::memory_order_relaxed );
        const bool had = last_valid;
        const uint32_t dm = now - last_min;
        last_min = now;
        last_valid = now != 0;
        const float leak = settings::g_settings.air_leak;
        if ( !had || dm == 0 || dm > 3 * 24 * 60 || leak <= 0.f ) return;
        if ( !engine_known.load( std::memory_order_relaxed ) || engine_on.load( std::memory_order_relaxed ) ) return;
        if ( !air_tanks_seen || air_truck == nullptr || air_field_base <= 0.f ) return;
        float frac = 0.f;
        if ( !air_safe_read_f( static_cast< uint8_t* >( air_truck ) + air_scale_off, frac ) ) return;
        const float lost = leak * ( static_cast< float >( dm ) / 60.f ) / air_field_base;
        const float next = ( std::max )( frac - lost, 0.f );
        if ( air_safe_write_f( static_cast< uint8_t* >( air_truck ) + air_scale_off, next ) && dm >= 10 )
        {
            CCore::g_instance->info( "air: engine off, {} min -> leak {:.2f} bar ({:.1f} -> {:.1f} psi)",
                dm, lost * air_field_base, frac * air_units, next * air_units );
        }
    }

    // 233 - por frame: boost do compressor (malha fechada)
    void update_air( const float dt )
    {
        update_air_leak();
        const float psi = air_psi.load();
        const float rpm = engine_rpm.load();
        const int boost = settings::g_settings.air_boost;

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

                    const float predicted = field * frac * air_psi_const;
                    if ( predicted > psi * 0.7f && predicted < psi * 1.3f )
                    {
                        if ( !air_tanks_seen )
                        {
                            air_tanks_seen = true;
                            air_field_base = field;
                            air_units = field * air_psi_const;
                            CCore::g_instance->info( "air: pressure locked max={:.3f} frac={:.4f} -> {:.1f} vs psi {:.1f}",
                                field, frac, predicted, psi );
                        }

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

        if ( boost > 0 && air_tanks_seen && air_truck != nullptr && rpm > 0.f &&
             psi < 133.f && dt > 0.f && air_units > 0.f && air_inject > 0.f )
        {
            float frac = 0.f;
            if ( air_safe_read_f( static_cast< uint8_t* >( air_truck ) + air_scale_off, frac ) &&
                 frac < 0.95f )
            {
                const float add_psi = air_inject * dt;

                air_safe_write_f( static_cast< uint8_t* >( air_truck ) + air_scale_off,
                    ( std::min )( frac + add_psi / air_units, 0.95f ) );
                air_win_ours += add_psi;
            }
        }
    }

    // 234 - registra os canais de telemetria de ar
    void air_register_channels()
    {
        const auto* init_params = CCore::g_instance->get_init_params();
        if ( init_params->register_for_channel( SCS_TELEMETRY_TRUCK_CHANNEL_brake_air_pressure,
                SCS_U32_NIL, SCS_VALUE_TYPE_float, SCS_TELEMETRY_CHANNEL_FLAG_none,
                &on_air_pressure, nullptr ) != SCS_RESULT_ok )
        {
            CCore::g_instance->warning( "Could not register 'truck.brake.air.pressure'" );
        }
        if ( init_params->register_for_channel( SCS_TELEMETRY_CHANNEL_game_time,
                SCS_U32_NIL, SCS_VALUE_TYPE_u32, SCS_TELEMETRY_CHANNEL_FLAG_none,
                &on_game_time, nullptr ) != SCS_RESULT_ok )
        {
            CCore::g_instance->warning( "Could not register 'game.time'" );
        }
        if ( init_params->register_for_channel( SCS_TELEMETRY_TRUCK_CHANNEL_engine_enabled,
                SCS_U32_NIL, SCS_VALUE_TYPE_bool, SCS_TELEMETRY_CHANNEL_FLAG_none,
                &on_engine_enabled, nullptr ) != SCS_RESULT_ok )
        {
            CCore::g_instance->warning( "Could not register 'truck.engine.enabled'" );
        }
        if ( init_params->register_for_channel( SCS_TELEMETRY_TRUCK_CHANNEL_engine_rpm,
                SCS_U32_NIL, SCS_VALUE_TYPE_float, SCS_TELEMETRY_CHANNEL_FLAG_none,
                &on_engine_rpm, nullptr ) != SCS_RESULT_ok )
        {
            CCore::g_instance->warning( "Could not register 'truck.engine.rpm'" );
        }
    }
    namespace
    {
        const bool fn_idx_reg = []()
        {
            fn_index_add( fn_addr_of( &on_air_pressure ), 224, "on_air_pressure" );
            fn_index_add( fn_addr_of( &on_game_time ), 225, "on_game_time" );
            fn_index_add( fn_addr_of( &on_engine_enabled ), 226, "on_engine_enabled" );
            fn_index_add( fn_addr_of( &on_engine_rpm ), 227, "on_engine_rpm" );
            fn_index_add( fn_addr_of( &air_safe_read_f ), 228, "air_safe_read_f" );
            fn_index_add( fn_addr_of( &air_safe_read_q ), 229, "air_safe_read_q" );
            fn_index_add( fn_addr_of( &air_safe_write_f ), 230, "air_safe_write_f" );
            fn_index_add( fn_addr_of( &air_object ), 231, "air_object" );
            fn_index_add( fn_addr_of( &update_air_leak ), 232, "update_air_leak" );
            fn_index_add( fn_addr_of( &update_air ), 233, "update_air" );
            fn_index_add( fn_addr_of( &air_register_channels ), 234, "air_register_channels" );
            return true;
        }();
    }
}
