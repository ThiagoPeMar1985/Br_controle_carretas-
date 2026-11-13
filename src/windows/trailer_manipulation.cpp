#include "trailer_manipulation.hpp"
#include "trailer_coupling_internal.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <intrin.h>
#include <utility>
#include <vector>

#include "imgui.h"

#include "common/scssdk_telemetry_truck_common_channels.h"

#include "core.hpp"
#include "hooks/function_hook.hpp"
#include "memory/memory_utils.hpp"
#include "prism/common.hpp"
#include "prism/offsets.hpp"
#include "prism/functions.hpp"
#include "settings/settings.hpp"
#include "license/license.hpp"

namespace br_carretas
{

    void trigger_native_sleep();

    void* canon[ max_trailers ] = {};

    uint32_t canon_age = 0;
    bool lw_ctx_logged[ max_trailers ] = {};

    // 065 - registra a carreta na ordem canon (slot fixo por peça)
    void track_canon( void* t )
    {
        if ( t == nullptr ) return;
        for ( auto& c : canon )
        {
            if ( c == t ) return;
            if ( c == nullptr ) { c = t; canon_age = 0; return; }
        }
    }

    // 066 - slot canon da carreta (-1 = desconhecida)
    int canon_index_of( const void* t )
    {
        for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
            if ( canon[ i ] == t ) return static_cast< int >( i );
        return -1;
    }

    struct synthetic_slave_edge
    {
        std::atomic< void* > owner{ nullptr };
        std::atomic< void* > child{ nullptr };
    };
    synthetic_slave_edge synthetic_edges[ max_trailers ];

    // 067 - diz se um link slave foi escrito pelo plugin
    bool claim_edge_ours( const void* owner, const void* child )
    {
        if ( owner == nullptr || child == nullptr ) return false;
        for ( auto& edge : synthetic_edges )
        {
            if ( edge.owner.load( std::memory_order_acquire ) == owner &&
                 edge.child.load( std::memory_order_acquire ) == child )
                return true;
        }
        return false;
    }

    // 068 - apaga o registro do link do plugin
    void clear_claim_edge( const void* owner )
    {
        if ( owner == nullptr ) return;
        for ( auto& edge : synthetic_edges )
        {
            if ( edge.owner.load( std::memory_order_acquire ) != owner ) continue;
            edge.owner.store( nullptr, std::memory_order_release );
            edge.child.store( nullptr, std::memory_order_release );
        }
    }

    // 069 - registra um link slave escrito pelo plugin
    void set_claim_edge( void* owner, void* child )
    {
        if ( owner == nullptr ) return;
        if ( child == nullptr )
        {
            clear_claim_edge( owner );
            return;
        }
        for ( auto& edge : synthetic_edges )
        {
            if ( edge.owner.load( std::memory_order_acquire ) == owner )
            {
                edge.child.store( child, std::memory_order_release );
                return;
            }
        }
        for ( auto& edge : synthetic_edges )
        {
            if ( edge.owner.load( std::memory_order_acquire ) != nullptr ) continue;
            edge.child.store( child, std::memory_order_relaxed );
            edge.owner.store( owner, std::memory_order_release );
            return;
        }
    }

