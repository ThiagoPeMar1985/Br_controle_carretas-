// Cargo reset checkbox, teleport interception and parked-position restore.

#include "trailer_manipulation.hpp"
#include "trailer_shared.hpp"
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

namespace ts_extra_utilities
{
    //========================================================================
    // Log de formação em teleporte — a correção de membro empilhado em
    // hk_trailer_teleport e o assentamento de formação em hk_actor_teleport
    //========================================================================
    tp_slot_t tp_log[ 16 ] = {};
    int tp_n = 0;
    bool tp_active = false;
    float3_t tp_snap[ max_trailers ] = {};
    quat_t tp_snap_q[ max_trailers ] = {};
    bool tp_snap_ok[ max_trailers ] = {};
    // frames que a emulação de âncora estacionada fica desligada depois de um
    // teleport de ator — a sequência de re-joint da doca deixa as carretas
    // sem junta por alguns frames no meio do movimento e a âncora as
    // arrastaria de volta pro ponto pré-teleporte
    int tp_cooldown = 0;
    // um teleport de ATOR (doca/carga/balsa/garagem — o que move o conjunto
    // inteiro) está em voo agora — a recusa do hk_trailer_teleport só vale
    // dentro dele; teleports isolados (ex.: o snap de posicionamento do
    // load) precisam pousar ou a carreta fica na pose de staging
    bool in_actor_tp = false;

    //========================================================================
    // Estado da trava de estacionamento (parked lock)
    //
    // Uma carreta desatrelada só fica plantada enquanto o record de
    // parked-state (+0x1188) reafirma a pose todo frame. Carretas soltas no
    // meio da cadeia nunca ganham record (a fábrica dá purecall nelas — NÃO
    // chamar), então update_parked_locks emula: snapshot do transform quando
    // a junta cai e re-teleporte se derivar. Indexado pelo canon pra deslize
    // de slot nunca apontar a trava pro objeto errado.
    //========================================================================
    hook_xform_t anchor_xform[ max_trailers ]{};
    hook_xform_t anchor_home[ max_trailers ]{};
    bool anchor_valid[ max_trailers ] = {};
    bool anchor_settling[ max_trailers ] = {};
    int anchor_settle[ max_trailers ] = {};
    int anchor_age[ max_trailers ] = {};
    int anchor_delay[ max_trailers ] = {};
    bool record_tried[ max_trailers ] = {};
    // a fábrica nativa de parked-record (0x1407B69E0, assinatura verificada):
    // constrói o objeto de 0x198 bytes onde a trava de pose mora. O bloqueio
    // dela em peças de meio de cadeia era uma purecall de componente —
    // hk_purecall a engole. O global do manager sai do prólogo da própria
    // fábrica (mov rsi, [rip+...] -> 0x1436AF828); o array de slots dele é
    // +0x740 base + +0x748 count (verificado na tail call 0x140C377C0)
    parked_record_factory_fn* parked_record_factory = nullptr;
    void** parked_record_mgr = nullptr;

    //========================================================================
    // Snapshot do transform do conjunto
    //
    // O cinemático de carga rotaciona o conjunto inteiro — caminhão incluso
    // — pros slots da doca através do teleport de ator (0x1405DE1E0). Esse
    // hook fotografa o caminhão e cada slot de carreta logo antes do
    // movimento; depois que a sequência assenta, restore_parked devolve o
    // conjunto do jeito que o jogador estacionou (de ré continua de ré).
    // Só movimentos curtos armam a restauração — um teleport através do mapa
    // é viagem, não alinhamento de doca
    //========================================================================
    bool rig_armed = false;
    uint32_t rig_restore_delay = 0;
    hook_xform_t rig_truck_xform{};
    bool rig_truck_valid = false;
    uint8_t rig_replay_a3 = 0;
    uint8_t rig_replay_a4 = 0;

    void render_cargo_restore_option()
    {
        ImGui::SeparatorText( "Carga separada" );
        bool cargo_restore = settings::g_settings.cargo_restore;
        if ( ui::toggle_switch( "##cargo_restore", &cargo_restore ) )
        {
            settings::g_settings.cargo_restore = cargo_restore;
            settings::save();
        }
        ImGui::SameLine();
        ImGui::TextWrapped( "Carreta fica parada na doca" );
        ImGui::TextDisabled( "ao carregar/descarregar separado, ela continua separada depois" );
    }

    void restore_parked_if_enabled()
    {
        if ( settings::g_settings.cargo_restore ) restore_parked();
    }

