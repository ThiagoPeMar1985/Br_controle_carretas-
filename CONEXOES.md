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
