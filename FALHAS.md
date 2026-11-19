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
