# ENDERECOS.md — mapa de endereços fixos do plugin

Referência de **todos os endereços virtuais (VA)** que o plugin consulta no
`eurotrucks2.exe`. Quando o jogo atualiza, este arquivo é a lista de compras:
cada endereço abaixo precisa ser re-localizado no executável novo.

## Como o sistema funciona

1. O código nunca usa um VA direto — sempre chama
   `validated_va(0xVA_ANTIGO)` (definida em `src/windows/trailer_shared.hpp`).
2. A função procura o VA antigo na tabela `va_sentinels[]`: se a assinatura
   (primeiros ~20 bytes) bate no endereço novo cadastrado, retorna o VA novo;
   senão retorna 0 e loga `endereco fixo 0x... nao confirmado - recurso
   desligado`.
3. **Sem entrada na tabela = recurso desligado em silêncio.** É assim que se
   detecta o que quebrou numa atualização.

Colunas: `ANTIGO` = VA no exe 1.61.1.9s · `NOVO` = VA no exe 1.62.3.2.

## 1. Confirmados — já traduzidos na tabela (exe 1.62.3.2)

### Funções (assinatura verificada em tempo de execução)

| Sentinela | ANTIGO      | NOVO        | Recurso                                     |
|-----------|-------------|-------------|---------------------------------------------|
| 001 | `0x1401CBE10` | `0x1401D3140` | leitora de cvar inteira (`get_cvar_int_fn`) |
| 002 | `0x1401DB560` | `0x1401E27E0` | leitora de cvar float (`get_cvar_float_fn`) |
| 003 | `0x1405DCC20` | `0x1405DB4F0` | despachante de engate nativo (`native_attach_fn` + `attach_dispatch_hook`) |
| 004 | `0x140623C90` | `0x140622F30` | religa âncoras de tráfego (`relink_anchors_fn`) |
| 005 | `0x140623DA0` | `0x140623040` | `post_event` — dispara evento no jogo |
| 006 | `0x1406359E0` | `0x14062A620` | destrutor de carreta (`trailer_dtor_hook`) |
| 007 | `0x140639090` | `0x14062DCF0` | engate vfunc98 impl A (`trailer_connect_hook`) |
| 008 | `0x1407890A0` | `0x1407746E0` | engate vfunc98 impl B (`trailer_connect_hook2`) |
| 009 | `0x140789C20` | `0x140775260` | **desengate vfunc99** — único impl de todas as classes de carreta; também é a âncora do `is_trailer_obj` |
| 010 | `0x140874010` | `0x140863280` | `set_lines` — liga/desliga mangueiras |
| 011 | `0x1408762E0` | `0x140865550` | `apply_cables` — aplica cabos do cavalo |
| 012 | `0x1409FB700` | `0x14096DF10` | settle da carreta no engate |
| 013 | `0x140642080` | `0x140642080` | `set_towed` — **não mudou entre versões** |
| 014 | `0x1408663F0` | `0x140866410` | `rebuild_lines` / `refresh_accessories` |
| 015 | `0x1408633D0` | `0x1408633F0` | `notify_decoupled` |
| 016 | `0x140864160` | `0x140864180` | `pre_detach` — solta a junta no cavalo |
| 017 | `0x1408771A0` | `0x1408655D0` | `pair_vehicles` — pareia conectores de cabo |
| 018 | `0x140636F50` | `0x14062BBB0` | animação do apoio/`brace` (`brace_anim_fn`) |
| 019 | `0x1409DB4B0` | `0x14094C9A0` | setter da máscara de luz (`lamp_mask_hook`) |
| 020 | `0x140790DC0` | `0x14077C380` | propagador de luzes (`lamp_propagate_hook`) |
| 021 | `0x1401E7F80` | `0x1401E7F80` | `exec_line` do console — **não mudou** |
| 022 | `0x1408653A0` | `0x1408653C0` | geometria de mangueira por frame (`hose_update_hook`) |
| 240 | `0x14063C2D0` | `0x140630D50` | `cam_set_target` — setter real da âncora (vfunc 31, `cam_set_target_hook`) |
| 241 | `0x140AC75A0` | `0x140A411E0` | `cam_update` — update por frame da câmera 2 (vfunc 32, `cam_update_hook`) |
| 242 | `0x140789AB0` | `0x1407750F0` | `combo_length` — mede o comboio andando `trailer_slave` (`combo_length_hook`) |
| 243 | `0x140789880` | `0x140774EC0` | posição de engate usada pelo `combo_length` (`combo_hook_pos_fn`) |
| 246 | `0x1405DD410` | `0x1405DBCE0` | despachante de desengate nativo (`detach_dispatch_hook`) |
| 247 | `0x1405DCBC0` | `0x1405DB490` | corretor de flags do ator (`actor_flag_fixup_fn`) — anda a cadeia `+0x1058` checando eixo levantado `+0x6C8/+0x3C`; chamado no fim do engate, `-0x60` antes do attach dispatch |
| 248 | `0x1405DE1E0` | `0x1405DCAB0` | teleporte do ator (`actor_teleport_hook`) — `(self, transform, a3, a4)` |
| 249 | `0x140786320` | `0x1407864A0` | teleporte por carreta (`trailer_teleport_hook`) |
| 250 | `0x1405E14D1` | `0x1405DFDE0` | `trailer_state_apply` — reaplica estado salvo; **no exe novo mapeado p/ a ENTRADA da função** (o site velho era mid-function); lê `record+0xB8`, chama attach/detach/rebuild_lines/apply_cables |
| 251 | `0x1407B69E0` | `0x14070A130` | `parked_record_factory` — aloca registro `0x198`, carreta em `+0x178`, insere no gerenciador |
| 253 | `0x141D23610` | `0x141D571D0` | `purecall` do CRT — **4404 slots de vtable apontam p/ ele**; forma `sub rsp,0x28; mov rax,[rip+..]; test; je; call [..]; call; int3` |
| 254 | `0x141BEA1C5` | `0x141A959DB` | RIP da leitura de curva de navegação — `movss xmm5,[rdi+rcx*4]` (5 bytes); índice vem de tabela de bytes `[rbp+0x258]+r10`; função `0x141A94620` |
| 255 | `0x14064B7EF` | `0x14064037F` | retorno do broadcast de freio real — após `call [rax+0x180]` em `0x140640379`; bloco do caminhão em `0x140640339` |
| 256 | `0x14064B806` | `0x140640396` | retorno do broadcast forçado — após `call [rax+0x188]` em `0x140640390` (o `+0x178` = `+0x730` espelho morto, não usar) |

