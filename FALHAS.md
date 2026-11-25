# Falhas diagnosticadas — pendências para correção

Levantamento feito com medição (contadores por chamada, tempo real de frame,
memória do processo). Cada item diz o que é fato medido e o que é hipótese.

## 1. CONFIRMADO — ponteiro de medição de comboio no meio de instrução

`combo_hook_pos_fn_ptr` apontava para `0x140789880` (VA do exe 1.62).
Esse endereço cai **no meio de uma instrução** (`mov r9d, 1` = `41 B9 01 00 00 00` —
as bytes `00 00` no ponto de entrada decodificam como `add [rax], al`).

Efeito por chamada:
- `add [rax], al` — incrementa um byte num endereço qualquer = corrupção
  silenciosa de memória do jogo, centenas de vezes por segundo
- a função real (início `0x140789780`) recebe rcx/rdx copiados pra r15/rbp
  no prólogo que era pulado — registradores lixo → exceção atrás de exceção

Evidência: 117 access violations em menos de 0.5s no log, todas em
`exe+0x857458` (função chamada com rcx=0 lendo `[rcx]` => `addr=0x0`).
Depois da remoção do ponteiro: `av+0` em todas as sessões seguintes.

Correção que funcionou (arquivo trailer_camera.cpp): `combo_measure_live()`
anda a cadeia `trailer_slave` com guarda, acha o primeiro elo morto/cíclico,
zera `trailer_slave` do dono, chama o original de `hk_combo_length`
(`0x140789AB0`), restaura o elo com `slave_link_write()`. Tudo sob `__try`.

NUNCA reutilizar `0x140789880`. A assinatura real da função em
`0x140789780` é diferente: recebe dois objetos + contexto e retorna ponteiro.

## 2. MEDIDO — explosão de re-registro de obstáculos de pista

`lane_obstacle_insert` (`0x1412b7dd0`, sentinel antigo `0x14094DFC0`) é um
**re-sort**: borbulha UM elemento na posição correta da lista ordenada.
O jogo o chama uma vez por binding de obstáculo por tick de física.

Medido em sessão de direção (janelas de ~300 frames):

| horário | gap/frame | inserts/janela | elems 0xFFF | mem |
|---------|-----------|----------------|-------------|-----|
| 16:47:43 | 22.6ms | 256 | 7 | 4107MB |
| 16:47:53 | 19.0ms | 7210 | 41 | 4114MB |
| 16:48:25 | 31.9ms | 28361 | 87 | 4467MB |
| 16:49:16 | 75.8ms | 110599 | 68 | 4876MB |

Taxa/segundo: ~28 -> ~5000 (~180x). `hb+` constante ~180/s (motor ok),
`wheel/steer/jwalk` constantes por segundo — só `lane+` explodiu.

Interpretação: inserts/s ≈ bindings registrados × 60 ticks. ~85 bindings
ativos ⇒ população de obstáculos cresceu ~10x e **não devolve**.
Crescimento monotônico = registro novo sem remoção do antigo (ou binding
que nunca expira).

Hipóteses a verificar (não provadas):
- `native_park_detached` -> `traffic_detach_tail` -> `relink_anchors_fn`
  (`0x140623C90`, chamado com `reg+0xD8, 2`): se park/relink roda mais de
  uma vez por carreta, cada chamada cria binding novo sem remover o velho.
- parked records em `mgr+0x740` (count `+0x748`): se a factory
  (`0x1407B69E0`) for chamada de novo pra carreta que já tem record,
  acumula.
- corpos vazados ("ghost") que ficam registrados como obstáculo depois de
  teleportados 10km pra baixo do mapa.

### Estruturas descobertas (exe 1.62)

- elemento de obstáculo: `+0x80` = record {vptr, +8 obstacle, +0x10 pos},
  `+0x88` = objeto dono, `+0x90` = posição na pista (float)
- container: `+0x08` = array de elementos, `+0x10` = count
- lane node: `+0x18` = container, flag `+0x10 >> 0x15 & 1` = "skip" (o
  insert sobe por `node+0x78` até a raiz)
- `get_type` = vfunc[1] do record; tipo `0xFFF` = binding de obstáculo

### Chamadores do insert (5 call sites diretos)

- `0x14129bac9` dentro de `0x14129ba15` (registrada em tabela RTTI-like
  `0x142681a30`, pares de fn + 0x21)
- `0x1412c42ea` dentro de `0x1412c4207` (tabela `0x142684514`)
- `0x1412e800d` dentro de `0x1412e7fd5` (tabela `0x142686a3c`)
- `0x141315219` e `0x1413152d9` dentro de `0x141315130` (vfunc, presente
  em 4 vtables: `0x142410bb8`, `0x142412860`, `0x142412a08`, `0x142413348`)

Os 3 primeiros não têm caller direto — são chamados via as tabelas de
descriptor (métodos de classe registrados por RVA par).

## 3. Descartado por medição (não é a causa)

- UI/render: `ui 0.00ms` com tela fechada; queda acontece sem abrir a tela
- custo por frame do plugin: `upd` ficou 0.09-0.20ms enquanto gap ia a 106ms
- exceções: `av+0` após o item 1 corrigido
- saves de ini: `saves+0` durante a queda
- escritas de suspensão: `suspw+0` (só escreve no botão Salvar)
- heartbeat do sim: ~180/s constante; `simdead` só sobe DEPOIS que o frame
  já está lento (sintoma, não causa)

## 4. Suspensão — mapa do campo (para quando mexer de novo)

- `trailer+0x4D4` = acumulador do controle de suspensão (0.5..2.0, o jogo
  anima e reseta pra 1.0) — é o campo que a régua escreve
- `trailer+0x4D0` = acumulador irmão, também resetado a 1.0
- `trailer+0x258` = byte "suspensão ainda ajustando"
- animadora nativa na região `0x140777120`; outra leitora/escritora em
  `0x140777230`; constantes do clamp em volta de `0x14085cc40`

Guarda em uso: só escrever se o valor atual está em 0.3..2.3 (evita tocar
objeto reciclado/de outra classe na lista).

## 4b. RESOLVIDO — suspensão afunda no 0 e trava o resgate (24-11)

- A régua 0..100 escrevia `stored/50` direto: régua no 0 = `0.0` escrito,
  ABAIXO do piso real do campo (0.5) — a carreta atravessava o chão.
- A guarda `f < 0.3f || f > 2.3f` olhava o valor ATUAL: com f=0.0 ela
  recusava qualquer escrita — a carreta afundada ficava presa pra sempre.
- Reparo: a régua mapeia os limites reais do jogo — 0→0.5, 50→1.0,
  100→2.0. A guarda passou a rejeitar só valor insano (não-finito, <0,
  >4) — carreta afundada volta a subir pela régua.

## 4c. RESOLVIDO — trava do "padrão nativo" matou eixos + suspensão (24-11)

- Sintoma: "Sem dados de eixo" nas 3 carretas e "Salvar altura" morto
  (suspw+0). Leitura ao vivo provou TUDO no lugar: controlador `+0x6C8`,
  flags `chassi+0x5C8/+0x5D0` = `01 00`, suspensão `+0x4D4` = 1.0.
- Causa: `lift_axles_update` + `suspension_update` dentro de
  `!cargo_freeze() && !native_pattern()` — native_pattern é verdadeiro no
  estado normal de jogo (tudo engatado), então as features nunca rodavam.
  A trava foi criada pra vigia de corrente, não pra feature.
- Reparo: o gate ficou só `!cargo_freeze()`.
- LIÇÃO: o scan linear do .text morre no primeiro byte inválido —
  "zero referências a offset X" por esse método NÃO prova campo morto.
  Só per-function ou leitura ao vivo.

## 4d. RESOLVIDO — churn de escrita por frame nos eixos (24-11)

- Depois do reparo 4c, o loop de eixos das carretas rodava no estado
  normal — e reescrevia `flags[eixo]` + `controlador+0x38` TODO FRAME sem
  checar se mudou (a seção do cavalo já escrevia só na diferença).
- Reparo: writes de flags/máscara dentro de `selection != applied_mask`
  — escrita única no evento (clique), zero escrita em frame parado.
  Ordem do usuário: o jogo recebe a decisão quando ele decide, nunca por
  frame.

## 5. Causa raiz do crash de ciclo — FECHADA

A amostragem ficou obsoleta: a causa foi provada por disassembly + log
(item 6) e corrigida (item 7). O crash não era re-registro em loop —
era elemento de obstáculo com dono destruído sobrevivendo nas duas
estruturas do nó de pista.

## 6. RESOLVIDO — crash em ciclo desatrelar/reatrelar (sessão limpa 17:36)

Sequência do usuário: desatrelou T2, atrelou T2, desatrelou T1, atrelou
T1, desatrelou T0, atrelou T0, desatrelou T1 de novo -> crash 50ms depois.

- AV fatal: `exe+0x12B9495` (comparador de ordenação `0x12B9480`) lendo
  `elemento+0x88` (dono) = NULL -> `mov [dono+0x10]` -> `addr=0x10`.
  Pilha: `exe+0x1310647` -> `exe+0x1296689` -> comparador. Acontece DENTRO
  do caminho de desatrelamento (o jogo atualiza a estrutura ordenada e o
  comparador anda nos elementos já existentes).
- Depois do AV: flood de AVs em `exe+0x14BFD2A96` (fora do exe, endereços
  `0x1c4f97XXXX` andando byte a byte) = o dump de crash do próprio jogo
  (`mov edx,'CRAS'` em ~`0x12BA20`) imprimindo uma tabela corrompida de
  136x32 bytes e lendo além do fim. NÃO é a causa — é o sintoma terminal.
- AVs repetidos em `exe+0x857458` durante o load do mundo: caminho de
  `hk_combo_length` engolido pelo `__try` (mesmo do item 1) — não fatais,
  mas mostram que o jogo chama a função com rcx=0 em rajadas.

### Estrutura REAL do lane node (mapeada por disassembly)

- `nó+0x18` = container ordenado embutido (array interno de ponteiros
  para elementos — visto `0x122cea09d40..0x122cea09e40` = 32 ponteiros).
  É ele que o comparador `0x12B9480` anda.
- `nó+0x20` = array de ponteiros p/ elementos, `+0x28` = count. É o que
  a função de varredura `0x1412B85A0` itera.
- Elemento: `+0x88` = dono, `+0x90` = posição na pista. Várias funções
  leem `dono+0x10` e chamam vfunc no dono SEM checar nulo — dono morto
  ou nulo = crash garantido, em QUALQUER leitora.
- Função de varredura `0x1412B85A0`: assinatura `(nó, flags, lo, hi,
  float* out)` retorna `void*` — ESCREVE pelo 5º parâmetro em `0x12B8A36`
  (`movss [rax],xmm7`). Hook com 4 args = crash instantâneo no load.
