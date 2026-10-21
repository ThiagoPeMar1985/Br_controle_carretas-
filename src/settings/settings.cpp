#include "settings.hpp"

#include <ShlObj.h>
#include <filesystem>
#include <utility>
#include <intrin.h>
#include <mmsystem.h>

#include "fmt/core.h"

#pragma comment( lib, "winmm.lib" )

namespace ts_extra_utilities::settings
{
    Settings g_settings;

    namespace
    {
        constexpr UINT default_keys[ action_count ] = {
            VK_DELETE, // TOGGLE_UI
            VK_INSERT, // TOGGLE_CURSOR
            0, // ATTACH_DETACH_2
            0, // ATTACH_DETACH_3
            0, // ATTACH_TRUCK_2
            0, // ATTACH_TRUCK_3
        };

        // ini keys, kept stable so saved files keep working if display names change
        constexpr const wchar_t* ini_keys[ action_count ] = {
            L"toggle_ui",
            L"toggle_cursor",
            L"attach_detach_2",
            L"attach_detach_3",
            L"attach_truck_2",
            L"attach_truck_3",
        };

        constexpr const char* names[ action_count ] = {
            "Abrir/fechar janela",
            "Liberar cursor do mouse",
            "Engatar/desengatar carreta 2",
            "Engatar/desengatar carreta 3",
            "Engatar carreta 2 no cavalo",
            "Engatar carreta 3 no cavalo",
        };

        int capturing = -1;

        // friendly key names accepted in settings.ini — a plain number still
        // works and stays the authoritative write format. Combos parse as
        // "CTRL+DELETE", "SHIFT+F5", "ALT+J", joystick buttons as "JOY0:B3"
        UINT key_from_name( const wchar_t* text )
        {
            if ( text == nullptr || text[ 0 ] == 0 ) return UINT_MAX;

            if ( text[ 0 ] >= L'0' && text[ 0 ] <= L'9' )
            {
                const long v = _wtol( text );
                // packed values with modifiers/joystick bits land above 0xFE too
                return v > 0 && v <= 0x01FFFFFF ? static_cast< UINT >( v ) : UINT_MAX;
            }

            wchar_t name[ 32 ] = {};
            size_t i = 0;
            for ( ; text[ i ] != 0 && i < 31; ++i ) name[ i ] = towupper( text[ i ] );
            while ( i > 0 && name[ i - 1 ] == L' ' ) name[ --i ] = 0;

            // joystick button: JOY<n>:B<btn> — n = device index 0..15
            if ( name[ 0 ] == L'J' && name[ 1 ] == L'O' && name[ 2 ] == L'Y' )
            {
                const wchar_t* colon = wcschr( name, L':' );
                if ( colon != nullptr && colon[ 1 ] == L'B' )
                {
                    const long joy = _wtol( name + 3 );
                    const long btn = _wtol( colon + 2 );
                    if ( joy >= 0 && joy <= 15 && btn >= 0 && btn <= 31 )
                    {
                        return KEY_JOYSTICK | ( static_cast< UINT >( joy ) << 12 ) | static_cast< UINT >( btn );
                    }
                }
                return UINT_MAX;
            }

            // split off "CTRL+"/"SHIFT+"/"ALT+" prefixes; the remainder resolves
            // as a normal key name
            UINT mods = 0;
            const wchar_t* key_part = name;
            while ( const wchar_t* plus = wcschr( key_part, L'+' ) )
            {
                const size_t len = plus - key_part;
                if ( len == 4 && wcsncmp( key_part, L"CTRL", 4 ) == 0 ) mods |= KEY_MOD_CTRL;
                else if ( len == 5 && wcsncmp( key_part, L"SHIFT", 5 ) == 0 ) mods |= KEY_MOD_SHIFT;
                else if ( len == 3 && wcsncmp( key_part, L"ALT", 3 ) == 0 ) mods |= KEY_MOD_ALT;
                else break;
                key_part = plus + 1;
            }
            if ( key_part[ 0 ] == 0 ) return UINT_MAX;

            UINT vk = UINT_MAX;
            if ( key_part[ 0 ] == L'F' && key_part[ 1 ] >= L'1' && key_part[ 1 ] <= L'9' )
            {
                const long f = _wtol( key_part + 1 );
                if ( f >= 1 && f <= 24 ) vk = VK_F1 + f - 1;
            }
            else if ( key_part[ 1 ] == 0 &&
                ( ( key_part[ 0 ] >= L'A' && key_part[ 0 ] <= L'Z' ) || ( key_part[ 0 ] >= L'0' && key_part[ 0 ] <= L'9' ) ) )
            {
                vk = key_part[ 0 ]; // letters/digits: VK code is the ASCII char
            }
            else
            {
                static const std::pair< const wchar_t*, UINT > table[] = {
                    { L"INSERT", VK_INSERT }, { L"DELETE", VK_DELETE }, { L"HOME", VK_HOME },
                    { L"END", VK_END }, { L"PAGEUP", VK_PRIOR }, { L"PAGEDOWN", VK_NEXT },
                    { L"UP", VK_UP }, { L"DOWN", VK_DOWN }, { L"LEFT", VK_LEFT }, { L"RIGHT", VK_RIGHT },
                    { L"SPACE", VK_SPACE }, { L"TAB", VK_TAB }, { L"ENTER", VK_RETURN },
                    { L"RETURN", VK_RETURN }, { L"BACKSPACE", VK_BACK }, { L"ESCAPE", VK_ESCAPE },
                    { L"NUMPAD0", VK_NUMPAD0 }, { L"NUMPAD1", VK_NUMPAD1 }, { L"NUMPAD2", VK_NUMPAD2 },
                    { L"NUMPAD3", VK_NUMPAD3 }, { L"NUMPAD4", VK_NUMPAD4 }, { L"NUMPAD5", VK_NUMPAD5 },
                    { L"NUMPAD6", VK_NUMPAD6 }, { L"NUMPAD7", VK_NUMPAD7 }, { L"NUMPAD8", VK_NUMPAD8 },
                    { L"NUMPAD9", VK_NUMPAD9 }, { L"MINUS", VK_OEM_MINUS }, { L"EQUALS", VK_OEM_PLUS },
                    { L"COMMA", VK_OEM_COMMA }, { L"PERIOD", VK_OEM_PERIOD },
                };
                for ( const auto& entry : table )
                {
                    if ( wcscmp( key_part, entry.first ) == 0 ) { vk = entry.second; break; }
                }
            }
            return vk == UINT_MAX ? UINT_MAX : mods | vk;
        }