    void snapshot_rig()
    {
        rig_truck_valid = false;
        if ( auto* game_actor = CCore::g_instance->get_game_actor() )
        {
            void* truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
            rig_truck_valid = truck != nullptr && hook_world( truck, rig_truck_xform );
        }
    }
    // puts back what a cargo load/unload respawn destroyed: every marked
    // chain slot whose fresh object came back jointed is disconnected again
    // and teleported to the transform it was parked at. Runs on the sim
    // thread (queued op), descending index order so a tail trailer parked
    // on its own leaves before the section it was taken with — each marked
    // slot is the head of what the player actually detached
    void restore_parked()
    {
        // put the rig itself back first — replay the game's own actor teleport
        // with the pre-load transform and the original flags. The truck goes
        // through its proper path (vfunc +0x2A8), attached trailers follow the
        // actor-relative formation, and plugin-detached trailers are skipped
        // by our own per-trailer teleport hook. Then the detach pass below
        // re-parks the respawned sections
        if ( rig_armed )
        {
            rig_armed = false;
            // replaying the rig teleport with the snapshot transform yanks a
            // settled jointed combination across the map mid-solve — only do
            // it while a detached trailer still needs re-parking; if its
            // marks were cleared since the arm there is nothing to restore
            bool any_detached = false;
            for ( int i = 0; i < max_trailers; ++i )
            {
                if ( detached_idx[ i ] ) { any_detached = true; break; }
            }
            if ( any_detached && rig_truck_valid && actor_teleport_hook != nullptr )
            {
                if ( auto* game_actor = CCore::g_instance->get_game_actor() )
                {
                    using fn_t = void __fastcall( void*, void*, uint8_t, uint8_t );
                    actor_teleport_hook->get_original< fn_t >()(
                        game_actor, &rig_truck_xform, rig_replay_a3, rig_replay_a4 );
                }
            }
        }

        for ( int i = static_cast< int >( max_trailers ) - 1; i >= 0; --i )
        {
            if ( !detached_idx[ i ] ) continue;
            // a marked slot whose previous slot is also marked belongs to the
            // same parked section — the section head's walk below covers it;
            // disconnecting it here would split the parked combination apart
            if ( i > 0 && detached_idx[ i - 1 ] && canon[ i - 1 ] != nullptr ) continue;
            void* t = canon[ i ];
            if ( t == nullptr || !readable_ptr( t ) || joint( t ) == nullptr ) continue;

            // the parent it was respawned under — canon order is preserved
            void* parent = nullptr;
            bool parent_is_truck = false;
            if ( auto* game_actor = CCore::g_instance->get_game_actor() )
            {
                parent = ( i == 0 || t == truck_towed )
                    ? prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck )
                    : canon[ i - 1 ];
                parent_is_truck = ( i == 0 || t == truck_towed );
            }
            CCore::g_instance->info( "Restoring detached trailer {} after cargo respawn", i );
            if ( parent != nullptr )
            {
                disconnect_now( t, parent, parent_is_truck );
            }

            // teleport the head and every member still jointed to it back to
            // its saved parked transform — straight through the original, so
            // the pointer marks guarding the hook don't block the restore
            for ( void* m = t; m != nullptr; )
            {
                const int mi = canon_index_of( m );
                if ( mi >= 0 && parked_xform_valid[ mi ] && trailer_teleport_hook != nullptr )
                {
                    using teleport_fn_t = void __fastcall( void* /*trailer*/, void* /*transform*/ );
                    trailer_teleport_hook->get_original< teleport_fn_t >()( m, &parked_xform[ mi ] );
                    anchor_valid[ mi ] = false;
                }
                void* next = prism::offsets::field< void* >( m, prism::offsets::trailer_slave );
                m = ( next != m && next != nullptr && joint( next ) != nullptr ) ? next : nullptr;
            }
        }
    }
    // the trailer-state record apply (0x1405E14D1 on game_actor+0x10): copies
    // a recorded transform/velocity into the head trailer's state record and
    // reconciles its joint through the attach/detach dispatchers — the path
    // the cargo sequence uses to teleport the set into dock slots. A
    // plugin-detached trailer keeps its current record untouched: it stays
    // parked where the player left it
    using trailer_state_apply_fn = void __fastcall( void* /*record*/ );
    void __fastcall hk_trailer_state_apply( void* record )
    {
        void* trailer = record != nullptr
            ? prism::offsets::field< void* >( record, 0xB8 ) : nullptr;
        if ( trailer != nullptr && is_plugin_detached( trailer ) )
        {
            CCore::g_instance->info( "trailer state apply skipped: {:p} is plugin-detached", trailer );
            return;
        }
        // mid-teleport reconcile: the parked-machine's pose re-assert must
        // not fight the game's own member placement
        if ( tp_cooldown > 0 ) return;
        trailer_state_apply_hook->get_original< trailer_state_apply_fn >()( record );
    }
    // the per-trailer teleport (0x140786320): the actor-teleport walks the
    // trailer list and calls this on each one to move it into its dock slot.
    // Trailer objects are NOT recreated — this is the call that actually
    // drags a detached trailer to the dock during cargo load/unload, where
    // it lands under its parent's hook and gets re-jointed. A
    // plugin-detached trailer is a world object: it is never teleported,
    // by any caller — dock alignment, garage, ferry, job start
    using trailer_teleport_fn = void __fastcall( void* /*trailer*/, void* /*transform*/ );
    void __fastcall hk_trailer_teleport( void* self, void* transform )
    {
        // the refusal only covers a JOINTLESS parked piece being dragged by
        // an ACTOR teleport (dock/cargo/ferry — the moves that relocate the
        // rig). Isolated teleports MUST land: the load-time placement snap
        // puts each parked trailer at its saved ground pose, and refusing
        // that leaves it at the staging pose — it drops nose-first into the
        // ground and the parked lock then freezes the sunken pose
        if ( self != nullptr && is_plugin_detached( self ) &&
             joint( self ) == nullptr && in_actor_tp )
        {
            CCore::g_instance->info( "trailer teleport skipped: {:p} is plugin-detached", self );
            return;
        }
        // um teleport isolado pousou numa peça nossa estacionada — a âncora
        // recaptura a pose nova em vez de brigar com ela
        if ( self != nullptr && is_plugin_detached( self ) &&
             joint( self ) == nullptr )
        {
            const int ci = canon_index_of( self );
            if ( ci >= 0 && anchor_valid[ ci ] )
            {
                anchor_valid[ ci ] = false;
                anchor_settling[ ci ] = false;
                anchor_delay[ ci ] = 0;
            }
        }
        // a trailer riding along with the rig must not keep an armed parked
        // record — the parked machine would yank it back to the stored pose
        // every frame while physics holds it at the new spot (that pose war
        // is what drags the truck through the ground on F9)
        if ( self != nullptr && joint( self ) != nullptr )
        {
            prism::offsets::field< uint8_t >( self, prism::offsets::trailer_parked ) = 0;
            prism::offsets::field< uint8_t >( self, prism::offsets::trailer_parked_registered ) = 0;
        }
        // untouched during world assembly — the slot-correction math reads
        // snapshots taken at staging poses and would seat members in the
        // void
        if ( canon_age < 90 )
        {
            trailer_teleport_hook->get_original< trailer_teleport_fn >()( self, transform );
            return;
        }
        const float* dst = static_cast< const float* >( transform );
        // a jointed trailer can never occupy its parent's slot — same dst
        // means the game lost its member offset (missing state record).
        // Never write the game's transform buffer — the reconcile reads it
        // back to compute the next member's slot. Run the original first,
        // then re-teleport through our OWN buffer at the parent's final
        // slot plus the trailer's snapshot offset — the whole formation
        // lands translated, exactly as it was assembled
        float3_t corrected{};
        bool do_fix = false;
        void* parent = self != nullptr
            ? prism::offsets::field< void* >( self, prism::offsets::trailer_parent ) : nullptr;
        if ( dst != nullptr && self != nullptr && joint( self ) != nullptr && tp_active )
        {
            const int ti = canon_index_of( self );
            const int pi = canon_index_of( parent );
            // latest recorded slot for the parent (search back — the
            // reconcile calls each member more than once)
            for ( int i = tp_n; i-- > 0; )
            {
                if ( tp_log[ i ].t != parent ) continue;
                const float ddx = dst[ 0 ] - tp_log[ i ].orig.x;
                const float ddz = dst[ 2 ] - tp_log[ i ].orig.z;
                // a later reconcile pass can land the parent on its real
                // slot while a member still arrives at the shared anchor —
                // matching the first logged destination catches that too
                const float hdx = dst[ 0 ] - tp_log[ 0 ].orig.x;
                const float hdz = dst[ 2 ] - tp_log[ 0 ].orig.z;
                if ( ( ddx * ddx + ddz * ddz < 1.f || hdx * hdx + hdz * hdz < 1.f ) &&
                     ti >= 0 && pi >= 0 && tp_snap_ok[ ti ] && tp_snap_ok[ pi ] )
                {
                    // Y comes from the game's own destination, never the
                    // origin formation delta — preserving the old snapshot's
                    // height difference bakes the ORIGIN terrain slope into
                    // the landing
                    corrected = {
                        tp_log[ i ].dst.x + ( tp_snap[ ti ].x - tp_snap[ pi ].x ),
                        dst[ 1 ],
                        tp_log[ i ].dst.z + ( tp_snap[ ti ].z - tp_snap[ pi ].z ) };
                    do_fix = true;
                }
                break;
            }
        }
        if ( dst != nullptr && tp_active && tp_n < 16 )
        {
            tp_log[ tp_n ].t = self;
            tp_log[ tp_n ].orig = { dst[ 0 ], dst[ 1 ], dst[ 2 ] };
            tp_log[ tp_n ].dst = do_fix ? corrected : tp_log[ tp_n ].orig;
            ++tp_n;
        }
        // substitute OUR buffer for the game's transform — the trailer goes
        // straight to its real slot and never spends a frame overlapping
        // the parent at the anchor
        if ( do_fix )
        {
            hook_xform_t fix = *static_cast< const hook_xform_t* >( transform );
            fix.pos = corrected;
            trailer_teleport_hook->get_original< trailer_teleport_fn >()( self, &fix );
            CCore::g_instance->info( "trailer teleport {:p}: re-seated at corrected slot "
                "({:.1f},{:.1f},{:.1f})", self, corrected.x, corrected.y, corrected.z );
            return;
        }
        trailer_teleport_hook->get_original< trailer_teleport_fn >()( self, transform );
    }
    // the actor teleport (0x1405DE1E0): rotates/moves truck + attached set
    // into the dock slots for the cargo cinematic — the same call travel
    // teleports use. Snapshot the rig on the first call of a sequence, then
    // arm the restore only when the move was short (a dock shuffle, not a
    // ferry/garage/job hop across the map)
    using actor_teleport_fn = void __fastcall( void* /*actor*/, void* /*arg*/, uint8_t, uint8_t );
    // final formation pass after an actor teleport — the reconcile seats
    // every member at the shared anchor and can move the head AGAIN
    // mid-sequence, stranding members at offsets measured from a slot the
    // head no longer occupies (the F9 floor dive: stacked members + live
    // joints launch the rig, truck through the ground). Re-seat each
    // jointed member at its parent's FINAL live position + the
    // pre-teleport formation offset; Y tracks the parent's live height
    void settle_all_members()
    {
        for ( int i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
        {
            void* t = canon[ i ];
            // only members that rode along with the rig — a trailer jointed
            // to a PARKED parent never got teleported and must keep its
            // parked flags
            if ( !readable_ptr( t ) || joint( t ) == nullptr || !trailer_is_live( t ) ) continue;
            // a teleported member must not keep an armed parked record —
            // the re-assert would yank it back to the pre-teleport pose
            // every frame while physics holds it at the new spot
            prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked ) = 0;
            prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked_registered ) = 0;
            void* parent = prism::offsets::field< void* >( t, prism::offsets::trailer_parent );
            const int pi = canon_index_of( parent );
            if ( pi < 0 || !tp_snap_ok[ i ] || !tp_snap_ok[ pi ] ) continue;
            hook_xform_t pt{}, tt{};
            if ( !hook_world( parent, pt ) || !hook_world( t, tt ) ) continue;
            const float3_t world_delta{
                tp_snap[ i ].x - tp_snap[ pi ].x,
                tp_snap[ i ].y - tp_snap[ pi ].y,
                tp_snap[ i ].z - tp_snap[ pi ].z };
            const quat_t inverse_parent{
                tp_snap_q[ pi ].w, -tp_snap_q[ pi ].x,
                -tp_snap_q[ pi ].y, -tp_snap_q[ pi ].z };
            const float3_t local_delta = quat_rot( inverse_parent, world_delta );
            const float3_t rotated_delta = quat_rot( pt.quat, local_delta );
            const float wx = pt.pos.x + rotated_delta.x;
            const float wz = pt.pos.z + rotated_delta.z;
            const float ex = tt.pos.x - wx;
            const float ez = tt.pos.z - wz;
            if ( ex * ex + ez * ez < 0.5f * 0.5f ) continue;
            hook_xform_t fix = tt;
            fix.pos.x = wx;
            fix.pos.y = pt.pos.y;
            fix.pos.z = wz;
            using teleport_fn_t = void __fastcall( void*, void* );
            trailer_teleport_hook->get_original< teleport_fn_t >()( t, &fix );
            CCore::g_instance->info(
                "teleport settle: {:p} re-seated at formation slot ({:.1f}m off)",
                t, std::sqrt( ex * ex + ez * ez ) );
        }
    }
    void __fastcall hk_actor_teleport( void* self, void* arg, uint8_t a3, uint8_t a4 )
    {
        // pure passthrough while the world assembles — snapshot/settle math
        // on half-built or staging-position trailers re-seats them in the
        // void (dolly under the map)
        if ( canon_age < 90 )
        {
            actor_teleport_hook->get_original< actor_teleport_fn >()( self, arg, a3, a4 );
            return;
        }
        bool has_detached_slots = rig_armed;
        for ( bool detached : detached_idx ) has_detached_slots |= detached;

        // the reconcile seats every member at the shared anchor and can move
        // the head AGAIN mid-sequence — members then sit stacked or stranded
        // and the joint solver launches the rig (the F9 floor dive). The
        // formation pass below re-seats each member at the parent's FINAL
        // slot + the pre-teleport offset — it must run on EVERY actor
        // teleport, cargo_restore or not (the original did exactly that and
        // F9 worked)
        if ( !settings::g_settings.cargo_restore || !has_detached_slots )
        {
            if ( !settings::g_settings.cargo_restore ) rig_armed = false;
            if ( has_detached_slots ) sever_parked_claims();
            tp_n = 0;
            tp_active = true;
            for ( int i = 0; i < max_trailers; ++i )
            {
                tp_snap_ok[ i ] = false;
                if ( canon[ i ] == nullptr || !readable_ptr( canon[ i ] ) ) continue;
                hook_xform_t xf{};
                if ( hook_world( canon[ i ], xf ) ) { tp_snap[ i ] = xf.pos; tp_snap_q[ i ] = xf.quat; tp_snap_ok[ i ] = true; }
            }
            in_actor_tp = true;
            actor_teleport_hook->get_original< actor_teleport_fn >()( self, arg, a3, a4 );
            in_actor_tp = false;
            settle_all_members();
            restore_parked_claims();
            tp_active = false;
            tp_cooldown = 90;
            return;
        }

        tp_n = 0;
        // Do not patch the game's intermediate per-trailer destinations.
        // The final formation correction runs only after we confirm a cargo
        // respawn, not on a normal teleport.
        tp_active = false;
        // the reconcile seats members by walking the slave chain — a parked
        // visibility claim makes it count a detached trailer as a member:
        // it either gets dragged to the shared anchor on top of a real
        // trailer, or shifts a real member into its slot. Hide the claims
        // for the duration of the reconcile — the maintenance rebuilds
        // them right after
        sever_parked_claims();
        // snapshot every canon trailer's position — the stacked-member
        // correction needs the pre-teleport formation, and members get
        // re-teleported mid-sequence so a live read drifts between
        // reconcile passes
        for ( int i = 0; i < max_trailers; ++i )
        {
            tp_snap_ok[ i ] = false;
            if ( canon[ i ] == nullptr || !readable_ptr( canon[ i ] ) ) continue;
            hook_xform_t xf{};
            if ( hook_world( canon[ i ], xf ) )
            {
                tp_snap[ i ] = xf.pos;
                tp_snap_q[ i ] = xf.quat;
                tp_snap_ok[ i ] = true;
            }
        }

        const bool fresh = !rig_armed;
        if ( fresh )
        {
            snapshot_rig();
        }
        in_actor_tp = true;
        actor_teleport_hook->get_original< actor_teleport_fn >()( self, arg, a3, a4 );
        in_actor_tp = false;
        settle_all_members();
        restore_parked_claims();
        tp_active = false;
        tp_cooldown = 90;
        if ( !rig_truck_valid ) return;
        if ( auto* game_actor = CCore::g_instance->get_game_actor() )
        {
            void* truck = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_truck );
            hook_xform_t now{};
            if ( truck != nullptr && hook_world( truck, now ) )
            {
                const float dx = now.pos.x - rig_truck_xform.pos.x;
                const float dz = now.pos.z - rig_truck_xform.pos.z;
                // the restore exists to re-park plugin-detached trailers the
                // dock sequence respawned into the set. With nothing detached
                // the arm would only replay the rig teleport ~60 frames
                // later, yanking a settled jointed combination mid-solve —
                // camera-0/F9 launches were exactly this. No detached
                // trailer → nothing to restore → never arm
                bool any_detached = false;
                bool any_displaced = false;
                for ( int i = 0; i < max_trailers; ++i )
                {
                    if ( !detached_idx[ i ] ) continue;
                    any_detached = true;
                    // the dock sequence is the only caller that needs the
                    // replay — it respawns detached trailers INTO the set or
                    // scatters them off their parked marks. A travel/F9 hop
                    // leaves them parked exactly where they were
                    void* d = canon[ i ];
                    if ( d == nullptr || !readable_ptr( d ) ) continue;
                    if ( joint( d ) != nullptr ) { any_displaced = true; break; }
                    if ( parked_xform_valid[ i ] )
                    {
                        hook_xform_t dx2{};
                        if ( hook_world( d, dx2 ) )
                        {
                            const float mx = dx2.pos.x - parked_xform[ i ].pos.x;
                            const float mz = dx2.pos.z - parked_xform[ i ].pos.z;
                            if ( mx * mx + mz * mz > 2.f * 2.f ) { any_displaced = true; break; }
                        }
                    }
                }
                if ( any_detached && any_displaced && dx * dx + dz * dz < 200.f * 200.f )
                {
                    // This looks like the short cargo-dock respawn. Only
                    // now apply formation correction; ordinary travel/F9
                    // teleports remain entirely under the game's control.
                    tp_cooldown = 90;
                    if ( fresh )
                    {
                        rig_armed = true;
                        rig_restore_delay = 60;
                        rig_replay_a3 = a3;
                        rig_replay_a4 = a4;
                        CCore::g_instance->info( "actor teleported ~{:.1f}m — rig restore armed",
                            std::sqrt( dx * dx + dz * dz ) );
                    }
                }
            }
        }
    }

    // Watches detached slots after the cargo cinematic and restores their
    // saved transforms once the game finishes reattaching the respawned rig.
    void coupling_parked_update()
    {
        bool in_section = false;
        for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
        {
            void* t = canon[ i ];
            if ( !readable_ptr( t ) ) { detached_idx[ i ] = false; in_section = false; continue; }
            if ( detached_idx[ i ] )
            {
                if ( joint( t ) == nullptr )
                {
                    in_section = true;
                    rejoined_frames[ i ] = 0;
                    hook_xform_t xf{};
                    if ( hook_world( t, xf ) ) { parked_xform[ i ] = xf; parked_xform_valid[ i ] = true; }
                }
                else
                {
                    in_section = false;
                    if ( !settings::g_settings.cargo_restore )
                    {
                        rejoined_frames[ i ] = 0;
                    }
                    else if ( ++rejoined_frames[ i ] >= 30 && pending.kind == pending_kind::none )
                    {
                        if ( sim_is_quiet() )
                        {
                            CCore::g_instance->info( "restoring parked section early (sim suspended)" );
                            restore_parked();
                        }
                        else
                        {
                            queue_op( pending_kind::restore, nullptr, nullptr, false );
                        }
                    }
                }
            }
            else if ( in_section && joint( t ) != nullptr )
            {
                hook_xform_t xf{};
                if ( hook_world( t, xf ) ) { parked_xform[ i ] = xf; parked_xform_valid[ i ] = true; }
            }
            else
            {
                in_section = false;
            }
        }
        for ( uint32_t i = trailer_count; i < max_trailers; ++i )
        {
            detached_idx[ i ] = false;
            parked_xform_valid[ i ] = false;
        }

        if ( rig_armed )
        {
            if ( !settings::g_settings.cargo_restore )
            {
                rig_armed = false;
            }
            else if ( rig_restore_delay > 0 )
            {
                --rig_restore_delay;
            }
            else if ( pending.kind == pending_kind::none )
            {
                if ( sim_is_quiet() )
                {
                    CCore::g_instance->info( "restoring rig early (sim suspended)" );
                    restore_parked();
                }
                else
                {
                    queue_op( pending_kind::restore, nullptr, nullptr, false );
                }
            }
        }
    }

    //========================================================================
    // Ator/provider de obstáculo de tráfego das carretas
    //========================================================================

    // cache trailer -> trailer-actor: preenchido sempre que o scan de slots
    // funciona, pra continuar alcançando o actor depois que a carreta
    // desatrela e some dos slots do game_actor
    std::pair< void*, void* > trailer_actors[ max_trailers ] = {};

    // O detach nativo resolve o actor da carreta solta pelo array de slots em
    // game_actor+0x1D38 (entradas de 0x28, entry+0 = actor). O backpointer do
    // actor pra carreta fica num offset que essa build não documenta, então
    // em vez de chutar os campos do actor são escaneados por um ponteiro de
    // volta pra carreta — auto-descoberta.
    // A âncora de obstáculo de tráfego da carreta estacionada é um componente
    // dela mesma, não do slot array — a vtable do provider é amostrada do
    // actor da carreta cabeça (o único slot que o nativo preenche) e casada
    // contra os campos da própria carreta na hora do detach.
    void* provider_vtbl = nullptr;

    void* find_trailer_actor( void* trailer, void* game_actor )
    {
        if ( trailer == nullptr || game_actor == nullptr ) return nullptr;
        for ( uint32_t s = 0; s < 16; ++s )
        {
            void* entry = reinterpret_cast< uint8_t* >( game_actor ) + 0x1D38 + s * 0x28;
            if ( !readable_ptr( reinterpret_cast< uint8_t* >( entry ) + 8 ) ) break;
            void* actor = *reinterpret_cast< void** >( entry );
            if ( !plausible_object( actor ) ) continue;
            for ( uint32_t bo = 8; bo < 0x400; bo += 8 )
            {
                if ( prism::offsets::field< void* >( actor, bo ) == trailer )
                {
                    return actor;
                }
            }
        }
        // sem backpointer em lugar nenhum: a posição da carreta na cadeia
        // mapeia pro mesmo índice de slot — pega se o slot tiver actor real
        for ( uint32_t i = 0; i < trailer_count; ++i )
        {
            if ( trailers[ i ] != trailer ) continue;
            void* entry = reinterpret_cast< uint8_t* >( game_actor ) + 0x1D38 + i * 0x28;
            void* actor = *reinterpret_cast< void** >( entry );
            if ( plausible_object( actor ) )
            {
                return actor;
            }
            break;
        }
        // fallback pelo cache: quando a carreta sai do convoy o actor dela
        // some do slot array inteiro — uma carreta desatrelada só é achada
        // pelo ponteiro cacheado enquanto ela ainda estava engatada
        for ( uint32_t i = 0; i < max_trailers; ++i )
        {
            if ( trailer_actors[ i ].first == trailer )
            {
                void* cached = trailer_actors[ i ].second;
                return plausible_object( cached ) ? cached : nullptr;
            }
        }
        // último recurso com a vtable amostrada: acha o objeto em forma de
        // actor — qualquer coisa cujo campo +0x1F0 segura um provider do tipo
        // certo — dentro da própria carreta e pelo game_actor. Carretas de
        // meio de cadeia mantêm o actor fora do array de slots (só a cabeça)
        if ( provider_vtbl != nullptr )
        {
            auto actor_at = [ & ]( void* obj ) -> void*
            {
                if ( obj == nullptr || !plausible_object( obj ) ||
                    !readable_ptr( reinterpret_cast< uint8_t* >( obj ) + 0x1F0 ) )
                {
                    return nullptr;
                }
                void* prov = prism::offsets::field< void* >( obj, 0x1F0 );
                return prov != nullptr && plausible_object( prov ) &&
                    *reinterpret_cast< void** >( prov ) == provider_vtbl ? obj : nullptr;
            };
            for ( uintptr_t off = 0x100; off < 0x2000; off += 8 )
            {
                if ( !readable_ptr( reinterpret_cast< uint8_t* >( trailer ) + off ) ) continue;
                if ( void* a = actor_at( prism::offsets::field< void* >( trailer, off ) ) )
                {
                    return a;
                }
            }
            for ( uintptr_t off = 0x1C00; off < 0x3000; off += 8 )
            {
                if ( !readable_ptr( reinterpret_cast< uint8_t* >( game_actor ) + off ) ) continue;
                void* a = actor_at( prism::offsets::field< void* >( game_actor, off ) );
                if ( a == nullptr ) continue;
                return a;
            }
        }
        return nullptr;
    }

    void sample_provider_vtbl( void* game_actor )
    {
        if ( provider_vtbl != nullptr || game_actor == nullptr ) return;
        void* actor = prism::offsets::field< void* >( game_actor, 0x1D38 );
        if ( !plausible_object( actor ) ||
            !readable_ptr( reinterpret_cast< uint8_t* >( actor ) + 0x1F0 ) )
        {
            return;
        }
        void* provider = prism::offsets::field< void* >( actor, 0x1F0 );
        if ( provider != nullptr && plausible_object( provider ) &&
            plausible_vfunc( provider, 0x28 / 8 ) && plausible_vfunc( provider, 0x30 / 8 ) )
        {
            provider_vtbl = *reinterpret_cast< void** >( provider );
        }
    }

    // escaneia os campos da própria carreta (e do objeto de parked-state)
    // procurando um componente cuja vtable case com o provider amostrado —
    // a carreta estacionada carrega a âncora de obstáculo dentro dela
    void* find_trailer_provider( void* trailer )
    {
        if ( trailer == nullptr || provider_vtbl == nullptr ) return nullptr;
        auto scan = [ & ]( void* obj, uintptr_t end ) -> void*
        {
            if ( obj == nullptr ) return nullptr;
            for ( uintptr_t off = 0; off < end; off += 8 )
            {
                if ( !readable_ptr( reinterpret_cast< uint8_t* >( obj ) + off ) ) continue;
                void* cand = *reinterpret_cast< void** >( reinterpret_cast< uint8_t* >( obj ) + off );
                if ( cand != nullptr && plausible_object( cand ) &&
                    *reinterpret_cast< void** >( cand ) == provider_vtbl )
                {
                    return cand;
                }
            }
            return nullptr;
        };
        if ( void* p = scan( trailer, 0x1400 ) ) return p;
        return scan( prism::offsets::field< void* >( trailer, prism::offsets::trailer_parked_state ), 0x800 );
    }

    void cache_trailer_actor( void* trailer, void* game_actor )
    {
        if ( trailer == nullptr ) return;
        sample_provider_vtbl( game_actor );
        void* actor = find_trailer_actor( trailer, game_actor );
        for ( uint32_t i = 0; i < max_trailers; ++i )
        {
            if ( trailer_actors[ i ].first == trailer || trailer_actors[ i ].first == nullptr )
            {
                trailer_actors[ i ] = { trailer, actor };
                return;
            }
        }
    }

    //========================================================================
    // Registro de estacionamento (parked-state record) e obstáculo de lane
    //========================================================================

    // Filtro SEH que loga ONDE dentro da fábrica de records a falha aconteceu
    // — o catch genérico não distingue deref de componente morto de frame de
    // chamador velho; o RIP nomeia o local exato.
    int factory_fault_filter( uint32_t i, EXCEPTION_POINTERS* ep )
    {
        const auto rip = ep != nullptr && ep->ContextRecord != nullptr
            ? ep->ContextRecord->Rip : 0;
        const auto base = reinterpret_cast< uint64_t >( GetModuleHandle( nullptr ) );
        CCore::g_instance->warning(
            "parked record factory faulted for c{} at exe+0x{:X} code=0x{:X}",
            i, rip >= base ? rip - base : rip,
            ep != nullptr && ep->ExceptionRecord != nullptr
                ? ep->ExceptionRecord->ExceptionCode : 0 );
        return EXCEPTION_EXECUTE_HANDLER;
    }

    // O record da carreta está realmente dentro da scan-list do manager?
    // Um record linkado em +0x1188 mas ausente de mgr+0x740/+0x748 é INERTE —
    // o re-assert de pose só itera esse array, então uma carreta vinda de
    // save pode mostrar record + flag armada e mesmo assim cair fisicamente.
    bool parked_record_listed( void* t )
    {
        void* mgr = readable_ptr( parked_record_mgr ) ? *parked_record_mgr : nullptr;
        if ( mgr == nullptr ) return false;
        __try
        {
            void** it = readable_span( mgr, 0x750 ) >= 0x750
                ? prism::offsets::field< void** >( mgr, 0x740 ) : nullptr;
            const uint64_t n = it != nullptr
                ? prism::offsets::field< uint64_t >( mgr, 0x748 ) : 0;
            if ( it == nullptr || n == 0 || n >= 0x1000 ||
                 readable_span( it, n * sizeof( void* ) ) < n * sizeof( void* ) )
                return false;
            for ( uint64_t k = 0; k < n; ++k )
            {
                if ( it[ k ] != nullptr && readable_ptr( it[ k ] ) &&
                     prism::offsets::field< void* >( it[ k ], 0x178 ) == t )
                    return true;
            }
        }
        __except ( EXCEPTION_EXECUTE_HANDLER ) {}
        return false;
    }

    // Tira todo record desta carreta da scan-list do manager — enquanto
    // continuar listada, a máquina de estacionamento re-asserta a pose
    // guardada do record contra a carreta viva a cada frame.
    void parked_record_unlist( void* t )
    {
        __try
        {
            void* mgr = readable_ptr( parked_record_mgr ) ? *parked_record_mgr : nullptr;
            void** it = mgr != nullptr && readable_span( mgr, 0x750 ) >= 0x750
                ? prism::offsets::field< void** >( mgr, 0x740 ) : nullptr;
            auto* nrec = it != nullptr
                ? &prism::offsets::field< uint64_t >( mgr, 0x748 ) : nullptr;
            if ( nrec == nullptr || *nrec == 0 || *nrec >= 0x1000 ||
                 readable_span( it, *nrec * sizeof( void* ) ) < *nrec * sizeof( void* ) )
                return;
            for ( uint64_t k = 0; k < *nrec; ++k )
            {
                if ( it[ k ] != nullptr && readable_ptr( it[ k ] ) &&
                     prism::offsets::field< void* >( it[ k ], 0x178 ) == t )
                {
                    std::memmove( it + k, it + k + 1, ( *nrec - k - 1 ) * sizeof( void* ) );
                    --*nrec;
                    --k;
                }
            }
        }
        __except ( EXCEPTION_EXECUTE_HANDLER ) {}
    }

    // Todo container visto pelo hook de insert fica cacheado aqui e é podado —
    // a região de position-find (exe+0x94E3CB) também é alcançada por chamadas
    // FORA do insert (erase de obstáculo no un-park) e dereferencia +0x88 sem
    // checar (AV em exe+0x94E45C, 48ms depois de um detach liberar o node).
    // Órfãos aparecem em EVENTOS (a queda da junta libera o owner-node do
    // obstáculo), então a poda densa só roda na janela pós-detach + uma
    // cadência lenta — scan por frame custava 2 syscalls VirtualQuery por
    // elemento e crescia com as listas do tráfego.
    void* lane_containers[ 128 ] = {};
    uint32_t lane_prune_frames = 0;
    uint32_t lane_prune_idle = 0;

    // O elemento ainda tem owner-node vivo? SEH lê o campo direto —
    // VirtualQuery por elemento era o custo de syscall que escalava com as
    // listas de obstáculo do mundo.
    bool lane_element_dead( void* el )
    {
        if ( el == nullptr ) return true;
        void* owner = nullptr;
        __try
        {
            owner = *reinterpret_cast< void** >( static_cast< uint8_t* >( el ) + 0x88 );
        }
        __except ( EXCEPTION_EXECUTE_HANDLER )
        {
            return true;
        }
        return owner == nullptr;
    }

    // Remove entradas órfãs de todos os containers de lane-obstacle vistos —
    // uma entrada cujo owner-node foi liberado sem o slot sair do array quebra
    // o scan inteiro na worker thread do tráfego.
    void prune_lane_containers()
    {
        for ( auto& c : lane_containers )
        {
            if ( c == nullptr ) continue;
            if ( !readable_ptr( c ) ||
                 !readable_ptr( static_cast< uint8_t* >( c ) + 0x10 ) )
            {
                c = nullptr;
                continue;
            }
            __try
            {
                void** elements = *reinterpret_cast< void*** >( static_cast< uint8_t* >( c ) + 8 );
                auto* count = reinterpret_cast< uint64_t* >( static_cast< uint8_t* >( c ) + 0x10 );
                if ( elements == nullptr || !readable_ptr( elements ) || *count > 0x10000 )
                {
                    c = nullptr;
                    continue;
                }
                uint64_t n = *count;
                for ( uint64_t i = n; i-- > 0; )
                {
                    void* el = elements[ i ];
                    if ( lane_element_dead( el ) )
                    {
                        CCore::g_instance->info( "lane obstacle: pruned dead entry {:p}", el );
                        memmove( &elements[ i ], &elements[ i + 1 ], ( n - i - 1 ) * sizeof( void* ) );
                        --n;
                    }
                }
                if ( n != *count )
                {
                    // worker no meio do scan com count velho dereferencia
                    // elements[n] — deixa a última entrada VIVA no lugar da
                    // morta que o memmove expôs
                    if ( n > 0 ) elements[ n ] = elements[ n - 1 ];
                    *count = n;
                }
            }
            __except ( EXCEPTION_EXECUTE_HANDLER )
            {
                // container liberado ou realocado no meio da poda — descarta
                c = nullptr;
            }
        }
    }

    // Hook do insert ordenado de lane-obstacle (0x14094DFC0, roda nas worker
    // threads do tráfego): o scan dereferencia o +0x88 de cada elemento — o
    // owner-node dentro do link inline do obstáculo — então uma entrada morta
    // quebra o job inteiro. É exatamente o estado em que um obstáculo de
    // carreta estacionada órfão pode ficar. Sanitiza o container alvo antes
    // do original rodar: elementos com owner-node nulo (ou memória morta)
    // saem, e o original só vê entradas vivas.
    using lane_obstacle_insert_fn = uint8_t __fastcall( void* /*node*/, void* /*link*/, float /*pos*/ );
    uint8_t __fastcall hk_lane_obstacle_insert( void* node, void* link, float pos )
    {
        if ( node != nullptr && link != nullptr && plausible_vfunc( link, 1 ) )
        {
            using type_fn = uint32_t __fastcall( void* );
            const auto get_type = reinterpret_cast< type_fn* >( ( *reinterpret_cast< void** const* >( link ) )[ 1 ] );
            // tipo de link 0xFFF = a lista de lane-elements ordenada por
            // posição — o caminho cujo scan dereferencia +0x88 sem checar
            if ( get_type( link ) == 0xFFF && plausible_vfunc( node, 0xD0 / 8 ) )
            {
                using container_fn = void* __fastcall( void* );
                const auto get_container = reinterpret_cast< container_fn* >( ( *reinterpret_cast< void** const* >( node ) )[ 0xD0 / 8 ] );
                void* container = get_container( node );
                if ( readable_ptr( container ) && readable_ptr( static_cast< uint8_t* >( container ) + 0x18 ) )
                {
                    // lembra o container — a poda por frame mantém os caminhos
                    // de erase/scan fora deste insert sem crash também
                    for ( auto& c : lane_containers )
                    {
                        if ( c == container ) break;
                        if ( c == nullptr ) { c = container; break; }
                    }
                    void** elements = *reinterpret_cast< void*** >( static_cast< uint8_t* >( container ) + 8 );
                    auto* count = reinterpret_cast< uint64_t* >( static_cast< uint8_t* >( container ) + 0x10 );
                    if ( elements != nullptr && readable_ptr( elements ) && *count <= 0x10000 )
                    {
                        uint64_t n = *count;
                        for ( uint64_t i = n; i-- > 0; )
                        {
                            void* el = elements[ i ];
                            if ( lane_element_dead( el ) )
                            {
                                CCore::g_instance->info( "lane obstacle: pruned dead entry {:p}", el );
                                memmove( &elements[ i ], &elements[ i + 1 ], ( n - i - 1 ) * sizeof( void* ) );
                                --n;
                            }
                        }
                        *count = n;
                    }
                }
            }
        }
        return lane_obstacle_insert_hook->get_original< lane_obstacle_insert_fn >()( node, link, pos );
    }

    // Estaciona uma carreta que já está sem junta (save carregado solta ou
    // auto-mark): brace nativo (desce o pé e arma a trava de pose), freio de
    // mão aplicado, animação da manivela que CONSTRÓI o apoio físico do pé
    // (sem a anim o +0x126C marca baixado mas nenhum corpo de apoio existe —
    // a frente atravessa o chão) e o registro de tráfego — a mesma cauda que
    // o dispatcher de T executa, sem precisar de um pai.
    void native_park_detached( void* t )
    {
        if ( t == nullptr || !is_trailer_obj( t ) ) return;
        __try
        {
            if ( prism::offsets::field< void* >( t, prism::offsets::trailer_parked_state ) != nullptr )
            {
                prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked ) = 1;
                prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked_registered ) = 1;
            }
            if ( plausible_vfunc( t, 0x328 / 8 ) )
            {
                vcall< prism::physics_trailer_u_set_trailer_brace_fn >( t, 0x328 / 8, true );
            }
            prism::offsets::field< float >( t, prism::offsets::trailer_brake ) = 1.f;
            prism::offsets::field< float >( t, prism::offsets::trailer_service_brake ) = 0.f;
            prism::offsets::field< uint8_t >( t, prism::offsets::trailer_brake_forced ) = 1;
            if ( brace_anim_fn != nullptr )
            {
                prism::offsets::field< float >( t, 0x1270 ) = 1.f;
                brace_anim_fn( t, 1, 1.f / 3.f );
            }
            if ( notify_decoupled_fn != nullptr ) notify_decoupled_fn( t );
            traffic_detach_tail();
            lane_prune_frames = 240;
        }
        __except ( EXCEPTION_EXECUTE_HANDLER )
        {
            CCore::g_instance->warning( "native park sequence faulted on {:p}", t );
        }
    }

    //========================================================================
    // Proteção contra crashes: purecall fake + tracer de AV
    //========================================================================

    // CRT __purecall (0x141D23610): nós de listener mortos em carretas
    // desatreladas ainda recebem vfunc por slots puros pela máquina de
    // estacionamento — o handler original mata o processo. Retornar null é o
    // que o caminho de implementação-faltante do chamador tolera; a fábrica
    // de parked-record completa com um membro nulo em vez de morrer.
    // Tracer permanente: qualquer AV não-tratado no processo loga o RIP da
    // falha e os registradores — o abort silencioso do teardown e o crash da
    // fábrica eram invisíveis sem ele. First chance, pass-through
    // (CONTINUE_SEARCH) pra o crash handling do próprio jogo ainda rodar.
    LONG WINAPI crash_tracer( EXCEPTION_POINTERS* ep )
    {
        if ( ep == nullptr || ep->ExceptionRecord == nullptr ) return EXCEPTION_CONTINUE_SEARCH;
        const auto code = ep->ExceptionRecord->ExceptionCode;
        if ( code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW &&
             code != 0xE24C4A02 /* purecall */ )
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        const auto rip = ep->ContextRecord != nullptr ? ep->ContextRecord->Rip : 0;
        const auto base = reinterpret_cast< uint64_t >( GetModuleHandle( nullptr ) );
        CCore::g_instance->warning(
            "AV 0x{:X} at exe+0x{:X} rax={:p} rbx={:p} rcx={:p} rdx={:p} rsi={:p} rdi={:p} r8={:p} addr={:p}",
            code, rip >= base ? rip - base : rip,
            ep->ContextRecord ? reinterpret_cast< void* >( ep->ContextRecord->Rax ) : nullptr,
            ep->ContextRecord ? reinterpret_cast< void* >( ep->ContextRecord->Rbx ) : nullptr,
            ep->ContextRecord ? reinterpret_cast< void* >( ep->ContextRecord->Rcx ) : nullptr,
            ep->ContextRecord ? reinterpret_cast< void* >( ep->ContextRecord->Rdx ) : nullptr,
            ep->ContextRecord ? reinterpret_cast< void* >( ep->ContextRecord->Rsi ) : nullptr,
            ep->ContextRecord ? reinterpret_cast< void* >( ep->ContextRecord->Rdi ) : nullptr,
            ep->ContextRecord ? reinterpret_cast< void* >( ep->ContextRecord->R8 ) : nullptr,
            ep->ExceptionRecord->NumberParameters > 1
                ? reinterpret_cast< void* >( ep->ExceptionRecord->ExceptionInformation[ 1 ] ) : nullptr );
        return EXCEPTION_CONTINUE_SEARCH;
    }
    void install_crash_tracer()
    {
        static PVOID h = AddVectoredExceptionHandler( 1, crash_tracer );
        if ( h != nullptr )
        {
            CCore::g_instance->info( "crash tracer installed" );
        }
    }

    // O iterador de componentes em exe+0x927C60 dereferencia o retorno do
    // purecall sem checar (call [rax+0x40] depois mov rdx,[rax] + call
    // [rdx+8] — switch de tipo em 0x3000/0x7000/0xF000/0x17000). Retornar
    // null quebra ele; entrega um componente benigno cujas vfuncs retornam
    // null — eax=0 não casa nenhum case do switch, então o iterador pula o
    // componente morto e segue. Só o range dessa função ganha o fake: a
    // fábrica de record e o site de comparação 0x9277F4 dependem do null que
    // já tratam.
    void* __fastcall purecall_null_vfunc( void* /*self*/ ) { return nullptr; }
    void* purecall_fake_vtbl[ 16 ] = {};
    struct purecall_fake_t { void* vtbl; uint8_t pad[ 248 ]; };
    purecall_fake_t purecall_fake = { purecall_fake_vtbl };

    void init_purecall_fake()
    {
        for ( auto& slot : purecall_fake_vtbl )
        {
            slot = reinterpret_cast< void* >( &purecall_null_vfunc );
        }
    }

    void* __fastcall hk_purecall( void* /*object*/ )
    {
        const auto ret = reinterpret_cast< uint64_t >( _ReturnAddress() );
        const auto base = reinterpret_cast< uint64_t >( GetModuleHandle( nullptr ) );
        const auto rva = ret - base;
        static uint64_t seen[ 32 ] = {};
        for ( auto& s : seen )
        {
            if ( s == ret ) break;
            if ( s == 0 )
            {
                s = ret;
                CCore::g_instance->warning( "purecall swallowed at exe+0x{:X}", rva );
                break;
            }
        }
        if ( rva >= 0x927C60 && rva < 0x927CBF ) return &purecall_fake;
        return nullptr;
    }

    //========================================================================
    // Trava de estacionamento por frame (roda dentro de update_convoy_state)
    //========================================================================

    // A máquina de estacionamento esconde uma carreta que não alcança mais
    // pela cadeia viva — regra de visibilidade do próprio parked-record.
    // Uma carreta desatrelada pelo plugin tem que continuar no mundo, então
    // uma vez por detach re-assertamos as flags de parked + brace; peças sem
    // record recebem o transform re-escrito no lugar atual (o mesmo nudge
    // "estou aqui" que o parked-record faz).
    void unhide_detached()
    {
        // reconciliação de meio-teleporte: as juntas leem null por alguns
        // frames e um nudge de pose aqui briga com o posicionamento do jogo
        if ( tp_cooldown > 0 ) return;
        for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
        {
            void* t = canon[ i ];
            if ( !readable_ptr( t ) || joint( t ) != nullptr ) continue;
            if ( !is_detached( t ) && !is_plugin_detached( t ) ) continue;
            if ( prism::offsets::field< void* >( t, prism::offsets::trailer_parked_state ) != nullptr )
            {
                prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked ) = 1;
                prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked_registered ) = 1;
                prism::offsets::field< uint8_t >( t, prism::offsets::trailer_brace_state ) = 1;
            }
            else if ( trailer_teleport_hook != nullptr )
            {
                hook_xform_t xf{};
                if ( hook_world( t, xf ) )
                {
                    using teleport_fn_t = void __fastcall( void*, void* );
                    trailer_teleport_hook->get_original< teleport_fn_t >()( t, &xf );
                }
            }
        }
    }



    // Carreta sem junta e sem parked-record (+0x1188) não tem âncora: o record
    // é o que re-asserta a pose estacionada a cada frame — freio de roda
    // sozinho só desliza. A fábrica nativa roda uma vez por slot (a purecall
    // dela no meio da cadeia é engolida pelo hk_purecall); as peças pra quem
    // ela nunca produz record ganham a trava emulada: fotografa a pose uma
    // vez na queda da junta e teleporta de volta quando deriva.
    // Suspenso logo após um teleport de ator: a sequência de re-joint da doca
    // deixa as carretas sem junta no meio do movimento e a âncora brigaria
    // com o posicionamento do próprio jogo.
    void update_parked_locks()
    {
        if ( tp_cooldown > 0 ) --tp_cooldown;
        for ( uint32_t i = 0; i < max_trailers && canon[ i ] != nullptr; ++i )
        {
            if ( tp_cooldown > 0 )
            {
                anchor_valid[ i ] = false;
                anchor_settling[ i ] = false;
                anchor_delay[ i ] = 0;
                continue;
            }
            void* t = canon[ i ];
            // só as NOSSAS peças desatreladas ganham a trava emulada — uma
            // carreta sem junta e sem marca (respawn do jogo, estacionada por
            // um T-detach simples, ainda assentando sozinha) é problema do
            // jogo; pinar uma pose de meio-assentamento a congela no ar
            if ( !readable_ptr( t ) || joint( t ) != nullptr ||
                 ( !is_detached( t ) && !is_plugin_detached( t ) ) )
            {
                anchor_valid[ i ] = false;
                anchor_settling[ i ] = false;
                anchor_delay[ i ] = 0;
                continue;
            }
            // uma peça estacionada não pode estar no grafo físico de tração
            // (+0x60 sobe pro pai, +0x68 desce pro filho): um link stale —
            // criado antes da marca, ou por uma claim que o hk_phys_link
            // antigo deixava passar — faz o solver puxar ela todo frame
            // enquanto a âncora/record a segura = as tremidas. Corta dos
            // dois lados pra não deixar meio-link pendurado
            void* phys_up = prism::offsets::field< void* >( t, 0x60 );
            if ( phys_up != nullptr )
            {
                if ( readable_ptr( phys_up ) &&
                     prism::offsets::field< void* >( phys_up, 0x68 ) == t )
                    prism::offsets::field< void* >( phys_up, 0x68 ) = nullptr;
                prism::offsets::field< void* >( t, 0x60 ) = nullptr;
            }
            void* phys_down = prism::offsets::field< void* >( t, 0x68 );
            if ( phys_down != nullptr )
            {
                if ( readable_ptr( phys_down ) &&
                     prism::offsets::field< void* >( phys_down, 0x60 ) == t )
                    prism::offsets::field< void* >( phys_down, 0x60 ) = nullptr;
                prism::offsets::field< void* >( t, 0x68 ) = nullptr;
            }
            // roda a fábrica de record nativa uma vez — o bloqueador dela no
            // meio da cadeia era uma purecall de componente que o hk_purecall
            // engole — e linka o record registrado de volta em trailer+0x1188
            // com as flags que o dispatcher arma na mesma condição.
            // A fábrica dereferencia o singleton do MANAGER de records em
            // +0x558 antes de tudo — ele só existe depois que alguma carreta
            // foi estacionada pelo fluxo NATIVO (fault: mov r8,[rsi+0x558],
            // rsi=null, AV em exe+0x7B6A03). Pula enquanto não existe e
            // TENTA de novo todo frame: quando qualquer park nativo o criar,
            // toda carreta em limbo ganha o record na próxima passada.
            void* mgr = readable_ptr( parked_record_mgr ) ? *parked_record_mgr : nullptr;
            if ( prism::offsets::field< void* >( t, prism::offsets::trailer_parked_state ) == nullptr &&
                !record_tried[ i ] &&
                ( is_detached( t ) || is_plugin_detached( t ) ) &&
                parked_record_factory != nullptr && parked_record_mgr != nullptr &&
                mgr != nullptr )
            {
                record_tried[ i ] = true;
                __try
                {
                    parked_record_factory( t );
                    void** it = mgr != nullptr && readable_ptr(
                        reinterpret_cast< uint8_t* >( mgr ) + 0x748 )
                        ? prism::offsets::field< void** >( mgr, 0x740 ) : nullptr;
                    const uint64_t nrec = mgr != nullptr && readable_ptr(
                        reinterpret_cast< uint8_t* >( mgr ) + 0x748 )
                        ? prism::offsets::field< uint64_t >( mgr, 0x748 ) : 0;
                    void* rec = nullptr;
                    if ( it != nullptr && nrec > 0 && nrec < 0x1000 )
                    {
                        for ( uint64_t k = 0; k < nrec; ++k )
                        {
                            void* e = it[ k ];
                            if ( e != nullptr && readable_ptr( e ) &&
                                prism::offsets::field< void* >( e, 0x178 ) == t )
                            {
                                rec = e;
                                break;
                            }
                        }
                    }
                    if ( rec != nullptr )
                    {
                        prism::offsets::field< void* >( t, prism::offsets::trailer_parked_state ) = rec;
                        prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked ) = 1;
                        prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked_registered ) = 1;
                        CCore::g_instance->info( "parked record {:p} created for c{}", rec, i );
                    }
                    else
                    {
                        CCore::g_instance->warning( "parked record factory produced no record for c{}", i );
                    }
                }
                __except ( factory_fault_filter( i, GetExceptionInformation() ) )
                {
                }
            }
            // um ponteiro de record sozinho não trava a carreta: o re-assert
            // nativo de pose só roda enquanto o record está na scan-list do
            // manager E +0x1258 continua armado — uma carreta de save pode
            // carregar record+flag sem estar na lista, o que deixa o record
            // INERTE e a carreta caindo enquanto a âncora fica pulada à toa
            if ( prism::offsets::field< void* >( t, prism::offsets::trailer_parked_state ) != nullptr &&
                !record_tried[ i ] &&
                prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked_registered ) != 0 &&
                parked_record_listed( t ) )
            {
                anchor_valid[ i ] = false;
                anchor_settling[ i ] = false;
                anchor_delay[ i ] = 0;
                continue;
            }
            // carência sem junta — a âncora não pode capturar nos primeiros
            // frames depois da queda da junta: load/assembly segura as
            // carretas sem junta enquanto o jogo as reposiciona por poses de
            // preparo como (0,0,-512), e ancorar uma dessas teleporta a
            // carreta pro vazio no frame seguinte
            if ( anchor_delay[ i ] < 40 )
            {
                ++anchor_delay[ i ];
                continue;
            }
            if ( !anchor_valid[ i ] )
            {
                anchor_valid[ i ] = hook_world( t, anchor_xform[ i ] );
                anchor_home[ i ] = anchor_xform[ i ];
                anchor_settle[ i ] = 0;
                anchor_age[ i ] = 0;
                anchor_settling[ i ] = true;
                continue;
            }
            hook_xform_t now{};
            if ( !hook_world( t, now ) || trailer_teleport_hook == nullptr ) continue;
            ++anchor_age[ i ];
            const float mx = now.pos.x - anchor_xform[ i ].pos.x;
            const float my = now.pos.y - anchor_xform[ i ].pos.y;
            const float mz = now.pos.z - anchor_xform[ i ].pos.z;
            const quat_t& aq = anchor_xform[ i ].quat;
            const float qdot = aq.w * now.quat.w + aq.x * now.quat.x +
                               aq.y * now.quat.y + aq.z * now.quat.z;
            const float rotd = 1.f - std::fabs( qdot );   // 0 = mesma rotação
            const bool moving = mx * mx + my * my + mz * mz > 0.02f * 0.02f || rotd > 0.001f;
            if ( anchor_settle[ i ] < 30 && anchor_age[ i ] < 240 )
            {
                // fase de assentamento: uma carreta recém-desatrelada ainda
                // está tombando em cima do apoio — segue a pose em vez de
                // travar, senão a pose nivelada guardada briga com o
                // assentamento a cada frame
                anchor_settling[ i ] = true;
                const float hx = now.pos.x - anchor_home[ i ].pos.x;
                const float hy = now.pos.y - anchor_home[ i ].pos.y;
                const float hz = now.pos.z - anchor_home[ i ].pos.z;
                // andou demais rolando livre OU afundou: assentar no pé é
                // queda de centímetros — meio metro abaixo da casa é queda
                // livre por falta de apoio, e seguir a descida capturaria a
                // pose já afundada. Puxa de volta e deixa assentar de novo
                if ( ( hx * hx + hz * hz > 1.5f * 1.5f || hy < -0.5f ) &&
                     trailer_teleport_hook != nullptr )
                {
                    using teleport_fn_t = void __fastcall( void* /*trailer*/, void* /*transform*/ );
                    trailer_teleport_hook->get_original< teleport_fn_t >()( t, &anchor_home[ i ] );
                    anchor_xform[ i ] = anchor_home[ i ];
                    anchor_settle[ i ] = 0;
                    continue;
                }
                if ( moving )
                {
                    anchor_xform[ i ] = now;
                    anchor_settle[ i ] = 0;
                }
                else
                {
                    ++anchor_settle[ i ];
                }
                continue;
            }
            anchor_settling[ i ] = false;
            // nunca trava uma pose caída: se a pose guardada está visivelmente
            // inclinada, o park nativo ainda deve apoio real pra essa carreta
            // (o pé nunca foi acionado) — recaptura e continua seguindo em vez
            // de congelar ela com a frente afundada no chão
            {
                const quat_t& q = anchor_xform[ i ].quat;
                const float up_y = 1.f - 2.f * ( q.x * q.x + q.z * q.z );
                if ( up_y < 0.85f )
                {
                    anchor_valid[ i ] = false;
                    anchor_settling[ i ] = true;
                    continue;
                }
            }
            if ( mx * mx + my * my + mz * mz > 0.01f * 0.01f || rotd > 0.0004f )
            {
                // travado — emula o parked-record nativo: re-asserta a pose
                // INTEIRA guardada (posição E rotação, incluindo Y) em
                // qualquer deriva real. Um pin só de posição nunca enxerga
                // uma carreta balançando: ela pivota no ponto de contato sem
                // mexer a origem, e tolerar deriva vertical deixa a frente
                // afundar pra sempre abaixo do limiar
                using teleport_fn_t = void __fastcall( void* /*trailer*/, void* /*transform*/ );
                trailer_teleport_hook->get_original< teleport_fn_t >()( t, &anchor_xform[ i ] );
            }
        }
    }

    // Uma carreta de volta ao convoy precisa estar totalmente desestacionada
    // — mas o parked-record (+0x1188) TEM que ficar linkado: o registro de
    // obstáculo de tráfego o referencia, e órfaná-lo deixa uma entrada de
    // lane morta que a varredura do tráfego quebra. O que de fato re-asserta
    // a pose estacionada é o update por frame gated pelo flag de registro
    // +0x1258 — então o byte de parked e esse flag caem, o record fica.
    // Só carretas JOINTADAS: uma peça sem junta fica na cadeia de propósito,
    // e limpar as flags dela briga com a manutenção que as re-asserta.
    void unpark_jointed( const bool* in_convoy )
    {
        for ( uint32_t i = 0; i < trailer_count; ++i )
        {
            if ( !in_convoy[ i ] ) continue;
            void* t = trailers[ i ];
            if ( joint( t ) == nullptr ) continue;
            const bool parked_flag = prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked ) != 0;
            const bool parked_rec = prism::offsets::field< void* >( t, prism::offsets::trailer_parked_state ) != nullptr;
            // só um estado estacionado vivo precisa de limpeza — o record
            // (+0x1188) fica linkado de propósito pro registro de tráfego, e
            // a presença dele sozinha não pode re-disparar isso (dispararia
            // todo frame e zeraria os canais de freio da carreta engatada)
            const bool parked_reg = prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked_registered ) != 0;
            if ( !parked_flag && !parked_reg ) continue;
            // os bytes de flag caem todo frame — duas escritas que fecham o
            // gate que o re-assert de pose lê, mesmo se a máquina de
            // estacionamento os re-armar entre passadas. O trabalho por junta
            // era carregado inteiro e chaveado pela posição em trailers[], que
            // ROTACIONA durante o reboque — uma carreta re-jointada podia
            // herdar o selo de "feito" de outra peça e brigar com a máquina de
            // estacionamento pra sempre (o re-assert por frame contra a junta
            // viva é a guerra de física que derruba o sim pra 3-5 FPS)
            prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked ) = 0;
            prism::offsets::field< uint8_t >( t, prism::offsets::trailer_parked_registered ) = 0;
            static void* unparked_t[ max_trailers ] = {};
            static void* unparked_j[ max_trailers ] = {};
            uint32_t slot = max_trailers, free_slot = max_trailers;
            for ( uint32_t s = 0; s < max_trailers; ++s )
            {
                if ( unparked_t[ s ] == t ) { slot = s; break; }
                if ( unparked_t[ s ] == nullptr && free_slot == max_trailers ) free_slot = s;
            }
            if ( slot == max_trailers )
            {
                if ( free_slot == max_trailers ) continue;
                slot = free_slot;
                unparked_t[ slot ] = t;
                unparked_j[ slot ] = nullptr;
            }
            if ( unparked_j[ slot ] == joint( t ) ) continue;
            unparked_j[ slot ] = joint( t );
            prism::offsets::field< float >( t, prism::offsets::trailer_brake ) = 0.f;
            prism::offsets::field< uint8_t >( t, prism::offsets::trailer_brake_forced ) = 0;
            // tira o record da scan-list da máquina de estacionamento —
            // enquanto ficar listado, o manager re-arma as flags e re-asserta
            // a pose guardada contra a junta viva todo frame. O record em si
            // continua linkado na carreta (+0x1188) pro registro de tráfego
            if ( parked_rec ) parked_record_unlist( t );
            CCore::g_instance->info( "un-parked trailer {} (flag {} record {})", i, parked_flag, parked_rec );
        }
    }
}
