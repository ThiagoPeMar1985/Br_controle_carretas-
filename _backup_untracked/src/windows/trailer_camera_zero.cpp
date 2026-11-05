#include "trailer_shared.hpp"
#include "trailer_coupling_internal.hpp"

#include "core.hpp"
#include "prism/offsets.hpp"
#include "settings/settings.hpp"

namespace br_carretas
{
    bool block_camera_zero_teleport( void* arg )
    {
        if ( !settings::g_settings.cargo_restore ||
             ( GetAsyncKeyState( VK_F9 ) & 0x8000 ) == 0 ||
             ( GetAsyncKeyState( VK_CONTROL ) & 0x8000 ) == 0 ||
             arg == nullptr || !readable_ptr( arg ) )
        {
            return false;
        }
        const auto* request = static_cast< const hook_xform_t* >( arg );
        hook_xform_t current{};
        void* truck = nullptr;
        if ( auto* game_actor = CCore::g_instance->get_game_actor() )
            truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
        if ( truck == nullptr || !hook_world( truck, current ) ) return false;
        const float dx = request->pos.x - current.pos.x;
        const float dz = request->pos.z - current.pos.z;
        if ( dx * dx + dz * dz >= 3000.f * 3000.f ) return false;
        tp_block_watch = 120;
        camera_zero_block_tick.store( GetTickCount64(), std::memory_order_release );
        dump_chain( "pre-camera-zero-blocked" );
        return true;
    }
}
