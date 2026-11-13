# Índice de funções

Numeração **global** — cada função do plugin tem um número único. Quando algo
crashar ou falhar, o número diz exatamente qual função é, sem ambiguidade.

| Arquivo | Faixa |
|---------|-------|
| src/windows/trailer_shared.hpp | 001–064, 236–245 |
| src/windows/trailer_manipulation.cpp | 065–107 |
| src/windows/trailer_steering.cpp | 108–114 |
| src/windows/trailer_loadunload.cpp | 115–118 |
| src/windows/trailer_loadstate.cpp | 119–143, 235 |
| src/windows/trailer_oficina.cpp | 144–145 |
| src/windows/trailer_loadgame.cpp | 146–147 |
| src/windows/trailer_lines.cpp | 148–165 |
| src/windows/trailer_garage.cpp | 166 |
| src/windows/trailer_coupling.cpp | 167–197 |
| src/windows/trailer_camera.cpp | 198–202 |
| src/windows/trailer_camera_zero.cpp | 203 |
| src/windows/trailer_brake.cpp | 204–216 |
| src/windows/trailer_axles.cpp | 217–223 |
| src/windows/trailer_air.cpp | 224–234 |

## src/windows/trailer_shared.hpp

Tabela de endereços do jogo — cada entrada é um endereço fixo com assinatura
de bytes; se não bater no exe, aquele recurso desliga sozinho:

| Nº | Item | Papel |
|----|------|-------|
| 001 | get_cvar_int | leitora de cvar inteira |
| 002 | get_cvar_float | leitora de cvar float |
| 003 | native attach dispatcher | despachante de engate nativo |
| 004 | anchor relink | religa âncoras de tráfego |
| 005 | post_event | dispara evento no jogo |
| 006 | trailer dtor | destrutor de carreta |
| 007 | vfunc98 impl A | engate (impl A) |
| 008 | vfunc98 impl B | engate (impl B) |
| 009 | vfunc99 | desengate |
| 010 | set_lines | liga/desliga linhas (mangueiras/luzes) |
| 011 | apply_cables | aplica cabos |
| 012 | trailer settle | settle da carreta no engate |
| 013 | set_towed | marca carreta rebocada |
| 014 | rebuild_lines | reconstrói linhas |
| 015 | notify_decoupled | aviso de desengate |
| 016 | pre_detach | solta a junta no cavalo |
| 017 | pair_vehicles | pareia/despareia conectores |
| 018 | brace anim | animação do apoio |
| 019 | lamp mask setter | setter da máscara de luz |
| 020 | lamp propagator | propagador de luzes |
| 021 | execute_line | execute_line do console |
| 022 | hose geometry | geometria de mangueira (por frame) |
| 023 | traffic manager | gerenciador de tráfego (dado) |
| 024 | traffic game state | estado do jogo (dado: 2 = no mundo) |
| 025 | cables mode cvar | cvar g_trailer_cables_mode (dado) |
| 026 | attach settle cvar | cvar de settle do engate (dado) |
| 027 | event param block | bloco de parâmetros de evento (dado) |
| 028 | "@@trailer_coupling@@" | string do evento de engate |
| 029 | "@@trailer_decoupling@@" | string do evento de desengate |
| 030 | `validated_va` | confere o endereço fixo contra a assinatura (cache) |
| 031 | `expected_build` | portão do build: todas as assinaturas batem com o exe |
| 032 | `readable_ptr` | ponteiro é legível e comprometido |
| 033 | `readable_span` | quantos bytes legíveis seguem o ponteiro |
| 034 | `plausible_object` | ponteiro parece um objeto do jogo (vtable no exe) |
| 035 | `plausible_vfunc` | o slot da vtable aponta pra código do exe |
| 036 | `is_trailer_obj` | o ponteiro é objeto-carreta de verdade (vfunc 99) |
| 037 | `vcall` | chama um slot da vtable |
| 038 | `joint` | junta física da carreta (+0xFE0) |
| 039 | `steering_data` | dados de eixo direcional da carreta |
| 040 | `index_of` | posição da carreta na lista atual |
| 041 | `is_detached` | carreta tem marca de desatrelada |
| 042 | `mark_detached` | marca a carreta como desatrelada |
| 043 | `unmark_detached` | tira a marca de desatrelada |
| 044 | `is_plugin_detached` | desatrelada pelos controles do plugin |
| 045 | `mark_plugin_detached` | marca desatrelada pelo plugin |
| 046 | `unmark_plugin_detached` | tira a marca do plugin |
| 047 | `is_tracked_trailer` | o ponteiro é uma carreta rastreada |
| 048 | `queue_cmd` | enfileira um comando de console |
| 049 | `queue_op` | enfileira engate/desengate pro thread de física |
| 050 | `pump_sim_heartbeat` | batimento do thread de física |
| 051 | `watch_sim_heartbeat` | vigia se o sim continua rodando |
| 052 | `sim_is_quiet` | sim está parado - seguro executar inline |
| 053 | `hook_world` | transform do veículo no mundo (vfunc 0xF8) |
| 054 | `truck_xform` | transform do cavalo |
| 055 | `steady_ms` | relógio em milissegundos |
| 056 | `in_cargo_window` | janela de carga/descarga (20s após teleporte) |
| 057 | `handbrake_ok` | regra do engate: freio de mão puxado |
| 058 | `game_has_focus` | o jogo está em primeiro plano |
| 059 | `key_down` | a tecla configurada está pressionada |
| 060 | `ui::pdv` | cor hex linearizada pra UI |
| 061 | `ui::pdv32` | cor hex convertida pra ImU32 |
| 062 | `ui::toggle_switch` | botão liga/desliga da UI |
| 063 | `ui::right_side` | alinha o próximo widget à direita |
| 064 | `ui::text_disabled_wrapped` | texto cinza com quebra de linha |