- Comparador `0x12B9480`: chamado por `0x1296689` (busca na estrutura
  ordenada), que é chamado por `0x1310647` — caminho do des/reatrelamento,
  NÃO passa pela função de varredura.

### Ficha de estacionado órfã (a "lista morta")

- `trailer+0x1158` (`rec`/`trailer_parked_state`) = ponteiro da ficha.
  No log, c0 ficou com `rec=0x125bf34f248` a sessão INTEIRA — inclusive
  depois de atrelada — mas `manager records=0` sempre. A ficha existe na
  carreta porém NÃO consta na lista do jogo: o jogo a desregistra no
  engate por conta própria e deixa o ponteiro pendurado = ficha órfã.
- Os elementos de obstáculo de pista tem `+0x88` apontando pro dono
  (registro de estacionado). Quando o dono morre no engate, os elementos
  ficam vivos nas DUAS estruturas do nó apontando pra nada.
- `wants_record` exige `parked_state==nullptr` pra criar ficha nova —
  ficha morta pendurada trava a criação da próxima (carreta nunca ganha
  registro novo, só reusa o cadáver).
- Purecalls "swallowed" em `exe+0x1295554`/`0x12959E2` (vfunc 140,
  `call [rax+0x40]`): objeto da malha de pista destruído ainda linkado
  (vptr virou vtable de base com `_purecall` no slot).

### Tentativas que FALHARAM — não repetir

1. Cortar TODAS as notificações/registros (notify, relink, ficha):
   crash igual — o jogo registra a carreta solta sozinho; só mudou quem
   cria o elemento.
2. Destruir a ficha no engate (unlist + zerar `rec`): a ficha já morria
   sozinha — `rec` já era 0x0 no engate em c1/c2. Não tocou a ferida.
3. Modelo "ficha persistente" (só alternar byte conectado): a ficha
   continua órfã no gerente; elementos continuam morrendo no engate.
4. Hook na varredura `0x12B85A0` removendo elementos mortos do ARRAY:
   removeu elementos de `nó+0x20`, mas o mesmo elemento continuava no
   container ordenado `nó+0x18` — comparador crashou no elemento
   `0x122cea2cdf8`, que NUNCA passou pelo hook (outra estrutura, outra
   leitora).
5. Neutralizar dono morto com `lane_dead_owner` SEM vtable: próximo
   crash foi `call [vtbl+0x10]` no dono-fantasma (vptr zero). Com
   `purecall_fake_vtbl` (vfuncs retornam 0) as leitoras pulam o elemento
   — a neutralização FUNCIONA onde alcança; o defeito era cobertura.
6. Hook NÃO instalado por falta de entrada em `va_sentinels`:
   `validated_va` retorna 0 pra endereço não cadastrado — sempre conferir
   o log "hook ativo" na sessão antes de culpar a lógica.
7. Assinatura errada do hook (4 args numa função de 5): jogo crashava na
   abertura do mundo — conferir SEMPRE o número de parâmetros no disasm.

### O que a raiz prova

Remendar UMA leitora não resolve — existem pelo menos duas estruturas
por nó (array + container ordenado) e pelo menos três leitoras
(varredura `0x12B85A0`, busca `0x1296689`/comparador `0x12B9480`, dump).
O elemento morto nasce quando o jogo destrói o dono no engate e deixa o
elemento linkado nas duas estruturas. A correção tem que impedir o dono
de morrer OU varrer as DUAS estruturas de TODOS os nós na hora em que o
dono morre — antes de qualquer leitora rodar.

## 7. RESOLVIDO — correção que funcionou (sessão confirmada pelo usuário)

A chave foi cobrir o **ponto único** por onde TODAS as leitoras da
estrutura ordenada passam, em vez de remendar leitora por leitora.

Implementado em `trailer_loadstate.cpp` / `trailer_coupling.cpp`:

1. **Hook no resolvedor `0x14096AAB0`** (`hk_lane_obstacle_resolve`):
   toda vfunc da vtable `0x142412E60` (comparador `0x12B9480`,
   `0x12B94D0`, `0x12B9550` etc.) chama esse resolvedor pra transformar
   handle em elemento ANTES de ler `elemento+0x88`. O hook olha o
   elemento resolvido: dono morto/nulo -> devolve um elemento-fantasma
   estático cujo dono é `lane_dead_owner` (vptr = `purecall_fake_vtbl`,
   vfuncs retornam 0). As leitoras leem flags zeradas e PULAM o
   elemento — nunca mais leem `addr=0x10`. Um guarda só cobre todas as
   leitoras da estrutura ordenada.
2. **Hook na varredura `0x1412B85A0`** (`hk_lane_obstacle_scan`):
   assinatura correta `(nó, flags, lo, hi, float* out)` — 5 args, o 5º
   é ponteiro de saída escrito em `0x12B8A36`. Antes de chamar a
   original, o hook varre `nó+0x20/+0x28` e NEUTRALIZA (não remove)
   elementos com dono morto: `+0x88 = lane_dead_owner`. Neutralizar em
   vez de remover porque o mesmo elemento vive no container ordenado
   `nó+0x18` — tirar do array não tira da árvore.
3. **Registro de containers reais**: os hooks chamam
   `lane_track_container`/`lane_track_node` — a poda
   (`prune_lane_containers` + `lane_kill_owner`), que era código morto
   desde que o hook do insert foi removido, volta a ter o que limpar e
   roda no `update()` enquanto `lane_prune_frames` > 0.
4. **Desparque cobre ficha velha**: o trigger do desparque inclui
   `parked_rec` além dos bytes `parked` — limpa a ficha mesmo quando o
   jogo já zerou os flags (o caso do c0 com `rec` vivo e `records=0`).

Endereços cadastrados em `va_sentinels` com assinatura de prólogo
(sem entrada = `validated_va` retorna 0 = hook nunca instala).
Log de instalação: `lane_obstacle_resolve hook ativo` /
`lane_obstacle_scan hook ativo` na init.

Resultado medido: usuário atrelou/desatrelou T0/T1/T2 repetidamente
sem crash — confirmado em sessão real.

### Estado anterior (revertido pelo usuário antes da correção)

O usuário reverteu manualmente todas as tentativas acima (hook de
varredura removido, fila de fichas removida, `parked_record_unlist`
direto no desparque + `lane_kill_owner` + `lane_prune_frames=40`,
`notify_decoupled`+`traffic_detach_tail` no desatrelamento e em
`native_park_detached`, engate só zera o byte `parked`).

## 8. PENDENTE — dois apontamentos menores da sessão limpa (18:17)

Sessão sem nenhum crash, mas o log ainda mostra:

### a) 2 purecalls engolidos (18:18:16, logo após um reengate)

- `exe+0x1295554`: `call [rax+0x40]` (vfunc 8) num binding de
  obstáculo cujo objeto foi destruído — vptr aponta pra tabela base
  com `_purecall` no slot. O tracer devolve `purecall_fake` e o jogo
  pula o objeto. Mesma família dos elementos órfãos: o binding morre
  no engate e continua linkado.
- `exe+0x12959E2`: idem, outro ponto de iteração.
- Caminho de reparo (não aplicado): quando a poda/varredura acha um
  elemento morto, conferir o binding `elemento+0x80 -> +8`; se algum
  slot da vtbl dele == endereço do `_purecall` do jogo, trocar o vptr
  por `purecall_fake_vtbl` — a vfunc devolve nulo e o jogo pula o
  objeto sem disparar purecall.

### b) custo "chain" oscilando 0.9–3.0 ms/frame

- O slot "chain" mede `refresh_trailer_chain` (roda ~a cada 100ms).
  Dentro dele, `orphan_teardown_tick` chama `lane_kill_owner(t)` pra
  CADA carreta engatada do canon em TODO tick — varre todos os
  containers de pista fazendo scan de 96 campos por elemento
  (`lane_owner_refs`), mesmo quando nada morreu = reparo rodando sem
  necessidade.
- Caminho de reparo (não aplicado): só rodar a varredura por dono
  dentro da janela `lane_prune_frames > 0` (os 40-120 frames armados
  no desengate/desparque) — reparo só quando necessário.

## 9. EM CORREÇÃO — guarda de lane não segue o checkbox

Medido na sessão 21:15-21:18 (checkbox "Sistema de Desacoplamento"
DESLIGADO, plugin update próprio ~0.03ms/frame):

- `lane el` (elementos em containers de pista rastreados) subiu de 14
  para 44 enquanto havia carretas soltas; o tempo do hook `upd`
  subiu de ~8 para ~17-20 ms/s acompanhando.
- `hk_lane_obstacle_insert` / `hk_lane_obstacle_resolve` /
  `hk_lane_obstacle_scan` NÃO consultam `lane_guard_on` nem
  `cargo_restore`: registram nó, conferem elemento morto e podam
  listas em TODA chamada — trabalho permanente mesmo sem carreta
  solta e mesmo com o sistema desligado.
- `lane_guard_on` é armado em `mark_detached`/`mark_plugin_detached`
  e desarmado no `update()` quando `!any_loose && dead==0` — a flag
  existe e o ciclo de vida está certo, mas os hooks ignoram a flag:
  guarda ligada ou não, o custo roda.
- Com o checkbox desligado o usuário esperava jogo nativo; o que
  sobra: cada chamada de scan/resolve/insert das pistas paga a
  varredura de sanitização em cima de listas que crescem com cada
  carreta parada.

Caminho de reparo (aplicado): os três hooks de lane checam
`lane_guard_on` — desligada, chamam a original direto sem registrar,
sem podar, sem conferir dono. A guarda arma NO COMANDO de desatrelar,
antes da soltura: tecla do plugin (`queue_op` disconnect),
`disconnect_now`, despachante nativo (`hk_detach_dispatch`) e unlink
(`hk_trailer_disconnect`), além do `mark_detached` já existente.
Desarma no reengate quando não houver mais carreta solta e pendente
morto (lógica `any_loose`/`lane_dead_pending`). O `update()` exige
`cargo_restore` para armar a guarda e para rodar a poda — checkbox
desligado = guarda nunca arma = jogo nativo.

## 10. EM CORREÇÃO — elo trailer_slave apontando pra pilha no teardown

Duas ocorrências (21:17:07 e 21:57:36), mesmo padrão:

- AV `0xC0000005` em `exe+0x775327` — `mov [rcx+0x2A0], edi`: o
  andador nativo de desconexão (mesma família do item 6) escrevendo
  `+0x2A0` num elo da cadeia.
- `rcx=0x7ffff63e0448` = endereço de PILHA, não objeto de carreta —
  alguém escreveu um ponteiro de stack como elo `trailer_slave`.
- Pilha do AV: `block_camera_zero_teleport` → `hk_trailer_disconnect`
  → `hk_pre_detach` → `sever_parked_claims` → `hk_detach_dispatch` —
  a sequência de desengate interno do teleporte de câmera 0.
- Engolido pelo `__try` ("checkbox off") — não fatal, mas o teardown
  aborta no meio: flags `+0x2A0` ficam por escrever nos elos restantes.
