// Eixos direcionais de carretas oversize - split out of
// trailer_manipulation.cpp.

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

namespace ts_extra_utilities
{
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

    // steerable-axle controls for one oversize trailer: lock toggle, angle
    // slider, arrows and center - the angle is applied by the game's next
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

    // steerable axles (oversize trailers): the hook only swaps the angle,
    // so a miss just means no steering control - never fatal
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

    void steering_reset()
    {
        for ( uint32_t i = 0; i < max_trailers; ++i )
        {
            locked_steering[ i ].data.store( nullptr );
            locked_ui[ i ] = false;
        }
    }
}