## src/windows/trailer_manipulation.cpp

| Nº | Função | Papel |
|----|--------|-------|
| 065 | `track_canon` | registra a carreta na ordem canon (slot fixo por peça) |
| 066 | `canon_index_of` | slot canon da carreta (-1 = desconhecida) |
| 067 | `claim_edge_ours` | diz se um link slave foi escrito pelo plugin |
| 068 | `clear_claim_edge` | apaga o registro do link do plugin |
| 069 | `set_claim_edge` | registra um link slave escrito pelo plugin |
| 070 | `dump_chain` | despeja o estado da cadeia inteira no log |
| 071 | `canon_dead` | detecta reload do mundo (carreta deixou de ser objeto-carreta) |
| 072 | `canon_all_trailer_objs` | confere se toda peça do canon ainda é carreta válida |
| 073 | `sever_all_claims` | corta todos os links slave escritos pelo plugin |
| 074 | `reset_trailer_state` | zera todo o rastreio após reload ou falha |
| 075 | `release_detach_marks` | solta as marcas de desengate pra reengatar pela tecla T |
| 076 | `canon_coupled_predecessor` | antecessor canon acoplado da carreta |
| 077 | `trailer_is_live` | carreta alcançável pelo cavalo pela cadeia física |
| 078 | `trailer_is_known_parked` | peça estacionada conhecida do plugin |
| 079 | `trailer_maintenance_ready` | portão: mundo montado e estável |
| 080 | construtor | construtor |
| 081 | destrutor | solta hooks e corta claims |
| 082 | `init` | instala hooks e registra canais |
| 083 | `update_fault_filter` | filtro do protetor de falhas do update |
| 084 | `update` | update por frame protegido |
| 085 | `update_impl` | update por frame: assinatura da cadeia + sub-sistemas |
| 086 | `disconnect_trailer` | **DESATRELAMENTO**: T(i) solta do pai físico - T0 do cavalo, T1 de T0, T2 de T1, T1/T2 diretos do cavalo |
| 087 | `connect_trailer` | **ATRELAMENTO em cadeia**: T(i) no antecessor - T1 em T0, T2 em T1 |
| 088 | `connect_trailer_to_truck` | **ATRELAMENTO direto**: T(i) na quinta roda do cavalo |
| 089 | `connect_to_truck` | seleciona a carreta-alvo do engate nativo (tecla T) |
| 090 | `toggle_attach` | alterna atrelar/desatrelar de T(i) |
| 091 | `render_keybind_row` | linha de tecla na aba Teclas |
| 092 | `render_keybinds` | lista de teclas configuráveis |
| 093 | `render_trailer_connect` | botões de atrelar/desatrelar de T(i) na tela |
| 094 | `render` | janela principal do plugin |
| 095 | `render_keys_tab` | aba Teclas |
| 096 | `render_options_tab` | aba Opções |
| 097 | `render_oversize_tab` | aba Direcionais |
| 098 | `render_trailers_tab` | aba Carretas |
| 099 | `render_license_tab` | aba Licença |
| 100 | `render_logs_tab` | aba Logs |
| 101 | `rebuild_parked_claim_links` | reescreve os links slave das peças estacionadas |
| 102 | `refresh_trailer_chain` | reescan da cadeia + manutenção (links, linhas, marcas) |
| 103 | `update_convoy_state` | estado do comboio (freios, trava, mangueiras, tráfego) |
| 104 | `gc_detached_marks` | limpa marcas de desengate expiradas |
| 105 | `trigger_native_sleep` | injeta Shift+Num0 (dormir nativo) |
| 106 | `update_sleep_release` | solta as teclas do dormir |
| 107 | `update_hotkeys` | teclas de atrelar/desatrelar |