- Contexto: usuário fez 3 teleportes por F9/câmera 0; o AV estourou
  no desengate seguinte, quando o andador percorreu a cadeia.

Mecanismo confirmado pelo usuário: carga/descarga DESTRÓI a carreta e
RECONSTRÓI outra — todos os registros do plugin (canon, claims,
elos salvos) passam a apontar pra memória morta/reciclada. Com o
checkbox desligado a manutenção que renova esses registros não roda,
mas o caminho desligado do desengate AINDA rodava
`sever_parked_claims()` + `sanitize_slave_walk()` — leem `trailer_slave`
de objeto reciclado = leem lixo; se o lixo passa nos testes (legível,
vtable plausível), vira elo salvo e depois é restaurado na cadeia de
uma carreta viva. O mesmo ramo servia `load_game_active()` — durante a
reconstrução em si.

Correção parcial: caminho desligado/loading de `hk_trailer_disconnect`
e `hk_detach_dispatch` virou passagem nativa pura (só o `__try` de
proteção fica) — nada de canon velho, nada de escrita. E o
`pump_sim_heartbeat()` subiu pra antes do corte do checkbox — `hb=0/s`
+ `sd` infinito no log era o batimento nunca disparando com checkbox
desligado.

Repetição (22:29:03): o AV disparou de novo, mesma assinatura, com o
caminho de desengate já limpo — o elo podre não vinha do sever/sanitize.
Sessão: desengate nativo de T0, doca, teleporte por-carreta, carga,
teleporte do conjunto. Restava um escritor ativo no caminho desligado:
`hk_trailer_teleport` chamava `teleport_safe` com o checkbox OFF e em
`load_game_active()` — o esconde/restaura de `trailer_slave` rodava
justo na janela destruir/reconstruir, e o restaura às cegas podia
reescrever elo velho/morto na corrente.

Correção 2: `hk_trailer_teleport` no caminho desligado/loading agora
chama o original direto — sem hide/restore, sem gate.

## 11. Tabela de conexões nomeadas (A0..A19, B..T) — implementada

`trailer_connections.hpp` guarda um `conn_record` persistente por
conexão: `left` (cavalo na série A; carreta da frente na série B+),
`right` (carreta de trás) e `st` ∈ {off, parked, linked}.

- `linked` = junta física real (`joint` vivo + `parent == left`).
- `parked` = claim do plugin (elo slave sem junta, peça visível).
- `off` = sem relação.

`conn_observe` (fn277) lê junta/pai de cada canon e grava o físico;
`conn_commit` (fn278) reescreve a corrente a partir da tabela —
`rebuild_parked_claim_links` (fn101) virou observe + commit, mesmas
escritas de antes. sever/restore e o visibility pin sincronizam a
tabela (`conn_mark_parked`, fn279); `reset_trailer_state` zera
(`conn_reset`, fn280).

Rotação da cabeça: a A linkada decide a "T0" visível — A1 ativa põe
T1 como `game_actor_trailer`/`truck+0x1580` e as conexões adjacentes
formam a corrente física; a ex-T0 fica parked atrás.

## 12. EM CORREÇÃO — estouro de pilha no engate A2 (ciclo na cadeia +0x68)

Ocorrência 23:34:34, fatal (jogo fechou). Sequência do usuário:

1. desatrelou T2; desatrelou T0 do cavalo — T0+T1 ficaram engatadas
   entre si como par estacionado;
2. atrelou o cavalo direto em T2 (conexão A2) via
   `native_attach_via_slot` (fn172) — a junta `0x27fe92f44a0` foi
   criada com sucesso;
3. ~90ms depois: AV `0xC00000FD` (stack overflow) em `exe+0x124876D`,
   `addr=0xb8c2893fd8` (página de guarda da pilha).

### A função que explodiu (disasm confirmado)

`exe+0x1248750`: função AUTO-RECURSIVA — lê `[rcx+0x68]` como próximo
elo, chama a si mesma (`call 0x141248750` em `+0x76D`, retorno
`+0x772`), soma `[rbx+0x74]` por elo e `[rbx+0x5c]` no elo final.
Pilha do AV cheia de `exe+0x1248772` = a recursão nunca terminava =
**ciclo na cadeia `+0x68`**.

`+0x60`/`+0x68` são as arestas físicas das carretas (as mesmas que
`sever_phys_claims`, fn184, e `sanitize_phys_walk`, fn181, tratam —
a checagem de ciclo do `phys_link` hook também sobe por `+0x60`).

### O furo confirmado no código

Toda chamada nativa de engate protege as DUAS cadeias antes do jogo
andar nelas — `run_native_connect` (fn186) e `hk_detach_dispatch_raw`
(fn191) chamam `sever_parked_claims()` **e** `sever_phys_claims()`.

`native_attach_via_slot` (fn172) — o caminho "atrelar direto numa
carreta específica" usado no A2 — chama **só** `sever_parked_claims()`:
os elos físicos `+0x60/+0x68` velhos do par T0+T1 e da T2 solta ficaram
pendurados; quando o jogo inseriu T2 na árvore física o ciclo fechou.
`native_detach_via_slot` (fn171) tem o mesmo furo — só saneia a cadeia
slave, nunca corta aresta física. E nenhum dos dois caminhos por-slot
tem `__try` — a falha subiu direto e derrubou o jogo.

Falta provado por diff de caminhos: fn186/fn191 fazem as duas
limpezas; fn172/fn171 não.

### Hipótese da formação do ciclo (não provada)

Elos `+0x68` velhos sobrevivendo à reorganização (T0+T1 engatadas
entre si, T2 solta) — ao inserir T2 na árvore do cavalo, um elo
apontava de volta pra nó já dentro da árvore. `sever_phys_claims`
hoje corta elo pra si mesmo e elo pra carreta sem junta — um ciclo
entre carretas ENGATADAS passaria batido mesmo com a limpeza.

### Caminho de reparo (aplicado)

1. `native_attach_via_slot`/`native_detach_via_slot` agora chamam
   `sever_phys_claims()` antes do nativo, igual fn186/fn191, e a
   chamada roda em lambda `__try`/`__except` com `_resetstkoflw()`
   (mesmo padrão do `run_native_connect`) — falha vira aviso, não
   crash.
2. `sever_phys_claims` agora percorre cada direção da cadeia
   `+0x60`/`+0x68` com conjunto de visitados (até 96 nós): o elo
   que aponta pra nó já visto fecha um laço e é cortado com aviso
   no log — nenhum ciclo chega ao andador recursivo do jogo,
   engatada ou não.

### Recorrência 23:55:23 — ciclo criado DENTRO do engate nativo

Segunda morte, mesmo padrão: processo morreu instantâneo, sem dump
(overflow de pilha mata antes do tratador rodar). A linha do tempo
no log do plugin prova o mecanismo:

- 23:54:33 — engate A1 (cavalo→T1) via `native_attach_via_slot`, já
  com o sever físico pré-chamada ativo. ~30ms depois o hook
  `hk_joint_walk` logou `cyclic node 0x16bcad159c0 - skipped
  subtree`: **a junta nativa criou um nó em ciclo na árvore física
  por dentro da própria chamada** — depois da nossa limpeza.
- O ciclo sobreviveu ~50s protegido só pelo guard do `joint_walk`.
- 23:55:23 — tecla T1→T2 (`connect_now` → vfunc_connect): a junta
  `0x16f9b95a990` foi criada certa (dump post-connect íntegro) e o
  processo morreu logo depois — um andador recursivo nativo sem
  guarda (família `exe+0x1248750`, acumulador da cadeia `+0x68`)
  rodou por conta própria sobre o ciclo.
- Sintomas correlatos reportados: T2 travada por "junta física"
  fantasma (não aceitava dolly — a T2 estava presa na árvore
  corrompida) e um cabo extra aparecendo no cavalo (mesma
  corrupção de elos).

Conclusão: cortar elos velhos ANTES da chamada não basta — o laço
nasce dentro do código do jogo. Correção 2 aplicada:

3. `sever_phys_claims()` também DEPOIS de cada chamada nativa de
   cadeia (fn171, fn172, fn186/run_native_connect, fn191) — o laço
   que o jogo acabou de criar é cortado na saída.
4. `sever_phys_claims()` agora roda na manutenção contínua
   (`rebuild_parked_claim_links`, fn101) — qualquer ciclo de outra
   fonte morre no próximo tick (~100ms), não vive 50s.

### Recorrência 00:12:43 — rodovia, mesmo estouro

Terceira morte, mesma assinatura `0xC00000FD` em `exe+0x124876D`,
~20ms após engate A2 (cavalo→T2) na rodovia. Na empresa funciona —
na rua as carretas soltas ficam com física integral (na doca o jogo
as registra como estacionadas e simplifica a física), então a
corrente física corrupta só é percorrida na rua.

Achados decisivos nesta sessão:

- Os nós da corrente fatal vivem numa arena própria (`0x2be09...`)
  — NÃO são objetos de carreta. Os dtor-dumps mostram `e60` da
  carreta apontando pra essa arena (raiz da cadeia de nós).
- O acumulador `exe+0x1248750` segue `nó+0x68` — é chamado de 50+
  pontos do exe; proteger chamador a chamador é inviável.

### Recorrência 00:23:30 — startup, AV em outra função

Com a build que substituía o acumulador por versão iterativa
(`hk_phys_sum`), o jogo morreu na carga: `0xC0000005` em
`exe+0x12E8852` (`movss [rdx+8]` com rdx=0 — ponteiro de saída
nulo), segundos depois do `attach dispatch` da carga do mundo. A
abordagem de substituir a folha recursiva foi rejeitada/revertida.

### Correção 3 — controle no ponto de nascimento do elo (vigente)

Disasm do `phys_link` (`0x1412BE360`, já hookeado como fn179)
revelou a estrutura exata — é o ÚNICO criador de elos `+0x68`:

    phys_link(parent, child):
      child->+0x60  = parent        elo de subida
      parent->+0x68 = child         elo de descida (o que o
                                    acumulador percorre)
      parent->+0x74 = distância     o float da soma
      parent->+0x210 = child
    phys_link(parent, null):        desfaz — limpa +0x68/+0x74 do
                                    pai e +0x60 do filho velho

O defeito nativo: em reparent (carreta muda de pai sem unlink),
a função sobrescreve `child->+0x60` mas NÃO limpa `+0x68` do pai
antigo — a carreta fica em duas correntes ao mesmo tempo e, quando
elas se cruzam, o laço fecha. `hk_phys_link` (fn179) agora, antes
de deixar o link nativo rodar:

5. Se `child->+0x60` aponta pra pai velho cujo `+0x68` ainda é o
   filho → corta o elo velho (`+0x68=null`, `+0x74=0`) — reparent
   limpo, filho numa corrente só.
6. Percorre a corrente `+0x68` do filho com conjunto de visitados
   (até 96): elo que aponta pro pai novo, pra nó já visto ou pra
   endereço morto → cortado com aviso no log
   (`phys link: stale/bad/cyclic edge cut`).

