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

namespace br_carretas
{

    struct LockedSteering
    {
        std::atomic< prism::vehicle_wheel_steering_data_t* > data{ nullptr };
        std::atomic< float > angle{ 0.f };
    };

    LockedSteering locked_steering[ max_trailers ];

    bool locked_ui[ max_trailers ] = {};
    float angle_ui[ max_trailers ] = {};
    bool centering_ui[ max_trailers ] = {};

    // 108 - trava/destrava o controle manual do eixo de T(i)
    void set_locked( const uint32_t i, const bool lock )
    {
        if ( locked_ui[ i ] == lock ) return;
        locked_ui[ i ] = lock;
        if ( lock )
        {

            angle_ui[ i ] = prism::offsets::field< float >( trailers[ i ], prism::offsets::trailer_steering );
            locked_steering[ i ].angle.store( angle_ui[ i ] );
        }
    }

    // 109 - hook: injeta o ângulo do usuário no lugar do ângulo do jogo
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

    // 110 - por frame: teclas de direção + centralização suave
    void CTrailerManipulation::update_steering( const float dt )
    {

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
                    set_locked( i, true );
                }

                if ( !locked_ui[ i ] ) continue;

                const float step = settings::g_settings.steering_speed * dt;
                if ( center_pressed ) centering_ui[ i ] = true;
                if ( left || right ) centering_ui[ i ] = false;
                if ( centering_ui[ i ] )
                {

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

        for ( uint32_t i = 0; i < max_trailers; ++i )
        {
            locked_steering[ i ].data.store( locked_ui[ i ] ? steering_data( trailers[ i ] ) : nullptr );
            if ( i < trailer_count && !locked_ui[ i ] )
            {
                angle_ui[ i ] = prism::offsets::field< float >( trailers[ i ], prism::offsets::trailer_steering );
            }
        }

        if ( settings::g_settings.debug_logs && trailer_count > 0 )
        {
            static float probe_t = 0.f;
            probe_t += dt;
            if ( probe_t >= 0.5f )
            {
                probe_t = 0.f;
                for ( uint32_t i = 0; i < trailer_count; ++i )
                {
                    if ( trailers[ i ] == nullptr || steering_data( trailers[ i ] ) == nullptr ) continue;
                    CCore::g_instance->info( "PROBE steer t{} data={:p} angle={:.4f}",
                        i, static_cast< void* >( steering_data( trailers[ i ] ) ),
                        prism::offsets::field< float >( trailers[ i ], prism::offsets::trailer_steering ) );
                }
            }
        }
    }

    // 111 - controles de direção de uma carreta na tela
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
            centering_ui[ i ] = true;
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

    // 112 - painel Direcionais (todas as direcionáveis + teclas)
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

    // 113 - instala o hook de direção (achado por padrão)
    void steering_install()
    {
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
    }

    // 114 - limpa as travas (destrutor/reset)
    void steering_reset()
    {
        for ( uint32_t i = 0; i < max_trailers; ++i )
        {
            locked_steering[ i ].data.store( nullptr );
            locked_ui[ i ] = false;
        }
    }
    namespace
    {
        const bool fn_idx_reg = []()
        {
            fn_index_add( fn_addr_of( &set_locked ), 108, "set_locked" );
            fn_index_add( fn_addr_of( &hk_set_individual_steering ), 109, "hk_set_individual_steering" );
            fn_index_add( fn_addr_of( &steering_install ), 113, "steering_install" );
            fn_index_add( fn_addr_of( &steering_reset ), 114, "steering_reset" );
            return true;
        }();
    }
}