    // 070 - despeja o estado da cadeia inteira no log
    void dump_chain( const char* tag )
    {
        if ( !settings::g_settings.debug_logs ) return;
        CCore::g_instance->info( "=== {} ===", tag );
        for ( uint32_t i = 0; i < max_trailers; ++i )
        {
            void* t = canon[ i ];
            if ( t == nullptr ) continue;
            const bool live_obj = readable_ptr( t );
            char* const tl = live_obj ? static_cast< char* >( t ) : nullptr;
            const uint8_t e8d = tl != nullptr ? *reinterpret_cast< uint8_t* >( tl + 0xE8D ) : uint8_t( 0xFF );
            const uint32_t ent = tl != nullptr
                ? static_cast< uint32_t >( *reinterpret_cast< uint64_t* >( tl + 0xEA0 ) ) : 0;
            CCore::g_instance->info(
                "  c{} {:p} joint={:p} parent={:p} slave={:p} rec={:p} "
                "det={} pdet={} parked={} e8d={} ent={} {}",
                i, t,
                live_obj ? joint( t ) : nullptr,
                live_obj ? prism::offsets::field< void* >( t, prism::offsets::trailer_parent ) : nullptr,
                live_obj ? prism::offsets::field< void* >( t, prism::offsets::trailer_slave ) : nullptr,
                live_obj ? prism::offsets::field< void* >( t, prism::offsets::trailer_parked_state ) : nullptr,
                is_detached( t ), is_plugin_detached( t ),
                live_obj ? static_cast< int >( prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked ) ) : -1,
                e8d, ent,
                live_obj ? "" : "DEAD" );
        }
        dump_native_registry();
    }

    // 071 - detecta reload do mundo (carreta deixou de ser objeto-carreta)
    bool canon_dead()
    {
        for ( auto* c : canon )
        {

            if ( c != nullptr && !is_trailer_obj( c ) ) return true;
        }
        return false;
    }

    // 072 - confere se toda peça do canon ainda é carreta válida
    bool canon_all_trailer_objs()
    {
        for ( auto* c : canon )
            if ( c != nullptr && !is_trailer_obj( c ) ) return false;
        return true;
    }

    // 073 - corta todos os links slave escritos pelo plugin
    void sever_all_claims()
    {
        __try
        {
            for ( auto& edge : synthetic_edges )
            {
                void* owner = edge.owner.load( std::memory_order_acquire );
                void* child = edge.child.load( std::memory_order_acquire );
                if ( is_trailer_obj( owner ) )
                {
                    void** sl = &prism::offsets::field< void* >( owner, prism::offsets::trailer_slave );
                    if ( *sl == child ) *sl = nullptr;
                }
                edge.owner.store( nullptr, std::memory_order_release );
                edge.child.store( nullptr, std::memory_order_release );
            }
        }
        __except ( EXCEPTION_EXECUTE_HANDLER ) {}
    }

    // 074 - zera todo o rastreio após reload ou falha
    void reset_trailer_state()
    {

        sever_all_claims();
        const bool carry = carry_arm_tick != 0 && GetTickCount64() - carry_arm_tick < 15000;
        bool carry_keep[ max_trailers ] = {};
        bool was_head[ max_trailers ] = {};
        hook_xform_t carry_xf[ max_trailers ] = {};
        if ( carry )
        {
            for ( uint32_t i = 0; i < max_trailers; ++i )
            {
                was_head[ i ] = detached_idx[ i ] && parked_xform_valid[ i ];
                carry_keep[ i ] = parked_xform_valid[ i ] &&
                    ( detached_idx[ i ] || ( i > 0 && carry_keep[ i - 1 ] ) );
                carry_xf[ i ] = parked_xform[ i ];
            }
        }
        carry_arm_tick = 0;
        for ( auto& c : canon ) c = nullptr;
        for ( auto& j : canon_was_jointed ) j = false;
        for ( auto& j : canon_jointless_frames ) j = 0;
        for ( auto& j : lines_torn ) j = false;
        for ( auto& edge : synthetic_edges )
        {
            edge.owner.store( nullptr, std::memory_order_release );
            edge.child.store( nullptr, std::memory_order_release );
        }
        for ( auto& d : detached_trailers ) d = nullptr;
        for ( auto& d : plugin_detached ) d = nullptr;
        for ( auto& f : detached_idx ) f = false;
        for ( auto& v : parked_xform_valid ) v = false;
        for ( auto& v : anchor_valid ) v = false;
        for ( auto& v : anchor_settling ) v = false;
        for ( auto& v : record_tried ) v = false;
        for ( auto& v : record_retry ) v = 0;
        for ( auto& v : record_quiet ) v = 0;
        for ( auto& v : record_mgr_logged ) v = false;
        anchor_delay[ 0 ] = 0;
        truck_towed = nullptr;
        attach_target = nullptr;
        trailer_count = 0;
        pending = {};
        canon_age = 0;
        for ( auto& v : lw_ctx_logged ) v = false;
        carry_frames = 0;
        carry_brake_frames = 0;
        for ( uint32_t i = 0; i < max_trailers; ++i )
        {
            carry_hold[ i ] = carry_keep[ i ];
            if ( !carry_keep[ i ] ) { own_head[ i ] = false; continue; }
            carry_brake_frames = 900;

            detached_idx[ i ] = was_head[ i ];
            parked_xform[ i ] = carry_xf[ i ];
            parked_xform_valid[ i ] = true;
            CCore::g_instance->info( "carrying detached slot {} across the respawn", i );
        }
        CCore::g_instance->info( "trailer state reset" );
    }

    // 075 - solta as marcas de desengate pra reengatar pela tecla T
    void release_detach_marks( void* t )
    {
        unmark_detached( t );
        unmark_plugin_detached( t );
        const int ci = canon_index_of( t );
        if ( ci >= 0 ) detached_idx[ ci ] = false;
    }

    // 076 - antecessor canon acoplado da carreta
    void* canon_coupled_predecessor( void* t )
    {
        const int ci = canon_index_of( t );
        if ( ci <= 0 ) return nullptr;
        void* p = canon[ ci - 1 ];
        return p != nullptr && readable_ptr( p ) && joint( p ) != nullptr ? p : nullptr;
    }

    // 077 - carreta alcançável pelo cavalo pela cadeia física
    bool trailer_is_live( const void* t )
    {
        if ( t == nullptr || !readable_ptr( t ) ||
             joint( const_cast< void* >( t ) ) == nullptr ) return false;
        auto* game_actor = CCore::g_instance->get_game_actor();
        void* truck = game_actor != nullptr
            ? prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck ) : nullptr;
        if ( truck == nullptr || !readable_ptr( truck ) ) return false;
        uint32_t depth = 0;
        for ( const void* v = t; v != nullptr && readable_ptr( v ) && depth <= max_trailers; ++depth )
        {
            void* parent = prism::offsets::field< void* >(
                const_cast< void* >( v ), prism::offsets::trailer_parent );
            if ( parent == truck ) return true;
            if ( parent == nullptr || parent == v || !is_trailer_obj( parent ) ||
                 joint( parent ) == nullptr ) return false;
            v = parent;
        }
        return false;
    }

    // 078 - peça estacionada conhecida do plugin
    bool trailer_is_known_parked( const void* t )
    {
        for ( uint32_t depth = 0; t != nullptr && depth <= max_trailers; ++depth )
        {
            if ( !is_trailer_obj( t ) ) return false;
            if ( is_detached( t ) || is_plugin_detached( t ) ) return true;
            void* parent = prism::offsets::field< void* >(
                const_cast< void* >( t ), prism::offsets::trailer_parent );
            if ( parent == nullptr || parent == t ) return false;
            auto* game_actor = CCore::g_instance->get_game_actor();
            void* truck = game_actor != nullptr
                ? prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck ) : nullptr;
            if ( parent == truck ) return false;
            t = parent;
        }
        return false;
    }

    // 079 - portão: mundo montado e estável
    bool trailer_maintenance_ready()
    {
        if ( canon_age < 90 ||
             ( traffic_game_state != nullptr && *traffic_game_state != 2 ) )
            return false;
        auto* game_actor = CCore::g_instance->get_game_actor();
        void* truck = game_actor != nullptr
            ? prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck ) : nullptr;
        if ( truck == nullptr || !readable_ptr( truck ) ) return false;

        bool any = false;
        for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
        {
            void* t = canon[ i ];
            if ( !is_trailer_obj( t ) ) return false;
            any = true;

            if ( !trailer_is_live( t ) && !trailer_is_known_parked( t ) &&
                 !is_detached( t ) && !is_plugin_detached( t ) )
                return false;
        }
        return any;
    }

    // 080 - construtor
    CTrailerManipulation::CTrailerManipulation() = default;

    // 081 - destrutor: solta hooks e corta claims
    CTrailerManipulation::~CTrailerManipulation()
    {

        sever_all_claims();
        steering_reset();
        set_individual_steering_hook.reset();
        release_all_axles();
        crashes_when_disconnected_hook.reset();
        connect_slave_hook.reset();
        pre_detach_hook.reset();
        detach_dispatch_hook.reset();
        attach_dispatch_hook.reset();
        trailer_state_apply_hook.reset();
        trailer_teleport_hook.reset();
        actor_teleport_hook.reset();
        lane_obstacle_insert_hook.reset();
        combo_length_hook.reset();
        cam_set_target_hook.reset();
        cam_update_hook.reset();
        lamp_mask_hook.reset();
        lamp_propagate_hook.reset();
        trailer_dtor_hook.reset();
        apply_cables_hook.reset();
        set_lines_hook.reset();
        rebuild_lines_hook.reset();
        joint_walk_hook.reset();
        phys_link_hook.reset();
        purecall_hook.reset();
        trailer_disconnect_hook.reset();
        trailer_connect_hook.reset();
        trailer_connect_hook2.reset();
        svc_brake_chain_hook.reset();
        svc_brake_hook.reset();
        brake_chain_setter_hook.reset();
        brake_setter_hook.reset();
        forced_chain_setter_hook.reset();
        forced_setter_hook.reset();
        wheel_brake_hook.reset();
        chase_camera = nullptr;
    }

    // 082 - instala hooks e registra canais
    bool CTrailerManipulation::init()
    {
        static const bool fn_idx_members = []()
        {
            fn_index_add( fn_addr_of( &CTrailerManipulation::init ), 82, "init" );
            fn_index_add( fn_addr_of( &CTrailerManipulation::update ), 84, "update" );
            fn_index_add( fn_addr_of( &CTrailerManipulation::update_impl ), 85, "update_impl" );
            fn_index_add( fn_addr_of( &CTrailerManipulation::disconnect_trailer ), 86, "disconnect_trailer" );
            fn_index_add( fn_addr_of( &CTrailerManipulation::connect_trailer ), 87, "connect_trailer" );
            fn_index_add( fn_addr_of( &CTrailerManipulation::connect_trailer_to_truck ), 88, "connect_trailer_to_truck" );
            fn_index_add( fn_addr_of( &CTrailerManipulation::connect_to_truck ), 89, "connect_to_truck" );
            fn_index_add( fn_addr_of( &CTrailerManipulation::toggle_attach ), 90, "toggle_attach" );
            fn_index_add( fn_addr_of( &CTrailerManipulation::render_keybind_row ), 91, "render_keybind_row" );
            fn_index_add( fn_addr_of( &CTrailerManipulation::render_keybinds ), 92, "render_keybinds" );
            fn_index_add( fn_addr_of( &CTrailerManipulation::render_trailer_connect ), 93, "render_trailer_connect" );
            fn_index_add( fn_addr_of( &CTrailerManipulation::render ), 94, "render" );
            fn_index_add( fn_addr_of( &CTrailerManipulation::render_keys_tab ), 95, "render_keys_tab" );
            fn_index_add( fn_addr_of( &CTrailerManipulation::render_options_tab ), 96, "render_options_tab" );
            fn_index_add( fn_addr_of( &CTrailerManipulation::render_oversize_tab ), 97, "render_oversize_tab" );
            fn_index_add( fn_addr_of( &CTrailerManipulation::render_trailers_tab ), 98, "render_trailers_tab" );
            fn_index_add( fn_addr_of( &CTrailerManipulation::render_license_tab ), 99, "render_license_tab" );
            fn_index_add( fn_addr_of( &CTrailerManipulation::render_logs_tab ), 100, "render_logs_tab" );
            fn_index_add( fn_addr_of( &CTrailerManipulation::refresh_trailer_chain ), 102, "refresh_trailer_chain" );
            fn_index_add( fn_addr_of( &CTrailerManipulation::update_convoy_state ), 103, "update_convoy_state" );
            fn_index_add( fn_addr_of( &CTrailerManipulation::gc_detached_marks ), 104, "gc_detached_marks" );
            fn_index_add( fn_addr_of( &CTrailerManipulation::update_hotkeys ), 107, "update_hotkeys" );
            fn_index_add( fn_addr_of( &CTrailerManipulation::update_steering ), 110, "update_steering" );
            fn_index_add( fn_addr_of( &CTrailerManipulation::render_trailer_steering ), 111, "render_trailer_steering" );
            fn_index_add( fn_addr_of( &CTrailerManipulation::render_oversize ), 112, "render_oversize" );
            fn_index_add( fn_addr_of( &CTrailerManipulation::render_trailer_lift ), 220, "render_trailer_lift" );
            fn_index_add( fn_addr_of( &CTrailerManipulation::render_truck_lift ), 221, "render_truck_lift" );
            return true;
        }();


        this->connect_valid_ = coupling_install();

        steering_install();

        lift_register_channels();

        air_register_channels();

        coupling_register_channels();

        return true;
    }

    // 083 - filtro do protetor de falhas do update
    static int update_fault_filter( EXCEPTION_POINTERS* ep )
    {
        static DWORD last_log = 0;
        const DWORD now = GetTickCount();
        if ( now - last_log >= 5000 )
        {
            last_log = now;
            const auto* rec = ep != nullptr ? ep->ExceptionRecord : nullptr;
            const auto rip = rec != nullptr
                ? reinterpret_cast< uintptr_t >( rec->ExceptionAddress ) : 0;
            const auto base = reinterpret_cast< uintptr_t >( GetModuleHandle( nullptr ) );
            const std::string where = rip >= base && rip < base + 0x2000000
                ? fmt::format( "exe+0x{:X}", rip - base )
                : fmt::format( "{:p}", reinterpret_cast< void* >( rip ) );
            CCore::g_instance->warning(
                "update faulted (função 085, 0x{:X} at {}) - trailer state reset",
                rec != nullptr ? rec->ExceptionCode : 0u, where );
        }
        return EXCEPTION_EXECUTE_HANDLER;
    }

    // 084 - update por frame protegido
    void CTrailerManipulation::update()
    {

        __try { update_impl(); }
        __except ( update_fault_filter( GetExceptionInformation() ) )
        {
            reset_trailer_state();
        }
    }

    // 085 - update por frame: assinatura da cadeia + sub-sistemas
    void CTrailerManipulation::update_impl()
    {

        settings::poll_joystick_capture();

        license::refresh();
        if ( !license::active() ) return;

        static uint64_t idle_sig = ~0ull;
        static uint32_t active = 0;
        static uint32_t pulse = 0;
        uint64_t sig = 0;
        void* cam_truck = nullptr;
        if ( auto* game_actor = CCore::g_instance->get_game_actor() )
        {
            cam_truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
        }
        void* cam_towed = cam_truck != nullptr && readable_ptr( cam_truck )
            ? prism::offsets::field< void* >( cam_truck, 0x1580 ) : nullptr;
        sig = sig * 1315423911u ^ reinterpret_cast< uintptr_t >( cam_towed );
        if ( cam_towed != nullptr && readable_ptr( cam_towed ) && joint( cam_towed ) != nullptr )
        {
            sig ^= 1ull;
        }
        for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
        {
            void* t = canon[ i ];
            sig = sig * 1315423911u ^ reinterpret_cast< uintptr_t >( t );
            if ( !readable_ptr( t ) ) continue;
            if ( joint( t ) != nullptr ) sig ^= 1ull << i;
            void* s = readable_ptr( static_cast< uint8_t* >( t ) + prism::offsets::trailer_slave )
                ? prism::offsets::field< void* >( t, prism::offsets::trailer_slave ) : nullptr;
            sig = sig * 1315423911u ^ reinterpret_cast< uintptr_t >( s );
            if ( s != nullptr && readable_ptr( s ) &&
                 readable_ptr( static_cast< uint8_t* >( s ) + prism::offsets::trailer_physics_joint ) &&
                 joint( s ) != nullptr )
            {
                sig ^= 1ull << ( i + 8 );
            }
        }
        if ( sig != idle_sig ) { idle_sig = sig; active = 300; }
        else if ( active > 0 ) { --active; }

        const bool wake = active > 0 || ++pulse >= 30;
        if ( wake ) pulse = 0;

        load_game_update();
        const bool coupling_on = settings::g_settings.cargo_restore;

        static bool coupling_was_on = true;
        if ( coupling_was_on && !coupling_on )
        {
            sever_parked_claims();
            pending = {};
        }
        coupling_was_on = coupling_on;

        static DWORD convoy_tick = 0;
        const DWORD now_ms = GetTickCount();

        const bool convoy_due = convoy_refresh_requested.exchange( false, std::memory_order_acq_rel ) ||
            now_ms - convoy_tick >= 100;
        if ( convoy_due ) convoy_tick = now_ms;
        uint64_t pt[ 10 ] = {};
        pt[ 0 ] = __rdtsc();
        if ( wake && convoy_due )
        {
            if ( coupling_on && this->connect_valid_ )
            {
                refresh_trailer_chain();
            }
            else
            {

                trailer_count = 0;
                const auto native_walk = []( void* head )
                {
                    for ( void* t = head; t != nullptr && is_trailer_obj( t ) &&
                         trailer_count < max_trailers; )
                    {
                        bool dup = false;
                        for ( uint32_t i = 0; i < trailer_count; ++i ) dup |= trailers[ i ] == t;
                        if ( dup ) return;
                        trailers[ trailer_count++ ] = t;

                        track_canon( t );
                        void* next = readable_ptr( static_cast< uint8_t* >( t ) + prism::offsets::trailer_slave )
                            ? prism::offsets::field< void* >( t, prism::offsets::trailer_slave ) : nullptr;
                        t = ( next != t ) ? next : nullptr;
                    }
                };
                void* nw_truck = cam_truck;
                native_walk( nw_truck != nullptr && readable_ptr( nw_truck )
                    ? prism::offsets::field< void* >( nw_truck, 0x1580 ) : nullptr );
                if ( auto* game_actor = CCore::g_instance->get_game_actor() )
                {
                    native_walk( prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_trailer ) );
                }
            }
        }
        pt[ 1 ] = __rdtsc();
        if ( coupling_on && this->connect_valid_ && convoy_due ) update_convoy_state();
        pt[ 2 ] = __rdtsc();
        if ( coupling_on && this->connect_valid_ && convoy_due ) light_watch_tick();
        pt[ 3 ] = __rdtsc();
        if ( wake ) gc_detached_marks();
        watch_sim_heartbeat();
        pt[ 4 ] = __rdtsc();

        auto* game_actor = CCore::g_instance->get_game_actor();

        lift_axles_update( game_actor );
        pt[ 5 ] = __rdtsc();

        if ( wake && coupling_on && this->connect_valid_ ) coupling_parked_update();
        pt[ 6 ] = __rdtsc();
        update_hotkeys();
        update_steering( ImGui::GetIO().DeltaTime );
        pt[ 7 ] = __rdtsc();
        update_air( ImGui::GetIO().DeltaTime );
        pt[ 8 ] = __rdtsc();
        brake_boost_update();
        pt[ 9 ] = __rdtsc();

        static uint64_t acc[ 9 ] = {};
        static uint64_t tsc0 = 0;
        static LARGE_INTEGER qpc0{};
        static uint32_t win_frames = 0;

        if ( settings::g_settings.debug_logs )
        {
        if ( win_frames == 0 )
        {
            QueryPerformanceCounter( &qpc0 );
            tsc0 = __rdtsc();
        }
        for ( uint32_t s = 0; s < 9; ++s ) acc[ s ] += pt[ s + 1 ] - pt[ s ];
        if ( ++win_frames >= 300 )
        {
            LARGE_INTEGER qpc1, freq;
            QueryPerformanceCounter( &qpc1 );
            QueryPerformanceFrequency( &freq );
            const uint64_t tsc1 = __rdtsc();
            const double window_s =
                double( qpc1.QuadPart - qpc0.QuadPart ) / double( freq.QuadPart );
            uint64_t total = 0;
            for ( const uint64_t a : acc ) total += a;

            if ( window_s > 0.0 && tsc1 > tsc0 && total != 0 )
            {
                const double tsc_hz = double( tsc1 - tsc0 ) / window_s;
                const double plugin_ms = double( total ) / tsc_hz * 1000.0 / win_frames;
                static const char* names[ 9 ] =
                    { "chain", "convoy", "lights", "marks", "axles", "loadstate", "steer", "air", "brake" };
                uint32_t top = 0;
                for ( uint32_t s = 1; s < 9; ++s ) if ( acc[ s ] > acc[ top ] ) top = s;
                const double top_ms = double( acc[ top ] ) / tsc_hz * 1000.0 / win_frames;
                if ( plugin_ms > 0.5 )
                {
                    CCore::g_instance->warning(
                        "perf: plugin update {:.2f} ms/frame - top {} {:.2f} ms",
                        plugin_ms, names[ top ], top_ms );
                }
            }
            for ( auto& a : acc ) a = 0;
            win_frames = 0;
        }
        }
    }

    // 086 - DESATRELAMENTO: T(i) solta do pai físico - T0 do cavalo, T1 de T0, T2 de T1, T1/T2 diretos do cavalo
    void CTrailerManipulation::disconnect_trailer( const uint32_t i ) const
    {

        void* trailer = i < max_trailers ? canon[ i ] : nullptr;
        if ( trailer == nullptr || !readable_ptr( trailer ) ) return;
        if ( !handbrake_ok() )
        {
            CCore::g_instance->warning( "Puxe o freio de mão para desengatar a carreta {}", i + 1 );
            return;
        }
        void* truck = nullptr;
        if ( auto* game_actor = CCore::g_instance->get_game_actor() )
        {
            truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
        }
        const bool on_truck = trailer == truck_towed;

        void* parent = i == 0 || on_truck
            ? truck
            : ( readable_ptr( reinterpret_cast< uint8_t* >( trailer ) + prism::offsets::trailer_parent )
                ? prism::offsets::field< void* >( trailer, prism::offsets::trailer_parent ) : nullptr );
        if ( parent == nullptr && i > 0 ) parent = canon[ i - 1 ];

        const bool parent_is_truck = i == 0 || on_truck ||
            ( truck != nullptr && parent == truck );

        CCore::g_instance->info( "Disconnecting trailer {}", i );

        if ( queue_op( pending_kind::disconnect, trailer, parent, parent_is_truck, 0 ) )
        {
            traffic_detach_done.store( false, std::memory_order_release );
        }
    }

    // 087 - ATRELAMENTO em cadeia: T(i) no antecessor - T1 em T0, T2 em T1
    void CTrailerManipulation::connect_trailer( const uint32_t i ) const
    {

        void* trailer = i < max_trailers ? canon[ i ] : nullptr;
        if ( trailer == nullptr || !readable_ptr( trailer ) || joint( trailer ) != nullptr ) return;
        auto* game_actor = CCore::g_instance->get_game_actor();
        void* parent = i == 0
            ? ( game_actor != nullptr ? prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck ) : nullptr )
            : canon_coupled_predecessor( trailer );
        if ( parent == nullptr && i > 0 )
        {
            parent = canon[ i - 1 ];
        }
        if ( parent == nullptr )
        {
            CCore::g_instance->warning( "Não foi possível engatar a carreta {}: veículo anterior não encontrado", i );
            return;
        }

        if ( i > 0 && joint( parent ) == nullptr )
        {
            CCore::g_instance->warning( "Engate a carreta {} primeiro", i );
            return;
        }

        if ( hook_far( trailer, parent, i == 0 ) )
        {
            CCore::g_instance->warning( "A carreta {} está muito longe - aproxime dando ré para engatar", i + 1 );
            return;
        }
        if ( !handbrake_ok() )
        {
            CCore::g_instance->warning( "Puxe o freio de mão para engatar a carreta {}", i + 1 );
            return;
        }

        unmark_detached( trailer );
        queue_op( pending_kind::connect, trailer, parent, i == 0 );
    }

    // 088 - ATRELAMENTO direto: T(i) na quinta roda do cavalo
    void CTrailerManipulation::connect_trailer_to_truck( const uint32_t i ) const
    {
        if ( !this->connect_valid_ || i >= max_trailers ) return;
        void* trailer = canon[ i ];
        if ( trailer == nullptr || !readable_ptr( trailer ) || joint( trailer ) != nullptr ) return;
        auto* game_actor = CCore::g_instance->get_game_actor();
        void* truck = game_actor != nullptr && readable_ptr( game_actor )
            ? prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck )
            : nullptr;
        if ( truck == nullptr || !readable_ptr( truck ) ) return;

        void* towed = prism::offsets::field< void* >( truck, 0x1580 );
        bool wheel_free = towed == nullptr || !readable_ptr( towed ) || joint( towed ) == nullptr;
        for ( uint32_t n = 0; wheel_free && n < max_trailers; ++n )
        {
            void* other = canon[ n ];
            if ( other != nullptr && other != trailer && readable_ptr( other ) &&
                 joint( other ) != nullptr &&
                 prism::offsets::field< void* >( other, prism::offsets::trailer_parent ) == truck )
                wheel_free = false;
        }
        if ( !wheel_free )
        {
            CCore::g_instance->warning( "A quinta roda do cavalo está ocupada" );
            return;
        }
        if ( hook_far( trailer, truck, true ) )
        {
            CCore::g_instance->warning(
                "A carreta {} está longe do cavalo; aproxime até a quinta roda",
                i + 1 );
            return;
        }
        if ( !handbrake_ok() )
        {
            CCore::g_instance->warning( "Puxe o freio de mão para engatar a carreta {}", i + 1 );
            return;
        }
        if ( queue_op( pending_kind::connect, trailer, truck, true, 0 ) )
        {
            unmark_detached( trailer );
        }
    }

    // 089 - seleciona a carreta-alvo do engate nativo (tecla T)
    void CTrailerManipulation::connect_to_truck( void* trailer ) const
    {
        if ( trailer == nullptr || !readable_ptr( trailer ) ) return;
        const int ci = canon_index_of( trailer );
        if ( joint( trailer ) != nullptr )
        {
            CCore::g_instance->warning( "A carreta {} já está engatada", ci >= 0 ? ci + 1 : 0 );
            return;
        }

        if ( attach_target == trailer )
        {
            attach_target = nullptr;
            CCore::g_instance->info( "Alvo de engate: carreta original (1) - aperte T" );
            return;
        }

        attach_target = trailer;
        CCore::g_instance->info( "Alvo de engate: carreta {} - recue até ela e aperte T",
                                 ci >= 0 ? ci + 1 : 0 );
    }

    // 090 - alterna atrelar/desatrelar de T(i)
    void CTrailerManipulation::toggle_attach( const uint32_t i ) const
    {

        if ( i == 0 || i >= max_trailers ) return;
        void* trailer = canon[ i ];
        if ( trailer == nullptr || !readable_ptr( trailer ) ) return;
        if ( joint( trailer ) != nullptr )
        {
            this->disconnect_trailer( i );
        }
        else
        {
            this->connect_trailer( i );
        }
    }

    // 091 - linha de tecla na aba Teclas
    void CTrailerManipulation::render_keybind_row( const int i ) const
    {
        const auto action = static_cast< settings::Action >( i );
        const bool fixed = action == settings::Action::TOGGLE_UI
            || action == settings::Action::TOGGLE_CURSOR;
        ImGui::PushID( i );
        ImGui::TextWrapped( "%s", settings::action_name( action ) );
        if ( fixed )
        {

            ImGui::TextDisabled( "%s (settings.ini)", settings::key_name( settings::key( action ) ).c_str() );
        }
        else
        {
            const auto label = settings::is_capturing( action )
                                   ? std::string( "Pressione uma tecla... (Esc cancela)" )
                                   : settings::key_name( settings::key( action ) );
            if ( ImGui::Button( fmt::format( "{}##bind", label ).c_str(), ImVec2( -ImGui::GetFrameHeight() * 1.8f, 0 ) ) )
            {
                settings::begin_capture( action );
            }
            ImGui::SameLine();
            ImGui::BeginDisabled( settings::key( action ) == 0 );
            if ( ImGui::SmallButton( "X##clear" ) )
            {
                settings::g_settings.keys[ i ] = 0;
                settings::save();
            }
            ImGui::EndDisabled();
        }
        ImGui::Spacing();
        ImGui::PopID();
    }

    // 092 - lista de teclas configuráveis
    void CTrailerManipulation::render_keybinds() const
    {
        for ( int i = 0; i < settings::action_count; ++i )
        {
            const auto action = static_cast< settings::Action >( i );

            if ( action == settings::Action::STEER_LEFT ||
                 action == settings::Action::STEER_RIGHT ||
                 action == settings::Action::STEER_CENTER ||
                 action == settings::Action::TOGGLE_STEERING_LOCK ||
                 action == settings::Action::TOGGLE_UI ||
                 action == settings::Action::TOGGLE_CURSOR ||
                 action == settings::Action::SLEEP_PARK ) continue;
            this->render_keybind_row( i );
        }
    }

    // 093 - botões de atrelar/desatrelar de T(i) na tela
    void CTrailerManipulation::render_trailer_connect( const uint32_t i ) const
    {

        void* trailer = i < max_trailers ? canon[ i ] : nullptr;
        if ( trailer == nullptr || !readable_ptr( trailer ) ) return;

        void* truck = nullptr;
        if ( auto* ga = CCore::g_instance->get_game_actor(); ga != nullptr && readable_ptr( ga ) )
        {
            truck = prism::offsets::field< void* >( ga, prism::offsets::game_actor_truck );
        }
        const bool coupling_enabled = settings::g_settings.cargo_restore;
        const bool attached = joint( trailer ) != nullptr;
        void* physical_parent = nullptr;
        if ( attached )
        {
            physical_parent = prism::offsets::field< void* >( trailer, prism::offsets::trailer_parent );
            if ( physical_parent == nullptr && trailer == truck_towed ) physical_parent = truck;
        }
        const bool direct_to_truck = attached && truck != nullptr &&
            ( physical_parent == truck || trailer == truck_towed );

        const auto paint = []( const bool on, const bool avail )
        {
            const ImVec4 bg = !avail ? ImVec4( 0.18f, 0.19f, 0.21f, 1.f )
                : on ? ImVec4( 0.13f, 0.55f, 0.22f, 1.f )
                : ImVec4( 0.15f, 0.40f, 0.80f, 1.f );
            ImGui::PushStyleColor( ImGuiCol_Button, bg );
            ImGui::PushStyleColor( ImGuiCol_ButtonHovered, bg );
            ImGui::PushStyleColor( ImGuiCol_ButtonActive, bg );
            ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 1.f, 1.f, 1.f, 1.f ) );
        };

        ImGui::SeparatorText( "Conexão entre carretas" );
        if ( i == 0 )
        {
            ImGui::TextDisabled( "T0: conexão direta com o cavalo (abaixo)" );
        }
        else
        {
            void* previous = canon[ i - 1 ];
            const bool chain_attached = attached && previous != nullptr &&
                physical_parent == previous;
            const bool previous_ready = previous != nullptr && readable_ptr( previous ) &&
                joint( previous ) != nullptr;
            const float chain_gap = !attached && previous_ready
                ? hook_gap( trailer, previous, false ) : -1.f;
            const bool chain_candidate = !attached && previous_ready &&
                ( chain_gap < 0.f || chain_gap <= 8.f ) && handbrake_ok();
            const bool chain_available = coupling_enabled && ( chain_attached ||
                ( this->connect_valid_ && chain_candidate ) );
            paint( coupling_enabled && chain_attached, chain_available );
            ImGui::BeginDisabled( !chain_available );
            const char* label = chain_attached ? "Desatrelar##chain" : "Atrelar##chain";
            if ( ImGui::Button( label ) )
            {
                if ( chain_attached ) this->disconnect_trailer( i );
                else this->connect_trailer( i );
            }
            ImGui::EndDisabled();
            ImGui::PopStyleColor( 4 );
        }

        ImGui::SeparatorText( "Engate direto no cavalo (T)" );
        bool wheel_free = truck != nullptr && readable_ptr( truck );
        if ( wheel_free )
        {
            void* towed = prism::offsets::field< void* >( truck, 0x1580 );
            wheel_free = towed == nullptr || !readable_ptr( towed ) || joint( towed ) == nullptr;
            for ( uint32_t n = 0; wheel_free && n < max_trailers; ++n )
            {
                void* other = canon[ n ];
                if ( other != nullptr && other != trailer && readable_ptr( other ) &&
                     joint( other ) != nullptr &&
                     prism::offsets::field< void* >( other, prism::offsets::trailer_parent ) == truck )
                    wheel_free = false;
            }
        }

        const float truck_gap = !attached && wheel_free && truck != nullptr
            ? hook_gap( trailer, truck, true ) : -1.f;
        const bool truck_candidate = !attached && wheel_free &&
            ( truck_gap < 0.f || truck_gap <= 8.f ) && handbrake_ok();
        const bool truck_available = coupling_enabled && ( direct_to_truck ||
            ( this->connect_valid_ && truck_candidate ) );
        paint( coupling_enabled && direct_to_truck, truck_available );
        ImGui::BeginDisabled( !truck_available );
        if ( ImGui::Button( direct_to_truck ? "Desatrelar##truck" : "Atrelar##truck" ) )
        {
            if ( direct_to_truck ) this->disconnect_trailer( i );
            else this->connect_trailer_to_truck( i );
        }
        ImGui::EndDisabled();
        ImGui::PopStyleColor( 4 );
    }

    // 094 - janela principal do plugin
    void CTrailerManipulation::render()
    {

        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 10.f, 10.f ) );
        ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 10.f, 8.f ) );
        ImGui::PushStyleVar( ImGuiStyleVar_FrameRounding, 3.f );
        ImGui::PushStyleVar( ImGuiStyleVar_ChildRounding, 3.f );
        ImGui::PushStyleVar( ImGuiStyleVar_FrameBorderSize, 1.f );
        ImGui::PushStyleColor( ImGuiCol_Text, ui::pdv( 0xFFFFFF ) );
        ImGui::PushStyleColor( ImGuiCol_TextDisabled, ui::pdv( 0x656565 ) );
        ImGui::PushStyleColor( ImGuiCol_WindowBg, ui::pdv( 0x303030 ) );
        ImGui::PushStyleColor( ImGuiCol_ChildBg, ui::pdv( 0x282828 ) );
        ImGui::PushStyleColor( ImGuiCol_Border, ui::pdv( 0x444444 ) );
        ImGui::PushStyleColor( ImGuiCol_FrameBg, ui::pdv( 0x020202 ) );
        ImGui::PushStyleColor( ImGuiCol_TitleBg, ui::pdv( 0x232359 ) );
        ImGui::PushStyleColor( ImGuiCol_TitleBgActive, ui::pdv( 0x232359 ) );
        ImGui::PushStyleColor( ImGuiCol_Button, ui::pdv( 0x4A6FA5 ) );
        ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ui::pdv( 0x222888 ) );
        ImGui::PushStyleColor( ImGuiCol_ButtonActive, ui::pdv( 0x232359 ) );
        ImGui::PushStyleColor( ImGuiCol_Header, ui::pdv( 0x222888 ) );
        ImGui::PushStyleColor( ImGuiCol_HeaderHovered, ui::pdv( 0x4A6FA5 ) );
        ImGui::PushStyleColor( ImGuiCol_HeaderActive, ui::pdv( 0x27AE60 ) );
        ImGui::PushStyleColor( ImGuiCol_CheckMark, ui::pdv( 0x27AE60 ) );
        ImGui::PushStyleColor( ImGuiCol_Separator, ui::pdv( 0x444444 ) );

        ImGui::SetNextWindowSize( ImVec2( 560.f, 540.f ), ImGuiCond_FirstUseEver );
        ImGui::Begin( "Controle de Carretas" );

        if ( !license::active() )
        {

            ImGui::PushStyleColor( ImGuiCol_Text, ui::pdv( 0xE74C3C ) );
            ImGui::TextWrapped( "LICENÇA EXPIRADA OU AUSENTE" );
            ImGui::PopStyleColor();
            ui::text_disabled_wrapped(
                "o plugin está bloqueado - informe uma chave válida abaixo ou "
                "renove a licença para reativar todos os recursos" );
            ImGui::Separator();
            this->render_license_tab();
        }
        else
        {

        static const char* tab_names[] = { "Teclas", "Direcionais", "Carretas", "Opções", "Licença", "Logs" };
        const float tab_width = ( ImGui::GetContentRegionAvail().x
            - ImGui::GetStyle().ItemSpacing.x * 5.f ) / 6.f;
        for ( int t = 0; t < 6; ++t )
        {
            if ( t > 0 ) ImGui::SameLine();
            const bool open = settings::g_settings.ui_tab == t;
            if ( ImGui::Selectable( tab_names[ t ], open, 0, ImVec2( tab_width, 0.f ) ) )
            {
                settings::g_settings.ui_tab = open ? -1 : t;
                if ( !open ) settings::save();
            }
        }
        ImGui::Separator();

        switch ( settings::g_settings.ui_tab )
        {
        case 0: this->render_keys_tab(); break;
        case 1: this->render_oversize_tab(); break;
        case 2: this->render_trailers_tab(); break;
        case 3: this->render_options_tab(); break;
        case 4: this->render_license_tab(); break;
        case 5: this->render_logs_tab(); break;
        default: break;
        }
        }

        ImGui::End();
        ImGui::PopStyleColor( 16 );
        ImGui::PopStyleVar( 5 );
    }

    // 095 - aba Teclas
    void CTrailerManipulation::render_keys_tab() const
    {
        ImGui::BeginChild( "##keys", ImVec2( 0.f, 0.f ) );
        ImGui::SeparatorText( "Teclas" );
        this->render_keybinds();
        ImGui::EndChild();
    }

    // 096 - aba Opções
    void CTrailerManipulation::render_options_tab() const
    {
        ImGui::BeginChild( "##options", ImVec2( 0.f, 0.f ) );

        ImGui::SeparatorText( "Cavalo" );
        this->render_truck_lift();

        ImGui::SeparatorText( "Assistente" );
        bool reverse_lift = settings::g_settings.reverse_lift;
        if ( ui::toggle_switch( "##reverse_lift", &reverse_lift ) )
        {
            settings::g_settings.reverse_lift = reverse_lift;
            settings::save();
        }
        ImGui::SameLine();
        ImGui::TextWrapped( "Eixo na ré (vanderleia/4º eixo)" );
        ImGui::TextDisabled( "ativado: engatou a ré - ergue os eixos eleváveis da carreta, se existirem" );

        ImGui::SeparatorText( "Freio" );
        float brake_boost = settings::g_settings.trailer_brake_boost;
        ImGui::SetNextItemWidth( -1.f );
        if ( ImGui::SliderFloat( "##brake_boost", &brake_boost, 0.f, 200.f, "+%.0f%%", ImGuiSliderFlags_AlwaysClamp ) )
        {
            settings::g_settings.trailer_brake_boost = brake_boost;
            settings::save();
        }
        ImGui::TextWrapped( "Carreta freia mais" );
        ImGui::TextDisabled( "regule o quanto de freio você quer aplicar a mais na carreta" );

        ImGui::SeparatorText( "Ar" );
        int air_boost = settings::g_settings.air_boost;
        ImGui::SetNextItemWidth( -1.f );
        if ( ImGui::SliderInt( "##air_boost", &air_boost, 0, 5,
             air_boost == 0 ? "original" : fmt::format( "+{}00%", air_boost ).c_str() ) )
        {
            settings::g_settings.air_boost = air_boost;
            settings::save();
        }
        ImGui::TextWrapped( "Compressor por rotação" );
        ImGui::TextDisabled( "multiplica a produção de ar (que já sobe com o giro) - sem mexer no consumo" );
        float air_leak = settings::g_settings.air_leak;
        ImGui::SetNextItemWidth( -1.f );
        if ( ImGui::SliderFloat( "##air_leak", &air_leak, 0.f, 2.f, "%.2f bar/h", ImGuiSliderFlags_AlwaysClamp ) )
        {
            settings::g_settings.air_leak = std::round( air_leak * 20.f ) / 20.f;
            settings::save();
        }
        ImGui::TextWrapped( "Vazamento de ar com o motor desligado" );
        ImGui::TextDisabled( "ao dormir ou descarregar com o motor off, o caminhão perde esta pressão por hora de jogo (0 = sem vazamento; ~1 bar/h esvazia o tanque em 10 h)" );
        ImGui::EndChild();
    }

    // 097 - aba Direcionais
    void CTrailerManipulation::render_oversize_tab() const
    {
        ImGui::BeginChild( "##oversize", ImVec2( 0.f, 0.f ) );
        ImGui::SeparatorText( "Direcionais" );
        this->render_oversize();
        ImGui::EndChild();
    }

    // 098 - aba Carretas
    void CTrailerManipulation::render_trailers_tab() const
    {
        render_cargo_restore_option();
        ImGui::BeginChild( "##trailers", ImVec2( 0.f, 0.f ) );
        uint32_t canon_count = 0;
        while ( canon_count < max_trailers && canon[ canon_count ] != nullptr ) ++canon_count;
        if ( canon_count == 0 )
        {
            ImGui::Text( "Nenhuma carreta encontrada" );
        }
        else
        {
            const float col_w = ( ImGui::GetContentRegionAvail().x
                - ImGui::GetStyle().ItemSpacing.x * ( canon_count - 1 ) ) / canon_count;
            for ( uint32_t i = 0; i < canon_count; ++i )
            {
                if ( i > 0 ) ImGui::SameLine();
                ImGui::PushID( static_cast< int >( i ) );
                ImGui::BeginChild( fmt::format( "##trailer{}", i ).c_str(), ImVec2( col_w, 0.f ), ImGuiChildFlags_Border );
                ImGui::SeparatorText( fmt::format( "Carreta {}", i + 1 ).c_str() );
                this->render_trailer_connect( i );
                ImGui::SeparatorText( "Eixos" );
                ui::text_disabled_wrapped(
                    "clique nas bolinhas: verde = eixo que vai levantar; "
                    "cinza claro = eixo que fica no chão" );

                int ti = -1;
                for ( uint32_t k = 0; k < trailer_count; ++k )
                {
                    if ( trailers[ k ] == canon[ i ] ) { ti = static_cast< int >( k ); break; }
                }
                if ( ti >= 0 ) this->render_trailer_lift( static_cast< uint32_t >( ti ) );
                ImGui::EndChild();
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }

    // 099 - aba Licença
    void CTrailerManipulation::render_license_tab() const
    {
        ImGui::BeginChild( "##license", ImVec2( 0.f, 0.f ) );

        ImGui::SeparatorText( "Sobre" );
        ImGui::TextWrapped( "BR Controle de Carretas" );
        ImGui::TextWrapped( "Copyright periard_br - todos os direitos reservados." );
        ImGui::TextWrapped( "Feito por periard_br. Contato com o desenvolvedor no Discord: periard_br" );
        ui::text_disabled_wrapped(
            "usa um mapa em que o plugin trava ou não funciona? chame no "
            "Discord e peça um bypass ou as correções necessárias para esse mapa" );

        ImGui::SeparatorText( "Máquina" );
        ImGui::TextWrapped( "ID do hardware" );
        const std::string hwid = license::hardware_id();

        const std::string tagged = hwid.empty() ? hwid : "ETS-" + hwid;

        ImGui::Selectable( tagged.empty() ? "(indisponível)" : tagged.c_str(),
            false, ImGuiSelectableFlags_Disabled, ImVec2( 200.f, 0.f ) );
        if ( !tagged.empty() )
        {
            ImGui::SameLine();
            if ( ImGui::SmallButton( "Copiar" ) )
            {

                if ( OpenClipboard( nullptr ) )
                {
                    EmptyClipboard();
                    const SIZE_T bytes = tagged.size() + 1;
                    if ( const HGLOBAL mem = GlobalAlloc( GMEM_MOVEABLE, bytes ) )
                    {
                        memcpy( GlobalLock( mem ), tagged.c_str(), bytes );
                        GlobalUnlock( mem );
                        SetClipboardData( CF_TEXT, mem );
                    }
                    CloseClipboard();
                }
            }
        }
        ui::text_disabled_wrapped(
            "envie este código na compra - a licença fica atrelada a este PC" );

        ImGui::SeparatorText( "Licença" );

        static char edit_key[ 256 ] = {};
        static bool edit_loaded = false;
        if ( !edit_loaded )
        {
            strncpy_s( edit_key, settings::g_settings.license_key, _TRUNCATE );
            edit_loaded = true;
        }
        ImGui::SetNextItemWidth( -1.f );
        ImGui::InputTextWithHint( "##license_key", "cole a chave emitida pelo site",
            edit_key, sizeof( edit_key ) );
        if ( ImGui::Button( "Ativar" ) )
        {
            strncpy_s( settings::g_settings.license_key, edit_key, _TRUNCATE );
            settings::save();
            license::refresh( true );
        }
        ImGui::SameLine();
        ImGui::BeginDisabled( settings::g_settings.license_key[ 0 ] == 0 );
        if ( ImGui::Button( "Limpar" ) )
        {
            edit_key[ 0 ] = 0;
            settings::g_settings.license_key[ 0 ] = 0;
            settings::save();
            license::refresh( true );
        }
        ImGui::EndDisabled();

        const license::info st = license::last();
        switch ( st.st )
        {
        case license::status::valid:
            ImGui::PushStyleColor( ImGuiCol_Text, ui::pdv( 0x27AE60 ) );
            ImGui::TextWrapped( "LICENÇA ATIVA: %d dia%s restante%s", st.days_left,
                st.days_left == 1 ? "" : "s", st.days_left == 1 ? "" : "s" );
            ImGui::PopStyleColor();
            break;
        case license::status::expired:
            ImGui::PushStyleColor( ImGuiCol_Text, ui::pdv( 0xE74C3C ) );
            ImGui::TextWrapped( "LICENÇA EXPIRADA - renove para desbloquear o plugin" );
            ImGui::PopStyleColor();
            break;
        case license::status::invalid:
            ImGui::PushStyleColor( ImGuiCol_Text, ui::pdv( 0xE74C3C ) );
            ImGui::TextWrapped( "CHAVE INVÁLIDA - confira o valor colado" );
            ImGui::PopStyleColor();
            break;
        case license::status::unconfigured:
            ui::text_disabled_wrapped( "build de desenvolvimento - nenhuma chave pública configurada ainda" );
            break;
        default:
            ui::text_disabled_wrapped( "nenhuma chave informada" );
            break;
        }
        ImGui::TextWrapped(
            "A licença vale pelo período emitido na chave (o padrão é 365 "
            "dias). Depois desse prazo ela expira e o plugin fica bloqueado "
            "até a licença ser renovada." );

        ImGui::SeparatorText( "Compra" );
        if ( license::purchase_url == nullptr || license::purchase_url[ 0 ] == 0 )
        {
            ui::text_disabled_wrapped(
                "a página da loja está sendo preparada - o link de compra abrirá "
                "neste botão quando estiver no ar" );
            ImGui::BeginDisabled();
            ImGui::Button( "Abrir página da loja" );
            ImGui::EndDisabled();
        }
        else
        {
            if ( ImGui::Button( "Abrir página da loja" ) )
            {
                ShellExecuteA( nullptr, "open", license::purchase_url,
                    nullptr, nullptr, SW_SHOWNORMAL );
            }
            ImGui::SameLine();
            ui::text_disabled_wrapped( license::purchase_url );
        }

        ImGui::SeparatorText( "Privacidade" );
        ui::text_disabled_wrapped(
            "nenhum dado é coletado. o hash do ID do hardware é usado somente "
            "para gerar a sua licença - nada é enviado para lugar nenhum e "
            "nada é guardado além da chave que você cola aqui" );

        ImGui::EndChild();
    }

    // 100 - aba Logs
    void CTrailerManipulation::render_logs_tab() const
    {
        ImGui::BeginChild( "##logs", ImVec2( 0.f, 0.f ) );

        ImGui::SeparatorText( "Diagnóstico" );
        bool debug_logs = settings::g_settings.debug_logs;
        if ( ui::toggle_switch( "##debug_logs", &debug_logs ) )
        {
            settings::g_settings.debug_logs = debug_logs;
            settings::save();
        }
        ImGui::SameLine();
        ImGui::TextWrapped( "Ativar logs de diagnóstico" );
        ui::text_disabled_wrapped(
            "desligado = custo zero, nada é escrito. ligue só quando algo der "
            "errado: reproduza o problema e envie ao desenvolvedor o arquivo "
            "br_carretas_debug.log em Documentos\\Euro Truck Simulator 2" );

        ImGui::EndChild();
    }

    // 101 - reescreve os links slave das peças estacionadas
    void rebuild_parked_claim_links()
    {
        if ( native_chain_call_active.load( std::memory_order_acquire ) ||
             !canon_all_trailer_objs() )
        {
            return;
        }
        void* live[ max_trailers ] = {};
        bool parkedish[ max_trailers ] = {};
        uint32_t ln = 0;
        void* truck = nullptr;
        if ( auto* game_actor = CCore::g_instance->get_game_actor() )
        {
            truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
        }
        for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
        {
            void* t = canon[ i ];
            if ( t == nullptr || !plausible_object( t ) ) continue;
            live[ ln ] = t;

            bool live_to_truck = false;
            for ( void* cur = t; cur != nullptr && joint( cur ) != nullptr; )
            {
                void* p = prism::offsets::field< void* >( cur, prism::offsets::trailer_parent );
                if ( p == truck ) { live_to_truck = true; break; }
                if ( p == nullptr || p == cur || !readable_ptr( p ) ) break;
                cur = p;
            }
            parkedish[ ln ] = !live_to_truck;
            ++ln;
        }
        if ( ln == 0 ) return;
        void* towed = truck != nullptr && readable_ptr( truck )
            ? prism::offsets::field< void* >( truck, 0x1580 ) : nullptr;
        const bool towing = towed != nullptr && joint( towed ) != nullptr;

        for ( uint32_t i = ln; i-- > 0; )
        {
            if ( towing ) continue;
            void** slave = &prism::offsets::field< void* >( live[ i ], prism::offsets::trailer_slave );
            void* want = ( i + 1 < ln ) ? live[ i + 1 ] : nullptr;
            if ( *slave != want ) *slave = want;
            if ( parkedish[ i ] ) set_claim_edge( live[ i ], want );
            else clear_claim_edge( live[ i ] );
        }
        if ( towing )
        {

            void* last_live = nullptr;
            for ( uint32_t i = 0; i < ln; ++i )
                if ( !parkedish[ i ] ) last_live = live[ i ];
            void* next = nullptr;
            for ( uint32_t i = ln; i-- > 0; )
            {
                if ( !parkedish[ i ] ) continue;
                void* t = live[ i ];
                void** sl = &prism::offsets::field< void* >( t, prism::offsets::trailer_slave );

                if ( prism::offsets::field< void* >( t, prism::offsets::trailer_parked_state ) != nullptr &&
                     prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked_registered ) != 0 &&
                     parked_record_listed( t ) )
                {
                    if ( *sl != nullptr && claim_edge_ours( t, *sl ) ) *sl = nullptr;
                    clear_claim_edge( t );
                    continue;
                }
                if ( *sl != next ) *sl = next;
                set_claim_edge( t, next );
                next = t;
            }
            void* anchor = last_live != nullptr ? last_live : towed;

            uint32_t ad = 0;
            for ( void* s = readable_ptr( anchor )
                      ? prism::offsets::field< void* >( anchor, prism::offsets::trailer_slave )
                      : nullptr;
                  s != nullptr && s != anchor && is_trailer_obj( s ) &&
                  joint( s ) != nullptr && ++ad <= max_trailers;
                  s = prism::offsets::field< void* >( anchor, prism::offsets::trailer_slave ) )
            {
                anchor = s;
            }
            void** sl = &prism::offsets::field< void* >( anchor, prism::offsets::trailer_slave );
            if ( *sl != next ) *sl = next;
            set_claim_edge( anchor, next );
        }

        if ( auto* game_actor = CCore::g_instance->get_game_actor() )
        {
            prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_trailer ) =
                towing ? towed : live[ 0 ];
        }
    }

    // 102 - reescan da cadeia + manutenção (links, linhas, marcas)
    void CTrailerManipulation::refresh_trailer_chain()
    {

        if ( canon_dead() ) reset_trailer_state();

        if ( lane_prune_frames > 0 )
        {
            --lane_prune_frames;
            prune_lane_containers();
        }
        else if ( ++lane_prune_idle >= 50 )
        {
            lane_prune_idle = 0;
            prune_lane_containers();
        }

        auto* game_actor = CCore::g_instance->get_game_actor();
        void* truck = game_actor != nullptr
            ? prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck )
            : nullptr;

        trailer_count = 0;
        const auto walk = [ & ]( void* head )
        {
            for ( void* t = head; t != nullptr && is_trailer_obj( t ) &&
                 trailer_count < max_trailers; )
            {
                bool dup = false;
                for ( uint32_t i = 0; i < trailer_count; ++i ) dup |= trailers[ i ] == t;
                if ( dup ) return;
                trailers[ trailer_count++ ] = t;
                track_canon( t );
                void* next = readable_ptr( static_cast< uint8_t* >( t ) + prism::offsets::trailer_slave )
                    ? prism::offsets::field< void* >( t, prism::offsets::trailer_slave ) : nullptr;
                t = ( next != t ) ? next : nullptr;
            }
        };
        walk( truck != nullptr && readable_ptr( truck )
            ? prism::offsets::field< void* >( truck, 0x1580 ) : nullptr );
        walk( game_actor != nullptr
            ? prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_trailer )
            : nullptr );
        for ( void* s : canon )
        {
            if ( s == nullptr || !readable_ptr( s ) ) continue;
            bool listed = false;
            for ( uint32_t i = 0; i < trailer_count; ++i ) listed |= trailers[ i ] == s;
            if ( !listed && trailer_count < max_trailers ) trailers[ trailer_count++ ] = s;
        }

        canon_age = canon[ 0 ] != nullptr ? canon_age + 6 : 0;

        if ( truck_towed != nullptr && joint( truck_towed ) == nullptr &&
            !( pending.kind == pending_kind::connect && pending.trailer == truck_towed ) )
        {
            if ( auto* game_actor = CCore::g_instance->get_game_actor() )
            {
                void* truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
                if ( truck != nullptr )
                {
                    if ( prism::offsets::field< void* >( truck, 0x1580 ) == truck_towed &&
                        set_towed_trailer_fn != nullptr )
                    {
                        set_towed_trailer_fn( truck, true, nullptr );
                    }
                    prism::offsets::field< uint32_t >( truck, prism::offsets::truck_coupling_state ) = 0;
                }
            }
            truck_towed = nullptr;
        }

        if ( truck_towed != nullptr )
        {
            bool listed = false;
            for ( uint32_t i = 0; i < trailer_count; ++i ) listed |= trailers[ i ] == truck_towed;
            if ( !listed && trailer_count < max_trailers ) trailers[ trailer_count++ ] = truck_towed;
        }

        if ( trailer_count > 1 && truck != nullptr && readable_ptr( truck ) )
        {
            void* head = prism::offsets::field< void* >( truck, 0x1580 );
            int hp = -1, cn = 0;
            for ( void* s : canon ) { if ( s == nullptr ) break; if ( s == head ) hp = cn; ++cn; }
            if ( head != nullptr && hp >= 0 && cn > 1 )
            {
                void* rot[ max_trailers ] = {};
                uint32_t rc = 0;
                for ( int k = 0; k < cn && rc < trailer_count; ++k )
                {
                    void* s = canon[ ( hp + k ) % cn ];
                    for ( uint32_t i = 0; i < trailer_count; ++i )
                    {
                        if ( trailers[ i ] == s ) { rot[ rc++ ] = s; break; }
                    }
                }
                for ( uint32_t i = 0; i < trailer_count && rc < max_trailers; ++i )
                {
                    bool in = false;
                    for ( uint32_t k = 0; k < rc; ++k ) in |= rot[ k ] == trailers[ i ];
                    if ( !in ) rot[ rc++ ] = trailers[ i ];
                }
                for ( uint32_t i = 0; i < rc; ++i ) trailers[ i ] = rot[ i ];
            }
        }

        if ( attach_target != nullptr &&
            ( !readable_ptr( attach_target ) || joint( attach_target ) != nullptr ) )
        {
            attach_target = nullptr;
        }

        {
            void* live[ max_trailers ] = {}; uint32_t ln = 0;
            bool jl[ max_trailers ] = {}, parkedish[ max_trailers ] = {};
            {
                void* truck = nullptr;
                if ( auto* game_actor = CCore::g_instance->get_game_actor() )
                {
                    truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
                }
                for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
                {
                    void* t = canon[ i ];
                    if ( t == nullptr || !plausible_object( t ) ) continue;
                    live[ ln ] = t;
                    jl[ ln ] = joint( t ) == nullptr;

                    bool live_to_truck = false;
                    for ( void* cur = t; cur != nullptr && joint( cur ) != nullptr; )
                    {
                        void* p = prism::offsets::field< void* >( cur, prism::offsets::trailer_parent );
                        if ( p == truck ) { live_to_truck = true; break; }
                        if ( p == nullptr || p == cur || !readable_ptr( p ) ) break;
                        cur = p;
                    }

                    parkedish[ ln ] = !live_to_truck;

                    if ( !live_to_truck && joint( t ) == nullptr )
                    {
                        if ( canon_jointless_frames[ i ] < 0xFFFF ) ++canon_jointless_frames[ i ];
                        if ( canon_jointless_frames[ i ] >= 15 &&
                             !is_detached( t ) && !is_plugin_detached( t ) &&
                             canon_age >= 90 && traffic_game_state != nullptr &&
                             *traffic_game_state == 2 )
                        {
                            mark_detached( t );
                            mark_plugin_detached( t );
                            detached_idx[ i ] = true;
                            CCore::g_instance->info( "maint: auto-marked settled parked trailer c{} {:p}", i, t );

                            native_park_detached( t );
                        }
                    }
                    else
                    {
                        canon_jointless_frames[ i ] = 0;
                    }

                    if ( live_to_truck && !carry_hold[ i ] &&
                        ( is_detached( t ) || is_plugin_detached( t ) || detached_idx[ i ] ) )
                    {
                        unmark_detached( t );
                        unmark_plugin_detached( t );
                        detached_idx[ i ] = false;
                    }
                    ++ln;
                }

                if ( trailer_maintenance_ready() && tp_cooldown <= 0 )
                {
                    for ( uint32_t i = 0; i < ln; ++i )
                    {
                        if ( parkedish[ i ] ) continue;
                        void* p = prism::offsets::field< void* >( live[ i ], prism::offsets::trailer_parent );
                        if ( p == nullptr || p == truck || !is_trailer_obj( p ) ) continue;
                        void** psl = &prism::offsets::field< void* >( p, prism::offsets::trailer_slave );
                        void* cur = *psl;
                        if ( cur == live[ i ] )
                        {
                            clear_claim_edge( p );
                            continue;
                        }
                        if ( cur != nullptr && is_trailer_obj( cur ) && joint( cur ) != nullptr ) continue;
                        *psl = live[ i ];
                        clear_claim_edge( p );
                    }
                }
            }

            static uint32_t torn_tick = 0;
            if ( ++torn_tick >= 10 )
            {
                torn_tick = 0;
                for ( auto& torn : lines_torn ) torn = false;
            }
            for ( uint32_t i = 0; i < ln && tp_cooldown <= 0; ++i )
            {
                if ( !jl[ i ] ) continue;

                void* s = readable_ptr( static_cast< uint8_t* >( live[ i ] ) + prism::offsets::trailer_slave )
                    ? prism::offsets::field< void* >( live[ i ], prism::offsets::trailer_slave ) : nullptr;
                const bool tows_section = s != nullptr && s != live[ i ] && is_trailer_obj( s ) &&
                    readable_ptr( static_cast< uint8_t* >( s ) + prism::offsets::trailer_physics_joint ) &&
                    joint( s ) != nullptr;
                if ( tows_section ) continue;

                if ( joint( live[ i ] ) == nullptr &&
                    *reinterpret_cast< uint8_t* >( static_cast< char* >( live[ i ] ) + 0xE8D ) != 0 )
                {
                    *reinterpret_cast< uint8_t* >( static_cast< char* >( live[ i ] ) + 0xE8D ) = 0;
                }
                prism::offsets::field< uint32_t >( live[ i ], 0xEE8 ) = 0;
                if ( set_lines_fn == nullptr ) continue;
                if ( !is_detached( live[ i ] ) && !is_plugin_detached( live[ i ] ) &&
                    live[ i ] != attach_target )
                {
                    continue;
                }

                const int ci = canon_index_of( live[ i ] );
                if ( ci >= 0 && lines_torn[ ci ] ) continue;
                char* const tl = static_cast< char* >( live[ i ] );
                void* const lbeg = *reinterpret_cast< void** >( tl + 0xE98 );

                const ptrdiff_t nent = static_cast< ptrdiff_t >( *reinterpret_cast< uint64_t* >( tl + 0xEA0 ) );
                if ( nent > 0 && nent <= 0x1000 && readable_ptr( lbeg ) )
                {
                    set_lines_fn( live[ i ], 0, 1 );
                    if ( ci >= 0 ) lines_torn[ ci ] = true;
                }
            }

            static bool edge_done[ max_trailers ] = {};
            if ( torn_tick == 0 ) for ( auto& d : edge_done ) d = false;
            for ( uint32_t i = 0; i < ln && tp_cooldown <= 0; ++i )
            {
                void* p = live[ i ];
                if ( joint( p ) == nullptr ) continue;
                void* sl = prism::offsets::field< void* >( p, prism::offsets::trailer_slave );
                if ( sl == nullptr || sl == p || !is_trailer_obj( sl ) || joint( sl ) != nullptr ) continue;
                const int si = canon_index_of( sl );
                if ( si < 0 || edge_done[ si ] ) continue;
                edge_done[ si ] = true;
                CCore::g_instance->info( "uncoupled edge c{} -> c{}: hiding its cables", canon_index_of( p ), si );

                __try
                {
                    if ( set_lines_fn != nullptr ) set_lines_fn( sl, 0, 1 );
                    void* behind = readable_ptr( static_cast< uint8_t* >( sl ) + prism::offsets::trailer_slave )
                        ? prism::offsets::field< void* >( sl, prism::offsets::trailer_slave ) : nullptr;
                    const bool section_behind = behind != nullptr && readable_ptr( behind ) &&
                        readable_ptr( static_cast< uint8_t* >( behind ) + prism::offsets::trailer_physics_joint ) &&
                        joint( behind ) != nullptr;
                    if ( pair_vehicles_fn != nullptr && !section_behind ) pair_vehicles_fn( p, sl );
                }
                __except ( EXCEPTION_EXECUTE_HANDLER )
                {
                }
            }
            bool any_parked = false;
            for ( uint32_t i = 0; i < ln; ++i ) any_parked |= parkedish[ i ];

            if ( any_parked && canon_age >= 15 && canon_all_trailer_objs() &&
                 !native_chain_call_active.load( std::memory_order_acquire ) )
            {
                void* truck = nullptr;
                void* towed = nullptr;
                if ( auto* game_actor = CCore::g_instance->get_game_actor() )
                {
                    truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
                    towed = truck != nullptr && readable_ptr( truck )
                        ? prism::offsets::field< void* >( truck, 0x1580 ) : nullptr;
                }
                const bool towing = towed != nullptr && joint( towed ) != nullptr;

                for ( uint32_t i = 0; i < ln; ++i )
                {
                    const bool jointed = joint( live[ i ] ) != nullptr;
                    if ( canon_was_jointed[ i ] && !jointed )
                    {
                        CCore::g_instance->info( "hook break on c{} - hose teardown", i );

                        lane_prune_frames = 40;

                        __try
                        {
                            if ( set_lines_fn != nullptr )
                            {
                                set_lines_fn( live[ i ], 0, 1 );
                            }
                            if ( truck != nullptr && pair_vehicles_fn != nullptr )
                            {
                                pair_vehicles_fn( truck, live[ i ] );
                            }
                            if ( truck != nullptr && apply_cables_fn != nullptr )
                            {
                                apply_cables_fn( truck, nullptr, 0, 0, 0 );
                            }
                        }
                        __except ( EXCEPTION_EXECUTE_HANDLER )
                        {
                            CCore::g_instance->warning( "hose teardown faulted (função 102) on c{} - skipped", i );
                        }
                    }
                    canon_was_jointed[ i ] = jointed;

                    if ( jointed ) lines_torn[ i ] = false;
                }

                rebuild_parked_claim_links();

                for ( uint32_t i = 0; i < ln; ++i )
                {
                    void* seen[ max_trailers ] = {}; uint32_t sn = 0;
                    void* prev = nullptr;
                    for ( void* t = live[ i ]; t != nullptr; )
                    {
                        bool dup = false;
                        for ( uint32_t k = 0; k < sn; ++k ) dup |= seen[ k ] == t;
                        if ( dup )
                        {
                            if ( prev != nullptr && readable_ptr( prev ) )
                            {
                                CCore::g_instance->warning( "maint: severed cyclic slave link {:p}->{:p}", prev, t );
                                prism::offsets::field< void* >( prev, prism::offsets::trailer_slave ) = nullptr;
                                clear_claim_edge( prev );
                            }
                            break;
                        }
                        seen[ sn++ ] = t;
                        prev = t;
                        if ( !readable_ptr( t ) ||
                            !readable_ptr( static_cast< uint8_t* >( t ) + prism::offsets::trailer_slave ) )
                        {
                            break;
                        }
                        t = prism::offsets::field< void* >( t, prism::offsets::trailer_slave );
                    }
                }

                {
                    void* head = nullptr;
                    if ( auto* game_actor = CCore::g_instance->get_game_actor() )
                        head = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_trailer );
                    void* vis[ max_trailers ] = {}; uint32_t vn = 0;
                    void* tail = nullptr;
                    for ( void* t = head; t != nullptr && vn < max_trailers; )
                    {
                        bool dup = false;
                        for ( uint32_t k = 0; k < vn; ++k ) dup |= vis[ k ] == t;
                        if ( dup || !is_trailer_obj( t ) ) break;
                        vis[ vn++ ] = t;
                        tail = t;
                        t = prism::offsets::field< void* >( t, prism::offsets::trailer_slave );
                    }
                    if ( tail == nullptr )
                    {

                        for ( uint32_t i = 0; i < ln && tail == nullptr; ++i )
                            if ( is_trailer_obj( live[ i ] ) ) tail = live[ i ];
                        if ( tail != nullptr )
                            vis[ vn++ ] = tail;
                    }
                    for ( uint32_t i = 0; i < ln && tail != nullptr; ++i )
                    {
                        bool seen = false;
                        for ( uint32_t k = 0; k < vn; ++k ) seen |= vis[ k ] == live[ i ];
                        if ( seen || !is_trailer_obj( live[ i ] ) ) continue;
                        CCore::g_instance->info( "visibility pin: re-chained unreachable trailer {:p}", live[ i ] );
                        prism::offsets::field< void* >( tail, prism::offsets::trailer_slave ) = live[ i ];
                        set_claim_edge( tail, live[ i ] );
                        tail = live[ i ];
                    }
                }

                for ( uint32_t i = 0; i < ln; ++i )
                {
                    if ( !parkedish[ i ] ) continue;
                    void* t = live[ i ];

                    if ( !is_detached( t ) && !is_plugin_detached( t ) ) continue;
                    if ( prism::offsets::field< void* >( t, prism::offsets::trailer_parked_state ) != nullptr )
                    {
                        prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked ) = 1;
                        prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked_registered ) = 1;
                    }
                    else if ( ( is_detached( t ) || is_plugin_detached( t ) ||
                                t == attach_target ) &&
                              joint( t ) == nullptr &&
                              plausible_vfunc( t, 0x328 / 8 ) )
                    {

                        static uint32_t brace_retry[ max_trailers ] = {};
                        const int ci = canon_index_of( t );
                        if ( ci >= 0 && brace_retry[ ci ]++ > 20 )
                        {
                            brace_retry[ ci ] = 0;
                            vcall< prism::physics_trailer_u_set_trailer_brace_fn >( t, 0x328 / 8, true );
                        }
                    }
                }
            }
        }

        if ( auto* game_actor = CCore::g_instance->get_game_actor() )
        {
            for ( uint32_t i = 0; i < trailer_count; ++i )
            {
                cache_trailer_actor( trailers[ i ], game_actor );
            }
        }
    }

    // 103 - estado do comboio (freios, trava, mangueiras, tráfego)
    void CTrailerManipulation::update_convoy_state()
    {

        if ( !trailer_maintenance_ready() ) return;
        bool in_convoy[ max_trailers ] = {};
        bool parent_in_convoy = true;
        for ( uint32_t i = 0; i < trailer_count; ++i )
        {
            const bool attached = joint( trailers[ i ] ) != nullptr;
            in_convoy[ i ] = trailers[ i ] == truck_towed ? attached : attached && parent_in_convoy;
            parent_in_convoy = in_convoy[ i ];
        }

        void* truck = nullptr;
        if ( auto* game_actor = CCore::g_instance->get_game_actor() )
        {
            truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
        }
        if ( truck_towed != nullptr && truck != nullptr && joint( truck_towed ) != nullptr )
        {
            if ( prism::offsets::field< void* >( truck, 0x1580 ) != truck_towed &&
                set_towed_trailer_fn != nullptr )
            {
                set_towed_trailer_fn( truck, true, truck_towed );
            }
            prism::offsets::field< uint32_t >( truck, prism::offsets::truck_coupling_state ) = 5;
        }

        if ( truck != nullptr && readable_ptr( truck ) &&
            prism::offsets::field< void* >( truck, 0x1580 ) == nullptr )
        {
            for ( uint32_t i = 0; i < trailer_count; ++i )
            {
                void* t = trailers[ i ];
                if ( t == nullptr || !readable_ptr( t ) || joint( t ) == nullptr ) continue;
                if ( prism::offsets::field< void* >( t, prism::offsets::trailer_parent ) == truck )
                {

                    if ( !is_detached( t ) && !is_plugin_detached( t ) &&
                         set_towed_trailer_fn != nullptr )
                    {
                        CCore::g_instance->info( "stuck tow joint on trailer {} - re-pinning towed slot", i );
                        set_towed_trailer_fn( truck, true, t );

                        prism::offsets::field< uint32_t >( truck, prism::offsets::truck_coupling_state ) = 5;
                    }
                    else
                    {
                        CCore::g_instance->info( "stuck tow joint on trailer {} - releasing", i );
                        disconnect_now( t, truck, true );
                    }
                    break;
                }
            }
        }

        update_convoy_brakes( truck, in_convoy );

        update_parked_locks();

        uint64_t lines_sig = trailer_count;
        for ( uint32_t i = 0; i < trailer_count && i < 20; ++i )
        {
            void* t = trailers[ i ];
            const bool live = t != nullptr && readable_ptr( t ) && joint( t ) != nullptr;
            void* slave = t != nullptr && readable_ptr( t ) &&
                readable_ptr( static_cast< uint8_t* >( t ) + prism::offsets::trailer_slave )
                ? prism::offsets::field< void* >( t, prism::offsets::trailer_slave ) : nullptr;
            const bool edge = slave != nullptr && readable_ptr( slave ) && joint( slave ) != nullptr;
            lines_sig |= ( live ? 1ull : 0 ) << i;
            lines_sig |= ( edge ? 1ull : 0 ) << ( i + 20 );
        }
        void* towed_sig = truck != nullptr && readable_ptr( truck )
            ? prism::offsets::field< void* >( truck, 0x1580 ) : nullptr;
        lines_sig |= ( towed_sig != nullptr && readable_ptr( towed_sig ) &&
            readable_ptr( static_cast< uint8_t* >( towed_sig ) + prism::offsets::trailer_physics_joint ) &&
            joint( towed_sig ) != nullptr ? 1ull : 0 ) << 40;
        static uint64_t last_lines_sig = ~0ull;
        if ( lines_sig != last_lines_sig )
        {
            last_lines_sig = lines_sig;
            refresh_all_lines();
        }

        unpark_jointed( in_convoy );

        update_landing_gear();

        enforce_chase_camera_anchor();

        if ( saved_traffic >= 0.f && !pending_cmd_queued.load( std::memory_order_acquire ) )
        {
            bool all_coupled = expected_trailers > 0 && trailer_count >= expected_trailers &&
                traffic_detach_done.load( std::memory_order_acquire );
            for ( uint32_t i = 0; all_coupled && i < trailer_count; ++i ) all_coupled &= in_convoy[ i ];

            for ( void* d : detached_trailers )
                if ( d != nullptr && joint( d ) == nullptr ) all_coupled = false;
            if ( all_coupled )
            {
                snprintf( pending_cmd, sizeof( pending_cmd ), "g_traffic %g", saved_traffic );
                pending_cmd_queued.store( true, std::memory_order_release );
                CCore::g_instance->info( "restoring g_traffic = {:.2f}", saved_traffic );
                saved_traffic = -1.f;
                expected_trailers = 0;
                traffic_detach_done.store( false, std::memory_order_release );
            }

            else if ( get_cvar_float_fn != nullptr && g_traffic_cvar != nullptr &&
                get_cvar_float_fn( g_traffic_cvar ) > 0.f )
            {
                queue_cmd( "g_traffic 0" );
            }
        }
    }

    // 104 - limpa marcas de desengate expiradas
    void CTrailerManipulation::gc_detached_marks()
    {
        for ( void*& d : detached_trailers )
        {
            if ( d == nullptr || d == truck_towed ) continue;
            bool live = false;
            for ( uint32_t i = 0; i < trailer_count; ++i ) live |= trailers[ i ] == d;
            if ( !live || joint( d ) != nullptr ) d = nullptr;
        }
        for ( void*& d : plugin_detached )
        {
            if ( d == nullptr || d == truck_towed ) continue;
            bool live = false;
            for ( uint32_t i = 0; i < trailer_count; ++i ) live |= trailers[ i ] == d;
            if ( !live || joint( d ) != nullptr ) d = nullptr;
        }
    }

    int sleep_release_countdown = 0;
    // 105 - injeta Shift+Num0 (dormir nativo)
    void trigger_native_sleep()
    {
        INPUT keys[ 2 ] = {};
        keys[ 0 ].type = INPUT_KEYBOARD;
        keys[ 0 ].ki.wVk = VK_LSHIFT;
        keys[ 1 ].type = INPUT_KEYBOARD;
        keys[ 1 ].ki.wVk = VK_NUMPAD0;
        SendInput( 2, keys, sizeof( INPUT ) );

        sleep_release_countdown = 4;
        CCore::g_instance->info( "sleep: Shift+Num0 injected" );
    }
    // 106 - solta as teclas do dormir
    void update_sleep_release()
    {
        if ( sleep_release_countdown <= 0 ) return;
        if ( --sleep_release_countdown > 0 ) return;
        INPUT keys[ 2 ] = {};
        keys[ 0 ].type = INPUT_KEYBOARD;
        keys[ 0 ].ki.wVk = VK_NUMPAD0;
        keys[ 0 ].ki.dwFlags = KEYEVENTF_KEYUP;
        keys[ 1 ].type = INPUT_KEYBOARD;
        keys[ 1 ].ki.wVk = VK_LSHIFT;
        keys[ 1 ].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput( 2, keys, sizeof( INPUT ) );
    }

    // 107 - teclas de atrelar/desatrelar
    void CTrailerManipulation::update_hotkeys()
    {
        update_sleep_release();
        if ( !connect_valid_ || !game_has_focus() || settings::is_capturing() ) return;

        if ( settings::g_settings.cargo_restore )
        {
            bool ad2 = false, ad3 = false, tk2 = false, tk3 = false;
            key_down( settings::Action::ATTACH_DETACH_2, &ad2 );
            key_down( settings::Action::ATTACH_DETACH_3, &ad3 );
            key_down( settings::Action::ATTACH_TRUCK_2, &tk2 );
            key_down( settings::Action::ATTACH_TRUCK_3, &tk3 );
            if ( ad2 ) this->toggle_attach( 1 );
            if ( ad3 ) this->toggle_attach( 2 );

            if ( tk2 ) this->connect_to_truck( canon[ 1 ] );
            if ( tk3 ) this->connect_to_truck( canon[ 2 ] );
        }
        bool slp = false;
        key_down( settings::Action::SLEEP_PARK, &slp );
        if ( slp ) trigger_native_sleep();
    }

    namespace
    {
        const bool fn_idx_reg = []()
        {
            fn_index_add( fn_addr_of( &track_canon ), 65, "track_canon" );
            fn_index_add( fn_addr_of( &canon_index_of ), 66, "canon_index_of" );
            fn_index_add( fn_addr_of( &claim_edge_ours ), 67, "claim_edge_ours" );
            fn_index_add( fn_addr_of( &clear_claim_edge ), 68, "clear_claim_edge" );
            fn_index_add( fn_addr_of( &set_claim_edge ), 69, "set_claim_edge" );
            fn_index_add( fn_addr_of( &dump_chain ), 70, "dump_chain" );
            fn_index_add( fn_addr_of( &canon_dead ), 71, "canon_dead" );
            fn_index_add( fn_addr_of( &canon_all_trailer_objs ), 72, "canon_all_trailer_objs" );
            fn_index_add( fn_addr_of( &sever_all_claims ), 73, "sever_all_claims" );
            fn_index_add( fn_addr_of( &reset_trailer_state ), 74, "reset_trailer_state" );
            fn_index_add( fn_addr_of( &release_detach_marks ), 75, "release_detach_marks" );
            fn_index_add( fn_addr_of( &canon_coupled_predecessor ), 76, "canon_coupled_predecessor" );
            fn_index_add( fn_addr_of( &trailer_is_live ), 77, "trailer_is_live" );
            fn_index_add( fn_addr_of( &trailer_is_known_parked ), 78, "trailer_is_known_parked" );
            fn_index_add( fn_addr_of( &trailer_maintenance_ready ), 79, "trailer_maintenance_ready" );
            fn_index_add( fn_addr_of( &update_fault_filter ), 83, "update_fault_filter" );
            fn_index_add( fn_addr_of( &rebuild_parked_claim_links ), 101, "rebuild_parked_claim_links" );
            fn_index_add( fn_addr_of( &trigger_native_sleep ), 105, "trigger_native_sleep" );
            fn_index_add( fn_addr_of( &update_sleep_release ), 106, "update_sleep_release" );
            return true;
        }();
    }
}
