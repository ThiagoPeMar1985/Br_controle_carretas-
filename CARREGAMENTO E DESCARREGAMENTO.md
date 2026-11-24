# CARREGAMENTO E DESCARREGAMENTO

O sistema do plugin funciona de **duas formas diferentes** conforme o contexto.
Este arquivo especifica a segunda forma: a carga e a descarga na empresa.

## AS DUAS FORMAS

| Forma | Onde | Comportamento |
|---|---|---|
| **Rua / local seguro** | rodovia, posto, grama, empresa SEM dock ativo | o sistema de visibilidade (claims) - funciona hoje |
| **Carga / descarga** | empresa COM dock ativo (apertou pra carregar/descarregar) | **o comportamento do GitHub** - as carretas separadas ficam onde o usuario deixou |

## O COMPORTAMENTO EXIGIDO NA EMPRESA (o jeito do GitHub)

1. O usuario desengata as carretas perto da doca → as posicoes ficam salvas
2. O usuario aperta pra carregar/descarregar → **o jogo tenta unir tudo** (comportamento nativo dele)
3. **O plugin impede**: as carretas separadas **ficam onde o usuario deixou** -
   nao se unem, nao se movem, nao perdem a posicao
4. A carga acontece normalmente na carreta atrelada
5. Depois da operacao, as separadas continuam separadas, no lugar exato

## O QUE NAO PODE ACONTECER

- O jogo unir as carretas separadas durante a operacao
- As carretas se moverem da posicao que o usuario deixou
- O sistema da rua (claims/pinos) interferir na operacao

## A SEPARACAO

- O mecanismo da empresa vive **separado** do sistema da rua
- Os arquivos da rua (manipulation, coupling, detach) nao carregam a logica da empresa
- O estado da empresa nao vaza pra rua e o da rua nao vaza pra empresa

## AS FUNCOES DO GITHUB QUE IMPLEMENTAM ISSO

Extraidas do commit `6d64f1b` (o estado em que funcionava):

- `restore_parked` (fn116) - re-destaca e teleporta de volta pra posicao salva
- `restore_parked_if_enabled` (fn115) - o gate da opcao
- `carry_set_brace` (fn117) - o apoio da carreta
- `coupling_parked_update` (fn118) - o vigia por frame
- `snapshot_rig` (fn120) - a foto do conjunto
- `hk_trailer_state_apply` (fn121) - o record na doca
- `hk_trailer_teleport` (fn122) - o teleporte por carreta
- `settle_all_members` (fn123) - o assentamento
- `hk_actor_teleport` (fn124) - o teleporte do ator
- `native_park_detached` (fn136) - o estacionamento nativo
- `unhide_detached` (fn141) - a reexibicao
- `update_parked_locks` (fn142) - a trava de estacionamento
- `unpark_jointed` (fn143) - o destravamento das engatadas
- `load_game_active` (fn146) - a deteccao do load
- `load_game_update` (fn147) - o fim do load

Estas funcoes devem rodar **byte a byte** como estao no GitHub - sem
adicao, sem gate novo, sem mecanismo novo. O que muda entre o GitHub e
hoje e que as funcoes em si nao foram alteradas: foram os **mecanismos
novos** (a rua, os pinos, o lane, os gates) que sangraram pra dentro
delas.
