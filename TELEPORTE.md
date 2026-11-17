# Teleporte por-carreta do plugin

Documento do caminho usado pra colocar uma carreta separada na posição salva.
Reverse-engineered em eurotrucks2.exe 1.62.

## Premissa

- Não existe função nativa que teleporte uma carreta individual. O teleporte
  nativo é sempre do conjunto inteiro (cavalo + todas as carretas) — pro jogo
  é um ator só.
- Quem separa as carretas é o plugin; quem recoloca cada uma na posição é o
  plugin também. O plugin escreve o estado, o jogo lê e interpreta.
- `teleport_safe(trailer, xform)` em `src/windows/trailer_shared.hpp` é o
  ponto único — todos os restauradores (`restore_parked`, âncoras, oficina)
  passam por ele.
- **A escrita só acontece depois que a carreta terminou de ser reconstruída**
  (`teleport_ready`). Escrever durante a reconstrução corrompe estado e
  derruba o jogo.

## Caminho de escrita (o que teleport_safe faz)

1. `teleport_ready(trailer)` — confere tudo que a vfunc toca antes de chamar:
   - `trailer+0x200` (chassis — lido em `+0x740/+0x748`) legível;
   - `trailer+0x30` (nó raiz — recebe `vfunc+0x110`) é objeto com vtable;
   - `trailer+0x478` (segundo nó — recebe `vfunc+0x120` se não-nulo) idem;
   - os três arrays internos — `+0x218/+0x220`, `+0x530/+0x538`,
     `+0x1020/+0x1028` (início/contagem): contagem ≤ 0x100, bloco legível e
     cada elemento vivo. Cada array tem seu próprio formato de elemento:
     - `+0x218`: elemento = holder de 0x68 bytes; o objeto fica em `[elem+0]`
       (recebe `vfunc+0x130`) e o transform cacheado vai pra `elem+0x10`/`+0x48`.
     - `+0x530`: elemento = struct simples, só leva escrita em `+0x30`.
     - `+0x1028`: elemento = nó (recebe `vfunc+0x110` + `vfunc+0x1B0`).
   Se algo falta → retorna false sem escrever nada (o chamador tenta no
   próximo frame; carreta morta nunca fica pronta, sem custo).
2. Guarda `trailer+0x1058` (`trailer_slave`) e zera — a vfunc+0x2A8 anda a
   cadeia slave recursivamente; com as nossas claims estacionadas o link
   ainda aponta pra próxima carreta e ela seria arrastada junto.
3. Chama `vtbl[0x2A8/8](trailer, &xform, 1)` — a mesma chamada por-membro
   que o teleporte do conjunto dispara (vista no laço do teleporte de ator
   em `exe+0x5DCA39`). Por dentro (`exe+0x857670` + `exe+0x771A60`) ela:
   - compõe `xform ∘ trailer_local(+0x464)` e escreve o transform do nó raiz
     (`[trailer+0x30].vfunc+0x110`);
   - escreve os nós do array `+0x1020/+0x1028` (`vfunc+0x110` + `vfunc+0x1B0`);
   - recarrega o transform cacheado de cada objeto do array `+0x218/+0x220`
     (lê `vfunc+0x130` e grava em `elem+0x10..0x2F` e `elem+0x48..0x64`) —
     é a própria ressincronização dos dependentes, não precisa marcar nada;
   - zera `elem+0x30` de cada item do array `+0x530/+0x538` e recalcula
     limites (`0x14085EB50`, `0x1408681A0`).
   O efeito líquido: `get_transform` (vfunc 0xF8) passa a devolver o
   `xform` que escrevemos — a carreta fica exatamente na pose salva, o
   offset local `trailer+0x464` é preservado sozinho.
4. Restaura `trailer+0x1058` (sempre, mesmo com falha).

## Formato do transform

`hook_xform_t` — 0x20 bytes, mesmo buffer que `get_transform` (vfunc 0xF8)
produz e que a vfunc+0x2A8 consome:

    struct { float3 pos; float pad; quat_t quat; }

`parked_xform[i]` já está nesse formato — é capturado por `hook_world()`.

## Guards

- `is_trailer_obj(trailer)` — vtbl[99] == `exe+0x789C20` (objeto-carreta real,
  não slot reciclado).
- `cargo_freeze()` — nunca escreve durante a janela de carga.
- `plausible_vfunc(trailer, 85)` — slot 0x2A8 apontando pro exe.
- `teleport_ready(trailer)` — reconstrução concluída.
- Tudo sob `__try/__except`: falha → loga "trailer teleport faulted" uma vez
  por alvo a cada 5s e retorna false. Sucesso → true no mesmo frame.

