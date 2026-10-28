#pragma once
#include <string>
#include <Windows.h>

namespace ts_extra_utilities::settings
{
    enum class Action
    {
        TOGGLE_UI,
        TOGGLE_CURSOR,
        ATTACH_DETACH_2, // toggle attach/detach of the trailer at index 1 (shown as "Carreta 2")
        ATTACH_DETACH_3, // toggle attach/detach of the trailer at index 2 (shown as "Carreta 3")
        ATTACH_TRUCK_2, // connect the index-1 trailer directly to the truck
        ATTACH_TRUCK_3, // connect the index-2 trailer directly to the truck
        STEER_LEFT,     // steerable-axle trailers: turn wheels left (hold)
        STEER_RIGHT,    // steerable-axle trailers: turn wheels right (hold)
        STEER_CENTER,   // steerable-axle trailers: ease wheels back to center
        TOGGLE_STEERING_LOCK, // take over / release automatic steering
        SLEEP_PARK,     // press -> native rest screen (injects Shift+Num0)
        COUNT,
    };

    constexpr int action_count = static_cast< int >( Action::COUNT );
    constexpr int trailer_slot_count = 20; // matches max_trailers in trailer_manipulation.cpp

    struct Settings
    {
        UINT keys[ action_count ] = {}; // virtual-key codes, 0 = unbound
        // per-slot axle selection masks for the trailer-lift feature (-1 = native).
        // The N-th trailer in the chain always uses slot N.
        int32_t axle_masks[ trailer_slot_count ];
        // raise the first liftable axle while reverse gear is engaged
        bool reverse_lift = true;
        // restore the rig and re-park detached trailers after the cargo
        // cinematic teleports the set into the dock. Off = short teleports
        // (camera-0, convoy sync) move the truck freely
        bool cargo_restore = true;
        // steering wheel response while a steer key is held, angle units
        // per second (full lock = 1)
        float steering_speed = 0.5f;
        // Oversize (steerable axle) panel: hidden when the user collapses it
        bool oversize_collapsed = false;
        // width of the keybinds pane, user-resizable
        float keys_width = 250.f;
        // active UI tab (0 Teclas, 1 Oversize, 2 Carretas, 3 Licença); -1 = collapsed
        int32_t ui_tab = 0;
        // license key issued by the site (screen only, not validated yet)
        char license_key[ 256 ] = {};
        // extra brake factor forwarded to coupled trailers, in percent of the
        // truck's value (0 = native balance): the trailers brake proportionally
        // harder so they take the load first on slippery surfaces
        float trailer_brake_boost = 0.f;
        // air compressor output multiplier: 0 = native, each step adds 100%
        // of the native (rpm-dependent) production — N means N+1 times total
        int32_t air_boost = 0;
        // air lost per game hour while the engine is off (bar/h, 0 = none)
        float air_leak = 0.f;
    };

    extern Settings g_settings;

    // Packed key value: low byte = VK code or joystick button index,
    // bits 8-10 = Ctrl/Shift/Alt modifiers, bit 24 = joystick flag with the
    // device index in bits 12-15. Plain VK codes keep working untouched.
    constexpr UINT KEY_MOD_CTRL  = 0x00000100;
    constexpr UINT KEY_MOD_SHIFT = 0x00000200;
    constexpr UINT KEY_MOD_ALT   = 0x00000400;
    constexpr UINT KEY_JOYSTICK  = 0x01000000;
    constexpr UINT KEY_CODE_MASK = 0x000000FF;

    // %APPDATA%\br_controle_de_carretas\settings.ini, shared by ETS2 and ATS
    void load();
    void save();

    UINT key( Action action );
    const char* action_name( Action action );
    std::string key_name( UINT vk );

    // joystick helpers (winmm — covers wheels/button boxes, up to 32 buttons)
    bool joystick_button_down( UINT packed );
    // call every frame: binds a joystick button pressed while capturing
    void poll_joystick_capture();

    // Rebinding: while capturing, the next key press is taken by capture_key() instead of the game.
    void begin_capture( Action action );
    bool is_capturing();
    bool is_capturing( Action action );
    // Returns true when the key was consumed. Escape cancels, anything else becomes the binding.
    bool capture_key( UINT vk );
}