## src/windows/trailer_steering.cpp

| Nº | Função | Papel |
|----|--------|-------|
| 108 | `set_locked` | trava/destrava o controle manual do eixo de T(i) |
| 109 | `hk_set_individual_steering` | hook: injeta o ângulo do usuário no lugar do do jogo |
| 110 | `update_steering` | por frame: teclas de direção + centralização suave |
| 111 | `render_trailer_steering` | controles de direção de uma carreta na tela |
| 112 | `render_oversize` | painel Direcionais (todas as direcionáveis + teclas) |
| 113 | `steering_install` | instala o hook de direção (achado por padrão) |
| 114 | `steering_reset` | limpa as travas (destrutor/reset) |

## src/windows/trailer_loadunload.cpp

| Nº | Função | Papel |
|----|--------|-------|
| 115 | `restore_parked_if_enabled` | chama o restore só se a opção "restaurar carga" estiver ligada |
| 116 | `restore_parked` | recoloca o conjunto após respawn de carga: re-detacha seções e teleporta pras poses salvas |
| 117 | `carry_set_brace` | desce o apoio da carreta pela vfunc nativa (protegido) |
| 118 | `coupling_parked_update` | por frame: vigia o respawn de carga e enfileira o restore |

## src/windows/trailer_loadstate.cpp