O elo cíclico não nasce mais — sem hook novo, sem tocar no
andador recursivo, sem varredura contínua de nós.

### Recorrência 00:35:43 — laço criado fora do portão do link

Quarta morte, mesma assinatura. A linha do tempo prova que o
guarda do `phys_link` funcionou (cortou dois elos velhos de
reparent reais, nos engates da T2 e da T1) MAS o laço nasceu por
outro caminho — o `joint_walk` viu o nó cíclico `0x1bed2bec090`
50ms depois do engate T2 e ele ficou vivo 54 segundos (o guarda
só pulava o galho), até o acumulador recursivo matar no engate
T1→T2. Provável criador: o remendo de corrente do "desengate
nativo pendente" (fn191 adia o unlink real pra uma fila — reescreve
`+0x68` fora das nossas janelas de limpeza).

Correção 4 aplicada:

7. `hk_joint_walk` (fn178) — ao detectar o nó na própria trilha,
   segue a corrente `+0x68` do nó com conjunto de visitados e
   corta a aresta que fecha o laço (`joint walk: cyclic chain
   edge cut`). O ciclo morre no mesmo frame em que é detectado
   (~1ms) em vez de viver ~50s até o acumulador tropeçar nele —
   não importa qual escritor criou o elo podre.

### Recorrência 00:57:09 — crash imediato após o corte pré-chamada

Quinta morte. Padrão novo e suspeito: os últimos três crashes
(00:44:10, 00:35:43, 00:57:09) morreram 15-120ms DEPOIS do
`stale parent edge cut` disparar. Hipótese: cortar o elo velho
do pai antigo ANTES da chamada nativa muda o estado que o remendo
interno do jogo espera — ele pode religar o rabo da corrente no
lugar errado e FECHAR o laço que a correção tentava evitar.

Correção 5 aplicada:

8. O corte de elo velho de reparent foi movido pra DEPOIS da
   chamada nativa no `hk_phys_link` — o jogo faz o remendo dele
   primeiro; se `old_up->+0x68` continuar apontando pro filho já
   reparentado (`child->+0x60 == parent`), aí sim corta (resto
   podre de verdade). Se o jogo resolveu sozinho, nada é escrito.
   Os cortes pré-chamada que restam são só os de ciclo
   comprovado (dup, back-edge pro pai, endereço morto) — esses
   precisam morrer antes ou o elo ruim nasce.
9. Diagnóstico novo no tracer (fn235): quando o crash é no
   acumulador `exe+0x12487xx`, despeja a corrente `+0x68` real
   dos nós nos registradores (`phys chain A->B B->C C->A LOOP`)
   — só leitura, mostra o desenho exato do laço.

Sessão 10:04 — crash no engate T1→cavalo (não era T2: o defeito
é genérico em qualquer posição). O dump pré-engate provou que
`trailer+0x60/+0x68` são campos compartilhados (idênticos nas 3
carretas, +0x68 aponta pra dentro do exe) — NÃO é a corrente
física. O `sever_phys_claims` vigiava a estrutura errada.

Disasm do `phys_link` (0x1412be360) revelou a semântica real:
`child->+0x60 = parent; parent->+0x68 = child` — sobrescreve
meio-elos e NUNCA limpa os viúvos (pai antigo do filho, filho
deslocado do pai, rabo do filho). Na rua a varredura do jogo
fica pendente, os viúvos sobrevivem, e o reengate de qualquer
carreta sob seu antigo "dono viúvo" fecha ciclo 2-nós
(ex.: viúvo T1→T2 + link T2→T1 = laço). Genérico — toda
posição da lista.

Correção 6 aplicada:

10. `hk_phys_link` agora faz reconciliação de reciprocidade
    pós-chamada nos 4 nós tocados (parent, child, pai antigo do
    filho, filho deslocado do pai): se A.dn=B mas B.up≠A, o elo
    é viúvo comprovado → corta dn e zera +0x74; se A.up=B mas
    B.dn≠A → corta up. Elo viúvo morre no instante em que nasce,
    em qualquer posição. Elos recíprocos (rabo legítimo) são
    preservados — a lista é atualizada, não destruída.

Sessão 10:28 — tráfego DESLIGADO: engate T1→cavalo SOBREVIVEU
(reconciliação cortou 2 elos viúvos reais). Depois engate T1→T2
completou e o jogo morreu com ASSINATURA NOVA: ACCESS_VIOLATION
lendo ponteiro rasgado 0x20900000000 num container refcounted,
na pilha do teardown adiado (exe+0x4DFA05 ~ desengate nativo
pendente) — não mais estouro de pilha.

Causa raiz encontrada no dump post-connect: c0.slave = c2
(claim parked velho) ENQUANTO c2.parent = c1 (link real T1→T2).
A T2 ficou em DUAS listas slave — o teardown adiado processou
ela duas vezes com pais diferentes → ponteiro rasgado. O claim
velho só seria limpo na manutenção do frame seguinte — tarde.

Correção 7 aplicada:

11. `drop_stale_claims` (fn236): varre o canon e mata todo
    slave-edge cujo alvo ganhou junta e tem parent diferente —
    claim órfão de trailer que virou membro real de outra
    corrente morre NA HORA (marca conexão off na tabela).
    Chamada no fim de todo engate (antes do teardown adiado
    poder rodar) e dentro da manutenção por-frame, entre
    observe e commit.

Sessão 11:07 — crash idêntico ao anterior (mesmo AV, sem dump
novo — crash.txt ficou velho em 00:23). Morte ~20ms após os
cortes manuais de elo viúvo → a suspeita endureceu: anular
campos na mão briga com o remendo adiado do jogo.

Correção 8 aplicada:

12. `hk_phys_link` não escreve mais nulo nos elos. Antes do
    link nativo, chama o PRÓPRIO `phys_link(nó, nullptr)` —
    a forma de deslink do jogo (trunca pai.dn, zera filho.up
    e a distância, com o bookkeeping interno de +0x88/+0x210):
    (a) no pai antigo do filho, se ele ainda aponta pro filho
    — splice-out nativo da posição velha; (b) no pai novo, se
    seu slot dn está ocupado por outro nó — libera o slot. O
    link nasce sobre terreno limpo usando o vocabulário do
    próprio jogo, sem escrita manual.

Sessão 11:17 — T1 atrelou e desatrelou limpo (34s). T2 morreu
logo após o deslink nativo — morte instantânea, sem dump.

Causa encontrada no disasm: o próprio phys_link chama o
somador recursivo 0x141248750 (anda +0x68) quando o elo novo
fica não-nulo. O jogo recria elos velhos no meio do engate
(visto na T1: elo desfeito foi religado e o joint_walk
capturou). Na T2 o círculo existia no instante do link →
somador estourou a pilha dentro do phys_link, antes de
qualquer guarda.

Correção 9 aplicada:

13. `hk_chain_accum` hooka o somador 0x141248750 e substitui
    a recursão por iteração com detecção de ciclo — mesmo
    resultado em corrente sã; elo em círculo devolve soma
    parcial em vez de estourar a pilha. Genérico para todas
    as posições.

Sessão 11:24 — PROVA DEFINITIVA via WER/minidump:

- Windows Error Reporting: falha em br_controle_de_carretas.dll
  +0x41DC3, código 0xc0000005 — escrita de pilha no prólogo de
  função com frame de 0xbe0 bytes = pilha esgotada.
- Minidump (CrashDumps/eurotrucks2.exe.4436.dmp): o retorno
  exe+0x1248772 aparece 21.427 vezes na pilha = o somador
  recursivo 0x141248750 recursou ~21 mil níveis num ciclo
  +0x68. Nossa função foi chamada no fundo da pilha e morreu
  ao escrever o próprio frame.
- O somador tem 49 call sites no exe — qualquer um pega a
  corrente cíclica transitória do engate = morte instantânea.
- CAUSA RAIZ da não-correção anterior: o hook do somador nunca
  instalou porque 0x141248750 não estava na tabela de
  sentinelas — validated_va devolveu 0 em silêncio.

Correção 10 aplicada:

14. Sentinela 272 para 0x141248750 + `hk_chain_accum` (237):
    iteração substitui a recursão; elo que fecha círculo é
    cortado (dn=null, +0x74=0 — mesmo estado do unlink nativo)
    e devolve a soma parcial. Cobre os 49 call sites de uma
    vez — ciclo transitório deixa de ser fatal.

Sessão 11:43 — crash pegado pelo nosso handler (2x, mesmo
endereço): AV em exe+0x12E8852 `movss [rdx+8]` com rdx nulo.

PROVA no disasm: a função 0x1412E8740 escreve [rdx] no início
com sucesso (rdx chegou válido), depois chama o somador
0x141248750 e usa rdx na volta. O somador nativo é asm que só
toca rcx/xmm0 — o jogo conta com rdx preservado. Nosso detour
em C++ suja rdx (e demais voláteis) → crash nosso, não do
jogo.

Correção 11 aplicada:

15. `hk_chain_accum` agora entra via trampoline asm emitido
    em página executável: salva rax/rdx/r8-r11 + xmm1-5,
    chama a lógica C++, restaura tudo. Contrato de
    registradores idêntico ao somador nativo — nada além de
    rcx e xmm0 muda para o chamador.

Sessão 11:52 — a sequência completa de ~8 operações sobreviveu
(o somador iterativo funcionou: a corrente transitória deixou
de ser fatal). O crash migrou para a LISTA DE OBSTÁCULOS DE
PISTA — a "lista do tráfego" que o usuário suspeitava: o
reordenador 0x1412B7A20 iterou os elementos e leu [owner+0x10]
de um elemento cujo dono era ponteiro selvagem (WER:
exe+0x12B7B13).

Correção 12 aplicada:

16. `lane_element_dead`: sonda `owner+0x10` dentro do __try
    existente — dono selvagem/não-mapeado marca o elemento
    como morto. Custo zero (uma leitura de memória, sem
    syscall). Todas as podas passam a remover esse elemento.
17. `hk_lane_obstacle_sort` (281) hooka o reordenador
    0x1412B7A20 — chamada original dentro de __try; falha
    devolve 0 ("sem obstáculo", contrato testado pelos 16
    call sites) e arma a janela de poda. Cobre a janela entre
    o elemento morrer e a próxima poda.

Sessão 12:21 — tráfego ZERO: sucesso total
~24 operações (7 engates, 17 desengates) em ~3,5min, ordens
variadas em todas as posições. Proteções dispararam e
seguraram: `chain accum: cyclic edge cut` cortou círculo real
(exatamente o que matava instantâneo), `stale parent edge cut`
cortou viúvos. Fim = scs_telemetry_shutdown (fechamento
normal, sem dump). O que segurou: reconciliação de
reciprocidade (correção 6) + somador iterativo com corte de
ciclo (10/11) + drop_stale_claims (7) + poda de pista (12).