## Pipeline de carga/descarga com carretas separadas

As funções necessárias, na ordem em que rodam quando o jogo destrói e
reconstrói o mundo (carregar/descarregar na doca).

### 1. Bloqueio do teleporte do conjunto

- `hk_actor_teleport` (fn124) — intercepta o teleporte nativo do rig.
  Com carreta solta em `detached_idx`, BLOQUEIA: o jogo teleportaria o
  conjunto inteiro pra doca e levaria as carretas soltas junto. Salva o
  xform do cavalo (`rig_armed`) pra replay depois.

### 2. Detecção da reconstrução

- `coupling_parked_update` (fn118) — laço por frame que orquestra tudo.
- `workshop_respawn_update` (fn145) — percebe que os objetos-carreta foram
  destruídos/recriados; salva `parked_xform` das soltas e remarca
  `detached_idx` nos objetos novos.
- `canon_dead` — detecta reload: canon deixou de ser objeto-carreta.
- `garage_update` (fn166) — solta marcas de slots cujas carretas morreram.
- `reset_trailer_state` (fn74) — zera o rastreio depois do reload.

### 3. Restauração

- `restore_parked_if_enabled` (fn115) → `restore_parked` (fn116) — rejoga
  o xform do cavalo e, pra cada slot solto: `disconnect_now` +
  `teleport_safe` na `parked_xform`.
- `disconnect_now` (fn173) — desengate na thread de física; a carreta volta
  re-engatada da reconstrução e precisa desengatar de novo.
- `hk_trailer_disconnect` (fn182) — saneia a cadeia antes do desengate
  nativo (vfunc99).
- `teleport_ready` (fn263) + `teleport_safe` (fn260) — a escrita da pose
  salva (seção acima).

### 4. Estacionamento

- `carry_set_brace` (fn117) — desce o pé de apoio pela vfunc nativa.
- `native_park_detached` (fn136) — apoio + freio + registro, ~20 frames
  após o teleporte.
- `carry_brake_frames` — mantém `+0x760=1`, `+0x764=1` nos primeiros
  frames.
- `update_parked_locks` (fn142) — âncora de fallback.

### 5. Pregamento do corpo físico (o fix do pulo)

Contexto medido: ~2,7s depois do restore, o jogo roda um trabalho
atrasado de engate que religa a árvore física (chamadas `phys_link`) e
ativa a ilha que contém os corpos das carretas soltas. O corpo que
dormia recebe impulso de contato num único tick → a carreta voa. A
junta `+0xFB0` não renasce e o `phys_link` é só sintoma — a ativação da
ilha é que injeta a velocidade.

- `phys_probe(trailer)` — resolve o corpo interno: `trailer+0x30` →
  `phys+0xF8` → `inner`; pos em `inner+0xA0` (4 floats), vel em
  `inner+0xD0` (4 floats).
- `phys_pin(trailer, pos4)` — regrava a pos capturada e zera a vel.
  Roda todo frame durante a janela (`watch_frames` ≈ 300f ≈ 5s) com a
  pos gravada no arm (`watch_pin[i]`).
- O corpo continua vivo e colidindo — não afunda, aceita contato —
  só não acumula movimento: qualquer impulso injetado é zerado no
  frame seguinte e a posição é recolocada.

### 6. Corpo fantasma (a barreira invisível)

Sintoma: depois do carregamento, o conjunto engatado anda ~1m e trava,
inclinando como se colidisse com algo invisível — a "carreta dentro da
carreta". Teleportar o conjunto pra longe liberava porque deixava o
obstáculo pra trás. Nenhum freio, âncora ou flag explicava — os dumps
de estado saíam limpos, com juntas vivas e cadeia correta.

Causa provada pelo log: quando o jogo destrói e recria as carretas no
respawn de carga, o **corpo físico interno da carreta velha vaza** — o
destruidor não o remove do mundo. O corpo órfão fica parado e invisível
na vaga onde a carreta estava estacionada; a carreta nova é colocada no
mesmo lugar e colide com o próprio resto. Confirmado no log: dois
corpos órfãos (de c1 e c2) banidos e a trava acabou.

#### Reparo no destruidor (a fonte do vazamento)

- O teardown nativo lê um objeto ref-counted compartilhado em
  `trailer+0xEE8` (`mov rbx,[rcx]; mov eax,[rbx+0x34]` no crash). Nas
  nossas carretas esse campo chegava amputado — metade baixa zerada
  (`0x26b00000000`), assinatura de escrita de 4 bytes sobre ponteiro
  de 8 — e o destruidor crashava → teardown pulado → corpo vazado.