| Nº | Função | Papel |
|----|--------|-------|
| 119 | `render_cargo_restore_option` | checkbox "Sistema de Desacoplamento" na aba Carretas |
| 120 | `snapshot_rig` | fotografa o transform do caminhão antes do teleporte |
| 121 | `hk_trailer_state_apply` | hook: aplica o record de estado na carreta (doca) |
| 122 | `hk_trailer_teleport` | hook: teleporte por-carreta - recusa peça estacionada |
| 123 | `settle_all_members` | assenta a formação após o teleporte de ator |
| 124 | `hk_actor_teleport` | hook: teleporte do ator (conjunto inteiro) |
| 125 | `find_trailer_actor` | acha o objeto-ator da carreta |
| 126 | `sample_provider_vtbl` | amostra a vtable do provider de obstáculo |
| 127 | `find_trailer_provider` | acha o provider de obstáculo na carreta |
| 128 | `cache_trailer_actor` | cacheia carreta→ator |
| 129 | `factory_fault_filter` | filtro SEH da fábrica de parked-record |
| 130 | `dump_native_registry` | log do registro de parked do jogo |
| 131 | `parked_record_listed` | carreta está na lista de parked-records |
| 132 | `parked_record_unlist` | tira a carreta da lista de parked-records |
| 133 | `lane_element_dead` | elemento de lane-obstacle está morto |
| 134 | `prune_lane_containers` | poda elementos mortos dos containers de lane |
| 135 | `hk_lane_obstacle_insert` | hook: sanitiza o container antes do insert nativo |
| 136 | `native_park_detached` | estaciona uma carreta solta (apoio + freio + registro) |
| 137 | `install_crash_tracer` | instala o rastreador de crashes (VEH) |
| 138 | `purecall_null_vfunc` | vfunc fake que devolve nulo |
| 139 | `init_purecall_fake` | monta a vtable fake do purecall |
| 140 | `hk_purecall` | hook: engole purecall do iterador de componentes |
| 141 | `unhide_detached` | reexibe carretas escondidas |
| 142 | `update_parked_locks` | trava de estacionamento emulada (âncora de pose) |
| 143 | `unpark_jointed` | desmarca "estacionada" nas carretas reengatadas |

## src/windows/trailer_oficina.cpp

| Nº | Função | Papel |
|----|--------|-------|
| 144 | `workshop_respawn_discard` | descarta o espelho do mundo antigo após um load |
| 145 | `workshop_respawn_update` | por tick: detecta destruição/recriação da oficina e devolve as poses |

## src/windows/trailer_loadgame.cpp

| Nº | Função | Papel |
|----|--------|-------|
| 146 | `load_game_active` | jogo está carregando (fora do mundo) |
| 147 | `load_game_update` | por tick: detecta fim de load e zera o estado do mundo antigo |

## src/windows/trailer_lines.cpp

| Nº | Função | Papel |
|----|--------|-------|
| 148 | `play_brace_sound` | toca o som do apoio subindo/descendo |
| 149 | `update_landing_gear` | por tick: apoio das carretas (levanta no engate, desce na estacionada) |
| 150 | `light_ctx_of` | acha o contexto de luzes do veículo |
| 151 | `light_source_of` | acha a fonte de luzes do veículo |
| 152 | `rebuild_one_edge` | reconstrói as mangueiras de uma aresta (link escondido na chamada) |
| 153 | `rebuild_parked_edge` | redesenha a aresta real atrás de uma cabeça estacionada |
| 154 | `refresh_all_lines` | reescreve todas as linhas vivas |
| 155 | `repair_lamp_ctx` | repina o src de luz reciclado de volta pro membro |
| 156 | `repair_lamp_ctx_all` | repina os contextos de luz de todas as carretas |
| 157 | `pin_ctx_lamp_state` | prende o estado de luz num contexto |
| 158 | `lamp_ctx_sweep` | varre os contextos de luz do dono |
| 159 | `light_watch_tick` | vigia de lâmpadas por tick |
| 160 | `hk_lamp_mask` | hook: máscara de lâmpadas |
| 161 | `hk_lamp_propagate` | hook: propagação de lâmpadas pela cadeia |
| 162 | `hk_apply_cables` | hook: cabos/mangueiras do engate |
| 163 | `hk_set_lines` | hook: liga/desliga as linhas do veículo |
| 164 | `hk_hose_update` | hook: atualização de mangueiras por aresta |
| 165 | `hk_rebuild_lines` | hook: cascata de desengate das linhas |

## src/windows/trailer_garage.cpp

| Nº | Função | Papel |
|----|--------|-------|
| 166 | `garage_update` | por tick: limpa marcas de carretas destruídas (garagem/recall) |