Sessão 13:33 — tráfego LIGADO: crash no engate T2→cavalo
Sintomas em ordem: desengates OK; engate T1 com tráfego =
queda de FPS (health 13:33:09: fps=19.6, worst=109ms,
lane el=111 own=25); desengate T1 = caiu mais; engate
T2→cavalo = morte. Linha do tempo do engate fatal:
junta criada -> stale edge cut -> `chain accum: cyclic
edge cut` -> 4ms depois AV em exe+0x12B8B64 (`mov ecx,
[rax+0x10]` — comparador da lista de pista com owner nulo,
mesma família do 0x12B7B13 mas OUTRO leitor, não coberto
pela poda) -> o handler continuou execução -> flood de AVs
byte-a-byte em DLL externa (scanner 0x7FF950002A96 lendo
buffer de descritor exe+0x222BBF0 além do fim) -> morte
real em exe+0x11E0D8 (container refcounted,
rcx=0x20900000000 — MESMO valor selvagem do crash de T1→T2)
na pilha do teardown adiado.
Leituras: a poda de pista funciona mas cobre só o scan e o
reordenador — com tráfego ligado a lista tem 111 elementos e
OUTROS leitores (o comparador 0x12B8B64) tocam elementos
mortos fora das janelas de poda. A queda de FPS antes do
crash sugere a lista já crescida/corrompida antes do engate
fatal.

Correção 13 aplicada — fechamento da família de pista:

Mapeamento completo: a lista de obstáculos tem exatamente 4
pontos de entrada (insert 0x12B7DD0, sorter 0x12B7A20, scan
0x12B85A0, comparador 0x12B8AC0). Os 3 primeiros já tinham
guarda. O comparador era o buraco: chamado só pela consulta
"obstáculo mais próximo" da IA (fn 0x12D5FC0, call site
0x12D6005) — roda por veículo de tráfego por tick, por isso
só mata com tráfego ligado.

18. `hk_lane_obstacle_nearest` (282) + sentinela 274 hooka o
    comparador: original dentro de __try; falha devolve 0
    ("sem obstáculo" — o chamador testa rax e tem caminho de
    fallback legítimo) e arma a janela de poda. Assinatura
    completa de 6 args (rcx ctx, edx flags, xmm2/3 floats,
    stack arg5 ptr, arg6 byte — o callee lê [rbp+0x88]).
    Cobertura da lista de pista agora é total.

Sessão 14:02 — tráfego LIGADO: conjunto completo montou sem
crash nos engates (todas as posições). Dois problemas novos:

A. FPS caiu 58→8 progressivamente. Health: lane el 0→106
   monotônico, jw 181→984/s, custo por chamada de
   update/render 0,3→22ms. O jogo processa a lista inchada
   por frame por veículo — trabalho cresce com a lista.

B. Após o conjunto completar, `lane_guard_on` desarmou
   (any_loose=false + dead_pending=0). 14s depois um dono
   de elemento morreu e o borbulhador 0x12B7E3B (5º leitor,
   chamado pelo mesmo vfunc 0x1315130 do insert — não
   hookado) leu [rax+0x10] com rax=0 -> crash.

Causa de desenho: elementos mortos na lista de pista não
vêm só das nossas carretas — o tráfego gera os seus
continuamente (despawn, troca de pista). Armar a guarda só
com carreta solta deixava ela desligada justo quando o
tráfego continuava sujando.

Correção 14 aplicada:

19. Guarda armada permanente enquanto cargo_restore ligado
    — nunca desarma por conjunto completo. Custo ~zero com
    lista limpa (um teste atômico por chamada).
20. `prune_lane_containers` roda TODO frame (janela fixa
    removida) — elemento morto morre em <1 frame, antes de
    qualquer leitor tropeçar. A poda já era incremental com
    orçamento — cadência por-frame é segura.
21. Health agora separa mortos: `lane el=N own=N mortos=N`
    — mede se o inchaço é cadáver (poda resolve FPS+crash)
    ou registro legítimo duplicado (precisa mapear o
    caminho de registro, `ins=0/s` sugere que não passa
    pelo insert hookado).

## Sessão 14:33 — REGRESSÃO: correção 14 foi rejeitada (e estava errada)

Tráfego ligado: desatrelou o conjunto, atrelou T1 → morte
instantânea ~30ms após `stale parent edge cut`, sem dump e
sem AV logado (mesma classe das mortes antigas, e no
PRIMEIRO engate — pior que a build 14:02 que sobreviveu ~15
manobras). O código foi rejeitado pelo usuário.

### O que a sessão provou (fatos medidos)

- `mortos=0` em TODAS as linhas de health: a lista de pista
  cresceu 39→73 com ZERO elemento morto. A hipótese "inchaço
  por cadáveres" está REFUTADA — o crescimento é de elementos
  VIVOS (registro legítimo ou duplicado, não lixo).
- A queda de FPS aconteceu de novo (63→33) com a lista
  crescendo — correlato confirmado: custo por frame escala
  com o tamanho da lista (rend/upd ~250ms/s).
- `ins=0/s` persiste: elementos entram por um caminho que
  NÃO passa pelo insert hookado (provável: vfunc 0x1315130,
  que também chama o borbulhador não-hookado 0x12B7E3B).

### Por que a correção 14 provavelmente matou

- `prune_lane_containers` é um ESCRITOR: remove/move
  elementos dentro dos arrays da lista. Rodá-lo TODO frame
  na thread de update = escritor concorrente iterando os
  mesmos arrays que a thread de simulação lê/ordena durante
  engates e consultas da IA → estado rasgado → morte
  instantânea. O desenho antigo (podar só em janelas de 40
  frames após eventos) limitava essa colisão — a correção
  14 removeu o limite.
- Suspeito secundário: `hk_lane_obstacle_nearest` (282) —
  assinatura de 6 args inferida por disasm (arg6 byte lido
  de [rbp+0x88] no callee). Se o encaminhamento estiver
  errado, a primeira consulta da IA com tráfego corrompe.
  Não reprovado — mas desnecessário se a limpeza for feita
  na origem.

### Regras derivadas (NÃO violar de novo)

1. NUNCA escrever nas estruturas de pista a partir do loop
   de update por-frame — a poda só pode rodar serializada
   DENTRO de um hook de pista (mesma thread da chamada
   nativa) ou em janela curta armada por evento. Escritor
   livre por-frame = colisão garantida com os leitores
   nativos.
2. Guarda armada permanentemente É segura (só adiciona
   __try/leitura) — mas sozinha não resolve nada; só
   converte AV em retorno seguro nos leitores hookados.
3. O problema real do FPS é CARGA VIVA, não lixo: a lista
   cresce com elementos legítimos e todo leitor paga
   O(lista × veículos). Corrigir = achar QUEM registra
   (mapear o caminho de registro de obstáculo — começar
   pelo vfunc 0x1315130 e pelo que chama o insert e o
   borbulhador) e impedir registro duplicado/desnecessário
   das nossas carretas — não podar depois.
4. Validar assinatura de hook novo contra TODOS os
   call sites antes de instalar (o detour de 6 args foi
   especulativo — padrão já causou crash antes: somador,
   rdx).

============================================================
FALHAS ABERTAS — a lista do que falta resolver
(escrita após o mapeamento completo do disasm, 19/11)
============================================================

Modelo da lista de obstáculos de pista (PROVADO no disasm):

  elemento = ponteiro no vetor do nó; campos:
    el+0x80 = record { vptr, +8 obstáculo, +0x10 pos }
    el+0x88 = dono (objeto jogo — veículo/carreta)
    el+0x90 = posição na pista (float)
  nó de pista:
    +0x18 vetor principal { data+0x20, size+0x28, cap+0x30 }
    +0x48 vetor secundário { data+0x50, size+0x58 }
    +0x74 = flags de dirty (or 1 = re-sort, or 9 = alterado)
    +0x78 = próximo nó (cadeia); +0x10 bit 21 = flag de ramo

  As 3 operações da lista (chamadas DIRETAS, não vtable):
    REGISTRAR      = 0x1412B7A20 — cresce o vetor, insere o
                     elemento na posição ordenada. arg2 =
                     record do elemento, xmm2 = posição.
                     16 callers; um (0x1412C9FD1) registra
                     ~4 elementos por chamada = veículo se
                     registra em todos os nós que ocupa.
    REPOSICIONAR   = 0x1412B7DD0 — elemento já registrado
                     que mudou de posição; borbolha dentro
                     do vetor lendo el+0x88 (dono) e lendo
                     [dono+0x10] para o flag de direção.
    DESREGISTRAR   = 0x1412B8190 — remove por record ptr;
                     12 callers.

------------------------------------------------------------

F1 - CRASH: reposicionar lê dono morto/nulo
    Site: exe+0x12B7F6F = `mov ecx,[rax+0x10]` dentro do
    loop de borbolha de 0x12B7DD0; rax=[el+0x88]=0.
    Também já visto: exe+0x12B8B64 (mesmo padrão no
    comparador), exe+0x12B7B13 (dono selvagem).
    Causa PROVADA: elemento com dono morto permanece na
    lista (o dono morreu sem passar pelo desregistrar); a
    guarda estava DESARMADA no instante (F4) e o hook fn135
    chama a original sem __try mesmo com guarda armada.
    FECHA QUANDO: nenhum leitor de el+0x88→[dono+0x10]
    executa sem proteção E o dono morto não permanece na
    lista (a raiz: o caminho de morte do dono não chama
    desregistrar — mapear e reparar na origem).

F2 - CRESCIMENTO da lista sem devolução
    Medido: el 0->106 monotônico, own 12->33, ours=0,
    mortos=0 (TODOS VIVOS — a hipótese "cadáveres" morreu).
    ~3,2 elementos por dono = compatível com veículo
    registrando nos ~3-4 nós que ocupa.
    NÃO PROVADO qual das duas causas:
      a) acúmulo legítimo — carretas paradas na pista viram
         obstáculo, IA não passa, fila de tráfego cresce;
      b) vazamento — algum caller registra sem desregistrar
         (16 callers de registrar x 12 de desregistrar).
    FECHA QUANDO: medir taxa de registrar x desregistrar
    (contadores nas chamadas 0x12B7A20 e 0x12B8190).
    Se reg>>unreg = vazamento -> achar o caller desbalanceado.
    Se balanceado = fila de tráfego -> decisão de design
    (carreta parada na pista bloqueia a IA mesmo).

F3 - FPS 60->8 progressivo com tráfego
    Medido: fps 58->8; jw 181->984/s; upd/rend ~250ms/s.
    Correlação com F2 confirmada; CAUSA não separada —
    pode ser a lista inchada (custo por leitor x veículo)
    OU o próprio volume de veículos acumulados (fila).
    FECHA QUANDO: F2 resolvido e custo por frame medido
    antes/depois. Não corrigir "por cima" sem a medida.