        std::filesystem::path settings_path()
        {
            PWSTR appdata = nullptr;
            std::filesystem::path path;
            if ( SUCCEEDED( SHGetKnownFolderPath( FOLDERID_RoamingAppData, 0, nullptr, &appdata ) ) )
            {
                path = std::filesystem::path( appdata ) / L"br_controle_de_carretas" / L"settings.ini";
            }
            CoTaskMemFree( appdata );
            return path;
        }

        // a br-settings.ini shipped next to the dll lets a release preset
        // binds out of the box — used only while the user has no own file yet.
        // The br- prefix keeps it from colliding with other plugins' inis
        // sharing the same plugins folder
        std::filesystem::path plugin_dir_settings_path()
        {
            HMODULE self = nullptr;
            if ( !GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     reinterpret_cast< LPCWSTR >( &plugin_dir_settings_path ),
                                     &self ) )
            {
                return {};
            }
            wchar_t dir[ MAX_PATH ] = {};
            GetModuleFileNameW( self, dir, MAX_PATH );
            return std::filesystem::path( dir ).parent_path() / L"br-settings.ini";
        }
    }

    void load()
    {
        for ( int i = 0; i < action_count; ++i )
        {
            g_settings.keys[ i ] = default_keys[ i ];
        }
        for ( int i = 0; i < trailer_slot_count; ++i )
        {
            g_settings.axle_masks[ i ] = -1;
        }
        g_settings.reverse_lift = true;
        g_settings.cargo_restore = true;

        auto path = settings_path();
        if ( path.empty() ) return;

        const auto bundled = plugin_dir_settings_path();
        const bool has_bundled = !bundled.empty() && std::filesystem::exists( bundled );

        // first run: a shipped br-settings.ini next to the dll seeds the binds
        if ( has_bundled && !std::filesystem::exists( path ) ) path = bundled;

        for ( int i = 0; i < action_count; ++i )
        {
            wchar_t value[ 32 ] = {};
            GetPrivateProfileStringW( L"keybinds", ini_keys[ i ], L"", value, 32, path.c_str() );
            const UINT parsed = key_from_name( value );
            if ( parsed != UINT_MAX ) g_settings.keys[ i ] = parsed;
        }

        // the window/cursor keys can only be changed through a file, so when a
        // br-settings.ini sits next to the dll it governs those two even when
        // the user's own ini already exists — everything else still comes
        // from the user file
        if ( has_bundled && bundled != path )
        {
            for ( const int i : { static_cast< int >( Action::TOGGLE_UI ), static_cast< int >( Action::TOGGLE_CURSOR ) } )
            {
                wchar_t value[ 32 ] = {};
                GetPrivateProfileStringW( L"keybinds", ini_keys[ i ], L"", value, 32, bundled.c_str() );
                const UINT parsed = key_from_name( value );
                if ( parsed != UINT_MAX ) g_settings.keys[ i ] = parsed;
            }
        }

        // the two critical keys stay rebindable through the ini, but an
        // unbound/garbage value or a collision would leave the plugin
        // unreachable — fall back to the defaults instead
        const int ui = static_cast< int >( Action::TOGGLE_UI );
        const int cursor = static_cast< int >( Action::TOGGLE_CURSOR );
        if ( g_settings.keys[ ui ] == 0 || g_settings.keys[ cursor ] == 0 ||
            g_settings.keys[ ui ] == g_settings.keys[ cursor ] )
        {
            g_settings.keys[ ui ] = VK_DELETE;
            g_settings.keys[ cursor ] = VK_INSERT;
        }

        for ( int i = 0; i < trailer_slot_count; ++i )
        {
            wchar_t key_name_w[ 32 ] = {};
            swprintf_s( key_name_w, L"trailer_%d", i );
            g_settings.axle_masks[ i ] = GetPrivateProfileIntW(
                L"axle_lift", key_name_w, -1, path.c_str() );
        }
        g_settings.reverse_lift = GetPrivateProfileIntW( L"axle_lift", L"reverse_lift", 1, path.c_str() ) != 0;
        g_settings.cargo_restore = GetPrivateProfileIntW( L"general", L"cargo_restore", 1, path.c_str() ) != 0;
    }

    void save()
    {
        const auto path = settings_path();
        if ( path.empty() ) return;

        std::error_code ec;
        std::filesystem::create_directories( path.parent_path(), ec );

        for ( int i = 0; i < action_count; ++i )
        {
            WritePrivateProfileStringW( L"keybinds", ini_keys[ i ], std::to_wstring( g_settings.keys[ i ] ).c_str(), path.c_str() );
        }
        for ( int i = 0; i < trailer_slot_count; ++i )
        {
            wchar_t key_name_w[ 32 ] = {};
            swprintf_s( key_name_w, L"trailer_%d", i );
            WritePrivateProfileStringW( L"axle_lift", key_name_w,
                std::to_wstring( g_settings.axle_masks[ i ] ).c_str(), path.c_str() );
        }
        WritePrivateProfileStringW( L"axle_lift", L"reverse_lift",
            g_settings.reverse_lift ? L"1" : L"0", path.c_str() );
        WritePrivateProfileStringW( L"general", L"cargo_restore",
            g_settings.cargo_restore ? L"1" : L"0", path.c_str() );
    }

    UINT key( const Action action )
    {
        return g_settings.keys[ static_cast< int >( action ) ];
    }

    const char* action_name( const Action action )
    {
        return names[ static_cast< int >( action ) ];
    }

    std::string key_name( const UINT packed )
    {
        if ( packed == 0 ) return "(sem tecla)";

        if ( packed & KEY_JOYSTICK )
        {
            return fmt::format( "Joy {} Botão {}", ( packed >> 12 ) & 0xF, packed & 0xFF );
        }

        const UINT vk = packed & KEY_CODE_MASK;
        std::string mods;
        if ( packed & KEY_MOD_CTRL )  mods += "Ctrl+";
        if ( packed & KEY_MOD_SHIFT ) mods += "Shift+";
        if ( packed & KEY_MOD_ALT )   mods += "Alt+";

        LONG scan = static_cast< LONG >( MapVirtualKeyW( vk, MAPVK_VK_TO_VSC ) );
        switch ( vk )
        {
            // these share scan codes with the numpad and need the extended bit to get their own name
            case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
            case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_NUMLOCK:
            case VK_RCONTROL: case VK_RMENU:
                scan |= 0x100;
                break;
            default:
                break;
        }

        char name[ 64 ] = {};
        if ( scan != 0 && GetKeyNameTextA( scan << 16, name, sizeof( name ) ) > 0 )
        {
            return mods + name;
        }
        return mods + fmt::format( "Key 0x{:02X}", vk );
    }

    void begin_capture( const Action action )
    {
        capturing = static_cast< int >( action );
    }

    bool is_capturing()
    {
        return capturing >= 0;
    }

    bool is_capturing( const Action action )
    {
        return capturing == static_cast< int >( action );
    }

    bool capture_key( const UINT vk )
    {
        if ( capturing < 0 ) return false;

        // modifiers alone are not useful bindings; wait for a real key
        if ( vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
             vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LCONTROL || vk == VK_RCONTROL ||
             vk == VK_LMENU || vk == VK_RMENU ) return true;

        // fold in whatever modifiers are held so "Ctrl+J" binds as a combo
        UINT packed = vk;
        if ( GetAsyncKeyState( VK_CONTROL ) & 0x8000 ) packed |= KEY_MOD_CTRL;
        if ( GetAsyncKeyState( VK_SHIFT )   & 0x8000 ) packed |= KEY_MOD_SHIFT;
        if ( GetAsyncKeyState( VK_MENU )    & 0x8000 ) packed |= KEY_MOD_ALT;

        // whatever currently opens the window / releases the cursor is
        // reserved — stealing it here would leave the plugin unreachable
        if ( packed == key( Action::TOGGLE_UI ) || packed == key( Action::TOGGLE_CURSOR ) )
        {
            capturing = -1;
            return true;
        }

        if ( vk != VK_ESCAPE )
        {
            // a key can only do one thing: take it away from any other action
            for ( auto& bound : g_settings.keys )
            {
                if ( bound == packed ) bound = 0;
            }
            g_settings.keys[ capturing ] = packed;
            save();
        }
        capturing = -1;
        return true;
    }

    bool joystick_button_down( const UINT packed )
    {
        const UINT joy = ( packed >> 12 ) & 0xF;
        const UINT btn = packed & 0xFF;
        JOYINFOEX info{};
        info.dwSize = sizeof( info );
        info.dwFlags = JOY_RETURNBUTTONS;
        if ( joyGetPosEx( JOYSTICKID1 + joy, &info ) != JOYERR_NOERROR ) return false;
        return ( info.dwButtons & ( 1u << btn ) ) != 0;
    }

    void poll_joystick_capture()
    {
        if ( capturing < 0 ) return;

        // scan all present joysticks for a newly pressed button
        for ( UINT joy = 0; joy < 16; ++joy )
        {
            JOYINFOEX info{};
            info.dwSize = sizeof( info );
            info.dwFlags = JOY_RETURNBUTTONS;
            if ( joyGetPosEx( JOYSTICKID1 + joy, &info ) != JOYERR_NOERROR ) continue;

            static uint32_t prev_buttons[ 16 ] = {};
            const uint32_t pressed = info.dwButtons & ~prev_buttons[ joy ];
            prev_buttons[ joy ] = info.dwButtons;
            if ( pressed == 0 ) continue;

            DWORD btn = 0;
            BitScanForward( &btn, pressed );
            const UINT packed = KEY_JOYSTICK | ( joy << 12 ) | btn;
            for ( auto& bound : g_settings.keys )
            {
                if ( bound == packed ) bound = 0;
            }
            g_settings.keys[ capturing ] = packed;
            save();
            capturing = -1;
            return;
        }
    }
}