## src/windows/trailer_coupling.cpp

| Nº | Função | Papel |
|----|--------|-------|
| 167 | `quat_rot` | gira vetor por quaternion |
| 168 | `attach_offset` | offset do gancho traseiro no frame do pai |
| 169 | `hook_gap` | distância gancho-a-gancho no mundo |
| 170 | `hook_far` | recusa engate com ganchos longe demais |
| 171 | `native_detach_via_slot` | desatrelamento pela via nativa (slot do ator) |
| 172 | `native_attach_via_slot` | atrelamento pela via nativa (slot do ator) |
| 173 | `disconnect_now` | executa o desatrelamento na thread de física |
| 174 | `connect_now` | executa o atrelamento na thread de física |
| 175 | `hk_crashes_when_disconnected` | hook por-frame do sim: consome a fila de operações |
| 176 | `traffic_detach_tail` | cauda de notificação do tráfego após desengate |
| 177 | `hk_connect_slave` | hook: auto-engate de slave (recusa claims falsos) |
| 178 | `hk_joint_walk` | hook: caminhada de juntas (guarda anti-ciclo) |
| 179 | `hk_phys_link` | hook: link físico (recusa ciclo) |
| 180 | `sanitize_slave_walk` | saneia a cadeia slave antes do teardown nativo |
| 181 | `sanitize_phys_walk` | saneia a cadeia física (corta arestas mortas) |
| 182 | `hk_trailer_disconnect` | hook: desengate vfunc 99 (saneia antes do teardown) |
| 183 | `sever_parked_claims` | corta os links-claim estacionados antes da chamada nativa |
| 184 | `sever_phys_claims` | corta arestas físicas que tocam peça sem junta |
| 185 | `restore_parked_claims` | religa os claims cortados após a chamada nativa |
| 186 | `run_native_connect` | roda o connect nativo com a cadeia parked cortada |
| 187 | `hk_trailer_connect_a` | hook: engate impl A (vfunc 98) |
| 188 | `hk_trailer_connect_b` | hook: engate impl B (vfunc 98) |
| 189 | `hk_pre_detach` | hook: pré-desengate (só passa com junta viva) |
| 190 | `hk_detach_dispatch` | hook: despachante de desengate (regra do freio de mão) |
| 191 | `hk_detach_dispatch_raw` | hook: miolo do desengate (marca a cabeça) |
| 192 | `hk_attach_dispatch` | hook: despachante de engate |
| 193 | `hk_attach_dispatch_raw` | hook: miolo do engate |
| 194 | `resolve_native_fns` | resolve/valida os endereços e instala os hooks |
| 195 | `hk_trailer_dtor` | hook: destrutor de carreta (limpa registros) |
| 196 | `coupling_register_channels` | registra os canais de telemetria |
| 197 | `coupling_install` | instala o sistema de engate |

## src/windows/trailer_camera.cpp

| Nº | Função | Papel |
|----|--------|-------|
| 198 | `hk_cam_set_target` | hook: tira carreta sem junta da âncora da câmera 2 |
| 199 | `hk_cam_update` | hook: update da câmera 2 (esconde links mortos na medida) |
| 200 | `hk_combo_length` | hook: comprimento do comboio (para na primeira junta morta) |
| 201 | `chase_camera_obj` | acha o objeto da câmera 2 |
| 202 | `enforce_chase_camera_anchor` | mantém a âncora da câmera 2 na carreta certa |

## src/windows/trailer_camera_zero.cpp

| Nº | Função | Papel |
|----|--------|-------|
| 203 | `block_camera_zero_teleport` | bloqueia o teleporte do zerar-câmera (Ctrl+F9) com a opção ligada |

## src/windows/trailer_brake.cpp

