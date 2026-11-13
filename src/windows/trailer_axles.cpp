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

    constexpr uint32_t max_axles = 16;

    struct LiftAxles
    {
        void* controller = nullptr;
        void* axle_data = nullptr;
        uint8_t* flags = nullptr;
        uint8_t orig_flags[ max_axles ]{};
        uint32_t orig_mask = 0;
        uint32_t applied_mask = 0;
        uint32_t native_axles = 0;
        uint32_t steer_axles = 0;
        uint32_t axle_count = 0;
    };

    LiftAxles lift_axles[ max_trailers ];

    LiftAxles truck_lift;

    std::atomic< bool > reverse_light{ false };
    std::atomic< int32_t > engine_gear{ 0 };

    bool reverse_override = false;

    bool reverse_auto_lift = false;
    bool was_reversing = false;

    // 217 - callback de telemetria: luz de ré
    SCSAPI_VOID on_light_reverse( const scs_string_t, const scs_u32_t, const scs_value_t* const value, const scs_context_t )
    {
        reverse_light.store( value != nullptr && value->value_bool.value != 0, std::memory_order_relaxed );
    }

    // 218 - callback de telemetria: marcha do motor
    SCSAPI_VOID on_engine_gear( const scs_string_t, const scs_u32_t, const scs_value_t* const value, const scs_context_t )
    {
        engine_gear.store( value != nullptr ? value->value_s32.value : 0, std::memory_order_relaxed );
    }

    // 219 - devolve máscaras/flags dos eixos ao jogo
    void release_all_axles()
    {
        auto releases = []( LiftAxles& lift )
        {

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
        };
        releases( truck_lift );
        for ( auto& lift : lift_axles ) releases( lift );
    }

    // 220 - UI de eixos de uma carreta
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
        if ( controller != nullptr )
        {

            const bool up = state != 0;
            const char* label = up ? "Baixar" : "Levantar";
            const float bw = ImGui::CalcTextSize( label ).x + 20.f;
            ui::right_side( bw );
            if ( ImGui::Button( fmt::format( "{}##lift", label ).c_str(), ImVec2( bw, 0.f ) ) )
            {
                prism::offsets::field< uint8_t >( controller, prism::offsets::lift_controller_state ) =
                    up ? 1 : 2;
                if ( auto* ga = CCore::g_instance->get_game_actor() )
                {
                    if ( up )
                        prism::offsets::field< uint8_t >( ga, prism::offsets::game_actor_lift_state ) &= ~2u;
                    else
                        prism::offsets::field< uint8_t >( ga, prism::offsets::game_actor_lift_state ) |= 2u;
                }
            }
        }

        bool locked = state != 0 && lift.applied_mask != 0;
        if ( locked )
        {
            ImGui::TextDisabled( "Abaixe os eixos para alterar quais serão levantados." );
        }

        const bool none_liftable = lift.native_axles == 0;
        if ( none_liftable )
        {
            ImGui::TextDisabled( "Nenhum eixo elevável nesta carreta" );
        }

        auto& stored = settings::g_settings.axle_masks[ i ];

        uint32_t selection = ( stored < 0 ? lift.native_axles : static_cast< uint32_t >( stored ) )
            & lift.native_axles;
        if ( reverse_override )
        {
            selection = lift.native_axles & lift.steer_axles;
            locked = true;
            ImGui::TextDisabled( "Ré engatada: os eixos direcionais estão levantados." );
        }

        ImGui::BeginDisabled( locked );
        uint32_t next = selection;

        {
            auto* dl = ImGui::GetWindowDrawList();
            const float w = ImGui::GetContentRegionAvail().x;
            const float ax_h = 74.f;
            const ImVec2 org = ImGui::GetCursorScreenPos();
            ImGui::Dummy( ImVec2( w, ax_h ) );

            const float bx0 = org.x + 6.f, bx1 = org.x + w - 6.f;
            const float by0 = org.y + 2.f,  by1 = org.y + 36.f;
            const auto blk = IM_COL32( 12, 13, 15, 255 );
            dl->AddRectFilled( ImVec2( bx0, by0 ), ImVec2( bx1, by1 ), IM_COL32( 37, 44, 50, 255 ), 0 );
            dl->AddRect( ImVec2( bx0, by0 ), ImVec2( bx1, by1 ), IM_COL32( 55, 63, 70, 255 ), 0, 0, 1.5f );

            const float sk0 = bx0 + ( bx1 - bx0 ) * 0.16f;
            const float skd = ( bx1 - bx0 ) * 0.14f;
            const float skh = 12.f;
            dl->AddTriangleFilled( ImVec2( sk0, by1 ), ImVec2( sk0 + skd, by1 ),
                ImVec2( sk0 + skd, by1 + skh ), blk );
            dl->AddRectFilled( ImVec2( sk0 + skd, by1 ), ImVec2( bx1, by1 + skh ), blk );

            const float lg = sk0 + skd * 0.85f;
            dl->AddRectFilled( ImVec2( lg - 2.f, by1 + skh ), ImVec2( lg + 2.f, by1 + skh + 14.f ), blk );
            dl->AddRectFilled( ImVec2( lg - 7.f, by1 + skh + 12.f ), ImVec2( lg + 7.f, by1 + skh + 16.f ), blk );

            const float wr = 10.f;
            const float gap = wr * 2.4f;

            const float rear_x = bx1 - ( bx1 - bx0 ) * 0.16f;
            const uint32_t shown = lift.axle_count < 8 ? lift.axle_count : 8;
            for ( uint32_t a = 0; a < shown; ++a )
            {
                const float cx = rear_x - static_cast< float >( shown - 1 - a ) * gap;
                const bool liftable = ( lift.native_axles >> a ) & 1;
                const bool on = liftable && ( ( next >> a ) & 1 );

                const float cy = by1 + skh + wr + 4.f - ( on ? 10.f : 0.f );
                const ImU32 fill = !liftable ? IM_COL32( 10, 10, 10, 255 )
                    : on ? IM_COL32( 46, 180, 60, 255 )
                    : IM_COL32( 92, 98, 104, 255 );
                dl->AddCircleFilled( ImVec2( cx, cy ), wr, fill );
                dl->AddCircle( ImVec2( cx, cy ), wr, IM_COL32( 190, 195, 200, 255 ), 24, 1.5f );
                if ( liftable )
                {
                    ImGui::SetCursorScreenPos( ImVec2( cx - 11.f, cy - 11.f ) );
                    if ( ImGui::InvisibleButton( fmt::format( "##ax{}", a ).c_str(),
                        ImVec2( 22.f, 22.f ) ) )
                    {
                        next = on ? next & ~( 1u << a ) : next | ( 1u << a );
                    }
                }
            }
            ImGui::SetCursorScreenPos( ImVec2( org.x, org.y + ax_h ) );
        }
        ImGui::EndDisabled();

        if ( next != selection )
        {
            stored = static_cast< int32_t>( next );
            settings::save();
        }
    }

    // 221 - UI de eixos do cavalo
    void CTrailerManipulation::render_truck_lift() const
    {
        void* truck = nullptr;
        if ( auto* ga = CCore::g_instance->get_game_actor() )
        {
            truck = prism::offsets::field< void* >( ga, prism::offsets::game_actor_truck );
        }
        void* ctl = truck != nullptr && readable_ptr( truck )
            ? prism::offsets::field< void* >( truck, prism::offsets::trailer_lift_controller )
            : nullptr;

        const bool has_lift = ctl != nullptr && readable_ptr( ctl );
        uint8_t state = 0;
        if ( has_lift )
        {
            state = prism::offsets::field< uint8_t >( ctl, prism::offsets::lift_controller_state );
            ImGui::Text( "Eixos do cavalo: %s", state != 0 ? "levantados" : "no chão" );
            const char* label = state != 0 ? "Baixar" : "Levantar";
            const float bw = ImGui::CalcTextSize( label ).x + 20.f;
            if ( ImGui::Button( fmt::format( "{}##trucklift", label ).c_str(), ImVec2( bw, 0.f ) ) )
            {
                prism::offsets::field< uint8_t >( ctl, prism::offsets::lift_controller_state ) =
                    state != 0 ? 1 : 2;
                if ( auto* ga = CCore::g_instance->get_game_actor() )
                {
                    if ( state != 0 )
                        prism::offsets::field< uint8_t >( ga, prism::offsets::game_actor_lift_state ) &= ~1u;
                    else
                        prism::offsets::field< uint8_t >( ga, prism::offsets::game_actor_lift_state ) |= 1u;
                }
            }
        }
        else
        {
            ImGui::TextDisabled( "Sem eixo elevável no cavalo" );
        }

        const bool locked = state != 0 && truck_lift.applied_mask != 0
            && truck_lift.applied_mask != ~0u;
        if ( locked )
        {
            ImGui::TextDisabled( "Abaixe os eixos para alterar quais serão levantados." );
        }

        auto& stored = settings::g_settings.truck_axle_mask;
        uint32_t selection = ( stored < 0 ? truck_lift.native_axles
            : static_cast< uint32_t >( stored ) ) & truck_lift.native_axles;
        uint32_t next = selection;

        void* axle_data = truck != nullptr && readable_ptr( truck )
            ? prism::offsets::field< void* >( truck, prism::offsets::vehicle_chassis_data )
            : nullptr;
        uint64_t tcount = 0;
        if ( axle_data != nullptr && readable_ptr( axle_data ) )
        {
            tcount = prism::offsets::field< uint64_t >( axle_data, prism::offsets::axle_data_flags_count );
        }
        const uint32_t shown = tcount >= 2 && tcount <= 4
            ? static_cast< uint32_t >( tcount ) : 0;

        {
            auto* dl = ImGui::GetWindowDrawList();
            const float ax_h = 48.f;
            const ImVec2 org = ImGui::GetCursorScreenPos();
            const uint32_t ax_n = shown != 0 ? shown : 3u;

            const uint32_t lift_mask = has_lift ? truck_lift.native_axles : 0u;

            const float truck_w = 112.f + static_cast< float >( ax_n - 2 ) * 50.f;
            ImGui::Dummy( ImVec2( truck_w + 12.f, ax_h ) );
            const float cab_x = org.x + 16.f;
            const float cab_w = 42.f;
            const float ch1 = cab_x + truck_w;
            const auto blk = IM_COL32( 12, 13, 15, 255 );

            const float ry0 = org.y + 21.f;
            const float ry1 = org.y + 27.f;
            dl->AddRectFilled( ImVec2( cab_x + 4.f, ry0 ), ImVec2( ch1, ry1 ), blk, 1.f );

            dl->AddRectFilled( ImVec2( cab_x, ry0 - 2.f ), ImVec2( cab_x + 9.f, ry1 + 4.f ),
                blk, 1.f );
            dl->AddRectFilled( ImVec2( cab_x, ry1 + 4.f ), ImVec2( cab_x + 11.f, ry1 + 7.f ),
                blk, 0.f );

            dl->AddRectFilled( ImVec2( cab_x, org.y + 3.f ), ImVec2( cab_x + cab_w, ry0 ),
                IM_COL32( 172, 180, 186, 255 ), 3.f );
            dl->AddRectFilled( ImVec2( cab_x + cab_w * 0.12f, org.y + 7.f ),
                ImVec2( cab_x + cab_w * 0.80f, org.y + 14.f ),
                IM_COL32( 70, 78, 84, 255 ), 2.f );
            dl->AddRectFilled( ImVec2( cab_x, ry0 - 5.f ), ImVec2( cab_x + cab_w, ry0 ),
                IM_COL32( 118, 126, 132, 255 ), 0.f );

            const float wr = 10.f;
            const float wy = ry1 + wr - 2.f;
            const float steer_x = cab_x + 12.f + wr;
            const float rear1 = ch1 - wr - 4.f;
            const float gap = wr * 2.4f;
            const uint32_t last_steer = ax_n >= 4 ? 1u : 0u;
            const float first_drive = ax_n > last_steer + 1
                ? rear1 - static_cast< float >( ax_n - last_steer - 2 ) * gap
                : rear1;

            {
                const float tk0 = steer_x + ( ax_n >= 4 ? gap : 0.f ) + wr + 6.f;
                const float tk1 = ax_n > last_steer + 1
                    ? first_drive - wr - 6.f : tk0 + 30.f;
                dl->AddRectFilled( ImVec2( tk0, ry1 + 1.f ),
                    ImVec2( tk1 > tk0 + 10.f ? tk1 : tk0 + 10.f, ry1 + 13.f ),
                    IM_COL32( 60, 66, 72, 255 ), 2.f );
                dl->AddRect( ImVec2( tk0, ry1 + 1.f ),
                    ImVec2( tk1 > tk0 + 10.f ? tk1 : tk0 + 10.f, ry1 + 13.f ),
                    IM_COL32( 96, 102, 108, 255 ), 2.f, 0, 1.f );
            }
            ImGui::BeginDisabled( locked );
            for ( uint32_t a = 0; a < ax_n; ++a )
            {
                float cx;
                if ( a == 0 )
                {
                    cx = steer_x;
                }
                else if ( ax_n >= 4 && a == 1 )
                {
                    cx = steer_x + gap;
                }
                else
                {
                    const uint32_t d0 = ax_n >= 4 ? 2u : 1u;
                    const uint32_t dn = ax_n - d0;
                    cx = rear1 - static_cast< float >( dn - 1 - ( a - d0 ) ) * gap;
                }
                const bool liftable = ( lift_mask >> a ) & 1;
                const bool on = liftable && ( ( next >> a ) & 1 );

                const float cy = wy - ( on ? 7.f : 0.f );
                const ImU32 fill = !liftable ? IM_COL32( 10, 10, 10, 255 )
                    : on ? IM_COL32( 46, 180, 60, 255 )
                         : IM_COL32( 92, 98, 104, 255 );
                dl->AddCircleFilled( ImVec2( cx, cy ), wr, fill );
                dl->AddCircle( ImVec2( cx, cy ), wr, IM_COL32( 190, 195, 200, 255 ), 24, 1.5f );
                if ( liftable )
                {
                    ImGui::SetCursorScreenPos( ImVec2( cx - wr - 1.f, cy - wr - 1.f ) );
                    if ( ImGui::InvisibleButton( fmt::format( "##tax{}", a ).c_str(),
                        ImVec2( wr * 2.f + 2.f, wr * 2.f + 2.f ) ) )
                    {
                        next = on ? next & ~( 1u << a ) : next | ( 1u << a );
                    }
                }
            }
            ImGui::EndDisabled();
            ImGui::SetCursorScreenPos( ImVec2( org.x, org.y + ax_h ) );
        }

        if ( next != selection )
        {
            stored = static_cast< int32_t >( next );
            settings::save();
        }
    }

    // 222 - registra os canais de telemetria (ré/marcha)
    void lift_register_channels()
    {

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
    }

    // 223 - por frame: aplica o estado dos eixos levantados
    void lift_axles_update( void* game_actor )
    {

        const bool reversing = settings::g_settings.reverse_lift
            && ( reverse_light.load( std::memory_order_relaxed )
                || engine_gear.load( std::memory_order_relaxed ) < 0 );

        bool reverse_just_engaged = false;
        if ( reversing && !was_reversing )
        {

            bool all_down = trailer_count > 0, any_liftable = false;
            for ( uint32_t i = 0; i < trailer_count; ++i )
            {
                any_liftable |= ( lift_axles[ i ].native_axles & lift_axles[ i ].steer_axles ) != 0;
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

                    auto* controller = prism::offsets::field< void* >( trailers[ i ], prism::offsets::trailer_lift_controller );
                    if ( controller != nullptr
                        && prism::offsets::field< uint8_t >( controller, prism::offsets::lift_controller_state ) != 0 )
                    {
                        prism::offsets::field< uint8_t >( controller, prism::offsets::lift_controller_state ) = 1;
                    }
                }
                if ( game_actor != nullptr )
                {
                    prism::offsets::field< uint8_t >( game_actor, prism::offsets::game_actor_lift_state ) &= ~2u;
                }
            }
        }
        was_reversing = reversing;

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
                lift = LiftAxles{};
                continue;
            }

            if ( lift.axle_data != axle_data || lift.flags != flags
                || ( controller != nullptr && lift.controller != controller ) )
            {

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
                lift.applied_mask = ~0u;
            }
            else if ( controller != nullptr
                && prism::offsets::field< uint32_t >( controller, prism::offsets::lift_controller_axle_mask ) != lift.applied_mask )
            {

                lift.orig_mask = prism::offsets::field< uint32_t >( controller, prism::offsets::lift_controller_axle_mask );
                lift.native_axles = 0;
                for ( uint32_t a = 0; a < lift.axle_count; ++a )
                {
                    lift.orig_flags[ a ] = flags[ a ];
                    if ( flags[ a ] != 0 ) lift.native_axles |= 1u << a;
                }
            }

            if ( controller != nullptr
                && prism::offsets::field< uint32_t >( controller, prism::offsets::lift_controller_axle_count ) != lift.axle_count )
            {
                continue;
            }

            if ( controller == nullptr ) continue;

            const auto state = prism::offsets::field< uint8_t >( controller, prism::offsets::lift_controller_state );
            if ( state != 0 && lift.applied_mask == 0 )
            {

                prism::offsets::field< uint8_t >( controller, prism::offsets::lift_controller_state ) = 0;
            }
            else if ( state != 0 )
            {

                continue;
            }

            lift.steer_axles = 0;
            auto* sdata = steering_data( trailers[ i ] );
            if ( sdata != nullptr && readable_span( sdata, 0xB8 ) >= 0xB8 )
            {
                uint64_t* wmap = prism::offsets::field< uint64_t* >( sdata, 0xA8 );
                const uint64_t wcount = prism::offsets::field< uint64_t >( sdata, 0xB0 );
                if ( wmap != nullptr && readable_ptr( wmap ) && wcount <= 64 )
                {
                    for ( uint32_t a = 0; a < lift.axle_count && a < max_axles; ++a )
                    {
                        const uint64_t w0 = 2ull * a, w1 = w0 + 1;
                        if ( ( w0 < wcount && wmap[ w0 ] != ~0ull )
                            || ( w1 < wcount && wmap[ w1 ] != ~0ull ) )
                        {
                            lift.steer_axles |= 1u << a;
                        }
                    }
                }
            }

            const auto stored = settings::g_settings.axle_masks[ i ];

            uint32_t selection = ( stored < 0 ? lift.native_axles : static_cast< uint32_t >( stored ) )
                & lift.native_axles;
            if ( reverse_override )
            {
                selection = lift.native_axles & lift.steer_axles;
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

        {
            auto& lift = truck_lift;
            void* truck = game_actor != nullptr && readable_ptr( game_actor )
                ? prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck )
                : nullptr;
            void* controller = truck != nullptr && readable_ptr( truck )
                ? prism::offsets::field< void* >( truck, prism::offsets::trailer_lift_controller )
                : nullptr;
            if ( controller == nullptr || !readable_ptr( controller ) )
            {
                lift = LiftAxles{};
            }
            else
            {
                const auto raw_mask = prism::offsets::field< uint32_t >(
                    controller, prism::offsets::lift_controller_axle_mask );
                const auto raw_count = prism::offsets::field< uint32_t >(
                    controller, prism::offsets::lift_controller_axle_count );
                if ( lift.controller != controller || lift.axle_count != raw_count )
                {

                    lift = LiftAxles{};
                    lift.controller = controller;
                    lift.axle_count = raw_count <= max_axles ? raw_count : 0;
                    lift.orig_mask = raw_mask;
                    lift.native_axles = raw_mask;
                    lift.applied_mask = ~0u;
                }
                else if ( raw_mask != lift.applied_mask )
                {

                    lift.orig_mask = raw_mask;
                    lift.native_axles = raw_mask;
                }
                const auto state = prism::offsets::field< uint8_t >(
                    controller, prism::offsets::lift_controller_state );
                if ( state != 0 && lift.applied_mask == 0 )
                {

                    prism::offsets::field< uint8_t >( controller, prism::offsets::lift_controller_state ) = 0;
                }
                else if ( state == 0 && lift.native_axles != 0 )
                {
                    const auto stored = settings::g_settings.truck_axle_mask;
                    const uint32_t selection =
                        ( stored < 0 ? lift.native_axles : static_cast< uint32_t >( stored ) )
                        & lift.native_axles;
                    if ( selection != raw_mask )
                    {
                        prism::offsets::field< uint32_t >( controller, prism::offsets::lift_controller_axle_mask ) = selection;
                    }
                    lift.applied_mask = selection;
                }
            }
        }

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
                    prism::offsets::field< uint8_t >( controller, prism::offsets::lift_controller_state ) = 2;
                }
            }
            reverse_auto_lift = true;
        }
    }
    namespace
    {
        const bool fn_idx_reg = []()
        {
            fn_index_add( fn_addr_of( &on_light_reverse ), 217, "on_light_reverse" );
            fn_index_add( fn_addr_of( &on_engine_gear ), 218, "on_engine_gear" );
            fn_index_add( fn_addr_of( &release_all_axles ), 219, "release_all_axles" );
            fn_index_add( fn_addr_of( &lift_register_channels ), 222, "lift_register_channels" );
            fn_index_add( fn_addr_of( &lift_axles_update ), 223, "lift_axles_update" );
            return true;
        }();
    }
}