### Dados globais e strings (sem assinatura — `nullptr`)

| Sentinela | ANTIGO      | NOVO        | Recurso                                     |
|-----------|-------------|-------------|---------------------------------------------|
| 023 | `0x1436AF7C8` | `0x143714F50` | gerenciador de tráfego (global) |
| 024 | `0x143051820` | `0x1430B5960` | estado do jogo (dado: 2 = no mundo) |
| 025 | `0x142D485C0` | `0x142DA9FA0` | cvar `g_trailer_cables_mode` |
| 026 | `0x142D48200` | `0x142DA9BE0` | cvar de settle do engate |
| 027 | `0x142733198` | `0x1427949E8` | bloco de parâmetros de evento |
| 028 | `0x14226E368` | `0x1422BCCB8` | string `"@@trailer_coupling@@"` |
| 029 | `0x14226E4B0` | `0x1422BC710` | string `"@@trailer_decoupling@@"` |
| 244 | `0x1436AF830` | `0x143714FB8` | gerenciador de câmeras (global) |
| 245 | `0x1423126F0` | `0x1423552A8` | vtable da câmera 2 (`vehicle_behind_rotation_camera`) |
| 252 | `0x1436AF828` | `0x143714FB0` | gerenciador de registros estacionados — array `mgr+0x740`, contagem `+0x748`; era `traffic_mgr+0x60`/`camera_mgr-0x8`, 57 refs |
| 257 | `0x142D45500` | `0x142DA6EE0` | cvar `g_traffic` — **candidato forte, nome não confirmado**: objeto na região de cvars (mesmo delta `+0x619E0` dos cvars confirmados, mesmo padrão de registro com 2 `lea rcx` + `call`); uso só de leitura |

## 2. Mortos — sem entrada na tabela, recurso desligado (exe 1.62.3.2)

Restam 4 endereços sem tradução — os 12 anteriores já foram re-derivados e
cadastrados (sentinelas 246–257).

| ANTIGO | Recurso | Onde é usado | Como achar de novo |
|--------|---------|--------------|--------------------|
| `0x14094DFC0` | `lane_obstacle_insert_hook` — obstáculo na faixa p/ tráfego | `trailer_coupling.cpp:1549` | inserção de obstáculo na malha de faixas; assinatura `uint8(node, link, pos)`; região `0x14093-0x14095` (subsistema de física/IA) |
| `0x1409555F0` | `joint_walk_hook` — caminhada de juntas | `trailer_coupling.cpp:1650` | função **auto-recursiva** `float(node, arg)` que anda a árvore de juntas; guarda anti-ciclo `jw_seen[96]` |
| `0x140919680` | `phys_link_hook` — link físico | `trailer_coupling.cpp:1661` | `void(parent, child)` — escreve `child+0x60 = parent`; provável chamado no caminho de criação de junta |
| `0x142220EE60` | `sound_vtbl` — vtable do som da manivela do pé | `trailer_lines.cpp:33` | vtable da classe de evento de som; `audio = trailer+0x12A8`, canal `audio+0x50/+0x58`; degrada sem erro (som não toca) |

