#include "trailer_manipulation.hpp"
#include "trailer_shared.hpp"
#include "trailer_coupling_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core.hpp"
#include "prism/offsets.hpp"
#include "settings/settings.hpp"

namespace br_carretas
{
    // teleport de ator (0x1405DE1E0): move o conjunto inteiro (doca/viagem)
    using actor_teleport_fn = void __fastcall( void* /*actor*/, void* /*arg*/, uint8_t, uint8_t );
    void __fastcall hk_actor_teleport( void* self, void* arg, uint8_t a3, uint8_t a4 )
    {
        // checkbox off = sistema todo bypassado: chamada nativa pura
        if ( !settings::g_settings.cargo_restore )
        {
            rig_armed = false;
            garage_return_actor_teleport( self, arg, a3, a4 );
            return;
        }
        // mundo montando: passthrough puro
        if ( canon_age < 90 )
        {
            garage_return_actor_teleport( self, arg, a3, a4 );
            return;
        }
        if ( block_camera_zero_teleport( arg ) ) return;
        if ( ( GetAsyncKeyState( VK_F9 ) & 0x8000 ) != 0 &&
             ( GetAsyncKeyState( VK_CONTROL ) & 0x8000 ) != 0 )
        {
            garage_return_actor_teleport( self, arg, a3, a4 );
            return;
        }
        bool cargo_actor_teleport = false;
        if ( arg != nullptr && readable_ptr( arg ) )
        {
            const auto* req = static_cast< const hook_xform_t* >( arg );
            hook_xform_t cur{};
            void* truck = nullptr;
            if ( auto* game_actor = CCore::g_instance->get_game_actor() )
                truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
            if ( truck != nullptr && hook_world( truck, cur ) )
            {
                const float dx = req->pos.x - cur.pos.x;
                const float dz = req->pos.z - cur.pos.z;
                const float dist = std::sqrt( dx * dx + dz * dz );
                const float dot = std::fabs( req->quat.w * cur.quat.w + req->quat.x * cur.quat.x +
                                             req->quat.y * cur.quat.y + req->quat.z * cur.quat.z );
                const float deg = 2.f * std::acos( dot > 1.f ? 1.f : dot ) * 57.29578f;
                if ( dist < 3000.f && settings::g_settings.cargo_restore )
                {
                    bool any_detached = false;
                    for ( void* d : plugin_detached )
                        if ( d != nullptr ) { any_detached = true; break; }
                    for ( bool f : detached_idx ) any_detached |= f;
                    cargo_actor_teleport = any_detached;
                }
                CCore::g_instance->info(
                    "actor teleport request: {:.1f}m, {:.1f}deg turn, a3={} a4={} -> {}",
                    dist, deg, a3, a4, cargo_actor_teleport ? "cargo" : "native" );
            }
        }
        if ( !cargo_actor_teleport )
        {
            garage_return_actor_teleport( self, arg, a3, a4 );
            return;
        }
        cargo_trace_begin( "actor-cargo-teleport", self );
        cargo_actor_teleport_rig( self, arg, a3, a4 );
    }

    int tp_cooldown = 0;
}
