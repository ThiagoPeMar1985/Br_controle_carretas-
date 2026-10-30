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

namespace ts_extra_utilities
{

    void trigger_native_sleep();
    //========================================================================
    // Ordem canônica (canon) e rastreamento de estado das carretas
    //
    // trailers[] é reconstruído todo frame a partir da raiz de cadeia que o
    // jogo entrega; uma carreta que sai do walk de +0xC8 (desengate nativo,
    // respawn de oficina) desloca todos os slots atrás dela e qualquer estado
    // indexado por posição passa a apontar pro objeto errado. canon[] mantém
    // a ordem ORIGINAL - peças entram na primeira vez vistas e nunca mudam de
    // lugar, então canon_index_of(t) é o slot estável em que os arrays de
    // âncora/snapshot se apoiam.
    //========================================================================
    void* canon[ max_trailers ] = {};
    // frames desde que o CONTEÚDO do canon mudou - resetado por
    // reset_trailer_state (reload do mundo), por cada novo membro do canon e
    // enquanto canon está vazio. Todo caminho que chama código nativo nas
    // carretas (rebuild de mangueiras, registro de parked, reescrita de
    // cadeia, recusa de hook) depende dessa contagem: durante a montagem do
    // save/mundo as carretas aparecem sem junta e pela metade por vários
    // frames, e chamar vfuncs nelas crasheia (assert do model_object). O
    // portão só abre quando o conjunto para de mudar por ~1.5s
    uint32_t canon_age = 0;
    bool lw_ctx_logged[ max_trailers ] = {};

    // registra uma carreta no canon na primeira vez que aparece - nunca
    // reordena (ver header acima)
    void track_canon( void* t )
    {
        if ( t == nullptr ) return;
        for ( auto& c : canon )
        {
            if ( c == t ) return;
            if ( c == nullptr ) { c = t; canon_age = 0; return; }
        }
    }

    // slot canônico estável de uma carreta (-1 se desconhecida)
    int canon_index_of( const void* t )
    {
        for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
            if ( canon[ i ] == t ) return static_cast< int >( i );
        return -1;
    }

    // arestas slave que NÓS escrevemos (dono -> filho) - os hooks nativos
    // consultam pra distinguir um claim nosso de um claim do próprio jogo
    struct synthetic_slave_edge
    {
        std::atomic< void* > owner{ nullptr };
        std::atomic< void* > child{ nullptr };
    };
    synthetic_slave_edge synthetic_edges[ max_trailers ];

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

    // dump permanente do estado - link/junção/record de cada carreta do canon (teste de edição)
    // em cada fronteira nativa. Fica no log pra sempre: os bugs de carreta
    // sumindo eram invisíveis justamente porque nada registrava como o mundo
    // estava antes/depois de um dispatch
    void dump_chain( const char* tag )
    {
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

    // qualquer entrada do canon que deixou de ser um objeto-carreta válido
    // significa reload do mundo (save, respawn de oficina): todo ponteiro ou
    // marca cacheada pode estar obsoleto, então o rastreador inteiro reseta
    // e reconstrói a partir do estado vivo
    bool canon_dead()
    {
        for ( auto* c : canon )
        {
            // um slot de carreta liberado e reusado por alocação estranha
            // continua legível - só a vtable revela que deixou de ser
            // carreta (os crashes do ponteiro em KERNELBASE)
            if ( c != nullptr && !is_trailer_obj( c ) ) return true;
        }
        return false;
    }

    // toda entrada não-nula do canon é objeto-carreta - o piso de validade
    // que o pin de visibilidade exige antes de escrever links slave
    bool canon_all_trailer_objs()
    {
        for ( auto* c : canon )
            if ( c != nullptr && !is_trailer_obj( c ) ) return false;
        return true;
    }

    // corta todos os links slave que nós escrevemos - usado no reset e no
    // shutdown pra nenhum walker nativo encontrar claim nosso apontando pra
    // objeto que já morreu. Só zera o slave quando o link atual ainda é o
    // que nós mesmos pusemos
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

    // zera todo ponteiro e marca cacheada - chamado por canon_dead e pelos
    // wrappers SEH pra uma falha nunca deixar rastreador montado pela metade
    void reset_trailer_state()
    {
        // o mundo está indo embora (reload ou quit): corta os claims antes
        // de largar os ponteiros, senão o teardown nativo anda links mortos
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
            // only the section head keeps the slot mark - a member jointed
            // behind it carries just the pose (a jointed marked slot would
            // re-arm the restore every frame)
            detached_idx[ i ] = was_head[ i ];
            parked_xform[ i ] = carry_xf[ i ];
            parked_xform_valid[ i ] = true;
            CCore::g_instance->info( "carrying detached slot {} across the respawn", i );
        }
        CCore::g_instance->info( "trailer state reset" );
    }

    // derruba todas as marcas de desengate de uma carreta escolhida pro
    // engate nativo por T - o hook do dispatcher recusa uma head
    // plugin-detached
    void release_detach_marks( void* t )
    {
        unmark_detached( t );
        unmark_plugin_detached( t );
        const int ci = canon_index_of( t );
        if ( ci >= 0 ) detached_idx[ ci ] = false;
    }

    // antecessor canônico de uma carreta, somente se estiver acoplado
    void* canon_coupled_predecessor( void* t )
    {
        const int ci = canon_index_of( t );
        if ( ci <= 0 ) return nullptr;
        void* p = canon[ ci - 1 ];
        return p != nullptr && readable_ptr( p ) && joint( p ) != nullptr ? p : nullptr;
    }

    // uma carreta é "viva" quando o comboio realmente a alcança - sobe pela
    // cadeia física de parent (+0x1080) até o caminhão. Parents/juntas só são
    // escritos em engate/desengate, então a leitura fica estável enquanto a
    // manutenção reescreve os links slave no meio do frame - descer pela
    // cadeia slave classificaria uma carreta engatada como estacionada por
    // um frame e o pin de lamp a apagaria (o flicker). Carretas canônicas
    // cuja cadeia de parent morre antes do caminhão são peças estacionadas,
    // NÃO membros do comboio: lamps/freios/mangueiras voltam a ser delas
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

    // Um objeto sem junta só faz parte de uma seção estacionada real quando
    // já existe marca de desengate nele ou num de seus parents físicos.
    // Durante save/load o jogo monta links slave nativos antes de criar as
    // juntas; esses objetos sem marca têm que ser deixados pro fluxo de
    // connect do próprio jogo.
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