F4 - GUARDA desarma e reabre os leitores
    Medido: `lane guard: desligado` às 14:02:15, crash 14s
    depois. A política arma só com carreta solta — mas
    donos de elementos morrem pelo TRÁFEGO continuamente,
    inclusive com o conjunto completo.
    FECHA QUANDO: política de armamento cobrir a janela
    real de risco (enquanto o sistema estiver ativo) SEM
    poda por-frame — a combinação poda-contínua + guarda
    permanente já foi tentada e REJEITADA (regressão F-A).

------------------------------------------------------------

REGRESSÕES — abordagens já rejeitadas, NÃO repetir:

F-A) Poda a cada frame a partir do loop de update
     (correção 14): prune_lane_containers é ESCRITOR —
     iterava/mutava os vetores 60x/s na thread de update
     enquanto a simulação lia os mesmos vetores durante
     engates. Resultado: morte instantânea no 1º engate
     (sessão 14:33). Poda só serializada dentro de hook
     ou em janela curta armada por evento — como sempre foi.

F-B) Hook do comparador 0x12B8AC0 com 6 args inferidos
     (fn282): assinatura especulativa, rejeitada. Antes de
     reinstalar, validar TODOS os call sites.

F-C) Censo mortos=/guarda permanente junto com F-A: o
     pacote inteiro foi rejeitado junto. mortos=0 medido
     está correto como DADO, mas as peças rejeitadas não
     voltam sem aprovação explícita.

INSTRUMENTAÇÃO que enganou (corrigir quando medir F2):
  - `ins` conta só chamadas com guarda armada (contador
    depois do early-return) — ins=0/s durante crescimento
    era guarda desligada, não "outro caminho".
  - registrar 0x12B7A20 não tem contador nenhum — era por
    ele que a lista crescia, invisível.

============================================================
ENGENHARIA REVERSA DO TRÁFEGO (19/11) — MODELO CORRIGIDO
============================================================

O modelo anterior ("el+0x88 = dono") estava ERRADO. O disasm
completo mostra:

ELEMENTO = o objeto ocupante inteiro (veículo/carreta/
obstáculo), com uma HANDLE de pista embutida. A handle tem
vptr próprio: { vptr, nó@+0x08, pos@+0x10 }. Offset da handle
dentro do ocupante depende da classe:

  estático (tipo 0xFFF): handle em el+0x80
    -> el+0x80 = vptr da handle
       el+0x88 = NÓ onde está registrado   (NÃO é "dono")
       el+0x90 = posição na pista (chave de ordenação)
  dinâmico:  handle em el+0x10 (el+0x18=nó, el+0x20=pos)

Nó de pista tem DOIS vetores: +0x18 = estáticos,
+0x48 = dinâmicos. O registrador escolhe pela vfunc de tipo
da handle (call [rax+8] -> 0xFFF = estático).

API nativa (chamadas diretas):
  registrar    0x1412B7A20  (nó, record, pos) — append
               ordenado; 16 callers TODOS dinâmicos (+0x10)
  reg. estático: wrapper 0x1412E7EF0 (handle, nó, pos) —
               44 callers; passa a handle inteira como record
  mover        0x1412E7F60  (remove+registra nó novo)
  reposicionar 0x1412B7DD0  — borbolha dentro do vetor;
               lê el+0x88 (nó) e [nó+0x10] bit21 = direção
  desregistrar 0x1412B8190  (nó, handle) — splice do vetor
  wrapper unreg 0x1408A54B0 — remove(nó,handle); handle.nó=0
  fábrica elem. 0x1412DC2F0 — aloca elemento, guarda no
               vetor privado do ocupante em obj+0x38/+0x40

Classes estáticas (vfunc type == 0xFFF): 6 vtables —
0x142410C70, 0x142412718, 0x1424128C0, 0x142413200,
0x1424133E0, 0x1424137D0 — todas gravadas em obj+0x80 por
construtores. Um deles (fn 0x14128E740) é objeto GRANDE
(+0x410/+0x428) = classe de veículo -> el PODE ser a
própria carreta/veículo.

CORREÇÃO DO CENSO (o que medíamos errado):
  el+0x88 é NÓ, não dono ->
    own=33 era "33 nós distintos", não donos;
    ours=0 era nó comparado com carreta — nunca bate;
    mortos=0 era legibilidade de nó+0x10 — sempre legível.
  -> os dados "todos vivos / não são nossos" eram inválidos.
  Forma certa de identificar: ler el (o ocupante) —
  is_trailer_obj(el) / comparar com canon.

CRASH 0x12B7F6F reinterpretado:
  rax=[el+0x88]=handle.nó=0 -> elemento cujo ocupante já se
  desregistrou (wrapper zera handle.nó) mas cuja entrada
  ficou ÓRFÃ no vetor. Órfão nasce quando remove() falha
  porque handle.nó já estava errado — ex.: corrompido por
  escrita externa.

NÓS SOMOS UM SUSPEITO DE CRIAR ÓRFÃOS:
  lane_kill_owner + os sweeps escrevem
  `el+0x88 = lane_dead_owner` = handle.nó do elemento.
  Se o elemento estiver VIVO (match errado do fuzzy
  lane_owner_refs, que compara nó~carreta por varredura),
  o desregistro nativo seguinte faz remove(sentinela) ->
  falha -> handle.nó=0 -> entrada órfã no vetor real ->
  crash no próximo leitor. O veneno não remove nada — só
  marca. Combinado com a observação do usuário (FPS cai A
  CADA MANOBRA, e lane_kill_owner roda em engate/desengate/
  rebuild), o padrão "órfão por manobra" é consistente.

RESSALVA sobre el=106: o censo só conta containers
rastreados (descobertos via hooks). Parte do crescimento
0->106 é a DESCOBERTA progressiva de nós, não registro
novo. Só medindo regxunreg + identidade de el se separa
vazamento real de mundo normal.

DIREÇÃO DE CORREÇÃO (a propor):
  1. Censo passa a classificar por el (ocupante):
     is_trailer_obj(el)/canon -> "ours" de verdade;
     handle.nó ilegível/zero + vptr inválido -> morto.
  2. PARAR de escrever em el+0x88 (handle.nó): o veneno só
     deve acontecer JUNTO do memmove (ou nunca — o memmove
     já remove). lane_kill_owner, como está, corrompe
     handles vivos e fabrica órfãos.
  3. O match de "elemento nosso" deve testar el (ocupante),
     nunca el+0x88 (nó).

=== TENTATIVA REJEITADA (2026-01): correção "+0x88/handle.nó" ===

O que foi feito (e revertido pelo usuário):
  - Censo passou a classificar por el (ocupante) em vez de own (el+0x88).
  - lane_kill_owner passou a remover via memmove em vez de
    escrever lane_dead_owner em el+0x88.
  - As escritas el+0x88=lane_dead_owner foram tiradas da
    poda, do sweep e do hook de scan.

Status: REVERTIDO integralmente pelo usuário — o código
voltou a escrever el+0x88=lane_dead_owner nos 3 pontos e
a identificar por el+0x88.

Aprendizado registrado: essa alteração, na forma feita,
não é a correção aceita. Não reaplicar.

PERGUNTA EM ABERTO para a próxima correção:
  A direção que o usuário pediu desde o início é outra:
  lista PERSISTENTE — cada carreta tem UMA linha e o
  plugin só troca o STATUS (conectada/desconectada/
  estacionada), em vez de inserir/remover/marcar.
  A engenharia reversa mostrou que a lista nativa é do
  jogo (um elemento por ocupante por nó). O que falta
  mapear: como o plugin mantém a PRÓPRIA lista de
  registros (a tabela de conexões A0-A4/B-E) sem depender
  de varrer/corromper a lista nativa.

=== CORREÇÃO APLICADA (2026-01): ficha correta + tabela nossa ===

Autorizado pelo usuário. Três mudanças:
  1. dead_els[256] — tabela do plugin com ponteiros de
     elementos mortos. lane_kill_owner marca aí, SEM
     escrever em objeto do jogo.
  2. Match por ocupante: el == carreta ou ponteiro pra
     carreta dentro de el (não mais o campo +0x88, que é
     o nó).
  3. Nos 3 pontos de splice (poda, sweep, hook scan) a
     escrita vira a ficha correta: el+0x88 = 0 — igual ao
     unregister nativo (entrada fora + nó zerado).
  4. lane_element_dead checa a tabela antes dos testes de
     validade.

Critério de fechamento: desatrelar/atrelar repetido com
tráfego sem crash e sem órfãos com nó zerado crescendo.

=== DIAGNÓSTICO CONFIRMADO (2026-01): multiplicador de obstáculos = gerenciador de fichas errado ===

Método: engenharia reversa do exe (disassembly), cruzada
com os logs de health (`lane el=` crescendo 15, 28, 42, 55)
e o dump do registry (`records=0` sempre).

O que acontece, provado peça por peça:

  1. A fábrica de fichas funciona. `validated_va` resolve
     corretamente pra `0x14070A160` (assinatura bate nos
     bytes do exe). Ela aloca registro de `0x198`, grava a
     carreta em `+0x178` e insere via `0x140B85700`.

  2. A inserção `0x140B85700` grava a ficha no slot
     "corrente" (`mgr+0x560`) e a registra na lista
     `mgr+0x740` via vfunc+8 do próprio registro. Ficha
     antiga que já está na lista FICA — não é removida.

  3. O gerenciador que a fábrica usa é o global em
     `exe+0x3715010`. TODAS as quatro fábricas de registro
     `0x198` encontradas (`0x14070A160`, `0x1407A26A0`,
     `0x1407D4760`, `0x1408008D0`) inserem nesse mesmo
     gerenciador — ele é o dono real das fichas.

  4. O plugin vigia a lista no gerenciador
     `exe+0x3714FB0` — que é o GERENCIADOR DE TRÁFEGO
     (sentinela 023, usado por traffic_detach_tail).
     Objeto diferente. A lista dele está sempre vazia:
     o log mostra `records=0` em todas as sessões.

  5. Resultado: a ficha nasce no gerenciador certo, mas
     `parked_record_listed` procura na lista errada ->
     nunca acha -> `wants_record` fica true -> retry a
     cada 20 ticks, pra sempre. Cada retry cria UMA ficha
     nova (alocação nova, ponteiro novo — a dedupe da
     inserção compara ponteiro de registro, não carreta)
     que fica acumulada na lista do gerenciador certo.

  6. Cada ficha registra a carreta como obstáculo de
     pista. O número `lane el=` sobe sem parar não porque
     o tráfego multiplica nem porque o jogo re-registra —
     é o PRÓPRIO plugin criando fichas duplicadas sem
     parar (3 carretas x ~3/segundo = o ritmo observado).
     O tráfego só agrava: cada veículo da IA consulta a
     lista de obstáculos a cada tick, então a lista
     inchada derruba o FPS e vira alvo do travamento.

Solução (autorizada): apontar a sentinela 252
(`parked_record_mgr`, hoje `exe+0x3714FB0`) para
`exe+0x3715010` — o gerenciador onde a fábrica insere.

