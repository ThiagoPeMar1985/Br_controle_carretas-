#include "core.hpp"

#include <cstdio>
#include <cstdlib>

#include <MinHook.h>
#include <eurotrucks2/scssdk_eut2.h>

#include "consts.hpp"

#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "memory/memory_utils.hpp"

#include "managers/window_manager.hpp"
#include "windows/trailer_manipulation.hpp"
#include "windows/hooks_window.hpp"
#include "settings/settings.hpp"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler( HWND, UINT, WPARAM, LPARAM );

namespace ts_extra_utilities
{
    void diag_log( const std::string& msg )
    {
        static FILE* f = nullptr;
        static std::string path;
        if ( f == nullptr && path.empty() )
        {
            const char* profile = getenv( "USERPROFILE" );
            path = std::string( profile != nullptr ? profile : "." ) +
                "\\Documents\\Euro Truck Simulator 2\\br_carretas_debug.log";
            f = fopen( path.c_str(), "a" );
        }
        if ( f == nullptr ) return;
        SYSTEMTIME st;
        GetLocalTime( &st );
        fprintf( f, "%02d:%02d:%02d.%03d %s\n",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg.c_str() );
        fflush( f );
    }

    CCore* CCore::g_instance = nullptr;

    CCore::CCore( const scs_telemetry_init_params_v101_t* init_params ) : init_params_( init_params )
    {
        this->hooks_manager_ = new CHooksManager();
        this->window_manager_ = new CWindowManager();
        scs_log_ = init_params->common.log;
        g_instance = this;
    }

    CCore::~CCore()
    {
        this->destroy();
        MH_RemoveHook( nullptr );
    }

    bool CCore::init()
    {
        const MH_STATUS mh_status = MH_Initialize();
        this->info( "Initializing {} (MH_Initialize={})", VERSION, static_cast< int >( mh_status ) );
        this->info( "game_id={}",
            init_params_->common.game_id != nullptr ? init_params_->common.game_id : "(null)" );

        // other plugins already mapped in the process - module collisions
        // with TM Real Walk showed up here before
        const HMODULE tm_walk = GetModuleHandleA( "TMRealWalk.dll" );
        if ( tm_walk != nullptr )
        {
            char tm_path[ MAX_PATH ] = {};
            GetModuleFileNameA( tm_walk, tm_path, MAX_PATH );
            this->info( "TMRealWalk.dll loaded at {:p} path={}",
                static_cast< void* >( tm_walk ), tm_path );
        }
        else
        {
            this->info( "TMRealWalk.dll not loaded" );
        }

        // this build targets ETS2 - its fixed image offsets were derived from
        // eurotrucks2.exe only; warn loudly if the dll gets loaded by ATS
        if ( init_params_->common.game_id == nullptr ||
            strcmp( init_params_->common.game_id, SCS_GAME_ID_EUT2 ) != 0 )
        {
            this->error( "This plugin is for Euro Truck Simulator 2 (game_id={})",
                init_params_->common.game_id != nullptr ? init_params_->common.game_id : "?" );
            return false;
        }

        truckersmp_ = GetModuleHandle( L"core_ets2mp.dll" ) != nullptr || GetModuleHandle( L"core_atsmp.dll" ) != nullptr;
        this->info( "truckersmp={}", truckersmp_ );

        this->dx11_hook = new CDirectX11Hook();
        if ( !this->dx11_hook->hook_present() )
        {
            this->error( "dx11 present hook failed - aborting init" );
            return false;
        }
        this->info( "dx11 present hook ok" );

        this->di8_hook = new CDirectInput8Hook();
        if ( !this->di8_hook->hook() )
        {
            this->error( "directinput8 hook failed - aborting init" );
            return false;
        }
        this->info( "directinput8 hook ok" );

        settings::load();
        this->info( "settings loaded" );

        this->trailer_manipulation_ = this->window_manager_->register_window( std::make_shared< CTrailerManipulation >() );

        if ( !this->trailer_manipulation_->init() )
        {
            g_instance->error( "Could not initialize the trailer control module" );
        }
        else
        {
            this->info( "trailer control module initialized" );
        }

        // hooks status stays registered for support, hidden by default - the
        // main window carries everything the user needs
        const auto hooks_window = this->window_manager_->register_window( std::make_shared< CHooksWindow >() );
        hooks_window->init();
        hooks_window->hide();

        return true;
    }

    void CCore::destroy()
    {
        delete this->dx11_hook;
        delete this->di8_hook;
        delete this->hooks_manager_;
        delete this->window_manager_;
    }

    bool CCore::on_mouse_input( LPDIDEVICEOBJECTDATA rgdod )
    {
        if ( rgdod->dwOfs == DIMOFS_Z && !this->disable_in_game_mouse )
        {
            this->in_game_wheel_delta += static_cast< int >( rgdod->dwData );
        }

        if ( !this->disable_in_game_mouse ) return false;

        auto& io = ImGui::GetIO();
        if ( rgdod->dwOfs == DIMOFS_X )
        {
            io.MousePos.x += static_cast< float >( static_cast< int >( rgdod->dwData ) );
        }
        else if ( rgdod->dwOfs == DIMOFS_Y )
        {
            io.MousePos.y += static_cast< float >( static_cast< int >( rgdod->dwData ) );
        }

        return true;
    }

