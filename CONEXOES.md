# Nomenclatura de conexões do conjunto

Cada conexão recebe um nome único. O nome identifica PAR DE PONTAS —
não depende de junta física, ponteiro ou ordem de criação.

## Série A — cavalo ↔ carreta (qualquer carreta do conjunto)

| Conexão | Par |
|---------|-----|
| **A0** | cavalo ↔ T0 |
| **A1** | cavalo ↔ T1 |
| **A2** | cavalo ↔ T2 |
| **A3** | cavalo ↔ T3 (ATS) |
| **A4** | cavalo ↔ T4 (ATS) |

Regra: `A` + posição da carreta no conjunto. O cavalo pode ter elo
físico direto com qualquer carreta da fila — cada par possível tem
nome próprio.

## Série B..E — carreta ↔ carreta (adjacentes)

| Conexão | Par |
|---------|-----|
| **B** | T0 ↔ T1 |
| **C** | T1 ↔ T2 |
| **D** | T2 ↔ T3 (ATS) |
| **E** | T3 ↔ T4 (ATS) |

Regra: a letra segue a carreta de trás do par — T1 leva **B**, T2
leva **C**, T3 leva **D**, T4 leva **E**. A série continua em ordem
alfabética conforme a fila cresce.

## Mapa completo

```
            A0
            A1
 cavalo ─── A2 ─── (cada A liga o cavalo a UMA carreta)
            A3
            A4

 T0 ──B── T1 ──C── T2 ──D── T3 ──E── T4
```

## Comparação de referência

- ETS2: T0/T1/T2 -> conexões A0, A1, A2, B, C
- ATS: T0..T4 -> conexões A0..A4, B, C, D, E

## Regra de escrita

Toda referência no código a "conexão entre cavalo e Tx" usa o nome
Ax; toda referência a "conexão Tx–Ty" usa a letra da série B em
diante. Documentos, logs e comentários seguem essa tabela — nada de
nomes improvisados ("junta do meio", "link da segunda carreta").

## Implementação — a tabela `conns[]`

`src/windows/trailer_connections.hpp` guarda um registro PERSISTENTE
por conexão (`conn_record`): ponta da frente (`left`), carreta de trás
(`right`) e estado. Os registros nunca são destruídos — só o estado
muda. Índices: `conn_a(i)` = A(i); `conn_t(i)` = conexão T(i-1)–T(i).

Estados (`conn_state`):

- `linked` — junta física existe: `joint(right)` vivo e
  `trailer_parent(right) == left`. Verdade escrita pelo jogo.
- `parked` — claim do plugin: elo `left->trailer_slave == right` sem
  junta; a carreta fica visível na corrente mas estacionada.
- `off` — sem relação.

Funções:

- `conn_observe` (fn277) — lê junta/pai de cada `canon[i]` e grava o
  estado físico: pai == cavalo → A(i) linked; pai == outra carreta →
  conexão adjacente linked. Registros parked sobrevivem ao observe —
  o commit revalida.
- `conn_commit` (fn278) — reescreve a corrente a partir da tabela:
  físicas primeiro (o jogo escreveu), parked em ordem canon depois do
  rabo; `game_actor_trailer` = A linkada ou live[0]. É o antigo
  `rebuild_parked_claim_links` (fn101 = observe + commit).
- `conn_mark_parked` (fn279) — registra na tabela um claim escrito
  fora do commit (sever/restore, visibility pin).
- `conn_reset` (fn280) — zera a tabela no reset de estado.

Rotação da cabeça: a conexão A linkada decide quem é a "T0" visível —
ligou A1, o `game_actor_trailer`/`truck+0x1580` apontam T1 e as
adjacentes (C, D, E...) formam a corrente física; T0 fica parked
atrás até o conjunto voltar ao padrão.

A série B cresce alfabeticamente conforme a fila: T4 leva E, T5 leva
F, ... até T19 (`max_trailers` = 20).

REGRA — record: `trailer+0x1158` existe somente na T0 (criado pelo
save-load do jogo). Nenhuma outra carreta recebe record — o jogo não tem
caminho para isso e o plugin não tenta. Carreta solta na rua é problema
de VISIBILIDADE apenas: some ou se mantém; o tráfego a vê ou não.