Efeito esperado:
  - `parked_record_listed` acha a ficha na 1a tentativa ->
    retry para -> UMA ficha por carreta = 3 fichas total.
  - `parked_record_unlist` volta a remover de verdade no
    reengate (hoje opera numa lista vazia, não remove
    nada) -> sem obstáculo órfão.
  - `dump_native_registry` passa a mostrar `records=3`.

Nota: a suposição antiga do ENDERECOS.md
("traffic = parked_record = 0x143714FB0") está errada —
são dois gerenciadores distintos no exe atual. O
ENDEREÇOS.md deve ser atualizado junto com a correção.

Critério de fechamento: desatrelar na rua com tráfego ->
`lane el=` estabiliza em número pequeno, `records=3`, FPS
estável; reengate -> fichas removidas, sem órfãos, sem
AppHang.

=== CORRECAO APLICADA (2026-01): despachante nativo para T1/T2 ===

Diagnostico (verificado por diff main x atual + git log):

- Na versao main (1.61), o fim do `disconnect_now` chamava
  `native_detach_via_slot(trailer)` para TODA carreta sem junta —
  T0, T1 e T2 passavam pelo despachante nativo de desengate uma
  vez. E essa passagem que converte a carreta de "veiculo vivo do
  comboio" em "obstaculo estacionado registrado" pro trafego.
- No port 1.62 (commit 9403501) a chamada foi restringida a
  `parent_is_truck` — so a T0 (solta direto do cavalo) passava
  pelo despachante. T1/T2, soltas no meio da corrente, nunca
  eram convertidas: ficavam com o registro dinamico do comboio
  em movimento.
- Objeto dinamico e o que o jogo re-registra na pista a cada
  tick — medido: `lane el` subindo sem parar (15, 28, 42, 55),
  `mortos=0` (elementos VIVOS), ~5000 re-sorts/s a 85 bindings.
- O medo que motivou a restricao (o despachante limpa o `towed`
  do cavalo quando a carreta e do meio) ja estava tratado na
  propria funcao: ela recoloca `truck+0x1580` quando o nativo
  zera e ainda existe carreta engatada.

Correcao: removida a restricao `parent_is_truck` da chamada —
igual a main. A funcao ja usava os enderecos do patch atual
(`game_actor_trailer`, `truck+0x1580`, despachante via hook) e
ja rodava com `sever_phys_claims` antes/depois + `__try`.

Criterio de fechamento: desatrelar T1/T2 na rua com trafego ->
`lane el` estabiliza (1 obstaculo por carreta solta), FPS
estavel, reengates sem crash e sem despachante rasgar o
registro das carretas que continuam atreladas.

### Recorrencia da mesma sessao — conjunto travado apos soltar T2

Sintoma (usuario): soltou T2, o conjunto inteiro ficou ancorado
— o cavalo nao puxava. T2 estacionou certo; quem travou foi
T0/T1.

Causa provada no disasm do despachante `0x1405DBCE0`: ele nao
sabe que so uma carreta do meio foi solta — faz a contabilidade
completa de "cavalo sem carreta":

- `set_towed(truck,0,null)` 2x (`0x1406420B0`) -> `truck+0x1580=0`
- `pre_detach(truck,T2)` (`0x1408641B0`) -> solta a quinta roda
  (`[truck+0x1278]` vfunc+0x38) e liga `truck+0xE5C=1`
- `truck+0x14B8=0` (estado de engate -> "sem carreta")
- `truck+0x4C4=0` (progresso do engate)
- `distribute_towed_mass(truck,0,0)` (`0x140857880`)

Com `+0x14B8=0` e a garra aberta, o jogo trata T0/T1 como
desengatadas -> freios de mola travam -> conjunto parado.
Na 1.61 a manutencao por-frame reescrevia `+0x14B8=5` (aqui
morta: exige `*traffic_game_state==2`, le 5 em jogo).

Bug adicional encontrado: o restore de `+0x1580` lia o campo
DEPOIS do despachante — sempre nulo -> restore nunca disparava
(codigo morto, tambem na main).

Correcao: capturar ANTES do despachante (`kept_towed`,
`kept_state`, `kept_prog`, `kept_jaw`) e, quando a carreta que
ficou na roda continua engatada, restaurar tudo:
`+0x1580` re-pin + `+0x14B8`/`+0x4C4`/`+0xE5C` ao valor
capturado + `distribute_towed_mass(truck, kept_towed+0x4B8, 0)`
— mesmos valores que o engate nativo escreve (`+0x14B8=5`,
`+0x4C4=1.0`, massa de `carreta+0x4B8`).

============================================================
REGRA PERMANENTE — RECORD NUNCA E ESCRITO FORA DA T0
============================================================

Por ordem direta do usuario (fato definitivo, nao reabrir):

  - NAO existe possibilidade de colocar record em nenhuma
    carreta alem da T0. O record de trailer+0x1158 so existe
    na T0 (vem do save-load). Nenhum record e escrito em
    nenhuma outra carreta — nunca.
  - Proibido tentar criar record para T1/T2 de qualquer
    forma: nem chamando funcoes nativas (lookup de dominio,
    0x14160E240 create, 0x140345F40 assign, 0x140428480
    alvo-de-pose, 0x141617400 bind, 0x141611AC0/0x141612E70
    registro de canal — essa linha inteira foi tentada em
    4 builds, crashou as 4 vezes e foi revertida), nem
    montando a estrutura na mao.
  - O unico mecanismo que existe para carreta solta na rua
    e a VISIBILIDADE: a carreta some ou se mantem; o
    trafego a ve ou nao a ve. Nada mais existe.

============================================================
DESATRELAMENTO MANUAL — DESPACHANTE NATIVO DESATIVADO (2026-11-23)
============================================================

Por ordem do usuario: o despachante nativo de desengate
(exe+0x5DBD20) foi desativado em TODOS os caminhos do plugin
— dentro e fora de empresa roda o MESMO script manual, que
escreve o estado de desengate na ordem do despachante sem
chama-lo.

Mapeamento completo do despachante (desassembly):

  1. set_towed(cavalo,0)       — zera +0x1580 e avisa os 12
     ouvintes em cavalo+0x1090..+0x10E8 (vfunc+0xF0 cada)
  2. pre_detach(cavalo,carreta) se +0x14B8 != 0 — abre a garra
  3. vfunc_disconnect (vfunc 99) — solta a junta
  4. cavalo+0x14B8=0 e +0x4C4=0 — estado/progresso de engate
  5. set_towed(cavalo,0) de novo + distribute_towed_mass(0,0)
  6. [traffic_mgr+0x2B30] vfunc+0x140 — inicio da troca de
     composicao (o plugin chamava o slot VIZINHO +0x138 —
     provavel off-by-one do port 1.61->1.62)
  7. carreta+0x1168=1 SO se +0x1158 existir (T1/T2 nunca)
  8. vfunc+0x328 — apoio/estado estacionado
  9. rebuild_lines(cavalo,carreta) = 0x140866440 — recursivo,
     desce a CADEIA INTEIRA marcando acessorios de conexao
 10. apply_cables(cavalo,0,modo)
 11. som de desengate (refcounted, emissor ator+0x1D40->+0x1F0)
 12. [traffic_mgr+0x2B30] vfunc+0x1E8 — fim da troca
 13. 0x1405E10F0 (massa do veiculo via gerenciador) ->
     0x1408389A0(cavalo+0x1F8,0,massa) — recalculo do chassi

Extra (a montante, NAO e do despachante): a tecla T passa
pela maquina 0x1405D7DC0, que grava +0x14B8=2, liga +0x25A=1
na cadeia e dispara o evento @@trailer_decoupling@@ — e o
evento (nao o despachante) que a entrega de carga escuta.

Implementacao (arquivo novo src/windows/trailer_detach.cpp):

  - fn284 trailer_manual_detach(trailer,parent,parent_is_truck)
    — mesmo script para T0/T1/T2, empresa ou rodovia
  - cobre todos os passos do despachante + as pecas do plugin
    (marcas, cabos, freio, ancora, unhide, rebuild de claims)
  - slots exatos do gerenciador +0x140/+0x1E8 (nao +0x138/+0x1E0)
  - recalculo de massa do chassi via 0x1405E10F0+0x1408389A0
    resolvidos por assinatura (fn282/fn283)
  - estado do cavalo so e tocado quando parent_is_truck —
    desatrelamento do meio nao polui +0x14B8/+0x4C4/+0x1580
    (a poluicao que exigia o restore morreu junto com o
    despachante)
  - disconnect_now (fn173) roteia tudo para a rotina manual
  - hk_detach_dispatch_raw (fn191, tecla T): dl!=0 -> manual;
    dl==0 (ramo de engate do mesmo despachante) -> original
  - native_detach_via_slot removida por completo

## 2025-11-23 (sessao 2) — consulta sem vigia + desengate pendente da tecla T

Causa raiz do crash/crescimento de lane el confirmada por disassembly:

- elementos de obstaculo mortos ficam na lista: o destrutor nativo
  (0x1412C631E) zera elemento+0x88 (dono) mas NAO tira o elemento do
  vetor do no {+0x20 arr,+0x28 count}
- a consulta "obstaculo mais proximo" 0x1412B8AC0 (vfunc do no,
  assinatura igual ao scan: node,flags,lo,hi,out -> elemento+0x80)
  itera o vetor e le [dono+0x10] SEM checar nulo -> AV -> memcpy final
- logs do teste: ins=0/s (insert hook nunca chamado) e ours=0 (nenhum
  elemento e das nossas carretas) — os elementos mortos sao do proprio
  trafego, nao inseridos por nos
- tecla T: o chamador exe+0x96A1D6 chama o despachante SEMPRE com
  dl=0 (tick de estado); o desengate real e deferido pela maquina
  0x1405D7DC0 que grava cavalo+0x14B8=2 e trailer+0x25A=1 na cadeia —
  a rotina manual nunca rodava nesse caminho

Correcoes:

  - fn285 hk_lane_obstacle_query @0x1412B8AC0 — saneia o vetor do no
    antes da consulta (remove elementos com dono nulo/morto)
  - fn286 lane_sanitize_node — bloco de limpeza extraido do scan e
    reutilizado pelos dois hooks
  - fn191 dl==0: se cavalo+0x14B8==2 (desengate pendente) e a carreta
    rebocada tem junta viva, roda trailer_manual_detach ANTES do
    original; o tick nativo depois ve +0x14B8==0 e nao agenda o
    teardown deferido — a tecla T passa pelo mesmo script do plugin
  - trailer_manual_detach limpa trailer+0x25A (flag de desengate
    pendente gravada pela maquina da tecla T) ao marcar as carretas
  - re-arm da poda de elementos mortos: lane_dead_pending() rodava
    TODO FRAME (O(elementos)) re-armando a janela de 40 frames —
    agora verifica 1x a cada 60 frames e faz UMA varredura, em vez
    de poda continua; os hooks de consulta limpam o resto no caminho

