#pragma once
#include <cstdint>

#include "window.hpp"
#include "prism/functions.hpp"

namespace br_carretas
{
    // carretas desatreláveis individualmente - engate/desengate pelas vfuncs
    class CTrailerManipulation : public CWindow
    {
    private:
        bool connect_valid_ = false; // engate precisa de vários hooks; desliga em build sem suporte

        void refresh_trailer_chain();
        void update_convoy_state();
        void gc_detached_marks();
        void update_hotkeys();
        void update_steering( float dt );
        void render_trailer_connect( uint32_t i ) const;
        void render_trailer_lift( uint32_t i ) const;
        void render_truck_lift() const;
        void render_trailer_steering( uint32_t i ) const;
        void render_keybind_row( int i ) const;
        void render_keybinds() const;
        void render_oversize() const;
        void render_keys_tab() const;
        void render_oversize_tab() const;
        void render_trailers_tab() const;
        void render_options_tab() const;
        void render_license_tab() const;
        void render_logs_tab() const;
        void connect_trailer( uint32_t i ) const;
        void connect_trailer_to_truck( uint32_t i ) const;
        void disconnect_trailer( uint32_t i ) const;
        void toggle_attach( uint32_t i ) const;
        void connect_to_truck( void* trailer ) const;

    public:
        CTrailerManipulation();
        ~CTrailerManipulation() override;

        bool init() override;
        void update();
        void update_impl();
        void render() override;
    };
}
