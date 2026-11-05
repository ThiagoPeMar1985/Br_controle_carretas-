#include "trailer_shared.hpp"
#include "trailer_coupling_internal.hpp"

namespace br_carretas
{
    using actor_teleport_fn = void __fastcall( void*, void*, uint8_t, uint8_t );

    void garage_return_actor_teleport( void* actor, void* transform, uint8_t a3, uint8_t a4 )
    {
        actor_native_teleport_tick.store( GetTickCount64(), std::memory_order_release );
        actor_teleport_hook->get_original< actor_teleport_fn >()( actor, transform, a3, a4 );
    }
}