## 3. Offsets de campos (`src/prism/offsets.hpp`)

Também são sensíveis à versão — a struct da carreta mudou entre 1.61 e 1.62
(ex.: junta `+0xFE0` → `+0xFB0`, slave → `+0x1058`). Valores atuais:

| Offset | Campo | Nota |
|--------|-------|------|
| `game_actor+0x18` | objeto físico do caminhão | |
| `game_actor+0xC8` | primeira carreta | |
| `trailer+0x1050` | `trailer_parent` — carreta anterior | |
| `trailer+0x1058` | `trailer_slave` — próxima carreta | reescrito pelo plugin a cada frame |
| `trailer+0xFB0` | junta física (`physics_joint`) | `is_trailer_obj`/vida da junta |
| `trailer+0x2A0` | máscara de lâmpada | bit 0 = alimentação |
| `trailer+0x730` | demanda de freio (espelho) | morto, não escrever |
| `trailer+0x760` | fator de freio real | alvo do "carreta freia mais" |
| `trailer+0x764` | flag de freio forçado | |
| `trailer+0x6B8`/`+0x6C0` | ângulo/dados de eixo direcional | |
| `trailer+0x1158`/`+0x1168`/`+0x1228` | estado estacionado | |
| `trailer+0x1160`/`+0x1254`/`+0xFD0` | brace / manivela | |
| `truck+0x14B8` | estado de engate (5 = engatado) | |
| `truck+0x1580` | carreta rebocada | zerado no desmanche da 182 |
| `truck+0x1148` | fração de enchimento do ar | alvo do compressor |
| `truck+0x200`→`+0x458` | máximo do tanque de ar | **não escrever** |
| `camera+0x3F0` | âncora (cabeça da cadeia) | raw store, nunca vfunc 31 |
| `camera+0x480`/`+0x4D8`/`+0x4DC` | zoom min/alvo/suavizado | |

Vfuncs: `98` = connect · `99` = disconnect · `101` = set_brace.

## 4. Receitas de descoberta (aprendidas nesta adaptação)

- **vtables de carreta**: toda classe de carreta compartilha o mesmo impl do
  slot 99 (vfunc_disconnect). Achando uma vtable de carreta, o slot 99 dá o
  endereço do desengate — e ele é a âncora do `is_trailer_obj`.
- **Câmera 2**: string `"vehicle_behind_rotation_camera"` na `.rdata` →
  descritor de unidade `{create_fn, delete_fn, ..., slot_global, tamanho}` →
  `lea` da vtable no construtor. Os slots da vtable: 14 = recompute
  (lê `âncora+0xFB0`), 31 = set_target, 32 = update.
- **`combo_length`**: função que anda `trailer+0x1058` acumulando
  `float(+0x2C) - float(+0x0x20) - gap`, retorna em `xmm0`. Procurar no
  `.pdata` funções que leem `+0x1058` + `+0x2C` + `+0x20`.
- **Globais colados**: os gerenciadores ficam em bloco na `.data`. Na 1.62:
  `traffic = 0x143714F50`, `parked_record ≈ +0x60`, `camera = +0x68`.
- **Strings de evento**: `"@@trailer_coupling@@"` / `"@@trailer_decoupling@@"`
  — scan literal na `.rdata`.
- **Sites mid-function**: não estão na `.pdata` como início de função —
  procurar a instrução/padrão específico dentro da função hospedeira.
- **Assinatura**: sempre os primeiros ~20 bytes na entrada da função —
  suficiente para distinguir builds vizinhos.

## 5. Fluxo quando o jogo atualizar

1. Rodar o jogo uma vez — o log lista cada `endereco fixo 0x... nao
   confirmado`.
2. Para cada endereço morto: localizar o novo VA (receitas acima), capturar
   a assinatura de 20 bytes e cadastrar na `va_sentinels[]` com o próximo
   número de sentinela livre.
3. Atualizar `INDICE.md` com os novos números.
4. Conferir também os **offsets de campos** (seção 3) — eles mudam
   independentemente das funções.
5. Testar em jogo; o log mostra `NOT FOUND`/validação para o que faltar.
