#include "trailer_shared.hpp"
#include "trailer_coupling_internal.hpp"

#include "core.hpp"
#include "prism/offsets.hpp"

namespace br_carretas
{
    namespace
    {
        bool was_loading = false;

        void* last_truck = nullptr;
    }

    // 146 - jogo está carregando (fora do mundo)
    bool load_game_active()
    {
        return traffic_game_state == nullptr || *traffic_game_state != 2;
    }

    // 147 - por tick: detecta fim de load e zera o estado do mundo antigo
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

        reset_trailer_state();
        workshop_respawn_discard();
    }
    namespace
    {
        const bool fn_idx_reg = []()
        {
            fn_index_add( fn_addr_of( &load_game_active ), 146, "load_game_active" );
            fn_index_add( fn_addr_of( &load_game_update ), 147, "load_game_update" );
            return true;
        }();
    }
}