| Nº | Função | Papel |
|----|--------|-------|
| 204 | `native_mask_of` | máscara de freio nativa do wheelset (cache) |
| 205 | `update_wheel_brake_mask` | reescreve a máscara de rodas freáveis por frame |
| 206 | `foreign_brake_write` | engole escrita de freio vinda de peça estacionada |
| 207 | `next_jointless_link` | acha o próximo link sem junta na cadeia |
| 208 | `biased_brake` | amplifica o valor do freio pela config |
| 209 | `hk_brake_chain_setter` | hook: setter de freio vindo da cadeia |
| 210 | `hk_brake_setter` | hook: setter de freio direto na carreta |
| 211 | `hk_forced_chain_setter` | hook: flag de freio forçado vindo da cadeia |
| 212 | `hk_forced_setter` | hook: flag de freio forçado direto na carreta |
| 213 | `hk_wheel_brake` | hook: solver de freio por roda (zera roda no ar) |
| 214 | `brake_register_hooks` | instala os hooks de freio |
| 215 | `brake_boost_update` | por frame: aplica o boost no fator de freio |
| 216 | `update_convoy_brakes` | freio de mão nas peças estacionadas do comboio |

## src/windows/trailer_axles.cpp

| Nº | Função | Papel |
|----|--------|-------|
| 217 | `on_light_reverse` | callback de telemetria: luz de ré |
| 218 | `on_engine_gear` | callback de telemetria: marcha do motor |
| 219 | `release_all_axles` | devolve máscaras/flags dos eixos ao jogo |
| 220 | `render_trailer_lift` | UI de eixos de uma carreta |
| 221 | `render_truck_lift` | UI de eixos do cavalo |
| 222 | `lift_register_channels` | registra os canais de telemetria (ré/marcha) |
| 223 | `lift_axles_update` | por frame: aplica o estado dos eixos levantados |

## src/windows/trailer_air.cpp

| Nº | Função | Papel |
|----|--------|-------|
| 224 | `on_air_pressure` | callback de telemetria: pressão do ar |
| 225 | `on_game_time` | callback de telemetria: tempo do jogo |
| 226 | `on_engine_enabled` | callback de telemetria: motor ligado |
| 227 | `on_engine_rpm` | callback de telemetria: rotação do motor |
| 228 | `air_safe_read_f` | leitura segura de float (protegida) |
| 229 | `air_safe_read_q` | leitura segura de uint64 (protegida) |
| 230 | `air_safe_write_f` | escrita segura de float (protegida) |
| 231 | `air_object` | resolve o objeto de ar do cavalo |
| 232 | `update_air_leak` | por frame: vazamento de ar com o motor desligado |
| 233 | `update_air` | por frame: boost do compressor (malha fechada) |
| 234 | `air_register_channels` | registra os canais de telemetria de ar |

## Funções de infraestrutura do índice

| Nº | Função | Arquivo | Papel |
|----|--------|---------|-------|
| 235 | `crash_tracer` | trailer_loadstate.cpp | tratador de exceções: loga código, RIP, registradores e a função numerada |
| 236 | `fn_index_table` | trailer_shared.hpp | tabela global função↔número (registro estático por arquivo) |
| 237 | `fn_addr_of` | trailer_shared.hpp | endereço de qualquer função (livre ou membro) |
| 238 | `fn_index_add` | trailer_shared.hpp | registra uma função no índice |
| 239 | `fn_index_lookup` | trailer_shared.hpp | RIP → função registrada mais próxima |
| 240 | cam_set_target | trailer_shared.hpp | setter real da âncora da câmera 2 (vfunc 31) |
| 241 | cam_update | trailer_shared.hpp | update por frame da câmera 2 (vfunc 32) |
| 242 | combo_length | trailer_shared.hpp | comprimento do comboio (anda cadeia slave) |
| 243 | combo pos | trailer_shared.hpp | posição de engate usada pelo combo_length |
| 244 | camera manager | trailer_shared.hpp | gerenciador de câmeras (dado) |
| 245 | chase vtable | trailer_shared.hpp | vtable vehicle_behind_rotation_camera (dado) |