- `hk_trailer_dtor` (`trailer_coupling.cpp`) confere o slot antes da
  chamada nativa: com a assinatura amputada, copia `+0xEE8` de uma
  carreta irmã viva (objeto compartilhado, valor idêntico em todas —
  igual ao `e60`) e só então chama o original sob `__try/__except`.
  Resultado: "teardown ok" em todas — bem menos vazamento.

#### Caçador de corpo órfão (a limpeza)

`coupling_parked_update` (fn118) vigia o corpo de cada slot todo frame:

- `last_inner[i]`/`last_inner_pos[i]` — cache por slot do `inner` que o
  `phys_probe` resolve. Quando o `inner` TROCA (carreta destruída e
  recriada) ou o slot deixa de ser `is_trailer_obj`, o inner antigo
  entra na fila `ghost_watch[]` com timestamp e posição.
- ~3s depois, cada suspeito é conferido:
  - igual ao `inner` de alguma carreta viva → corpo REAPROVEITADO,
    não toca (apagar corpo vivo = a carreta atravessa o chão);
  - `inner+0xA0` ilegível → o jogo já removeu, limpo;
  - legível e a <1m da posição de morte → **órfão confirmado**:
    `pos[1] -= 10000` (10km abaixo do mapa) + vel zerada em `+0xD0`,
    loga "corpo fantasma de cN banido".
- Roda o tempo todo, não só no respawn — a barreira some sozinha, sem
  engatar e sem teleporte manual.

### 7. Manutenção contínua

- `refresh_trailer_chain` (fn102) + `rebuild_parked_claim_links`
  (fn101) — claims de visibilidade (carreta solta fica visível).
- `sever_phys_claims` (fn184) — corta arestas `+0x60/+0x68` que
  apontam pra peça sem junta.
- `sever_parked_claims`/`restore_parked_claims` (fn183/185) —
  escondem e religam claims em volta de chamadas nativas.
- `update_convoy_brakes` (fn216) + `update_landing_gear` (fn149) —
  freio de mão e pé de apoio por estado.
- `hk_cam_set_target`/`hk_cam_update`/`hk_combo_length` (fn198-200)
  — câmera e medida do comboio fora das carretas sem junta.
- `queue_op` (fn49) + `hk_crashes_when_disconnected` (fn175) — fila
  de ops na thread de física.

## Caminhos abandonados — não reutilizar

- **Desligar o corpo físico** (bit em `inner+0xAC`): tira o corpo da
  simulação E da colisão — a carreta cai atravessando o chão em queda
  livre e some. O corpo é quem apoia a carreta no terreno.
- **Bloquear `phys_link` na janela**: não impede o impulso (o corpo
  recebe velocidade mesmo sem a religação) e ainda deixa a árvore
  inconsistente → andarilhos nativos morrem. Sintoma, não causa.
- **Esperar a junta `+0xFB0` renascer**: medido — fica zero no pulo.
- **Marcar objetos físicos via `node+0x10` → `obj+0x20`** (estado `+0xA9=7`,
  carimbo `+0x108`): os objetos alcançados assim nos nós do ator NÃO são os
  corpos da função nativa — `+0x108` é um campo de índice nesse tipo, e
  escrever o carimbo lá dentro fez o jogo indexar um array com 0x1E8480 →
  crash (bounds check `exe+0x12EAA0`). A ressincronização real já acontece
  dentro da vfunc+0x2A8 pelo array `+0x218`.
- **Re-teleportar a carreta pra posição atual após o engate** (resync):
  a vfunc+0x2A8 só move nós e transforms cacheados — nunca toca o corpo
  físico. Disparava e a trava voltava igual. O que libera é mexer no
  CORPO (ou remover o órfão), não no nó.
- **Chamar a função nativa por-carreta** (`exe+0x7864D0`, sentinela 249): ela
  espera outro tipo de objeto (listas {begin,count} em `+0x30`/`+0x58` que o
  ator não tem); `node_lists_sane` lia `+0x40` como contagem (era um ponteiro
  de código) e reprovava pra sempre → loop de ~14s de "node lists not ready".
  O gancho `hk_trailer_teleport` continua instalado só como guarda; a escrita
  não depende dele.

## Regra de bolso

Mover carreta separada = esperar `teleport_ready` +
`vfunc+0x2A8(trailer, &hook_xform_t, 1)` com `+0x1058` escondido.
Depois do restore, pregar o corpo ~5s (`phys_pin` por frame) — o
trabalho atrasado do jogo ativa a ilha aos ~2,7s e injeta impulso.
Nunca escrever durante a reconstrução, nunca delegar ao jogo.
