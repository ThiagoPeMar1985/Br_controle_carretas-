// Load Game flow - while a save is loading (game state outside 2 = in-world)
// the plugin stays fully passive: hooks pass through natively and, once the
// new world is ready, every mark kept from the previous world is dropped.
// The save is authoritative for trailer positions and coupling state.

#include "trailer_shared.hpp"
#include "trailer_coupling_internal.hpp"

#include "core.hpp"
#include "prism/offsets.hpp"

namespace br_carretas
{
    namespace
    {
        bool was_loading = false;
        // the truck object is only recreated on a world (re)load - workshop
        // and cargo respawns keep it, so a swap means a load happened even
        // when the game-state field never left "in-world"
        void* last_truck = nullptr;
    }

    bool load_game_active()
    {
        return traffic_game_state == nullptr || *traffic_game_state != 2;
    }

    void load_game_update()
    {
        void* truck = nullptr;
        if ( auto* ga = CCore::g_instance->get_game_actor() )
        {
            truck = prism::offsets::field< void* >( ga, prism::offsets::game_actor_truck );
        }
        if ( load_game_active() )
        {
            was_loading = true;
            return;
        }
        const bool truck_swapped =
            truck != nullptr && last_truck != nullptr && truck != last_truck;
        if ( truck != nullptr ) last_truck = truck;
        if ( !was_loading && !truck_swapped ) return;
        was_loading = false;
        // world finished loading: nothing from the old world may replay -
        // canon/marks/anchors rebuild from the freshly loaded save and the
        // workshop hold follows the load, not the old-world pointers
        reset_trailer_state();
        workshop_respawn_discard();
    }
}