    // portão de "mundo montado e estável" - true quando o canon parou de
    // mudar, o tráfego está no estado de jogo e toda entrada é classificável
    // (viva OU estacionada conhecida)
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
            // uma peça em limbo (sem record, não estacionada conhecida) NÃO
            // pode fechar o portão inteiro - é exatamente o que a manutenção
            // de cadeia existe pra manter alcançável. Tratá-la como inválida
            // parava toda escrita de claim e derrubava o conjunto estacionado
            // do walk de +0xC8
            if ( !trailer_is_live( t ) && !trailer_is_known_parked( t ) &&
                 !is_detached( t ) && !is_plugin_detached( t ) )
                return false;
        }
        return any;
    }

    //========================================================================
    // CTrailerManipulation - lifecycle
    //========================================================================
    CTrailerManipulation::CTrailerManipulation() = default;

    CTrailerManipulation::~CTrailerManipulation()
    {
        // devolve o mundo limpo antes de soltar os hooks: nossos claims
        // slave sintéticos ficam na cadeia de +0xC8 e o teardown do mundo
        // anda esses links em carretas já meio-destruídas - o hang/crash no
        // quit era walker nativo encontrando claim que a manutenção ainda
        // mantinha. Só escreve em objeto que ainda é carreta válida, e só
        // zera o link quando ele é nosso
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

    bool CTrailerManipulation::init()
    {
        // Individually detachable trailers - attach/detach hooks and convoy
        // maintenance live in trailer_coupling.cpp; false = unavailable
        this->connect_valid_ = coupling_install();

        // steerable axles (oversize trailers): the hook only swaps the angle,
        // so a miss just means no steering control - never fatal
        steering_install(); // trailer_steering.cpp

        // reverse-gear detection for the axle-lift assist lives in
        // trailer_axles.cpp now; a failed channel just narrows the fallback
        lift_register_channels();

        air_register_channels(); // trailer_air.cpp

        coupling_register_channels(); // trailer_coupling.cpp

        return true;
    }

    //========================================================================
    // Per-frame update - each subsystem exposes an entry point; convoy
    // refresh, state maintenance and hotkeys live in trailer_coupling.cpp.
    //========================================================================
    void CTrailerManipulation::update()
    {
        // joystick buttons have no WM_KEYDOWN - poll them while a capture is open
        settings::poll_joystick_capture();

        // license gate: the UI keeps rendering (so a key can be pasted) but
        // every feature update stops until a valid key is entered
        license::refresh();
        if ( !license::active() ) return;

        // per-section cycle accounting (rdtsc ≈ 20ns a read): when the game
        // crawls the next log line names the stage eating frame time instead
        // of guessing - silent while everything stays cheap
        uint64_t pt[ 10 ] = {};
        pt[ 0 ] = __rdtsc();
        refresh_trailer_chain();   // trailer_coupling.cpp
        pt[ 1 ] = __rdtsc();
        update_convoy_state();     // trailer_coupling.cpp
        pt[ 2 ] = __rdtsc();
        light_watch_tick();        // trailer_lines.cpp - parked lamp pin
        pt[ 3 ] = __rdtsc();
        gc_detached_marks();       // trailer_coupling.cpp
        watch_sim_heartbeat();
        pt[ 4 ] = __rdtsc();

        auto* game_actor = CCore::g_instance->get_game_actor();

        // reverse-gear lift assist + per-axle lift mask rewrite
        lift_axles_update( game_actor );   // trailer_axles.cpp
        pt[ 5 ] = __rdtsc();
        // cargo respawn watch + rig restore countdown
        coupling_parked_update();          // trailer_loadstate.cpp
        pt[ 6 ] = __rdtsc();
        update_hotkeys();                  // trailer_coupling.cpp
        update_steering( ImGui::GetIO().DeltaTime );
        pt[ 7 ] = __rdtsc();
        update_air( ImGui::GetIO().DeltaTime );
        pt[ 8 ] = __rdtsc();
        brake_boost_update(); // trailer_brake.cpp - +0x760 boost + wheel mask
        pt[ 9 ] = __rdtsc();

        static uint64_t acc[ 9 ] = {};
        static uint64_t tsc0 = 0;
        static LARGE_INTEGER qpc0{};
        static uint32_t win_frames = 0;
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
            // tsc calibrated against qpc over the same window - no assumed
            // clock speed, so the ms numbers stay honest on any core
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

    //========================================================================
    // User actions (buttons + hotkeys - they only queue sim-thread ops)
    //========================================================================
    void CTrailerManipulation::disconnect_trailer( const uint32_t i ) const
    {
        // canon index, not trailers[] position: the list is rotated while a
        // trailer sits on the fifth wheel, but the key/button must always
        // mean the same physical piece
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
        // the real physical parent (+0x1080) - the player can rebuild the
        // chain in any order, so the canonical predecessor is only a
        // fallback when the field reads empty
        void* parent = i == 0 || on_truck
            ? truck
            : ( readable_ptr( reinterpret_cast< uint8_t* >( trailer ) + prism::offsets::trailer_parent )
                ? prism::offsets::field< void* >( trailer, prism::offsets::trailer_parent ) : nullptr );
        if ( parent == nullptr && i > 0 ) parent = canon[ i - 1 ];
        // +0x1080 may resolve to the truck object for a fifth-wheel trailer -
        // that is a truck parent, not a trailer one (the truck has no trailer
        // fields at these offsets)
        const bool parent_is_truck = i == 0 || on_truck ||
            ( truck != nullptr && parent == truck );

        // a middle link may leave with its slave still jointed - the detached
        // pair (dolly + rear trailer) parks as a unit; the detach mark keeps
        // the game from auto-re-attaching the freed link to its old parent
        CCore::g_instance->info( "Disconnecting trailer {}", i );

        // traffic stays live: the parked vfunc (+0x328) registers the detached
        // trailer as a proper lane obstacle, so the sweep handles it like any
        // parked trailer - no suppression or despawn wait needed
        if ( queue_op( pending_kind::disconnect, trailer, parent, parent_is_truck, 0 ) )
        {
            traffic_detach_done.store( false, std::memory_order_release );
        }
    }

    void CTrailerManipulation::connect_trailer( const uint32_t i ) const
    {
        // canon index - "carreta 2" always means the second physical piece,
        // wherever it currently sits in the rotated trailers[] list
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
        // the chain parent must be attached itself - the game only ever creates
        // a joint onto an already-jointed parent; hooking onto a free trailer
        // leaves the component half-initialized (purecall crash)
        if ( i > 0 && joint( parent ) == nullptr )
        {
            CCore::g_instance->warning( "Engate a carreta {} primeiro", i );
            return;
        }
        // proximity rule like the native T: hooks must nearly touch - no
        // teleporting the trailer onto the hook (< 7 m measured hook-to-hook)
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
        // back on its chain parent: the game may manage this trailer again
        unmark_detached( trailer );
        queue_op( pending_kind::connect, trailer, parent, i == 0 );
    }

    // "connect to truck" only SELECTS which parked trailer the native T key
    // attaches next - nothing couples here. hk_attach_dispatch aims the
    // native attach at the selection while the fifth wheel is free
    void CTrailerManipulation::connect_to_truck( void* trailer ) const
    {
        if ( trailer == nullptr || !readable_ptr( trailer ) ) return;
        const int ci = canon_index_of( trailer );
        if ( joint( trailer ) != nullptr )
        {
            CCore::g_instance->warning( "A carreta {} já está engatada", ci >= 0 ? ci + 1 : 0 );
            return;
        }
        // switch: picking the trailer that is already the target hands the
        // T back to the original (first) trailer; picking another moves it
        if ( attach_target == trailer )
        {
            attach_target = nullptr;
            CCore::g_instance->info( "Alvo de engate: carreta original (1) - aperte T" );
            return;
        }
        // the detach marks stay until the dispatcher is actually aiming at
        // this trailer (released inside hk_attach_dispatch): an unmarked
        // parked piece becomes a candidate for the game's own slave
        // auto-attach and gets pulled in when a parent couples next to it
        attach_target = trailer;
        CCore::g_instance->info( "Alvo de engate: carreta {} - recue até ela e aperte T",
                                 ci >= 0 ? ci + 1 : 0 );
    }

    void CTrailerManipulation::toggle_attach( const uint32_t i ) const
    {
        // canon index: the first trailer stays on the game's own T key - its
        // detach path is not safe to replay manually
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

    // keybind list: action name, the bound key as a clickable capture button,
    // and a small clear button - packed tight for the narrow side panel
    void CTrailerManipulation::render_keybind_row( const int i ) const
    {
        const auto action = static_cast< settings::Action >( i );
        const bool fixed = action == settings::Action::TOGGLE_UI
            || action == settings::Action::TOGGLE_CURSOR;
        ImGui::PushID( i );
        ImGui::TextWrapped( "%s", settings::action_name( action ) );
        if ( fixed )
        {
            // rebindable in settings.ini only - keeping them off the
            // capture path makes the plugin unreachable harder
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

    void CTrailerManipulation::render_keybinds() const
    {
        for ( int i = 0; i < settings::action_count; ++i )
        {
            const auto action = static_cast< settings::Action >( i );
            // steering binds live in the Oversize panel; sleep não é
            // implementada - a linha some da tela
            if ( action == settings::Action::STEER_LEFT ||
                 action == settings::Action::STEER_RIGHT ||
                 action == settings::Action::STEER_CENTER ||
                 action == settings::Action::TOGGLE_STEERING_LOCK ||
                 action == settings::Action::SLEEP_PARK ) continue;
            this->render_keybind_row( i );
        }
    }

    // render_trailer_lift lives in trailer_axles.cpp;
    // render_trailer_steering + render_oversize in trailer_steering.cpp

    //========================================================================
    // UI
    //========================================================================
    void CTrailerManipulation::render_trailer_connect( const uint32_t i ) const
    {
        // i is the canon index - the button must always mean the same
        // physical trailer, no matter how trailers[] is currently rotated
        if ( i == 0 )
        {
            ImGui::TextDisabled( "Gerenciada pelo jogo (tecla T)" );
            return;
        }
        void* trailer = i < max_trailers ? canon[ i ] : nullptr;
        if ( trailer == nullptr || !readable_ptr( trailer ) ) return;

        ImGui::SeparatorText( "Engatar / Desengatar" );
        if ( !this->connect_valid_ )
        {
            ImGui::TextDisabled( "Indisponível nesta versão do jogo" );
            return;
        }

        const bool attached = joint( trailer ) != nullptr;
        ImGui::BeginDisabled( attached );
        if ( ImGui::Button( "Engatar##trailer" ) )
        {
            this->connect_trailer( i );
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled( !attached );
        if ( ImGui::Button( "Desengatar##trailer" ) )
        {
            this->disconnect_trailer( i );
        }
        ImGui::EndDisabled();

        // picking up a trailer behind the first one with the truck directly,
        // only possible while the fifth wheel is free (trailer 1 detached or
        // absent)
        if ( !attached )
        {
            const bool wheel_free = truck_towed == nullptr &&
                ( canon[ 0 ] == nullptr || joint( canon[ 0 ] ) == nullptr );
            ImGui::BeginDisabled( !wheel_free );
            const bool is_target = attach_target == trailer;
            if ( is_target ) ImGui::TextColored( ImVec4( 0.15f, 0.68f, 0.38f, 1.f ), "Alvo do T: esta carreta" );
            if ( ImGui::Button( is_target ? "Voltar alvo p/ carreta 1##trailer" : "Selecionar p/ engate (T)##trailer" ) )
            {
                this->connect_to_truck( trailer );
            }
            ImGui::EndDisabled();
            if ( !wheel_free )
            {
                ImGui::SameLine();
                ImGui::TextDisabled( "(quinta roda ocupada)" );
            }
        }
    }

    void CTrailerManipulation::render()
    {
        // PDV-CONTROLE palette, pre-linearized (ui::pdv): #232359 primary blue,
        // #222888 sidebar/hover, #303030 window, #282828 panels, #020202
        // inputs, #444444 border, #4a6fa5 buttons, #27ae60 green, white text
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
            // locked: banner + license tab only - every feature stays off
            // until a valid key is entered
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
        // folder tabs: click switches, re-clicking the open tab collapses it
        // for the session - the open tab itself is persisted to settings.ini
        static const char* tab_names[] = { "Teclas", "Oversize", "Carretas", "Licença" };
        for ( int t = 0; t < 4; ++t )
        {
            if ( t > 0 ) ImGui::SameLine();
            const bool open = settings::g_settings.ui_tab == t;
            if ( ImGui::Selectable( tab_names[ t ], open, 0, ImVec2( 110.f, 0.f ) ) )
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
        case 3: this->render_license_tab(); break;
        default: break;
        }
        }

        ImGui::End();
        ImGui::PopStyleColor( 16 );
        ImGui::PopStyleVar( 5 );
    }

    // the Teclas tab: the window binds plus every truck-side assist
    void CTrailerManipulation::render_keys_tab() const
    {
        ImGui::BeginChild( "##keys", ImVec2( 0.f, 0.f ) );
        ImGui::SeparatorText( "Teclas" );
        this->render_keybinds();
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

        render_cargo_restore_option(); // trailer_loadstate.cpp

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

    // the Oversize tab: steerable-axle controls (trailer_steering.cpp)
    void CTrailerManipulation::render_oversize_tab() const
    {
        ImGui::BeginChild( "##oversize", ImVec2( 0.f, 0.f ) );
        ImGui::SeparatorText( "Oversize" );
        this->render_oversize();
        ImGui::EndChild();
    }

    // the Carretas tab: one fixed column per trailer, side by side - CANON
    // order so "Carreta N" is always the same physical piece no matter how
    // the rotated trailers[] list is currently arranged
    void CTrailerManipulation::render_trailers_tab() const
    {
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
                // lift/steer still key on trailers[] position - resolve the
                // canon piece back to its current slot
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

    // the Licença tab: copyright/contact, the machine code the buyer sends
    // to the site, the issued key field (kept in settings.ini) and the
    // annual-license terms
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
        // the client copies the tagged id (ETS-...) - the tag tells the key
        // generator which game/private key the license must be signed for
        const std::string tagged = hwid.empty() ? hwid : "ETS-" + hwid;
        // fixed width: a size-0 Selectable eats the whole row and pushes the
        // Copy button outside the child clip region
        ImGui::Selectable( tagged.empty() ? "(indisponível)" : tagged.c_str(),
            false, ImGuiSelectableFlags_Disabled, ImVec2( 200.f, 0.f ) );
        if ( !tagged.empty() )
        {
            ImGui::SameLine();
            if ( ImGui::SmallButton( "Copiar" ) )
            {
                // ImGui::SetClipboardText only reaches the OS clipboard when a
                // platform backend installs io.SetClipboardTextFn - the in-game
                // overlay has none, so push the text through Win32 directly
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
        // edit buffer kept apart from the stored key: typing must not disturb
        // a live valid license - only Ativar commits and re-validates
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
            license::refresh( true ); // drops straight back to the locked page
        }
        ImGui::EndDisabled();
        // live status, refreshed every ~2 s by the update gate
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
                "por este botão quando estiver no ar" );
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

    //========================================================================
    // Per-frame update - chain refresh, convoy state, attach hotkeys
    //========================================================================

    // Escreve a topologia de claims das peças estacionadas - extraído de
    // refresh_trailer_chain pra rodar também no fim de um detach: sem isso a
    // seção recém-solta fica inalcançável ~1 frame e a máquina de
    // visibilidade a esconde/mostra = a piscada em todo desatrelar.
    // Re-deriva live[]/parkedish[]/towed/towing sozinha pra ficar standalone
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
            // "live" = a cadeia de juntas bate no caminhão - carreta presa
            // num pai ESTACIONADO é do conjunto parked
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
            ? prism::offsets::field< void* >( truck, 0x15A8 ) : nullptr;
        const bool towing = towed != nullptr && joint( towed ) != nullptr;
        // the writes: while the truck tows nothing, every piece's
        // slave = its canonical successor - the parked machine's
        // own claim shape, which keeps every parked piece
        // reachable and visible. Written tail-to-head so no
        // intermediate state passes through a cycle either.
        // WHILE TOWING the model changes: the trailer on the
        // fifth wheel IS the head - parked pieces claim nothing
        // (a claim is what made the game treat the parked piece
        // as "the truck's trailer": hoses, markers, the wrong-T
        // detach - all of it followed the claim root). Jointed
        // pieces keep whatever slave the native chain gave them
        for ( uint32_t i = ln; i-- > 0; )
        {
            if ( towing ) continue;   // parked pieces hang at the tail below
            void** slave = &prism::offsets::field< void* >( live[ i ], prism::offsets::trailer_slave );
            void* want = ( i + 1 < ln ) ? live[ i + 1 ] : nullptr;
            if ( *slave != want ) *slave = want;
            if ( parkedish[ i ] ) set_claim_edge( live[ i ], want );
            else clear_claim_edge( live[ i ] );
        }
        if ( towing )
        {
            // parked pieces hang after the last piece whose joint
            // chain reaches the truck - a member bolted to a parked
            // parent is parked too and appending after it writes a
            // backward claim into its own parent. Written
            // TAIL-TO-HEAD: a mid-pass write that points an upstream
            // piece at a downstream one whose stale link still aims
            // back (canon[1].slave=canon[0] while canon[0].slave is
            // still canon[1]) forms a 2-cycle for a few cycles -
            // a concurrent native chain walker spins on it forever
            // and dies by stack overflow with no crash record
            void* last_live = nullptr;
            for ( uint32_t i = 0; i < ln; ++i )
                if ( !parkedish[ i ] ) last_live = live[ i ];
            void* next = nullptr;
            for ( uint32_t i = ln; i-- > 0; )
            {
                if ( !parkedish[ i ] ) continue;
                void* t = live[ i ];
                void** sl = &prism::offsets::field< void* >( t, prism::offsets::trailer_slave );
                // peça com record nativo registrado é sustentada pela própria
                // máquina de estacionamento - claimada atrás do conjunto vivo,
                // o walker nativo a trata como membro do comboio e reescreve a
                // pose dela todo frame (a âncora re-aserta de volta = a
                // tremida). Sem claim não há caminho pro walker: ela fica
                // exatamente onde está, independente do que está engatado.
                // Se o record cair, o claim volta sozinho no próximo frame
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
            // last_live is the deepest live piece in canon order,
            // not necessarily the physical tail - a repaired or
            // native link can leave a jointed member behind it.
            // Walk jointed slaves to the real tail so the parked
            // append never overwrites a live link
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
        // +0xC8 = the real head: the trailer physically on the
        // fifth wheel while towing, the canonical parked root
        // otherwise (that is what the attach marker/T-attach
        // resolver needs when nothing is coupled)
        if ( auto* game_actor = CCore::g_instance->get_game_actor() )
        {
            prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_trailer ) =
                towing ? towed : live[ 0 ];
        }
    }

    // rebuilds trailers[] from game_actor and merges in truck_towed: a spliced
    // trailer no longer appears in the chain walk, but keeps being listed so
    // the UI and hotkeys can still reach it while it sits on the truck
    void CTrailerManipulation::refresh_trailer_chain()
    {
        // world reload: every cached trailer pointer is dead - start clean
        if ( canon_dead() ) reset_trailer_state();

        // drop orphaned lane-obstacle entries before any scan/erase meets
        // them - dense during the post-detach window (the crash hit 48ms
        // after a joint drop), a slow sweep otherwise
        if ( lane_prune_frames > 0 )
        {
            --lane_prune_frames;
            prune_lane_containers();
        }
        else if ( ++lane_prune_idle >= 300 )
        {
            lane_prune_idle = 0;
            prune_lane_containers();
        }

        auto* game_actor = CCore::g_instance->get_game_actor();
        void* truck = game_actor != nullptr
            ? prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck )
            : nullptr;

        // two roots carry trailers: the live towed chain (truck+0x15A8) and
        // the trailer-of-interest chain (+0xC8) which also covers parked
        // pieces. Merge both walks, deduped, then append every canon member
        // that fell outside both - a parked trailer must stay in the list or
        // the attach keys can never reach it
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
            ? prism::offsets::field< void* >( truck, 0x15A8 ) : nullptr );
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

        // frames with a populated canon - the assembly window detector every
        // risky path gates on
        canon_age = canon[ 0 ] != nullptr ? canon_age + 1 : 0;

        // the trailer parked on the truck through "connect to truck" is free
        // again once its joint is gone - release the coupling-state pin and
        // the towed slot too or the machine stays stuck at "coupled" and
        // refuses new attaches. But don't release while a connect op is still
        // queued: truck_towed is assigned at queue time, before the joint
        // exists
        if ( truck_towed != nullptr && joint( truck_towed ) == nullptr &&
            !( pending.kind == pending_kind::connect && pending.trailer == truck_towed ) )
        {
            if ( auto* game_actor = CCore::g_instance->get_game_actor() )
            {
                void* truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
                if ( truck != nullptr )
                {
                    if ( prism::offsets::field< void* >( truck, 0x15A8 ) == truck_towed &&
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

        // promotion ordering: the trailer on the fifth wheel leads the list
        // and the rest follow in canonical order - attach trailer 1 and the
        // list becomes [1, 2, 0]; detach and it falls back to canonical
        // [0, 1, 2] because nothing is towed
        if ( trailer_count > 1 && truck != nullptr && readable_ptr( truck ) )
        {
            void* head = prism::offsets::field< void* >( truck, 0x15A8 );
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
        // +0xC8 chain maintenance - a trailer is only visible when it is
        // reachable through trailer_slave links from the +0xC8 root; the
        // parked record keeps the pose, not the actor. ONE shape covers
        // every state - the canonical forward chain rooted at canon[0].
        // Forward links are the only direction the parked machine itself
        // ever claims, so our writes can never contradict a native writer,
        // and a chain that only points forward is acyclic by construction:
        // the fatal solver recursion needed a BACKWARD leaf (towed ->
        // canonically-earlier parked) to meet the machine's forward claim
        // and close a two-node loop
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
                    // "live" means the joint chain bottoms out at the truck -
                    // a trailer still bolted to a PARKED parent belongs to the
                    // parked set, and treating it as live makes the tail
                    // append write a backward claim into its own parent
                    bool live_to_truck = false;
                    for ( void* cur = t; cur != nullptr && joint( cur ) != nullptr; )
                    {
                        void* p = prism::offsets::field< void* >( cur, prism::offsets::trailer_parent );
                        if ( p == truck ) { live_to_truck = true; break; }
                        if ( p == nullptr || p == cur || !readable_ptr( p ) ) break;
                        cur = p;
                    }
                    // every piece that is not live is a parked piece of ours
                    // and must stay reachable - a trailer parked by a plain
                    // T-detach carries no marks and only the head gets a
                    // parked record, so gating on marks/records drops the
                    // rest of the parked set off the chain and it vanishes
                    parkedish[ ln ] = !live_to_truck;
                    // a canon piece that stays jointless while not live to
                    // the truck IS a parked trailer - mark it so the record
                    // factory, parked pin and anchor lock keep it alive,
                    // registered and attachable. The 90-frame threshold
                    // keeps mid-attach pieces out (they joint within a few
                    // frames); the game-state check keeps world-load
                    // assembly out (a spawning trailer is jointless too)
                    if ( !live_to_truck && joint( t ) == nullptr )
                    {
                        if ( canon_jointless_frames[ i ] < 0xFFFF ) ++canon_jointless_frames[ i ];
                        if ( canon_jointless_frames[ i ] >= 90 &&
                             !is_detached( t ) && !is_plugin_detached( t ) &&
                             canon_age >= 90 && traffic_game_state != nullptr &&
                             *traffic_game_state == 2 )
                        {
                            mark_detached( t );
                            mark_plugin_detached( t );
                            detached_idx[ i ] = true;
                            CCore::g_instance->info( "maint: auto-marked settled parked trailer c{} {:p}", i, t );
                            // a trailer we find jointless was never parked
                            // by a detach flow - it spawned bare: no record,
                            // no legs, no obstacle. Run the real park
                            // sequence once so the game's own machinery
                            // holds it instead of our teleports
                            native_park_detached( t );
                        }
                    }
                    else
                    {
                        canon_jointless_frames[ i ] = 0;
                    }
                    // a trailer that really is coupled to the truck can not
                    // still carry detached marks - a native-path re-attach
                    // never clears them, and stale marks arm the cargo
                    // restore / sever_parked_claims on every actor teleport
                    // (the F9 floor dive: claims cut mid-reconcile made the
                    // game disconnect a member in mid-air)
                    if ( live_to_truck && !carry_hold[ i ] &&
                        ( is_detached( t ) || is_plugin_detached( t ) || detached_idx[ i ] ) )
                    {
                        unmark_detached( t );
                        unmark_plugin_detached( t );
                        detached_idx[ i ] = false;
                    }
                    ++ln;
                }
                // repair live links: a jointed member whose slave edge is
                // missing - severed mid-teleport or never written by a
                // plugin attach - is unreachable through the +0xC8 chain
                // and vanishes even though it sits bolted in the right
                // spot. parent.slave must point at the child physically
                // bolted to it; only null links and stale/parked claims are
                // taken over, never a link feeding another jointed member
                // hands off during an actor-teleport reconcile: joints read
                // null mid-remount and writing links then corrupts the game
                // chain (the F9 dive)
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
            // stale lines on jointless pieces - kill them through the
            // NATIVE line-state setter (0x140874010): fn(vehicle, state,
            // force). state=0 tears down every line entity in the trailer's
            // +0xE98..+0xEA0 list via the real destroy vfunc and clears the
            // +0xE8D "lines connected" flag - the parked piece kept both
            // from its last coupling and no field write kills the recreated
            // hose, only this transition does. Gate the call on actual
            // stale state so it is a no-op once clean - and only touch
            // pieces WE parked: a trailer mid-spawn during world load is
            // jointless with half-built line entities and tearing them
            // down then crashes the build
            // safety net: a piece that is not coupled to anything must never
            // show hoses - re-run the once-per-mark teardown about once a
            // second, so whatever re-raised the entities (respawn, native
            // cascade, the game's own line pass) is undone
            static uint32_t torn_tick = 0;
            if ( ++torn_tick >= 60 )
            {
                torn_tick = 0;
                for ( auto& torn : lines_torn ) torn = false;
            }
            for ( uint32_t i = 0; i < ln && tp_cooldown <= 0; ++i )
            {
                if ( !jl[ i ] ) continue;
                // a parked SECTION HEAD still tows a jointed slave - its line
                // list legitimately holds the live edge's hoses (the front
                // vehicle owns them), and zeroing them every frame is what
                // ate the T1->T2 cables
                void* s = readable_ptr( static_cast< uint8_t* >( live[ i ] ) + prism::offsets::trailer_slave )
                    ? prism::offsets::field< void* >( live[ i ], prism::offsets::trailer_slave ) : nullptr;
                const bool tows_section = s != nullptr && s != live[ i ] && is_trailer_obj( s ) &&
                    readable_ptr( static_cast< uint8_t* >( s ) + prism::offsets::trailer_physics_joint ) &&
                    joint( s ) != nullptr;
                if ( tows_section ) continue;
                // the game's line broadcaster re-flags +0xE8D on the whole
                // +0xC8-reachable set every frame while mode>0 - a jointless
                // piece never carries "lines connected", and the slave-edge
                // hose draw reads the SLAVE's flag: pinned off on every
                // jointless canon piece, marked or not - no flag, no hose
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
                // teardown uma vez por marca, não por frame: state=0 só
                // ESCONDE as entidades de linha, não as remove do vetor -
                // um gate "lista não-vazia" nunca se fecha e chamaria a
                // função nativa a cada frame. E a faixa +0xE98..+0xEA0 tem
                // que parecer um vetor sano: em algumas classes de carreta
                // esses campos guardam lixo que leria como bilhões de
                // entidades
                const int ci = canon_index_of( live[ i ] );
                if ( ci >= 0 && lines_torn[ ci ] ) continue;
                char* const tl = static_cast< char* >( live[ i ] );
                void* const lbeg = *reinterpret_cast< void** >( tl + 0xE98 );
                // +0xE98 = array pointer, +0xEA0 = ENTRY COUNT (not an end
                // pointer - reading it as one made nent garbage and the
                // teardown below never ran)
                const ptrdiff_t nent = static_cast< ptrdiff_t >( *reinterpret_cast< uint64_t* >( tl + 0xEA0 ) );
                if ( nent > 0 && nent <= 0x1000 && readable_ptr( lbeg ) )
                {
                    set_lines_fn( live[ i ], 0, 1 );
                    if ( ci >= 0 ) lines_torn[ ci ] = true;
                }
            }
            // an uncoupled slave edge must carry no cable: a coupled piece
            // whose slave claim points at a jointless canon piece gets the
            // connector pair + its outgoing lines broken, about once a second
            // (the respawn / native passes rebuild them after the first break)
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
                // only hide the loose piece's own lines - rebuilding/pairing
                // on the live towed chain every second corrupted the physics
                // (rig thrown into limbo: a legit visibility claim c1->c0
                // got torn down 30x/s)
                if ( set_lines_fn != nullptr ) set_lines_fn( sl, 0, 1 );
            }
            bool any_parked = false;
            for ( uint32_t i = 0; i < ln; ++i ) any_parked |= parkedish[ i ];
            // the +0xC8/slave pin is what keeps parked trailers VISIBLE -
            // it must run whenever the canon objects are valid, not only in
            // the narrow "ready" window: every frame a parked piece spends
            // unreachable is a frame the sweep can hide it. The real load-
            // time hazard is half-built objects, which is_trailer_obj covers
            if ( any_parked && canon_age >= 15 && canon_all_trailer_objs() &&
                 !native_chain_call_active.load( std::memory_order_acquire ) )
            {
                void* truck = nullptr;
                void* towed = nullptr;
                if ( auto* game_actor = CCore::g_instance->get_game_actor() )
                {
                    truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
                    towed = truck != nullptr && readable_ptr( truck )
                        ? prism::offsets::field< void* >( truck, 0x15A8 ) : nullptr;
                }
                const bool towing = towed != nullptr && joint( towed ) != nullptr;
                // hook-break teardown: a canon piece whose joint just died
                // must lose every hose connection - the async crank sequence
                // drops the joint seconds after dispatch, so the teardown
                // has to ride the transition itself, not the dispatch tail
                for ( uint32_t i = 0; i < ln; ++i )
                {
                    const bool jointed = joint( live[ i ] ) != nullptr;
                    if ( canon_was_jointed[ i ] && !jointed )
                    {
                        CCore::g_instance->info( "hook break on c{} - hose teardown", i );
                        // the detached piece's lane-obstacle owner node dies
                        // around now - dense-prune the containers for a few
                        // seconds so no worker scan/erase meets the orphan
                        lane_prune_frames = 240;
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
                    canon_was_jointed[ i ] = jointed;
                    // engatou de novo - um desengate futuro precisa de um
                    // teardown novo de mangueiras
                    if ( jointed ) lines_torn[ i ] = false;
                }
                // a escrita dos links ficou em rebuild_parked_claim_links -
                // a mesma função roda no fim do detach pra não deixar a
                // seção solta inalcançável nem um frame (a piscada)
                rebuild_parked_claim_links();
                // cycle guard - whatever wrote the links, a slave chain
                // that revisits a piece makes a concurrent native walker
                // spin forever; sever the link that closes the loop
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
                // never-hide pass: the set must stay whole - walk +0xC8 ->
                // slave and append every canon piece the chain does not
                // reach. Any trailer outside the list is what the sweep
                // hides; reachability through the links is the show flag
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
                        // chain head is missing or not a trailer - root it on
                        // the first canon piece so the set stays enumerable
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
                // keep parked pieces in their true detached state and the
                // parked registration asserted - the parked machine tears
                // it down whenever a section member departs
                for ( uint32_t i = 0; i < ln; ++i )
                {
                    if ( !parkedish[ i ] ) continue;
                    void* t = live[ i ];
                    // only re-assert parked state on pieces WE detached -
                    // the game owns the flags on anything else, and arming
                    // +0x1258 on a trailer whose record holds a stale or
                    // staging pose drags it through the world (the parked
                    // machine re-asserts the stored pose every frame)
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
                        // the native brace call lowers the legs and asks the
                        // parked machine to register the piece - but running
                        // it EVERY frame re-arms the gear animation and
                        // re-notifies the machine each pass (the visible
                        // tremble). Once every ~2 s keeps the request alive
                        // without churning the trailer's physical state
                        static uint32_t brace_retry[ max_trailers ] = {};
                        const int ci = canon_index_of( t );
                        if ( ci >= 0 && brace_retry[ ci ]++ > 120 )
                        {
                            brace_retry[ ci ] = 0;
                            vcall< prism::physics_trailer_u_set_trailer_brace_fn >( t, 0x328 / 8, true );
                        }
                    }
                }
            }
        }

        // cache each trailer's actor while it is still registered: the slots
        // only hold convoy members, so the pointer must be captured before a
        // detach or the parked trailer's traffic anchor can never be updated
        if ( auto* game_actor = CCore::g_instance->get_game_actor() )
        {
            for ( uint32_t i = 0; i < trailer_count; ++i )
            {
                cache_trailer_actor( trailers[ i ], game_actor );
            }
        }
    }

    // the chase camera object lives in the camera-manager slot array; find it
    // by its vehicle_behind_rotation_camera vtable (exe+0x23126F0)

    // per-frame: while a trailer is detached the chase camera anchors on the
    // HEAD OF THE LIVE CHAIN - the trailer physically on the fifth wheel
    // (truck+0x15A8). Detaching a TAIL trailer does not change the head:
    // trailer 0 stays the anchor and the camera keeps measuring the live
    // combination through hk_combo_length (which stops at the first dead
    // joint). Only when the truck tows NOTHING does the anchor go null -
    // the native solo state that follows the truck. The store is a raw
    // write with NO refcounting: the native resolver writes this field
    // directly too, and releasing the previous value is what underflowed
    // refcounts and froze the camera after recoupling

    // trailer hose/cable visuals + the detach/restore traffic suppression.
    // g_trailer_cables_mode: 0 nothing coupled, 1 first trailer (or partial
    // set) coupled, 2 whole original set coupled - written only on change, so
    // the console stays free for the user. A trailer counts only while
    // actually coupled to the truck's convoy - trailer 1 through the fifth
    // wheel, the rest through the chain or straight on the truck.
    void CTrailerManipulation::update_convoy_state()
    {
        // not while the world is still being assembled: trailers exist
        // before their joints/models do, and this function calls native
        // vfuncs (line rebuilds, gear enforcement, parked registration)
        // that crash on half-built objects - the field writes are also
        // pointless before physics starts ticking. canon_age only counts
        // frames with a populated canon, so it restarts whenever the world
        // reload kills the tracked pointers
        if ( !trailer_maintenance_ready() ) return;
        bool in_convoy[ max_trailers ] = {};
        bool parent_in_convoy = true;
        for ( uint32_t i = 0; i < trailer_count; ++i )
        {
            const bool attached = joint( trailers[ i ] ) != nullptr;
            in_convoy[ i ] = trailers[ i ] == truck_towed ? attached : attached && parent_in_convoy;
            parent_in_convoy = in_convoy[ i ];
        }
        // the truck broadcasts its brake channels down the towed chain only
        // while the coupling state machine sits at 5 (0x14064B760), and it
        // only reaches the trailer registered at truck+0x15A8 - a plugin
        // attach can leave either short of that, which silently drops service
        // and park braking on the whole chain. Pin both while our trailer is
        // on the truck; natively coupled links run their own progression
        void* truck = nullptr;
        if ( auto* game_actor = CCore::g_instance->get_game_actor() )
        {
            truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
        }
        if ( truck_towed != nullptr && truck != nullptr && joint( truck_towed ) != nullptr )
        {
            if ( prism::offsets::field< void* >( truck, 0x15A8 ) != truck_towed &&
                set_towed_trailer_fn != nullptr )
            {
                set_towed_trailer_fn( truck, true, truck_towed );
            }
            prism::offsets::field< uint32_t >( truck, prism::offsets::truck_coupling_state ) = 5;
        }

        // a T-detach that bypassed the dispatcher can clear the towed slot
        // while leaving the fifth-wheel joint alive - the truck stays
        // physically locked to a trailer the game thinks it no longer tows
        // (gear sound played, truck can't move). Release it through the
        // same path the plugin detach uses; trailer->trailer joints in a
        // detached section have another trailer as parent and stay
        if ( truck != nullptr && readable_ptr( truck ) &&
            prism::offsets::field< void* >( truck, 0x15A8 ) == nullptr )
        {
            for ( uint32_t i = 0; i < trailer_count; ++i )
            {
                void* t = trailers[ i ];
                if ( t == nullptr || !readable_ptr( t ) || joint( t ) == nullptr ) continue;
                if ( prism::offsets::field< void* >( t, prism::offsets::trailer_parent ) == truck )
                {
                    // a physically-jointed trailer with no detach marks is an
                    // attach the game forgot to register - re-pin the towed
                    // slot instead of releasing the joint. Releasing turned
                    // every "stuck" detection into a phantom head detach
                    if ( !is_detached( t ) && !is_plugin_detached( t ) &&
                         set_towed_trailer_fn != nullptr )
                    {
                        CCore::g_instance->info( "stuck tow joint on trailer {} - re-pinning towed slot", i );
                        set_towed_trailer_fn( truck, true, t );
                        // the full registration the normal pin writes - a
                        // half-registered tow leaves the detach dispatcher
                        // refusing the release (sound, no drop)
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

        // canais de freio do convoy + pin de spring-brake nas peças
        // desatreladas - em trailer_brake.cpp
        update_convoy_brakes( truck, in_convoy );

        // trava de estacionamento: parked-record nativo + âncora emulada das
        // peças desatreladas - implementada em trailer_loadstate.cpp
        update_parked_locks();

        // hose sanity: every attach/detach path rebuilds only the pair it
        // touched, but parked trailers keep live slave links and stray
        // native calls re-pair connectors through them - leaving phantom
        // hoses on dead edges or missing ones on live edges. Whenever the
        // coupling signature changes, rebuild EVERY edge to match reality
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
            ? prism::offsets::field< void* >( truck, 0x15A8 ) : nullptr;
        lines_sig |= ( towed_sig != nullptr && readable_ptr( towed_sig ) &&
            readable_ptr( static_cast< uint8_t* >( towed_sig ) + prism::offsets::trailer_physics_joint ) &&
            joint( towed_sig ) != nullptr ? 1ull : 0 ) << 40;
        static uint64_t last_lines_sig = ~0ull;
        if ( lines_sig != last_lines_sig )
        {
            last_lines_sig = lines_sig;
            refresh_all_lines();
        }

        // mode = cables on how much of the LIVE convoy: 1 draws them only
        // on the first trailer, 2+ on the whole coupled set. Counting live
        // members - not "is the last chain trailer coupled" - keeps the
        // front set's hoses when the tail detaches
        uint32_t convoy_n = 0;
        for ( uint32_t i = 0; i < trailer_count; ++i )
        {
            if ( in_convoy[ i ] ) ++convoy_n;
        }
        const uint32_t mode = convoy_n >= 2 ? 2 : convoy_n;
        static uint32_t sent_cables_mode = UINT32_MAX;
        if ( mode != sent_cables_mode && execute_line_fn != nullptr &&
            !pending_cmd_queued.load( std::memory_order_acquire ) )
        {
            sent_cables_mode = mode;
            snprintf( pending_cmd, sizeof( pending_cmd ), "g_trailer_cables_mode %u", mode );
            pending_cmd_queued.store( true, std::memory_order_release );
            CCore::g_instance->info( "g_trailer_cables_mode = {}", mode );
        }

        // desestaciona as carretas que voltaram pro convoy - em
        // trailer_loadstate.cpp (o record +0x1188 fica linkado: o registro
        // de obstáculo de tráfego o referencia)
        unpark_jointed( in_convoy );

        update_landing_gear();

        // chase camera: while any detached trailer exists the orbit anchor
        // (camera+0x3F0) is forced onto the truck - the native resolver would
        // still pick the parked trailer through the live slave link
        enforce_chase_camera_anchor();

        // traffic was zeroed for a detach: bring the saved level back only once
        // the WHOLE original set is coupled again AND a detach has genuinely
        // completed - during the post-command delay every joint is still alive,
        // so the set reads as coupled while traffic would bounce straight back
        // before the joint drops. A pending console line defers the write by a
        // frame instead of being overwritten
        if ( saved_traffic >= 0.f && !pending_cmd_queued.load( std::memory_order_acquire ) )
        {
            bool all_coupled = expected_trailers > 0 && trailer_count >= expected_trailers &&
                traffic_detach_done.load( std::memory_order_acquire );
            for ( uint32_t i = 0; all_coupled && i < trailer_count; ++i ) all_coupled &= in_convoy[ i ];
            // a parked detached trailer that fell out of the chain must also
            // be back before traffic returns
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
            // until the whole set is back, traffic stays pinned at zero - even
            // if it was raised by hand through the console in between
            else if ( get_cvar_float_fn != nullptr && g_traffic_cvar != nullptr &&
                get_cvar_float_fn( g_traffic_cvar ) > 0.f )
            {
                queue_cmd( "g_traffic 0" );
            }
        }
    }

    // a detach mark only protects a trailer while it is jointless or sitting
    // on the truck; back on its chain parent the game may manage it again.
    // Entries no longer in the chain (respawned trailers) are dropped without
    // touching them
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

    // native rest screen: the game's own sleep action is Shift+Num0. Instead
    // of reverse-engineering the time-skip UI we inject that exact chord -
    // the dialog, the hours picker and the rest itself stay 100% native
    int sleep_release_countdown = 0;
    void trigger_native_sleep()
    {
        INPUT keys[ 2 ] = {};
        keys[ 0 ].type = INPUT_KEYBOARD;
        keys[ 0 ].ki.wVk = VK_LSHIFT;
        keys[ 1 ].type = INPUT_KEYBOARD;
        keys[ 1 ].ki.wVk = VK_NUMPAD0;
        SendInput( 2, keys, sizeof( INPUT ) );
        // release on a later frame - key-up in the same batch can be missed
        // by the game's edge detection
        sleep_release_countdown = 4;
        CCore::g_instance->info( "sleep: Shift+Num0 injected" );
    }
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

    // attach/detach hotkeys, ignored while the game is in the background or a
    // key is being rebound
    void CTrailerManipulation::update_hotkeys()
    {
        update_sleep_release();
        if ( !connect_valid_ || !game_has_focus() || settings::is_capturing() ) return;

        // one key toggles a trailer against its chain parent, the truck
        // variants connect that trailer straight to the fifth wheel. Trailer 0
        // (index 0) is left to the game's own T key
        bool ad2 = false, ad3 = false, tk2 = false, tk3 = false;
        key_down( settings::Action::ATTACH_DETACH_2, &ad2 );
        key_down( settings::Action::ATTACH_DETACH_3, &ad3 );
        key_down( settings::Action::ATTACH_TRUCK_2, &tk2 );
        key_down( settings::Action::ATTACH_TRUCK_3, &tk3 );
        if ( ad2 ) this->toggle_attach( 1 );
        if ( ad3 ) this->toggle_attach( 2 );
        // keys name the physical trailer (canon order), not the rotated
        // trailers[] slot - with trailer 1 on the wheel the list is [1,2,0]
        // and a raw index would select the wrong piece
        if ( tk2 ) this->connect_to_truck( canon[ 1 ] );
        if ( tk3 ) this->connect_to_truck( canon[ 2 ] );
        bool slp = false;
        key_down( settings::Action::SLEEP_PARK, &slp );
        if ( slp ) trigger_native_sleep();
    }

}
