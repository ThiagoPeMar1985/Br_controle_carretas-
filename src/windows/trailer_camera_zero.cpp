#include "trailer_shared.hpp"

#include "core.hpp"
#include "prism/offsets.hpp"
#include "settings/settings.hpp"

namespace br_carretas
{
    namespace
    {
        uint64_t last_block_log_ms = 0;
    }

    // 203 - bloqueia o teleporte do zerar-câmera (Ctrl+F9) com a opção ligada
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
        const float dist_sq = dx * dx + dz * dz;
        if ( dist_sq >= 3000.f * 3000.f ) return false;
        const uint64_t now = GetTickCount64();
        if ( now - last_block_log_ms > 1000 )
        {
            last_block_log_ms = now;
            CCore::g_instance->info( "camera-zero teleport blocked: {:.1f}m, truck stays in place",
                                     std::sqrt( dist_sq ) );
        }
        return true;
    }
    namespace
    {
        const bool fn_idx_reg = []()
        {
            fn_index_add( fn_addr_of( &block_camera_zero_teleport ), 203, "block_camera_zero_teleport" );
            return true;
        }();
    }
}