    bool CCore::render()
    {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if ( this->trailer_manipulation_ != nullptr )
        {
            this->trailer_manipulation_->update();
        }

        if ( this->render_ui )
        {
#ifdef _DEBUG
            bool s = true;
            ImGui::ShowDemoWindow( &s );
#endif
            this->window_manager_->render();
        }
        ImGui::EndFrame();
        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData( ImGui::GetDrawData() );
        return true;
    }

    void CCore::toggle_input_hook()
    {
        this->disable_in_game_mouse = !this->disable_in_game_mouse;
        auto& io = ImGui::GetIO();
        if ( this->disable_in_game_mouse )
        {
            // release the cursor at the center of the screen, then leave it free
            io.MousePos.x = io.DisplaySize.x * 0.5f;
            io.MousePos.y = io.DisplaySize.y * 0.5f;
        }
        else
        {
            last_mouse_pos_x_ = io.MousePos.x;
            last_mouse_pos_y_ = io.MousePos.y;
        }

        io.MouseDrawCursor = this->disable_in_game_mouse;

        this->debug( "Mouse hook is now {}", this->disable_in_game_mouse ? "active" : "disabled" );
    }

    void CCore::toggle_ui()
    {
        this->render_ui = !this->render_ui;
    }

    bool CCore::on_wnd_proc( HWND hwnd, UINT umsg, WPARAM wparam, LPARAM lparam )
    {
        if ( umsg == WM_KEYDOWN || umsg == WM_SYSKEYDOWN )
        {
            const auto vk = static_cast< UINT >( wparam );
            if ( settings::capture_key( vk ) )
            {
                return true;
            }

            // pack held modifiers so combo binds (Ctrl+X etc.) match too
            UINT packed = vk;
            if ( GetAsyncKeyState( VK_CONTROL ) & 0x8000 ) packed |= settings::KEY_MOD_CTRL;
            if ( GetAsyncKeyState( VK_SHIFT )   & 0x8000 ) packed |= settings::KEY_MOD_SHIFT;
            if ( GetAsyncKeyState( VK_MENU )    & 0x8000 ) packed |= settings::KEY_MOD_ALT;

            const bool repeat = ( lparam & ( 1 << 30 ) ) != 0; // held down: toggle once, not on every auto-repeat
            if ( packed == settings::key( settings::Action::TOGGLE_CURSOR ) )
            {
                if ( !repeat ) toggle_input_hook();
                return true;
            }
            if ( packed == settings::key( settings::Action::TOGGLE_UI ) )
            {
                if ( !repeat ) toggle_ui();
                return true;
            }
        }
        else if ( umsg == WM_MOUSEMOVE )
        {
            if ( this->disable_in_game_mouse )
            {
                return true;
            }
        }

        if ( ImGui_ImplWin32_WndProcHandler( hwnd, umsg, wparam, lparam ) )
        {
            return true;
        }
        return false;
    }

    prism::base_ctrl_u* CCore::get_base_ctrl_instance()
    {
        if ( this->base_ctrl_instance_ptr_address != 0 ) return *reinterpret_cast< prism::base_ctrl_u** >( this->base_ctrl_instance_ptr_address );
        if ( this->base_ctrl_scan_failed_ ) return nullptr;

        // 1.61: the `cheat repair` handler loads the game singleton and its game_actor back to back
        const auto addr = memory::get_address_for_pattern( "48 8b 3d ? ? ? ? 48 8b bf ? ? ? ? 48 8b 5f 18 48 8b 9b" );

        if ( addr == 0 )
        {
            this->base_ctrl_scan_failed_ = true;
            this->error( "Could not find base_ctrl, trailer control is unavailable" );
            return nullptr;
        }
        this->base_ctrl_instance_ptr_address = addr + *reinterpret_cast< int32_t* >( addr + 3 ) + 7;
        this->game_actor_offset_in_base_ctrl = *reinterpret_cast< int32_t* >( addr + 10 );

        this->info( "Found base_ctrl @ +{:x}", memory::as_offset( this->base_ctrl_instance_ptr_address ) );

        return *reinterpret_cast< prism::base_ctrl_u** >( this->base_ctrl_instance_ptr_address );
    }

    prism::game_actor_u* CCore::get_game_actor()
    {
        auto* base_ctrl = this->get_base_ctrl_instance();

        if ( base_ctrl == nullptr || this->game_actor_offset_in_base_ctrl == 0 )
        {
            return nullptr;
        }

        return *reinterpret_cast< prism::game_actor_u** >( reinterpret_cast< uint64_t >( base_ctrl ) + this->game_actor_offset_in_base_ctrl );
    }
}
