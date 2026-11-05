// Load Game flow - while a save is loading (game state outside 2 = in-world)
// the plugin stays fully passive: hooks pass through natively and, once the
// new world is ready, every mark kept from the previous world is dropped.
// The save is authoritative for trailer positions and coupling state.

#include "trailer_shared.hpp"
#include "trailer_coupling_internal.hpp"

#include "core.hpp"

namespace br_carretas
{
    namespace
    {
        bool was_loading = false;
    }

    bool load_game_active()
    {
        return traffic_game_state == nullptr || *traffic_game_state != 2;
    }

    void load_game_update()
    {
        if ( load_game_active() )
        {
            was_loading = true;
            return;
        }
        if ( !was_loading ) return;
        was_loading = false;
        // world finished loading: nothing from the old world may replay -
        // canon/marks/anchors rebuild from the freshly loaded save
        reset_trailer_state();
    }
}
