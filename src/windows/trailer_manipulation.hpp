#pragma once
#include <cstdint>

#include "window.hpp"
#include "prism/functions.hpp"

namespace ts_extra_utilities
{
    // Individually detachable trailers for 1.61 — connect/disconnect go through
    // vfunc slots verified against the native attach path.
    class CTrailerManipulation : public CWindow
    {
    private:
        bool connect_valid_ = false; // connect/disconnect needs several hooks; off on unsupported builds

        void refresh_trailer_chain();
        void update_convoy_state();
        void gc_detached_marks();
        void update_hotkeys();
        void update_steering( float dt );
        void render_trailer_connect( uint32_t i ) const;
        void render_trailer_lift( uint32_t i ) const;
        void render_trailer_steering( uint32_t i ) const;
        void render_keybind_row( int i ) const;
        void render_keybinds() const;
        void render_oversize() const;
        void connect_trailer( uint32_t i ) const;
        void disconnect_trailer( uint32_t i ) const;
        void toggle_attach( uint32_t i ) const;
        void connect_to_truck( uint32_t i ) const;

    public:
        CTrailerManipulation();
        ~CTrailerManipulation() override;

        bool init() override;
        // every frame, also while the window is hidden: refreshes the trailer chain and polls the attach hotkeys
        void update();
        void render() override;
    };
}