## 2025-11-23 (sessao 3) — sentinela faltando + guard desarmando

Log do teste pos-correcoes: desatrelar ficou limpo (junta=0 em todos),
mas ao ATRELAR o FPS caia de novo e lane el subia 19->108.

Causas encontradas:

  - hk_lane_obstacle_query NUNCA instalou: faltava a entrada na tabela
    va_sentinels — validated_va(0x1412B8AC0) logava "endereco fixo nao
    confirmado" e desligava o recurso. O leitor mais chamado do jogo
    continuava cruzando elementos mortos.
  - lane_obstacle_insert_hook existia escrito mas NUNCA era registrado
    — por isso ins=0/s em todos os logs (nao e que nao insiram; o
    contador nao existia).
  - lane guard desarmava com dead_pending()==0 mas containers
    monitorados ainda tinham elementos vivos — com o guard off os
    hooks passavam direto e os mortos voltavam a acumular sem faxina.
  - sim_dead_frames (sd=) subia ate 172 com det=0: a thread de fisica
    engasgava percorrendo cadeias de nos mortos — o custo do frame
    estava dentro do codigo nativo, nao nos hooks.

Correcoes:

  - sentinela 285 adicionada p/ 0x1412B8AC0 (assinatura 18 bytes:
    48 8b c4 48 89 58 08 48 89 70 18 55 57 41 56 48 8d 68 a8)
  - lane_obstacle_insert_hook agora e registrado (validated_va do
    par 269 0x14094DFC0->0x1412B7DD0) — o contador ins= passa a
    medir de verdade e o filtro de link corrompido fica ativo
  - desarme do lane guard: so desliga quando o censo dos containers
    monitorados zera (sem obstaculo nenhum por perto); com qualquer
    elemento presente o guard continua sanitizando a cada leitura

## 2025-11-23 (sessao 4) — o devorador era nosso: prune dentro do insert

Log pos-sentinela: todos os 5 hooks de lane ativos. No desatrelamento
da T2 com trafego: fps=10, ins=118/s, e o timing do hook mostrou
0.10ms no insert nativo contra 756ms/s no NOSSO wrapper.

  - hk_lane_obstacle_insert chamava prune_lane_containers() a cada
    insert com a janela armada — varredura global O(mundo) com
    VirtualQuery por container + log por elemento morto, ~6ms por
    chamada em rajada de 118 inserts/s. Trocado por
    lane_sanitize_node(node): limpa so o no recebido.
  - removido o log por elemento podado dentro de
    prune_lane_containers (I/O de arquivo no hot path).
  - lane_containers/lane_nodes/cache lane_known_nodes NUNCA eram
    limpos: apos save-load ficavam ponteiros mortos; endereco
    reutilizado fazia a faxina escrever dentro de objeto vivo —
    causa provavel do crash no load. Novo lane_tracking_reset()
    chamado por reset_trailer_state.
  - ins=77~118/s sao bursts naturais: trafego entrando na bolha e
    re-registro apos troca de composicao — nao e multiplicacao nossa.
    ours=0 confirma: as carretas nunca viram elementos de lane.

## 2025-11-23 (sessao 5) — faxina re-armada infinitamente + falso hook-break

Log: ins chegou a ~3000/s sustentado, upd=237ms/s, fps=8.

  - lane_prune_frames=40/120 significava prune_lane_containers()
    TODO FRAME ate zerar — varredura global com VirtualQuery +
    probes SEH por elemento. Todos os 8 pontos de arme agora
    usam =1 (uma varredura por evento).
  - "hook break on cN" disparava apos CADA desatrelamento
    manual: a manutencao via canon_was_jointed && !jointed e
    tratava como quebra externa — re-armava a faxina e matava
    donos sem checar se foi o plugin. Agora pula carretas com
    is_detached/is_plugin_detached/detached_idx.
  - ins=3000/s = churn nativo de residencia do trafego
    (carros re-registrando obstaculo ao andar/mudar de faixa
    perto das carretas paradas) — o custo era o nosso wrapper
    e a faxina, nao as insercoes em si.

## 2025-11-23 (sessao 6) — crashes da noite: mapping errado + corte em elemento de lane

### Crash ao DESATRELAR (~20:37, empresa e rua) — RESOLVIDO
- Sintoma: TODO desatrelamento falhava — `falha 0xC0000005 na funcao
  176 (traffic_detach_tail) +0x3CF`, 19x, AV lendo rbx+0x328
  (addr=0x201cd8ff188). O tracer abortava o dispatch e o detach
  nao acontecia.
- Causa raiz: erro de copia na tabela va_sentinels do commit
  9403501 (migracao 1.62.3.3) — traffic_manager_global
  (0x1436AF7C8) foi remapeado para 0x143714FB0, o MESMO endereco
  do parked_record_mgr (0x1436AF828). O codigo lia o registro de
  parked como traffic manager, pegava lixo em mgr+0x2B30 e fazia
  vcall nele -> +0x328 fora do objeto.
- Por que so hoje: so explode quando registry+0x2B30 esta
  preenchido — dependia do estado interno, nao do reboot/driver
  NVIDIA (coincidencia).
- Correcao: traffic_manager_global restaurado para 0x143714F50
  (parked fica +0x60 adiante em FB0).

### Crash ao ATRELAR EM CADEIA (~21:11, T1->T2) — RESOLVIDO
- Sequencia: desatrelou T2,T1,T0; atrelou T2, moveu, atrelou T1,
  desatrelou T2, reatrelou T1, foi atrelar T1 em T2 -> crash.
- Prova: `stale parent edge cut` disparou em 0x18b08fcff38 e
  0x18b08fd0188 — enderecos que o lane dump lista como ELEMENTOS
  DE FAIXA (rec_vtbl +0x12BF67C). O corte escreveu na malha de
  trafego -> chuva de `falha fora do plugin em 0x7FF9EF4F2A96`
  (dezenas) -> crash nativo.
- Mesma assinatura do crash anterior: exe+0x11F0D8 lendo
  [rcx+0x34] com rcx=0x0000020900000000 (ponteiro truncado —
  os 4 bytes baixos zerados pela escrita do float +0x74).
- Correcao aplicada nos 3 sitios de corte de aresta:
  - hk_phys_link stale-cut: exige +0x68==filho E +0x210==filho
    (o linker nativo mantem os dois em sincronia; elemento de
    lane nao tem essa coincidencia dupla).
  - phys_node_cut_edge: mesma dupla prova.
  - Removida a escrita de +0x74 (4 bytes de zero que truncavam
    ponteiros) nos 3 locais — cosmética, o mantenedor re-escreve.

### Acumulo de elementos de lane (15->28->42->55) — MECANISMO
- Nao sao carros de IA: acumula ate no acostamento sem trafego.
  Sao registros de obstaculo que o no de lane re-registra a cada
  reconstrucao (vfunc4 = 0x1412CABD0). A remocao oficial
  (vfunc5 = 0x1412B3850, desregistra cada registro e zera as
  listas) so roda na destruicao do no — por isso so zera quando
  o jogador sai da area.
- ins~2000/s = reordenacao por posicao suja, ~30x/s por elemento,
  custo quadratico no caminho serial -> fps ancora em ~50 com
  el~40 (com el=0 o mesmo jogo vai a 160).
- Correcao instalada: lane_native_clear() chama a vfunc5 nativa
  quando uma lista passa de 24 ou o censo total passa de 40
  (lane_clear_all_nodes). O jogo recria so o necessario.

## Sessao 7 - crash em thread de fisica ao engatar T0 (22:07)
- Falha: exe+0x12909E0, leitura de obj+0x228 com sub-registro nulo,
  em thread worker (pilha termina em RtlUserThreadStart).
- +0x228 nao e campo que o plugin escreve. Log: 863x re-chain do
  pino de visibilidade em T2 engatada - provavel corrida de escrita
  nos elos slave lidos pela thread de fisica.
- Mitigacao: pino de visibilidade so re-encadeia carreta solta
  (joint==nullptr). Re-chain em carreta engatada removido.
- Limpeza nativa de lanes (vfunc5) revertida por suspeita de
  corrida equivalente na lista de obstaculos.

### Corrupcao de heap no desatrelamento (0xC0000374) — RESOLVIDO
- Sintoma: crash no desengate, WER acusando corrupcao de heap em
  ntdll - o estouro vinha segundos depois da escrita errada, entao o
  tracer nunca via a falha nascer.
- Causa raiz: escritas do plugin em vetores de lane do jogo feitas
  NA THREAD DE RENDER (prune_lane_containers: memmove + *count +
  carimbos el+0x88) correndo contra a thread de SIM rebuildando os
  mesmos vetores (tempestade ins=722/s). Ponteiro de array lido ->
  jogo realoca e libera -> memmove cai no bloco morto -> free-list
  corrompida. readable_ptr/__try sao inuteis contra heap liberado
  (pagina continua mapeada) - o defeito era estrutural, nao azar.
- Agravante: lane_kill_owner em carreta engatada matava registros
  que o jogo re-criava = guerra + churn de FPS antes do crash.
- Licao: proteger com checagem de mapeamento nao basta para
  estruturas alheias - heap liberado passa em toda checagem e a
  escrita corrompe em silencio. Mutacao de container do jogo so e
  segura DENTRO do hook (serializado com o dono) e com prova de vida
  do objeto alvo (vtable em faixa do exe antes de escrever).
- Correcao: zero escrita em estruturas de lane fora dos hooks. Nos
  hooks: carimbo unico atomico el+0x88=dono-falso, so apos validar
  el+0x00 como vtable do exe. Funcoes mortas: prune_lane_containers,
  lane_cap_mark, lane_kill_owner, lane_dead_pending, lane_owner_refs.

### FPS ancorado pela populacao de obstaculos de lane — SEM SOLUCAO
- Correlacao medida duas vezes (documento antigo + sessao de hoje):
  el=0 -> 160fps, el~40 -> ~50fps, el~80 -> ~20fps. Os elementos sao
  registros vivos e legitimos do jogo - o custo e da propria iteracao
  nativa sobre listas que so esvaziam na destruicao do no.
- TENTADO E REVERTIDO (19:23, crash 0xC0000025): teto de 24 chamando a
  remocao nativa (vfunc5) dentro do hook do insert. A lista natural do
  no carrega ~70 elementos; o wipe zerava, o jogo re-registrava tudo,
  passava de 24 de novo = ciclo infinito apaga/re-registra (medido:
  ins=1166/s, 203 elementos tipo=0xFFF). Crashou no meio de
  desatrelar T2,T1,T0 (det=2 no ultimo health).
- Licao definitiva: QUALQUER remocao em massa de obstaculos de lane -
  nossa ou da funcao nativa do jogo - vira guerra contra o re-registro.
  A populacao viva e custo inerente do jogo; nao se toca nela.
