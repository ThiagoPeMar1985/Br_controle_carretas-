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
    // CTrailerManipulation — lifecycle
    //========================================================================
    CTrailerManipulation::CTrailerManipulation() = default;

    CTrailerManipulation::~CTrailerManipulation()
    {
        steering_reset();
        set_individual_steering_hook.reset();
        release_all_axles();
        crashes_when_disconnected_hook.reset();
        connect_slave_hook.reset();
        pre_detach_hook.reset();
        detach_dispatch_hook.reset();
        attach_dispatch_hook.reset();
        trailer_state_apply_hook.reset();
        trailer_teleport_hook.reset();
        actor_teleport_hook.reset();
        lane_obstacle_insert_hook.reset();
        combo_length_hook.reset();
        cam_set_target_hook.reset();
        cam_update_hook.reset();
        lamp_mask_hook.reset();
        lamp_propagate_hook.reset();
        apply_cables_hook.reset();
        set_lines_hook.reset();
        rebuild_lines_hook.reset();
        joint_walk_hook.reset();
        phys_link_hook.reset();
        purecall_hook.reset();
        trailer_disconnect_hook.reset();
        trailer_connect_hook.reset();
        trailer_connect_hook2.reset();
        svc_brake_chain_hook.reset();
        svc_brake_hook.reset();
        brake_chain_setter_hook.reset();
        brake_setter_hook.reset();
        forced_chain_setter_hook.reset();
        forced_setter_hook.reset();
        wheel_brake_hook.reset();
        chase_camera = nullptr;
    }

    bool CTrailerManipulation::init()
    {
        // Individually detachable trailers — attach/detach hooks and convoy
        // maintenance live in trailer_coupling.cpp; false = unavailable
        this->connect_valid_ = coupling_install();

        // steerable axles (oversize trailers): the hook only swaps the angle,
        // so a miss just means no steering control — never fatal
        steering_install(); // trailer_steering.cpp

        // reverse-gear detection for the axle-lift assist lives in
        // trailer_axles.cpp now; a failed channel just narrows the fallback
        lift_register_channels();

        air_register_channels(); // trailer_air.cpp

        return true;
    }

    //========================================================================
    // Per-frame update — each subsystem exposes an entry point; convoy
    // refresh, state maintenance and hotkeys live in trailer_coupling.cpp.
    //========================================================================
    void CTrailerManipulation::update()
    {
        // joystick buttons have no WM_KEYDOWN — poll them while a capture is open
        settings::poll_joystick_capture();

        // per-section cycle accounting (rdtsc ≈ 20ns a read): when the game
        // crawls the next log line names the stage eating frame time instead
        // of guessing — silent while everything stays cheap
        uint64_t pt[ 10 ] = {};
        pt[ 0 ] = __rdtsc();
        refresh_trailer_chain();   // trailer_coupling.cpp
        pt[ 1 ] = __rdtsc();
        update_convoy_state();     // trailer_coupling.cpp
        pt[ 2 ] = __rdtsc();
        light_watch_tick();        // trailer_lines.cpp — parked lamp pin
        pt[ 3 ] = __rdtsc();
        gc_detached_marks();       // trailer_coupling.cpp
        watch_sim_heartbeat();
        pt[ 4 ] = __rdtsc();

        auto* game_actor = CCore::g_instance->get_game_actor();

        // reverse-gear lift assist + per-axle lift mask rewrite
        lift_axles_update( game_actor );   // trailer_axles.cpp
        pt[ 5 ] = __rdtsc();
        // cargo respawn watch + rig restore countdown
        coupling_parked_update();          // trailer_loadstate.cpp
        pt[ 6 ] = __rdtsc();
        update_hotkeys();                  // trailer_coupling.cpp
        update_steering( ImGui::GetIO().DeltaTime );
        pt[ 7 ] = __rdtsc();
        update_air( ImGui::GetIO().DeltaTime );
        pt[ 8 ] = __rdtsc();
        brake_boost_update(); // trailer_brake.cpp — +0x760 boost + wheel mask
        pt[ 9 ] = __rdtsc();

        static uint64_t acc[ 9 ] = {};
        static uint64_t tsc0 = 0;
        static LARGE_INTEGER qpc0{};
        static uint32_t win_frames = 0;
        if ( win_frames == 0 )
        {
            QueryPerformanceCounter( &qpc0 );
            tsc0 = __rdtsc();
        }
        for ( uint32_t s = 0; s < 9; ++s ) acc[ s ] += pt[ s + 1 ] - pt[ s ];
        if ( ++win_frames >= 300 )
        {
            LARGE_INTEGER qpc1, freq;
            QueryPerformanceCounter( &qpc1 );
            QueryPerformanceFrequency( &freq );
            const uint64_t tsc1 = __rdtsc();
            const double window_s =
                double( qpc1.QuadPart - qpc0.QuadPart ) / double( freq.QuadPart );
            uint64_t total = 0;
            for ( const uint64_t a : acc ) total += a;
            // tsc calibrated against qpc over the same window — no assumed
            // clock speed, so the ms numbers stay honest on any core
            if ( window_s > 0.0 && tsc1 > tsc0 && total != 0 )
            {
                const double tsc_hz = double( tsc1 - tsc0 ) / window_s;
                const double plugin_ms = double( total ) / tsc_hz * 1000.0 / win_frames;
                static const char* names[ 9 ] =
                    { "chain", "convoy", "lights", "marks", "axles", "loadstate", "steer", "air", "brake" };
                uint32_t top = 0;
                for ( uint32_t s = 1; s < 9; ++s ) if ( acc[ s ] > acc[ top ] ) top = s;
                const double top_ms = double( acc[ top ] ) / tsc_hz * 1000.0 / win_frames;
                if ( plugin_ms > 0.5 )
                {
                    CCore::g_instance->warning(
                        "perf: plugin update {:.2f} ms/frame — top {} {:.2f} ms",
                        plugin_ms, names[ top ], top_ms );
                }
            }
            for ( auto& a : acc ) a = 0;
            win_frames = 0;
        }
    }

    //========================================================================
    // User actions (buttons + hotkeys — they only queue sim-thread ops)
    //========================================================================
    void CTrailerManipulation::disconnect_trailer( const uint32_t i ) const
    {
        // canon index, not trailers[] position: the list is rotated while a
        // trailer sits on the fifth wheel, but the key/button must always
        // mean the same physical piece
        void* trailer = i < max_trailers ? canon[ i ] : nullptr;
        if ( trailer == nullptr || !readable_ptr( trailer ) ) return;
        void* truck = nullptr;
        if ( auto* game_actor = CCore::g_instance->get_game_actor() )
        {
            truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
        }
        const bool on_truck = trailer == truck_towed;
        // the real physical parent (+0x1080) — the player can rebuild the
        // chain in any order, so the canonical predecessor is only a
        // fallback when the field reads empty
        void* parent = i == 0 || on_truck
            ? truck
            : ( readable_ptr( reinterpret_cast< uint8_t* >( trailer ) + prism::offsets::trailer_parent )
                ? prism::offsets::field< void* >( trailer, prism::offsets::trailer_parent ) : nullptr );
        if ( parent == nullptr && i > 0 ) parent = canon[ i - 1 ];
        // +0x1080 may resolve to the truck object for a fifth-wheel trailer —
        // that is a truck parent, not a trailer one (the truck has no trailer
        // fields at these offsets)
        const bool parent_is_truck = i == 0 || on_truck ||
            ( truck != nullptr && parent == truck );

        // a middle link may leave with its slave still jointed — the detached
        // pair (dolly + rear trailer) parks as a unit; the detach mark keeps
        // the game from auto-re-attaching the freed link to its old parent
        CCore::g_instance->info( "Disconnecting trailer {}", i );

        // traffic stays live: the parked vfunc (+0x328) registers the detached
        // trailer as a proper lane obstacle, so the sweep handles it like any
        // parked trailer — no suppression or despawn wait needed
        if ( queue_op( pending_kind::disconnect, trailer, parent, parent_is_truck, 0 ) )
        {
            traffic_detach_done.store( false, std::memory_order_release );
        }
    }

    void CTrailerManipulation::connect_trailer( const uint32_t i ) const
    {
        // canon index — "carreta 2" always means the second physical piece,
        // wherever it currently sits in the rotated trailers[] list
        void* trailer = i < max_trailers ? canon[ i ] : nullptr;
        if ( trailer == nullptr || !readable_ptr( trailer ) || joint( trailer ) != nullptr ) return;
        auto* game_actor = CCore::g_instance->get_game_actor();
        void* parent = i == 0
            ? ( game_actor != nullptr ? prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck ) : nullptr )
            : canon_coupled_predecessor( trailer );
        if ( parent == nullptr && i > 0 )
        {
            parent = canon[ i - 1 ];
        }
        if ( parent == nullptr )
        {
            CCore::g_instance->warning( "Não foi possível engatar a carreta {}: veículo anterior não encontrado", i );
            return;
        }
        // the chain parent must be attached itself — the game only ever creates
        // a joint onto an already-jointed parent; hooking onto a free trailer
        // leaves the component half-initialized (purecall crash)
        if ( i > 0 && joint( parent ) == nullptr )
        {
            CCore::g_instance->warning( "Engate a carreta {} primeiro", i );
            return;
        }
        // proximity rule like the native T: hooks must nearly touch — no
        // teleporting the trailer onto the hook (< 7 m measured hook-to-hook)
        if ( hook_far( trailer, parent, i == 0 ) )
        {
            CCore::g_instance->warning( "A carreta {} está muito longe — aproxime dando ré para engatar", i + 1 );
            return;
        }
        // back on its chain parent: the game may manage this trailer again
        unmark_detached( trailer );
        queue_op( pending_kind::connect, trailer, parent, i == 0 );
    }

    // "connect to truck" only SELECTS which parked trailer the native T key
    // attaches next — nothing couples here. hk_attach_dispatch aims the
    // native attach at the selection while the fifth wheel is free
    void CTrailerManipulation::connect_to_truck( void* trailer ) const
    {
        if ( trailer == nullptr || !readable_ptr( trailer ) ) return;
        const int ci = canon_index_of( trailer );
        if ( joint( trailer ) != nullptr )
        {
            CCore::g_instance->warning( "A carreta {} já está engatada", ci >= 0 ? ci + 1 : 0 );
            return;
        }
        // switch: picking the trailer that is already the target hands the
        // T back to the original (first) trailer; picking another moves it
        if ( attach_target == trailer )
        {
            attach_target = nullptr;
            CCore::g_instance->info( "Alvo de engate: carreta original (1) — aperte T" );
            return;
        }
        // the detach marks stay until the dispatcher is actually aiming at
        // this trailer (released inside hk_attach_dispatch): an unmarked
        // parked piece becomes a candidate for the game's own slave
        // auto-attach and gets pulled in when a parent couples next to it
        attach_target = trailer;
        CCore::g_instance->info( "Alvo de engate: carreta {} — recue até ela e aperte T",
                                 ci >= 0 ? ci + 1 : 0 );
    }

    void CTrailerManipulation::toggle_attach( const uint32_t i ) const
    {
        // canon index: the first trailer stays on the game's own T key — its
        // detach path is not safe to replay manually
        if ( i == 0 || i >= max_trailers ) return;
        void* trailer = canon[ i ];
        if ( trailer == nullptr || !readable_ptr( trailer ) ) return;
        if ( joint( trailer ) != nullptr )
        {
            this->disconnect_trailer( i );
        }
        else
        {
            this->connect_trailer( i );
        }
    }

    // keybind list: action name, the bound key as a clickable capture button,
    // and a small clear button — packed tight for the narrow side panel
    void CTrailerManipulation::render_keybind_row( const int i ) const
    {
        const auto action = static_cast< settings::Action >( i );
        const bool fixed = action == settings::Action::TOGGLE_UI
            || action == settings::Action::TOGGLE_CURSOR;
        ImGui::PushID( i );
        ImGui::TextWrapped( "%s", settings::action_name( action ) );
        if ( fixed )
        {
            // rebindable in settings.ini only — keeping them off the
            // capture path makes the plugin unreachable harder
            ImGui::TextDisabled( "%s (settings.ini)", settings::key_name( settings::key( action ) ).c_str() );
        }
        else
        {
            const auto label = settings::is_capturing( action )
                                   ? std::string( "Pressione uma tecla... (Esc cancela)" )
                                   : settings::key_name( settings::key( action ) );
            if ( ImGui::Button( fmt::format( "{}##bind", label ).c_str(), ImVec2( -ImGui::GetFrameHeight() * 1.8f, 0 ) ) )
            {
                settings::begin_capture( action );
            }
            ImGui::SameLine();
            ImGui::BeginDisabled( settings::key( action ) == 0 );
            if ( ImGui::SmallButton( "X##clear" ) )
            {
                settings::g_settings.keys[ i ] = 0;
                settings::save();
            }
            ImGui::EndDisabled();
        }
        ImGui::Spacing();
        ImGui::PopID();
    }

    void CTrailerManipulation::render_keybinds() const
    {
        for ( int i = 0; i < settings::action_count; ++i )
        {
            const auto action = static_cast< settings::Action >( i );
            // steering binds live in the Oversize panel
            if ( action == settings::Action::STEER_LEFT ||
                 action == settings::Action::STEER_RIGHT ||
                 action == settings::Action::STEER_CENTER ||
                 action == settings::Action::TOGGLE_STEERING_LOCK ) continue;
            this->render_keybind_row( i );
        }
    }

    // render_trailer_lift lives in trailer_axles.cpp;
    // render_trailer_steering + render_oversize in trailer_steering.cpp

    //========================================================================
    // UI
    //========================================================================
    void CTrailerManipulation::render_trailer_connect( const uint32_t i ) const
    {
        // i is the canon index — the button must always mean the same
        // physical trailer, no matter how trailers[] is currently rotated
        if ( i == 0 )
        {
            ImGui::TextDisabled( "Gerenciada pelo jogo (tecla T)" );
            return;
        }
        void* trailer = i < max_trailers ? canon[ i ] : nullptr;
        if ( trailer == nullptr || !readable_ptr( trailer ) ) return;

        ImGui::SeparatorText( "Engatar / Desengatar" );
        if ( !this->connect_valid_ )
        {
            ImGui::TextDisabled( "Indisponível nesta versão do jogo" );
            return;
        }

        const bool attached = joint( trailer ) != nullptr;
        ImGui::BeginDisabled( attached );
        if ( ImGui::Button( "Engatar##trailer" ) )
        {
            this->connect_trailer( i );
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled( !attached );
        if ( ImGui::Button( "Desengatar##trailer" ) )
        {
            this->disconnect_trailer( i );
        }
        ImGui::EndDisabled();

        // picking up a trailer behind the first one with the truck directly,
        // only possible while the fifth wheel is free (trailer 1 detached or
        // absent)
        if ( !attached )
        {
            const bool wheel_free = truck_towed == nullptr &&
                ( canon[ 0 ] == nullptr || joint( canon[ 0 ] ) == nullptr );
            ImGui::BeginDisabled( !wheel_free );
            const bool is_target = attach_target == trailer;
            if ( is_target ) ImGui::TextColored( ImVec4( 0.15f, 0.68f, 0.38f, 1.f ), "Alvo do T: esta carreta" );
            if ( ImGui::Button( is_target ? "Voltar alvo p/ carreta 1##trailer" : "Selecionar p/ engate (T)##trailer" ) )
            {
                this->connect_to_truck( trailer );
            }
            ImGui::EndDisabled();
            if ( !wheel_free )
            {
                ImGui::SameLine();
                ImGui::TextDisabled( "(quinta roda ocupada)" );
            }
        }
    }

    void CTrailerManipulation::render()
    {
        // PDV-CONTROLE palette, pre-linearized (ui::pdv): #232359 primary blue,
        // #222888 sidebar/hover, #303030 window, #282828 panels, #020202
        // inputs, #444444 border, #4a6fa5 buttons, #27ae60 green, white text
        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 10.f, 10.f ) );
        ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 10.f, 8.f ) );
        ImGui::PushStyleVar( ImGuiStyleVar_FrameRounding, 3.f );
        ImGui::PushStyleVar( ImGuiStyleVar_ChildRounding, 3.f );
        ImGui::PushStyleVar( ImGuiStyleVar_FrameBorderSize, 1.f );
        ImGui::PushStyleColor( ImGuiCol_Text, ui::pdv( 0xFFFFFF ) );
        ImGui::PushStyleColor( ImGuiCol_TextDisabled, ui::pdv( 0x656565 ) );
        ImGui::PushStyleColor( ImGuiCol_WindowBg, ui::pdv( 0x303030 ) );
        ImGui::PushStyleColor( ImGuiCol_ChildBg, ui::pdv( 0x282828 ) );
        ImGui::PushStyleColor( ImGuiCol_Border, ui::pdv( 0x444444 ) );
        ImGui::PushStyleColor( ImGuiCol_FrameBg, ui::pdv( 0x020202 ) );
        ImGui::PushStyleColor( ImGuiCol_TitleBg, ui::pdv( 0x232359 ) );
        ImGui::PushStyleColor( ImGuiCol_TitleBgActive, ui::pdv( 0x232359 ) );
        ImGui::PushStyleColor( ImGuiCol_Button, ui::pdv( 0x4A6FA5 ) );
        ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ui::pdv( 0x222888 ) );
        ImGui::PushStyleColor( ImGuiCol_ButtonActive, ui::pdv( 0x232359 ) );
        ImGui::PushStyleColor( ImGuiCol_Header, ui::pdv( 0x222888 ) );
        ImGui::PushStyleColor( ImGuiCol_HeaderHovered, ui::pdv( 0x4A6FA5 ) );
        ImGui::PushStyleColor( ImGuiCol_HeaderActive, ui::pdv( 0x27AE60 ) );
        ImGui::PushStyleColor( ImGuiCol_CheckMark, ui::pdv( 0x27AE60 ) );
        ImGui::PushStyleColor( ImGuiCol_Separator, ui::pdv( 0x444444 ) );

        ImGui::SetNextWindowSize( ImVec2( 1150.f, 500.f ), ImGuiCond_FirstUseEver );
        ImGui::Begin( "Controle de Carretas" );

        // left pane: keybinds, resizable by dragging the right border — the
        // chosen width is kept in settings.ini
        const float left_w = settings::g_settings.keys_width;
        ImGui::BeginChild( "##keybinds", ImVec2( left_w, 0.f ),
            ImGuiChildFlags_Border | ImGuiChildFlags_ResizeX );
        ImGui::SeparatorText( "Teclas" );
        this->render_keybinds();
        ImGui::SeparatorText( "Assistente" );
        bool reverse_lift = settings::g_settings.reverse_lift;
        if ( ui::toggle_switch( "##reverse_lift", &reverse_lift ) )
        {
            settings::g_settings.reverse_lift = reverse_lift;
            settings::save();
        }
        ImGui::SameLine();
        ImGui::TextWrapped( "Eixo na ré (vanderleia/4º eixo)" );
        ImGui::TextDisabled( "ativado: engatou a ré - ergue os eixos eleváveis da carreta, se existirem" );

        render_cargo_restore_option(); // trailer_loadstate.cpp

        ImGui::SeparatorText( "Freio" );
        float brake_boost = settings::g_settings.trailer_brake_boost;
        ImGui::SetNextItemWidth( -1.f );
        if ( ImGui::SliderFloat( "##brake_boost", &brake_boost, 0.f, 200.f, "+%.0f%%", ImGuiSliderFlags_AlwaysClamp ) )
        {
            settings::g_settings.trailer_brake_boost = brake_boost;
            settings::save();
        }
        ImGui::TextWrapped( "Carreta freia mais" );
        ImGui::TextDisabled( "regule o quanto de freio você quer aplicar a mais na carreta" );

        ImGui::SeparatorText( "Ar" );
        int air_boost = settings::g_settings.air_boost;
        ImGui::SetNextItemWidth( -1.f );
        if ( ImGui::SliderInt( "##air_boost", &air_boost, 0, 5,
             air_boost == 0 ? "original" : fmt::format( "+{}00%", air_boost ).c_str() ) )
        {
            settings::g_settings.air_boost = air_boost;
            settings::save();
        }
        ImGui::TextWrapped( "Compressor por rotação" );
        ImGui::TextDisabled( "multiplica a produção de ar (que já sobe com o giro) — sem mexer no consumo" );
        ImGui::EndChild();
        // the pane re-lays itself out automatically since every child sizes
        // from the available width; persist once the user lets go
        const float dragged_w = ImGui::GetItemRectSize().x;
        if ( dragged_w > 0.f && dragged_w != settings::g_settings.keys_width )
        {
            settings::g_settings.keys_width = dragged_w;
            if ( !ImGui::IsMouseDown( ImGuiMouseButton_Left ) ) settings::save();
        }

        ImGui::SameLine();

        // Oversize (steerable-axle) pane next to the keys — collapsible to a
        // thin strip so it stays out of the way when unused
        if ( settings::g_settings.oversize_collapsed )
        {
            ImGui::BeginChild( "##oversize_min", ImVec2( 34.f, 0.f ), ImGuiChildFlags_Border );
            if ( ImGui::Button( ">>", ImVec2( -1.f, ImGui::GetFrameHeight() ) ) )
            {
                settings::g_settings.oversize_collapsed = false;
                settings::save();
            }
            ImGui::EndChild();
        }
        else
        {
            ImGui::BeginChild( "##oversize", ImVec2( 290.f, 0.f ), ImGuiChildFlags_Border );
            if ( ImGui::Button( "<<", ImVec2( ImGui::GetFrameHeight() * 1.6f, 0.f ) ) )
            {
                settings::g_settings.oversize_collapsed = true;
                settings::save();
            }
            ImGui::SameLine();
            ImGui::SeparatorText( "Oversize" );
            this->render_oversize();
            ImGui::EndChild();
        }

        ImGui::SameLine();

        // right pane: one column per trailer, side by side — CANON order so
        // "Carreta N" is always the same physical piece no matter how the
        // rotated trailers[] list is currently arranged
        ImGui::BeginChild( "##trailers", ImVec2( 0.f, 0.f ) );
        uint32_t canon_count = 0;
        while ( canon_count < max_trailers && canon[ canon_count ] != nullptr ) ++canon_count;
        if ( canon_count == 0 )
        {
            ImGui::Text( "Nenhuma carreta encontrada" );
        }
        else
        {
            const float col_w = ( ImGui::GetContentRegionAvail().x
                - ImGui::GetStyle().ItemSpacing.x * ( canon_count - 1 ) ) / canon_count;
            for ( uint32_t i = 0; i < canon_count; ++i )
            {
                if ( i > 0 ) ImGui::SameLine();
                ImGui::PushID( static_cast< int >( i ) );
                ImGui::BeginChild( fmt::format( "##trailer{}", i ).c_str(), ImVec2( col_w, 0.f ), ImGuiChildFlags_Border );
                ImGui::SeparatorText( fmt::format( "Carreta {}", i + 1 ).c_str() );
                this->render_trailer_connect( i );
                ImGui::SeparatorText( "Eixos" );
                // lift/steer still key on trailers[] position — resolve the
                // canon piece back to its current slot
                int ti = -1;
                for ( uint32_t k = 0; k < trailer_count; ++k )
                {
                    if ( trailers[ k ] == canon[ i ] ) { ti = static_cast< int >( k ); break; }
                }
                if ( ti >= 0 ) this->render_trailer_lift( static_cast< uint32_t >( ti ) );
                ImGui::EndChild();
                ImGui::PopID();
            }
        }
        ImGui::EndChild();

        ImGui::End();
        ImGui::PopStyleColor( 16 );
        ImGui::PopStyleVar( 5 );
    }
}
