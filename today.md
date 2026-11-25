# Diário de hoje — mudanças e testes

Registro de tudo que foi mudado no plugin hoje, com hora, e o resultado de cada teste.
Serve para voltar atrás em qualquer ponto sem perder a evolução.

---

## 11:20 — Desatrelamento manual (versão 1)

**O que foi feito:** criada a rotina que escreve o desatrelamento na mão, sem
chamar o despachante nativo. Aplicada nas teclas do plugin e na tecla T
(quando o parâmetro indica desengate).

**Arquivos:** `src/windows/trailer_detach.cpp` (novo), `trailer_coupling.cpp`,
`trailer_coupling_internal.hpp`

**Teste:** desatrelar pelas teclas do plugin funcionou. Pela tecla T o jogo
desatrelava pelo nativo mesmo — o parâmetro vinha como "tick" e a rotina não
rodava. FPS caía e crashava ao atrelar. **Resultado: parcial.**

## 12:38 — Vigia na consulta perigosa + intercept do pendente

**O que foi feito:**
- Gancho na consulta de obstáculo que lia elemento morto (era o caminho do crash)
- Limpeza dos obstáculos mortos passou a rodar na leitura, não mais a cada frame
- Intercept para a tecla T quando o estado "desengate pendente" estiver marcado

**Arquivos:** `trailer_loadstate.cpp`, `trailer_coupling.cpp`,
`trailer_detach.cpp`, `trailer_shared.hpp`

**Problema encontrado depois:** o gancho novo não instalou — faltava registrar
a assinatura dele na tabela de validação. O jogo logou "endereco nao
confirmado - recurso desligado".

**Teste:** desatrelar melhorou mas o FPS caiu pra 8 e o jogo crashou no load.
**Resultado: piora — mas diagnosticado.**

## 13:01 — Assinatura registrada + gancho de inserção ligado

**O que foi feito:**
- Registrada a assinatura da consulta perigosa (o vigia passa a instalar)
- O gancho de inserção de obstáculo existia no código mas nunca tinha sido
  ligado — agora instala e conta inserções reais
- A proteção deixa de desligar no meio do teste (só desarma quando não há
  mais obstáculo nenhum por perto)

**Arquivos:** `trailer_shared.hpp`, `trailer_loadstate.cpp`

**Teste:** ao desatrelar a T2 o FPS caiu pra 8. Causa identificada no log: o
nosso gancho de inserção varria o mundo inteiro a cada inserção (756ms/s do
nosso próprio código). **Resultado: piora — causa provada era nossa.**

## ~13:20 — Faxina só no pedaço recebido + limpeza no load

**O que foi feito:**
- Gancho de inserção limpa só o pedaço que recebeu, não o mundo inteiro
- Removido log por elemento dentro da faxina (I/O em caminho quente)
- A lista de obstáculos vigiados é zerada no load — corrige o crash no load
  (ponteiros mortos sendo reescritos dentro de objetos novos do jogo)

**Arquivos:** `trailer_loadstate.cpp`, `trailer_coupling.cpp`

**Teste:** desatrelou as 3 sem queda de FPS, atrelou T1/T2 bem, moveu, soltou
T2, atrelou T1 de novo → FPS começou a cair até 8. Log mostrou ~3000
inserções/s de tráfego (ping-pong) + faxina de 40-120 frames re-armada a cada
operação por alarme falso de "quebra". **Resultado: evolução real — desatrelar
resolvido; atrelar/remontar ainda degradava.**

## ~13:45 — Faxina uma vez por evento + fim do alarme falso

**O que foi feito:**
- Toda limpeza armada passa a rodar UMA varredura por evento (antes: 40-120
  frames seguidos varrendo o mundo)
- O alarme de "quebra" ignora carretas que o próprio plugin desatrelou — não
  dispara mais faxina/cabos redundantes nos nossos desatrelamentos

**Arquivos:** `trailer_coupling.cpp`, `trailer_loadstate.cpp`,
`trailer_manipulation.cpp`, `trailer_shared.hpp`

**Teste (13:25-13:26):** desatrelou as 3 com FPS perfeito. Ao atrelar a T1 no
cavalo: junta criada, e o jogo morreu ~130ms depois, sem dump — morte por
pilha, assinatura de corrente física em círculo. Log prova arestas velhas e
círculos em todo atrelamento; a aresta cortada reaparece 122ms depois (o jogo
re-escreve). Causa provada: **nosso desatrelamento nunca desliga o elo físico
da carreta** — a corrente sai suja e explode na religação.
**Resultado: crash no atrelar — causa raiz localizada.**

---

## 14:00 — Desligar o elo físico no desatrelamento manual (AUTORIZADA)

**O que foi feito:** no desatrelamento manual, desfazer o elo da carreta na
corrente física — mesma sequência de escritas que o desligar nativo faz:
`pai+0x68=0`, `pai+0x74=0`, `pai+0x210=0` (só se o pai ainda apontar pra ela)
e `carreta+0x60=0`. Uma escrita por desatrelamento. A corrente atrás dela
(T2 pendurada na T1) fica intacta.

**Por quê:** o código nunca desligava esse elo — a carreta saía da corrente
pela metade. O log provou: cada atrelamento achava arestas velhas e círculos,
e a aresta cortada reaparecia porque o `+0x210` do pai continuava nomeando a
carreta como filha registrada.

**Arquivos:** `src/windows/trailer_detach.cpp`

**Backup:** `backups/br_controle_de_carretas_1345_desatrelar_ok.dll`
(build das ~13:45 — desatrelar perfeito, crash no atrelar)

**Teste (14:02):** crashou de novo ao atrelar T1 — mesma assinatura (junta criada,
aresta velha, ciclo, morte silenciosa). Os nós sujos (`0x1a6b82ccxxx`) ficam na
área das juntas, não das carretas — a limpeza no `+0x60` da carreta não alcançou
a corrente real. Diagnóstico via disasm: `phys_link` é vfunc 36 de uma classe de
nó; a desconexão nativa não desliga o nó diretamente — a limpeza física vive na
etapa deferida do desatrelamento nativo.
**Resultado: piora não causada pela mudança, mas a mudança não resolveu.**

## ~14:30 — Build de diagnóstico: despejo da corrente física (AUTORIZADA)

**O que foi feito:** nova função `dump_phys_chain` que anda a corrente `+0x60`
(para cima) e `+0x68` (para baixo) a partir de um objeto, logando cada nó:
endereço, elo seguinte, elo reverso, `+0x210` (filho registrado), `+0x88`
(dono), se é carreta e a junta. Detecção de ciclo embutida, máximo 32 nós.

**Pontos de log:**
- No engate (`connect_now`): despeja carreta + pai antes e depois
- Nas primeiras 24 chamadas de `phys_link` após o engate: loga pai/filho/donos
- No desatrelamento manual: despeja carreta + pai no fim

**Arquivos:** `trailer_coupling.cpp`, `trailer_coupling_internal.hpp`,
`trailer_detach.cpp`, `trailer_shared.hpp`

**Teste (14:24-14:28):** SEM CRASH, FPS caiu mas voltou — bem melhor.
O despejo revelou o modelo real:
- `trailer+0x60`/`+0x68` NÃO são a corrente física (+0x60=0, +0x68=string do exe)
- os nós físicos são objetos próprios (`0x160afbcxxxx`), dono em `+0x88`
- existe nó-raiz persistente; o engate faz `phys_link(raiz,0)` + `phys_link(raiz,nó)`
- o nó da carreta chega ao engate ainda pendurado no pai antigo — é o resto que
  vira ciclo
**Resultado: evolução — mapa da corrente obtido.**

## ~14:38 — Diagnóstico 2: mapear carreta -> objeto físico -> nó

**O que foi feito:** após o engate, varre os campos da carreta (+0x00..0x1000)
procurando ponteiros pro nó físico e pro dono dele (`+0x88`), e os campos do
dono procurando o nó e a carreta (+0x00..0x400). Isso revela a ponte
carreta→nó para a escrita correta do desligamento.

**Arquivos:** `trailer_coupling.cpp`, `trailer_shared.hpp`

**Teste:** aguardando — meta: achar o offset carreta→objeto-físico.

## Ponto de restauração

- DLL estável de hoje (desatrelar perfeito, crash no atrelar): backup a fazer
  antes da próxima instalação.
- Último ponto totalmente estável conhecido: build de ~13:45 (desatrelamento
  limpo; só atrelamento corrompe).

## ~14:50 — Resultado do teste do diagnóstico 2 (14:40-14:43)

**Teste do usuário:** sem crash; FPS caiu, voltou ao padrão, caiu de novo.
"Estamos quase acertando o ponto."

**O que o log mostrou:**
- `phys map:` gravou nó -> dono -> carreta, mas o varredor de campos NÃO
  achou nenhum ponteiro na carreta (+0x00..0x1000) para o nó nem para o
  dono, e nenhum no dono (+0x00..0x400) de volta. A ponte carreta->nó é
  indireta (provavelmente via junta / `+0xF0`).
- `joint_walk` (0x1412BF010) disassemblado: anda `nó+0x68` recursivamente
  (vfunc+0x18 no filho) e lê `nó+0xF0` (objeto dono, vfunc+0x30).
- vtable do nó físico em 0x1424131E0: slot 36 (+0x120) = phys_link.
- O corte atual de aresta velha zera `pai+0x68` e `+0x74`, mas o desligar
  nativo também zera `pai+0x210` (registro do filho). Nosso corte deixa o
  registro vivo -> fonte provável da aresta que "ressuscita" ~30-120ms
  depois (visto no log: corte e reaparecimento da mesma aresta).

**Próximo passo proposto:** completar os cortes existentes para valer o
mesmo que o desligar nativo: ao cortar aresta X->Y, zerar também
`X+0x210` (se == Y) e `Y+0x60` (se == X). Três pontos de corte.

## ~14:55 — Correção: cortes de aresta com semântica completa do desligar nativo (AUTORIZADA)

**O que muda:** cada ponto onde o plugin corta uma aresta X->Y da corrente
física passa a limpar também o registro `X+0x210` (se == Y) e o elo de
subida `Y+0x60` (se == X) — a mesma sequência que o phys_link(pai,0)
nativo faz (prova: disassembly 0x1412BE360). Antes limpava só +0x68/+0x74
e o registro vivo no pai antigo fazia o jogo re-escrever a aresta.

**Pontos:** joint walk cyclic cut, phys link bad/cyclic edge cut, phys
link stale parent edge cut (+0x210 no pai antigo), chain accum cyclic cut.

**Backup:** `backups/br_controle_de_carretas_1505_diag_sem_crash.dll`

## ~15:12 — Diagnóstico 3: ponte junta->no fisico (AUTORIZADA)

**O que foi feito:** log de phys_link agora mostra `hst=` (campo +0xF0 do
no — o objeto dono segundo o joint_walk). `report_phys_mapping` agora
varre tambem a JUNTA da carreta (+0x00..0x400) procurando o no e o dono,
e o objeto +0xF0 procurando o no e a carreta. Se a junta guardar o no,
o desligamento correto fica trivial: le o no pela junta no desatrelamento.

**Arquivos:** `trailer_coupling.cpp`

## ~15:30 — Correcao real: desligar o no fisico da carreta da corrente (AUTORIZADA)

**O que muda:** no desatrelamento manual, o plugin anda a corrente fisica
a partir dos pais vistos nos links (phys_seen_parents, registrado em
todo hk_phys_link), acha o NO da carreta pelo mapa provado
(no+0xF0 = host, host+0x328 = carreta) e o desliga com a sequencia
exata do phys_link(pai,0) nativo: pai+0x68=0, pai+0x74=0,
pai+0x210=0 (se == no), no+0x60=0.

**Substitui:** o bloco errado que escrevia +0x60/+0x68 na CARRETA
(provado que nao e a corrente - +0x60 dela e sempre 0).

**Backup:** `backups/br_controle_de_carretas_1512_mapa.dll`

**Arquivos:** trailer_shared.hpp, trailer_coupling.cpp,
trailer_coupling_internal.hpp, trailer_detach.cpp

## ~15:46 — Correcao v2: no gravado no engate + desligar ANTES do disconnect (AUTORIZADA)

**O que muda:** o ultimo no linkado no engate e provado ser o da carreta
(hst+0x328==carreta) - agora gravado em phys_node_of[canon]. O desligar
usa o no gravado direto (valida hst+0x328 quando o dono vive) e roda
ANTES de soltar a junta - antes o host ja morria e a busca nao achava nada
(por isso "phys unlink" nunca apareceu no log).

**Tambem:** phys_node_of zerado no reset_trailer_state; fallback walk
mantido usando phys_node_cut_edge compartilhado.

**Backup:** `backups/br_controle_de_carretas_1530_unlink_v1.dll`

## ~17:04 — Diagnostico 4: despejo da corrente real de nos (AUTORIZADA)

**O que foi feito:** dump_real_chain anda a corrente de verdade a partir
dos pais vistos nos links (sobe +0x60 ate o topo, desce +0x68) e grava
cada no: elos, registro, dono (+0xF0), dono+0x328 e se ele resolve pra
carreta do conjunto (indice, trailer estranho ou nada). Roda no fim de
cada engate e desengate. So leitura.

**Arquivos:** trailer_coupling.cpp, trailer_coupling_internal.hpp,
trailer_detach.cpp

## ~17:20 — Diagnostico 5: resolver o dono do no pela vfunc do proprio jogo (AUTORIZADA)

**O que foi feito:** o disassembly do joint_walk provou que o jogo obtem
o veiculo dono de um no chamando host->vfunc[+0x30] no objeto em no+0xF0
(e compara com vfunc[+0x8] no filho). O despejo da corrente agora chama
as duas vfuncs por no e grava o resultado + indice na canon. Isso define
a identidade certa para a limpeza - sem offset inventado.

**SCS Telemetry:** serve para confirmar quais carretas estao atreladas
(canal connected) - os campos internos da corrente ele nao expoe.

**Arquivos:** trailer_coupling.cpp

## ~17:45 - Emenda da corrente fisica (correcao)
- dump provou: cada no da corrente = gancho traseiro de um veiculo.
- o30 (vfunc+0x30 do host) = carreta pendurada; o8 (vfunc+0x8) = dono do gancho.
- regra de validade: para no com o8 carreta, vale iff trailer_slave(o8) == o30.
- capturado no invalido (o30=c2,o8=c0) - par de carretas nao vizinhas.
- implementado repair_phys_chain: anda a corrente 1x por evento e emenda
  (prev+0x68=dn; prev+0x210 se ==cur; dn+0x60 se ==cur; zera cur+0x60/+0x68)
  cortando so nos comprovadamente invalidos; host morto/nao classificavel = mantem.
- roda apos dump em post-connect e post-detach.
- backup: backups/br_controle_de_carretas_1720_mapa_donos.dll
- compilado e instalado ~17:42
- RESULTADO: (aguardando teste)

## ~17:55 - Correcao na FONTE: claims slave falsos
- DIAGNOSTICO FINAL: repair rodou e NAO cortou nada - porque o slave() que
  eu usava como verdade tambem esta contaminado. Log prova: no (o8=c0,o30=c2)
  valido p/ slave(c0)==c2 -> c0 ainda reivindica c2 ATE DEPOIS de c2 atrelar c1.
- RAIZ: conn_commit() reescreve slave() todo frame em ORDEM CANON, incluindo
  claims sobre carretas JA ATRELADAS a outro pai. Nada revoga claim quando a
  carreta atrela. O jogo deriva no fisico do claim -> fantasma ressuscita.
- FIX: (1) want/next so aceita alvo sem junta ou cujo parent==dono;
  (2) dono atrelado nao tem slave reescrito pela ordem canon;
  (3) sanidade no fim de conn_commit: slave(P)=X com X atrelada e
  parent(X)!=P -> corta; (4) no engate, limpa slave(P)==T de outros donos;
  (5) repair usa joint+parent como prova (nao mais slave).
- backup: backups/br_controle_de_carretas_1742_repair_v1.dll
- compilado e instalado ~18:04
- RESULTADO: (aguardando teste)

## ~18:19 - Correcao: cobertura completa da corrente slave
- REGRESSAO do teste 18:04: carreta sumiu. Causa: pular dono atrelado deixou
  slave(c2)=c0 velho -> guarda de ciclo recusou c0.slave=c1 -> c0 orfao,
  lista do jogo perdeu c1/c2 -> invisivel. A corrente slave E o registro.
- REGRA correta: a corrente cobre TODAS as carretas 1x; links reais
  (joint+parent) primeiro, claims de soltas depois, sem duplicar.
- conn_commit reescrito em 2 fases: fase A escreve slave(P)=X quando
  joint(X) e parent(X)==P; fase B reivindica a proxima solta nao
  reivindicada. Sem ciclo por construcao.
- compilado e instalado ~18:19
- build diag 20:xx: phys_link log completo 90s apos cada atrelar/desatrelar, com ret= endereco do chamador
- build diag: hook chain_maint 0x1412B9AC0 — conta nos distintos/s e loga religacoes por 90s apos evento

## Resultados do dia - resumo das rodadas
- Teste 18:19 (cobertura slave): nenhuma carreta sumiu; dump_chain mostrou
  topologia perfeita (c0->c1->c2, parents certos); claims velhos removidos.
  FPS caiu no atrelamento do conjunto MAS voltou em ~20s (antes ficava baixo
  para sempre). Custo na queda: jw=900-1100/s a ~1.5ms + ins=2700-3400/s.

## Diag: phys_link completo 90s com ret=
- RESULTADO: TODAS as chamadas vieram de um unico chamador
  (exe+0x12B9B46/0x12B9B6A/0x12B9B80) e SEMPRE com p=topo da corrente.
  Nenhum phys_link(pai_velho, filho) -> as arestas nao voltam pelo linker
  hookado. Apenas ~3 cortes de aresta no teste inteiro -> a queda NAO e
  briga de arestas, e quantidade de nos processados.

## RE: mantenedor por-no exe+0x12B9AC0
- Funcao por frame POR NO: wanted = node+0x208->vfunc+0x30();
  current = node+0x210->+0xF0->o8(); se diff -> slot+0x98(0) depois
  slot+0x98(novo) -> e quem chama phys_link (3 call sites no fim).
- Linker real = vtable slot 36 (0x1412BE360, ja hookado). Mantenedor usa
  slot+0x98 = impl DIFERENTE por classe de no -> religacoes de nos do meio
  sao invisiveis pro hook do phys_link.
- Arquitetura registrada no AGENTS.md.

## Build diag atual (instalada): hook chain_maint em 0x1412B9AC0
- Conta nos distintos processados/s (ate 256), loga "maint novo no" e
  "maint relink X: dn A->B reg C->D" durante janela de 90s pos-evento.
- Pergunta que responde: quantos nos rodam por segundo durante a queda
  (jw~1100/s sugere 30+ nos/frame = nos mortos acumulados).
- RESULTADO: (aguardando teste)

## Diag instalada: elo elemento-de-lane -> carreta
- lane_dump_refs(trailer): varre todos os elementos rastreados nos containers e
  despeja refs de el/el+0x80(rec)/el+0x88(own) apontando p/ qualquer carreta
  canon — chamado no post-connect e post-detach.
- hk_lane_obstacle_insert: na janela de 90s, dedup de links (64) e log de
  "ins link X tipo=0xY ref canon# em +0xZ" p/ cada link novo.
- Pergunta: como o elemento de obstaculo se liga a carreta (para matar no attach).
- RESULTADO: (aguardando teste)
- build diag instalada: lane dump agora mostra rec_vtbl/own_vtbl (exe+off) e
  histograma de classes de dono — identifica o que sao os elementos que acumulam

## Correcao: teto nos elementos de lane (autorizado)
- lane_el_cap=24 por array: elementos acima do teto sao marcados mortos
  (pelo mais distante, +0x90 maior) e a poda existente remove.
- guard_check: censo el>40 -> lane_prune_frames=2 (arma a poda mesmo sem
  elemento morto pendente — os excedentes estao vivos).
- Donos identificados via dump: vtbl +0x24141E0/+0x2414CA0 = classes da
  familia dos nos de faixa (mesma base de 4 slots) — elementos sao
  entradas da malha viaria subdividida ao redor da carreta estacionada.
- backup: backups/br_controle_de_carretas_pre_cap.dll
- RESULTADO: (aguardando teste)

## Crash no atrelamento em T1 (build com teto) + reversao
- TESTE 20:19 (build com lane cap): crash imediato ao atrelar T1.
  game.crash.txt: ACCESS_VIOLATION em exe+0x11F0D8 `mov ecx,[rcx+0x34]`
  com rcx=0x0000020900000000 — ponteiro heap com os 32 bits baixos
  ZERADOS (truncado). Pilha toca o mantenedor (+0x12B9B8) e +0x12B596
  (chain_accum).
- Suspeito principal: o "stale parent edge cut" do hk_phys_link — dispara
  3ms antes do crash, le filho+0x60 (campo NAO confiavel em nos do meio)
  como "pai antigo" e escreve 4 bytes de zero em +0x74 — se o objeto for
  estranho e +0x74 for a metade baixa de um ponteiro, produz exatamente
  0x20900000000. Mesmo padrao de escrita existe em phys_node_cut_edge.
- O TETO era inocente nesse crash: lane el max=20 < 24 (nunca disparou).
- Pedido do usuario na epoca: reverter o teto de lanes (depois o
  usuario REVERTEU essa reversao - o teto segue ATIVO no codigo).
- PENDENTE (proposto, nao autorizado): endurecer os cortes de aresta —
  so escrever se +0x68==filho E +0x210==filho (dois campos em sincronia
  = prova de pai real), e remover a escrita do float +0x74.

## Crash ao DESATRELAR (empresa/rua) - causa raiz encontrada
- Sessao 20:37: TODO desatrelamento falhava deterministicamente -
  "falha 0xC0000005 na funcao 176 (traffic_detach_tail) +0x3CF" (19x),
  AV lendo rbx+0x328 (rbx=0x201cd8fee60, addr=0x201cd8ff188) - o tracer
  abortava o dispatch e o detach nao acontecia.
- CAUSA RAIZ: erro de copia na tabela va_sentinels (commit 9403501,
  migracao 1.62.3.3): traffic_manager_global (0x1436AF7C8) foi remapeado
  para 0x143714FB0 - O MESMO endereco de parked_record_mgr. A funcao lia
  o registro de parked como se fosse o traffic manager, pegava lixo em
  mgr+0x2B30 e chamava vfunc nele -> crash em +0x328. So dispara quando
  registry+0x2B30 esta preenchido (por isso so apareceu hoje - coincid
  com a reiniciada do PC, NAO com driver NVIDIA nem update do jogo).
- CORRIGIDO: traffic_manager_global restaurado para 0x143714F50 (o
  valor anterior; parked fica +0x60 depois em FB0, layout preservado).
- O TETO DE LANES CONTINUA ATIVO (lane_el_cap=24 + gatilho el>40) -
  o usuario reverteu minha reversao; o teto nao era a causa do crash.
- build instalada 20:51 com a correcao do mapping + teto mantido.

## Crash 21:11 - atrelamento em cadeia (T1->T2) - causa confirmada
- Sequencia: desatrelou T2,T1,T0; atrelou T2, moveu, atrelou T1,
  desatrelou T2, reatrelou T1, puxou e foi atrelar T1 em T2 -> crash.
- Prova no log: "stale parent edge cut" disparou nos atrelamentos
  anteriores em objetos 0x18b08fcff38 e 0x18b08fd0188 - que aparecem
  no lane dump como ELEMENTOS DE FAIXA. Ou seja, o corte de aresta
  estava escrevendo em nos da malha de trafego, nao em pais fisicos.
- Consequencia: lista de lanes corrompida -> chuva de AVs "fora do
  plugin" 21:11:24 (dezenas em 0x7FF9EF4F2A96) -> crash nativo.
- CORRECAO APLICADA (autorizada): corte de aresta so escreve se o
  objeto provar que e pai real (+0x68==filho E +0x210==filho juntos;
  o linker nativo mantem os dois em sincronia - um elemento de lane
  nao tem essa coincidencia dupla). Removida a escrita de 4 bytes em
  +0x74 (truncava ponteiros; e cosmetico, o mantenedor re-escreve).
- Mesmo endurecimento aplicado em: phys_node_cut_edge (dupla prova +
  sem +0x74) e hk_chain_accum cyclic cut (sem +0x74; o corte de
  ciclo real continua para evitar hang).
- TETO DE LANES SEGUE ATIVO (lane_el_cap=24, gatilho el>40).
- build instalada 21:20.

## ~21:50 — Limpeza nativa de lanes (vfunc 5) - autorizada e instalada
- RE offline no exe identificou o ciclo de vida dos elementos:
  criador = vfunc[4] do no de lane (0x1412CABD0, monta/registra
  registros de obstaculo em lote a cada reconstrucao do no);
  limpador nativo = vfunc[5] (0x1412B3850, compartilhada pelas
  classes 0x24141E0/0x2414CA0): desregistra cada registro
  (chamando a vfunc de unregister dele) e zera as listas do no.
- A acumulacao acontece porque o no re-registra a cada
  reconstrucao, mas a vfunc 5 so roda na destruicao do no (saida
  de area). Com carreta parada por perto, registros empilham.
- Implementado lane_native_clear(no): valida que vtbl[5] do objeto
  e exatamente 0x1412B3850 e so entao chama a limpeza nativa.
- Disparo em dois niveis:
  - por lista: container (+0x8/+0x10) ou no (+0x20/+0x28) com
    n>24 -> chama vfunc5 do no dono (via lane_known_nodes p/
    container); fallback: lane_cap_mark antigo.
  - global: censo el>40 (tick ~1s da lane guard) ->
    lane_clear_all_nodes() limpa todos os nos conhecidos; o jogo
    recria so o necessario na proxima reconstrucao.
- Vantagem vs poda manual: a remocao usa o caminho oficial do
  jogo (unregister de cada registro), nao memmove forcado.
- build instalada ~21:50.

## Limpeza nativa de lanes - REVERTIDA (22:15)
- Crash novo em thread worker de fisica (exe+0x12909E0, obj+0x228 nulo)
  na primeira sessao com a limpeza - suspeita de corrida entre a
  limpeza nativa na thread principal e a leitura da mesma lista pela
  thread de fisica. Revertido para poda manual + teto por lista.
- Nota: lane_clear_all_nodes provavelmente nunca rodou (gatilho
  exige !any_loose e T2 estava solta), mas revertido por seguranca.

## Re-chain da visibilidade - corrigido (22:19)
- Log mostrou 863x "re-chained unreachable trailer" (~10/s): o pino
  de visibilidade re-escrevia o elo slave de T2 enquanto T2 estava
  ENGATADA (joint ativo, parent=T1) - escrita concorrente com a
  thread de fisica. Culprit exato do slave=null nao identificado
  (sever_phys_claims/corte ciclico/claim falso descartados: nenhum
  corta elo para trailer com joint ativo).
- Fix: pino so re-encadeia carreta SOLTA (joint==nullptr). Carreta
  engatada tem o elo gerenciado pelo jogo - plugin nao escreve.
- build instalada 22:19.

## 22:31 — Vigia de reescrita DESATIVADO
- Censo de escritas (auditoria completa): 115 pontos de escrita na
  memoria do jogo. Achados:
  - sever_phys_claims varre +0x60/+0x68 a partir do objeto CARRETA -
    mas esses campos na carreta NAO sao a corrente fisica (dump prova:
    +0x60=0, +0x68=ponteiro pra .rdata do exe). Andava memoria errada
    por design.
  - conn_commit re-escrevia slave de carretas engatadas e tecia
    estacionadas na corrente a cada ~100ms.
  - 5 escritores diferentes brigando pelo mesmo campo slave por ciclo
    = as 863 re-escritas da T2 (guerra interna, nem precisava do jogo).
- FIX: removida a chamada periodica de rebuild_parked_claim_links do
  refresh_trailer_chain (a cada ~100ms). As chamadas por EVENTO no
  atrelar/desatrelar continuam - escreve uma vez quando muda, como
  devia ser desde o inicio.
- Mantidos (escrevem so com necessidade provada): pino de
  visibilidade (agora so carreta solta, joint==null) e corte de ciclo
  slave (protecao contra stack overflow nativo).
- build instalada 22:32.

## 22:44 — Atrelar carreta no cavalo restaurava metade do estado - corrigido
- SINTOMA (relato do usuario): atrelou T1 no cavalo, T1 ficou
  travada no chao e os cabos dela ficaram ligados na T0.
- CAUSA (provada no codigo): connect_now tem dois caminhos - o
  caminho via slot (native_attach_via_slot, usado quando carreta vai
  pro cavalo) dava return no meio e pulava TODA a restauracao
  pos-engate: soltar brace, zerar freio forcado, clear_detached_flag,
  rebuild_lines(cavalo,T1), apply_cables(cavalo,T1), remover claim
  velho T0->T1, set_towed, distribuir massa. Ela ficava com brace
  cravado + freio puxado + cabos no pai antigo.
- FIX: extraido post_connect_committed() com a sequencia completa
  pos-engate; os DOIS caminhos chamam - comportamento identico.
  Nao precisou trocar T0/T1 de posicao: as conexoes se resolvem
  quando a sequencia certa roda (claim T0->T1 e removido e
  cavalo->T1 e montado).
- build instalada 22:44.

## 23:13 — Sessao de teste 23:04-23:08 lida; 4 correcoes aplicadas

FATOS PROVADOS NO LOG DA SESSAO (23:04:58 init, teste do usuario):

- SUMICO DA CARRETA NO PRIMEIRO ATRELAMENTO - mecanismo provado:
  - native_attach_via_slot RESTAURAVA o slot do ator pro valor antigo
    (linha 203: slot = prev) depois do engate. O pino de visibilidade
    caminha o slot do ator - com o slot no valor velho (ou null), a
    carreta recem-atrelada fica fora de qualquer corrente, e o pino
    nao pode consertar (pula carreta com junta - fix 22:19).
  - Log 23:06:11: atrelou T1 -> "claim velho removido: T0 soltou T1"
    + pino re-chainou so a T2 -> T1 orfa = invisivel.
  - Log 23:06:16 (2o atrelamento, apos desatrelar pela tecla T):
    funcionou porque o teardown nativo deixou o slot apontando pra T1
    - o restore pra prev manteve T1 no slot por acaso.
- T0 NAO ENGATA PELA TECLA T - provado: 6 tentativas 23:07:21-23:07:23,
  hook permitiu todas, dispatcher nativo rodou, NENHUMA junta criada.
  A mesma T0 a 4,76m engatou na hora pela janela do plugin (23:07:25 e
  23:07:31). Diferenca de estado: T0 com parked=1 + record ativo - o
  pipeline nativo da tecla T recusa carreta estacionada.
- FPS MEDIDO: 160 (antes) -> 52 (1o desatrelamento) -> 38-46 (estavel
  baixo) -> 18.8 no pior momento (23:07:52). Avalanche de obstaculos
  de lane: ins 0 -> 95 -> 997 -> 1923 -> 3123/s crescente, elementos
  totais t:0xfff 17 -> 764 -> 1556. Custo nativo de insert 11-12ms/s.
  Nao para com trem todo engatado.
- 2 AVs 0xC0000005 POR EVENTO (attach e detach): dump_phys_chain andava
  +0x60/+0x68 a partir do objeto carreta - o log mostra o "no" contendo
  ASCII do exe ("Established Credit..."). Mesmo defeito do
  sever_phys_claims (censo 22:31).
- Restauracao pos-engate (fix 22:44) FUNCIONOU: set_lines/apply_cables/
  brace rodando em todos os atrelamentos da sessao.

CORRECOES APLICADAS (build instalada 23:13):

1. Raiz da corrente no engate: native_attach_via_slot agora escreve o
   slot do ator = carreta atrelada quando a junta nasce (estado normal
   de reboque - o proprio fn191 escreve o slot=towed com junta viva).
   Falha no engate continua restaurando o valor anterior. Mata o sumico.
2. Log por sessao: diag_log abre em "w" (zera a cada partida - fim das
   35 mil linhas acumuladas) e reabre se o arquivo for apagado com o
   jogo aberto (handle morto escrevia no vazio - arquivo esponja de
   NULs desta sessao: 156 mil linhas falsas).
3. while duplicado removido (trailer_loadstate.cpp 797-798 - dois
   "while (budget > 0)" seguidos, o primeiro virava laco externo).
4. Diagnosticos que andam memoria errada desligados: dump_phys_chain
   sem chamadas (6 sitios), sever_phys_claims virou no-op (a caminhada
   por carreta e lixo provado; cortes reais ficam no hk_phys_link).
   Guarda nova: host em faixa do exe nao recebe vcall no
   dump_real_chain/repair_phys_chain - fim dos 2 AVs por evento.

PENDENTE (sem correcao ainda):
- T0 pela tecla T: o jogo recusa carreta estacionada - exige RE do
  pipeline nativo de attach pra saber o que ele checa.
- FPS: avalanche de obstaculos de lane continua sem solucao (dono dos
  elementos = classes de veiculo do exe, ours=0).

## 23:23 — Teste da build 23:13: sumiço tem OUTRO mecanismo - corrigido

TESTE DO USUARIO (23:15-23:18): desatrelou T2/T1/T0, atrelou T1 (ok,
visivel - fix do slot funcionou), desatrelou T1, atrelou T2 -> T1
FICOU INVISIVEL (solido: ele bateu nela). Desatrelou T2 -> T1 voltou.

MECANISMO PROVADO NO LOG (23:17:44):
- Pin escreveu T2->T0->T1 no 1o ciclo (.420, duas linhas) e do 2o
  ciclo em diao so a T1 re-aparece (~10x/s ate 23:17:50+) = alguem
  zera T0->T1 entre os ciclos.
- conn_commit esta confinado a eventos (grep provado) - o zerador e o
  JOGO: T0 esta estacionada COM RECORD (rec=0x175ad79c408, parked=1) -
  a gestao de estacionadas do jogo limpa o slave da estacionada
  (estacionada nao reboca nada no modelo do jogo).
- E o claim T0->T1 enraizado na estacionada e o que ESCONDE a T1: o
  jogo processa a estacionada "rebocando" a T1 e a limpa/esconde.
  Desatrelar T2 rodou unhide_detached -> T1 voltou.
- O slot do ator SEGIOU apontando pra T2 apos o attach (fix 23:13
  funcionou - o walk alcancou T2 no 1o ciclo).

FIX (instalada 23:23): o pino de visibilidade nao enraiza mais claim
em carreta COM RECORD estacionado - nem como raiz (fallback pula),
nem como portador (tail = ultimo alcancavel sem record), nem como
alvo (loop pula). O record gere a T0; claims das soltas saem de
carreta sem record (a engatada ou outra solta). Fim da guerra
T0.slave x pino e do esconde da T1.

## 23:27 — Fix 23:23 tava errado no caso tudo-solto - corrigido

TESTE (23:25): desatrelou T2/T1/T0 -> T1 E T2 SUMIRAM. Log: o pino
ficou re-escrevendo o claim da T2 a cada ciclo (guerra) e as duas
sumiram. O fix 23:23 tirou a ANCORAGEM: o record da T0 e o que ancora
a caminhada do jogo na corrente de claims (semanas de evidencia:
tudo-solto com claims T0->T1->T2 = visivel). Sem ancoragem na T0, as
soltas ficam sem ancora e o jogo esconde.

MODELO CORRETO (fechado pelos 3 testes de hoje):
- Tudo solto: claim ancora na T0 (record ancora) - comportamento
  antigo que funcionava. O jogo NAO zera T0->T1 nesse caso (o spam
  antigo era em T1->T2, com T2 engatada).
- Algo engatado: claim ancora na engatada e NAO passa pela T0 - o
  jogo limpa/esconde o que pende atras da estacionada dentro de
  combinacao engatada (provado 23:17:44).

FIX (instalada 23:27): fallback do pino com prioridade - primeiro
ENGATADO como ancora; sem engatado, o primeiro vivo (T0 com record).
Mantido: tail da caminhada = ultimo alcancavel sem record; alvos com
record sao pulados.

## 23:32 — Tecla T nao engata T0: portao achado no disasm - corrigido

RE do despachante de engate (0x1405DB530, inicio real via sentinela 003):
- ramo dl!=0 = engate imediato (escreve +0x14B8=5, +0x4C4=1.0, limpa
  +0x1168 do record, rebuild_lines, cabos)
- ramo dl==0 = tick de aproximacao da maquina de estado (escreve
  +0x14B8=3, geometria de settle) - so o jogo chama dl=1 quando a
  geometria fecha
- 1o portao: slot do ator ([self+0xC8]) nulo -> bail

Prova do log (23:07:21-23): 6 chamadas do despachante = a maquina
RODOU (o slot tinha a T0 - o jogo a escolheu mesmo estacionada), mas
o tick dl==0 nunca converge a 4,76m -> nunca vira dl=1 -> nunca
engata. O botao do plugin funciona na mesma distancia porque chama
o despachante direto com dl=1 (fn172).

FIX (instalada 23:32): no tick dl==0, se o alvo esta sem junta, o
estado do cavalo e 3 (engate em progresso) e o gap < 12m (limite do
plugin), completa o engate chamando o original com dl=1 - a tecla T
engata igual ao botao dentro do limite. Fora do limite, comportamento
nativo (aproximacao geometrica).

## 23:36 — Trava de velocidade + 2 defeitos do teste 23:33

TRAVA PEDIDA PELO USUARIO (instalada 23:35): desengate so com veiculo
parado. Canal de telemetria truck.speed registrado (float m/s) +
truck_stopped() (|v| < 0.05 m/s; sem telemetria = libera). Portas:
fn086 (botoes da janela, aviso "Pare o veiculo completamente") e
trailer_manual_detach fn284 (ponto unico: fn173 + fn191 T).

TESTE 23:33 (build 23:32) - 2 defeitos provados no log:

1. T0 SUMIU quando T1 engatou (23:34:28): o engate cortou o claim
   T0->T1 e a T0 ficou SEM claim de entrada; com o slot apontando pra
   T1, a caminhada do jogo nunca alcaca a T0 -> o jogo esconde. O
   record NAO mantem a T0 visivel sozinho quando ha engatada (so com
   tudo solto / slot nulo). E o jogo zera o claim de SAIDA da
   estacionada quando ha engatada (guerra 23:17) - logo a T0 tem de
   ficar no FIM da corrente de claims: entrada sim, saida nao.
   FIX: pino em 2 passadas - sem record primeiro, com record POR
   ULTIMO (entrada garantida, saida nula, sem guerra).
2. Tecla T RE-ENGATOU a T1 que tinha acabado de soltar (23:34:47):
   o desengate manual deixa o slot do ator apontando pra carreta
   desatrelada - apertar T de novo re-engata. O jogo nativo limpa o
   slot apos o desengate; o script manual nao limpa.
   FIX: trailer_manual_detach limpa game_actor_trailer = null no fim.

Build instalada 23:36 (inclui a trava de velocidade 23:35).

## 23:37 — Trava de velocidade estendida pro ENGATE

Pedido do usuario: engate E desengate somente parado, T2/T1 na regra.
- fn088 (botao atrelar cavalo): trava + aviso "Pare o veiculo
  completamente para engatar"
- connect_now fn174 (ponto unico do engate: fila + qualquer caminho):
  trava no topo
- force-complete da tecla T (23:32): so completa com truck_stopped()
Build instalada 23:37.

## 23:39 — CRASH no botao soltar T2: causa provada no disasm - corrigido

CRASH (23:38:34, build 23:37): desengatou T2 (junta=0x0, script
completo) e AV 0xC0000005 em exe+0x865321 (mov rax,[rdx] com rdx=0),
chamado da maquina de acoplamento exe+0x1405D7D41.

CAUSA PROVADA NO DISASM: a maquina le o slot do ator DUAS vezes
(0x1405D7D10 e 0x1405D7D36) e o caminho nulo NAO re-checa - chama
0x140865300(cavalo, null) -> AV. Essa maquina so roda com operacao de
acoplamento ativa; no nativo o slot nunca e nulo nessa janela. A
limpeza do slot que instalei no fim do desengate manual (23:36, thread
de sim) nulificou o slot com a maquina ainda ativa -> crash.

ERRO MEU: o comentario "o jogo nativo limpa o slot apos o desengate"
foi escrito sem prova - o log de 23:06:16 mostra o slot AINDA com T1
depois do teardown nativo. O nativo nao limpa na hora.

FIX (instalada 23:39):
1. REVERTIDA a limpeza do slot dentro do trailer_manual_detach.
2. Limpeza segura na manutencao (thread principal): slot apontando pra
   carreta sem junta + marcada desatrelada + maquina OCIOSA
   (truck_coupling_state==0) -> slot = null. Mata o re-engate da
   tecla T sem janela de crash.

## 23:44 — Gerenciador de estacionados apontado pro REAL (FALHAS pendente)

Trava de velocidade CONFIRMADA pelo usuario (engate+desengate so
parado - perfeita).

Teste 23:41: desatrelou tudo, atrelou T1, desatrelou T1 -> T0 sumiu.
Log: guerra de novo em T2->T0 (~10x/s apos o teardown da T1) - o jogo
zera o link que aponta PRA estacionada com tudo solto. O usuario
apontou: as carretas invertem de lugar na LISTA DE ESTACIONADOS.

Achado: o plugin vigia o gerenciador de estacionados ERRADO
(sentinel 252 = 0x143714FB0 = gerenciador de trafego, sempre vazio -
records=0 em todo log). O FALHAS.md ja tinha o diagnostico CONFIRMADO
e a solucao AUTORIZADA nunca aplicada: o real e exe+0x3715010 (a
fabrica insere ne - provado por disasm no documento).

E esse erro e o MULTIPLICADOR DE FICHAS (FALHAS): wants_record nunca
satisfeito -> retry a cada 20 ticks cria ficha nova -> cada ficha
registra a carreta como obstaculo de lane = o avalanche de FPS.

FIX (instalada 23:44): sentinel 252 -> 0x143715010. Efeito esperado:
records=3 no dump, fim do retry de fichas, unlist funciona no
reengate, avalanche de lane para. O dump do proximo teste mostra a
lista REAL de estacionados (ordem/membros) - base pra reescrever o
estado de T0/T1/T2 na ordem do jogo, como o usuario pediu.

## 23:51 — T0 sumiu de novo (minha culpa) + fim do "Bluetooth"

Teste 23:46: T0 sumiu DE NOVO. Causa provada: o gerenciador real em
exe+0x3715010 esta NULO nesta sessao (o jogo so o popula no
estacionamento NATIVO - tudo aqui foi manual; a fabrica confirma o
endereco: mov rsi,[rip+0x300AE9A] = 0x143715010). Com mgr nulo, o
bloco pre-existente "orphan parked record dropped (no manager)"
DESCARTOU o record da T0 (que e do save-load e valido) -> a T0
perdeu a ancora de visibilidade -> sumiu. Minha mudanca da sentinela
ATIVOU esse bloco (antes o mgr errado era nao-nulo e o bloco nunca
disparava).

FIX (instalada 23:51):
1. REMOVIDO o descarte de record com mgr nulo - o record do trailer
   e valido (save-load); ficha orfa se limpa no reengate (desparque
   cobre ficha velha, FALHAS 7.4). Com a sentinela no gerenciador
   real: quando ele existir, a busca acha a ficha (fim do
   multiplicador); enquanto nulo, a fabrica nao roda (guarda
   mgr!=null) e o record do trailer permanece.
2. "Bluetooth" (carreta se teleportando pra engatar): o limite de
   engate era 12m - gigantesco mesmo. attach_gap_limit 12 -> 3:
   engate (botao e tecla T) so com o cavalo praticamente na quinta
   roda, como o nativo.

## 23:52 — FPS 60->25: as duas causas medidas + custo nosso cortado

Numeros do pior momento (23:48:56, fps=24.9):
- avalanche do jogo: ins=3755/s, 1417 elementos de lane (t:0xfff) -
  o jogo processa por frame por veiculo (questao F2 do FALHAS:
  acumulo legitimo x vazamento, fecha com medir reg x unreg)
- CUSTO NOSSO: rend+upd = ~8ms/frame (um terco do orcamento de 40ms)
  - escalava com a lista: o orphan_teardown_tick varria
  lane_kill_owner por carreta engatada POR CICLO (containers x
  elementos x 96 campos) mesmo sem nada morto - FALHAS 8b ja
  documentava, caminho de reparo "nao aplicado"

FIX (instalada 23:52): a varredura por dono so roda na janela de
poda (lane_prune_frames > 0, armada no evento) - reparo so quando
necessario. O custo do plugin deixa de escalar com a lista.

## 23:55 — LEI DE ESCRITA gravada no AGENTS.md + fim da guerra do pino

ORDEM DO USUARIO: o plugin escreve UMA VEZ por evento - estacionou em
tal local, atreleou no cavalo, atreleou na carreta - e o jogo LE o
estado. Nada de 8x por frame, nada de 100x por frame.

GRAVADO NO AGENTS.md (topo do arquivo):
- ESTADO = uma escrita no evento, nunca por frame
- Escrita continua so para CONTROLE continuo (freio, direcao, ar,
  mascara de roda, luzes - as features vendidas)
- Manutencao por-frame = so LEITURA; estado nosso que sumiu =
  re-escreve UMA vez; o jogo zerando 3x seguidas = estado invalido
  no modelo dele -> registra o conflito e PARA (pin_retry)
- Rearme so no evento

APLICADO NO PINO (instalada 23:55): o pino contava as tentativas
consecutivas no mesmo claim (pin_retry por carreta) - 3 rejeicoes
seguidas do jogo = para de reescrever e loga o conflito UMA vez
("jogo rejeita claim - parando de reescrever"). Rearme no evento
(rebuild_parked_claim_links zera os contadores). Fim da guerra de
10 escritas/s: uma escrita, e se o jogo recusa, o log diz qual claim
e quem rejeitou - dado pra consertar o estado, nao briga.

## 00:02 — A regra do jogo PROVADA pela linha nova: T0 e a RAIZ

Teste 23:56-23:58 com o pin_retry ativo: a linha nova pegou o claim
rejeitado exatamente - "jogo rejeita claim T1->T0" (solta ->
estacionada). Cruzando os 5 casos do dia:

- T0 como RAIZ (sem claim de entrada) + saida T0->T1->T2: ACEITO em
  todo estado (sobreviveu 16s tudo-solto e 5s com T2 engatada)
- Claim de entrada NA T0 (T1->T0): REJEITADO - o jogo zera toda vez

FIX (instalada 00:02): o pino enraiza a corrente de claims na carreta
com record (T0) SEmpre - se a caminhada nao a alcancou, corta os
claims de entrada nela (o jogo os rejeita mesmo) e monta T0 ->
(soltas na ordem do canon). O estado que o jogo aceita em todos os
casos observados.

Nota: a tecla T agora so completa o engate a menos de 3m (limite
apertado 23:51) - "nao funcionou nativamente" do usuario = ele estava
longe. Comportamento correto (fim do Bluetooth).

## 00:07 — Regra "T0 raiz" REFUTADA pelos dados; proximo passo: a ordem real

Teste 00:04-00:05: o jogo REJEITOU TAMBEM o claim da raiz -
"jogo rejeita claim T0->T2" (00:05:14, T1 engatada). A regra
"T0 como raiz e sempre aceita" que derivei dos 5 casos anteriores
esta REFUTADA. Quadro completo:

  ACEITOS:  T0->T1->T2 (tudo solto, 16s) | T0->T1 (T2 engatada, 5s)
            T2->T0 (T1 engatada, 10s)
  REJEITADOS: T1->T0 (T2 engatada) | T0->T2 (T1 engatada)

Nao ha regra simples dedutivel desses 5 casos - e nao vou inventar
outra. O "as duas desapareceram" do usuario = o jogo FECHOU na
sequencia (dtors das 3 carretas + scs_telemetry_shutdown 00:05:30,
fechamento limpo, sem crash).

PROXIMO PASSO (o palpite do usuario de 23:41 - a ordem da lista de
estacionados): instrumentado o dump do record (dump_record_links) -
despeja os campos 0x0-0x198 do record de cada carreta marcando
ponteiros pra canon e pra outros records - achar os elos da lista
(next/prev/indice) e a ORDEM REAL do jogo. Os claims passam a seguir
essa ordem - dado, nao chute.

Build instalada 00:07.

## 00:14 — RE completa: a regra do jogo PROVADA pelos 5 casos - floresta

Varredura completa do exe por escritas no campo slave (+0x1058):
- 7 sitios de store: TODOS sao criacao/destruicao de objeto
  (construtores zerando o bloco, caminho de release)
- ZERO stores XMM cobrindo o bloco
- CONCLUSAO: nao existe "validador" que zere claim por escrita direta.
  O jogo valida o GRAFO slave ao vivo.

A regra que explica os 5 casos medidos:
  O GRAFO SLAVE TEM DE SER FLORESTA - cada carreta no maximo UM
  incoming (um "dono"), nenhum ciclo. O jogo zera qualquer escrita
  que crie segundo incoming ou feche ciclo.
  - T0->T1->T2 (1 incoming cada): aceito
  - T0->T1 (T2 engatada): aceito
  - T1->T2 + T2->T0 (T1 engatada): corrente truck->T1->T2->T0: aceito
  - T0->T1 + T2->T1 + T1->T0: T1 com 2 incoming E ciclo T0->T1->T0:
    zerado (a guerra do 23:58 era o ciclo!)
  - T1->T2 + T0->T2: T2 com 2 incoming: zerado (o rejeite do 00:05)

FIX (instalada 00:14): o pino agora, antes de religar tail->alvo:
1. CORTA o incoming antigo do alvo (quem aponta pro alvo) - uma
   escrita, com log
2. VERIFICA ciclo: se a caminhada a partir do alvo volta no tail,
   ignora a escrita com aviso (nunca fecha ciclo)
O estado resultante e floresta = o que o jogo aceita. Escrito uma
vez por mudanca (a lei de escrita).

Build instalada 00:14.

## 00:23 — Leitura de memoria: despejo completo dos veiculos em arquivo

Pedido do usuario: ler a memoria do computador, sem teste baseado em
suposicao. Feito em tres frentes:

1. MINIDUMPS de crash lidos (parser proprio em tools/dump_visibility.py):
   os dumps de 71MB nao contem os objetos do jogo (so stacks) - 0
   objetos encontrados. Gerenciador de estacionados confirmado NULO
   na memoria (exe+0x3715010 = 0x0).
2. ENDERECOS.md/CONEXOES.md lidos - mapa dos gerenciadores
   reconciliado: trafego F50, parked-factory FB0+0x60 (=0x3715010),
   camera FB8. A lista de estacionados nao existe em sessao MP
   (o jogo so a cria no estacionamento nativo).
3. DESPEJO PELO PLUGIN (instalado 00:23): memdump_world() le a
   memoria COMPLETA (0x1400 bytes) do game_actor, do cavalo e de
   cada carreta e escreve em
   Documents\Euro Truck Simulator 2\br_carretas_memdump.txt
   - uma vez por sessao, no primeiro frame no mundo com canon
     populado; leitura pura, nenhuma escrita.
   Com o arquivo em maos a analise e feita no disco: mapa completo
   de campos, comparacao de estados, o flag de visibilidade se
   isola por diferencas - sem testar nada.

## 00:29 — MEMORIA AO VIVO fecha o mecanismo do sumiço

O usuario abriu o jogo e eu li a memoria EM TEMPO REAL
(tools/live_mem.py, ReadProcessMemory) fotografando cada estado:

- ANTES (load): trem T0->T1->T2 montado, cavalo solto, TUDO VISIVEL.
  T0 rec=0x1C5BBECD698, flags +1168/+1228 = 00.
- pos_det_T2T1: T2 e T1 soltas. Claims sobreviveram (T0->T1->T2),
  flags de estacionado subiram (+1168=01 +1228=01). T0 visivel.
- T0_sumiu (T1 engatou): o claim T0->T1 foi CORTADO pelo engate
  ("claim velho removido") -> T0 sem ninguem apontando -> SUMIU.
  O record sozinho NAO protege (a lista de estacionados nao existe
  em MP - gerenciador nulo confirmado na memoria viva).
- T0_sumiu_2 (T1 solta de novo): corrente re-tecida T0->T2 mas a
  T1 ficou ORFA (sem link nenhum) -> T0 piscou e sumiu.
- T0_T2_sumiu: T1 re-engatada, T0+T2 sumidas.

COMPARACAO DECISIVA: T0 visivel (tudo solto) = corrente cobre TODOS
(T0->T1->T2). T0 sumida = corrente T0->T2 com T1 ORFA. **Orfa fora
da corrente faz o jogo esconder o conjunto.**

E a causa da orfa: o MEU teto de retry (23:55) - o claim T0->T1
rejeitado 3x num estado anterior fez o pino pular a T1 PARA SEMPRE.

FIX (instalada 00:29):
1. Teto por ARESTA: tail novo tem direito a tentar o alvo de novo
   (pin_last_tail[] - tail mudou, contador zera). Fim do orfamento.
2. Rodadas no pino (max 3): apos as 2 passadas, se sobrou solta fora
   da corrente, varre de novo com o tail novo ate cobrir todos.
3. Guardas da floresta mantidos (corta incoming antigo, nunca fecha
   ciclo).

## 00:35 — Teste 00:29 pegou MEU varredor de orfas no flagra (log)

Teste: desatrelou T2, T1, T0 -> T1 e T2 sumiram. Memoria ao vivo:
TODOS os claims zerados, sem guerra (2 fotos, 2s de intervalo) = o
pino NAO reconstruia. O log mostrou o por que - dois defeitos MEUS
no varredor de orfas (00:29):

1. vis[] velho entre as rodadas: o que o pino acabou de religar
   continuava "nao visto" -> ele tentava de novo -> T2->T2 (auto-liga,
   o guarda de ciclo pegou)
2. corte do incoming antigo ANTES da checagem de ciclo: a escrita
   ignorada mesmo assim cortava o claim da rodada anterior -> o pino
   DESTRIA a propria corrente a cada ciclo (o log mostra: re-chained
   T1, re-chained T2, cortado T0 soltou T1, T2->T1 fecharia ciclo,
   cortado T1 soltou T2, T2->T2 fecharia ciclo - a cada 10s)

FIX (compilado 00:35, aguardando fechamento do jogo p/ instalar):
1. alvo religado entra no vis[] (marcado visto) - fim do re-claim e
   do auto-liga
2. checagem de ciclo ANTES do corte - escrita ignorada nao corta
   nada

INSTALADA 00:39 (jogo fechado, copia limpa).

## 00:45 — MONITOR DE MEMORIA AO VIVO fecha o mecanismo: CICLO = DESTRUICAO

Ferramenta nova: tools/monitor_mem.py - roda em segundo plano, fotografa
a memoria dos veiculos a cada 500ms e registra SO as mudancas com
horario (br_mem_timeline.txt). O usuario jogou normal e a linha do
tempo completa foi capturada sem ele passar feedback nenhum.

LINHA DO TEMPO (00:41:24 - 00:42:47):
- load: trem T0->T1->T2 montado, cavalo solto
- engatou T0 no cavalo, soltou T2, soltou T1, soltou T0 - claims
  sobreviveram a tudo, T0 visivel
- engatou T1 no cavalo: engate cortou T0->T1, o varredor de orfas
  REAGIU certo (escreveu T2->T0 - corrente truck->T1->T2->T0)
- a re-tecida trocou o arranjo (T1->T0, T0->T2)
- soltou T1: T2->T0 escrito com T0->T2 ja vivo = CICLO T0->T2->T0
- 00:42:47: OS TRES OBJETOS DESTRUÍDOS (memoria ilegivel - nao e
  esconde, e destruicao!)

CAUSA: o escritor do ciclo foi o conn_commit (a re-tecida) - as 4
escritas de claim dele NAO tinham guarda de ciclo (so o pino tinha).

FIX (instalada 00:45): claim_link_write_safe() - caminha o alvo
seguindo slave; se alcancar a origem, recusa a escrita (fecharia
ciclo). Aplicada nas 4 escritas do conn_commit. Nenhuma escrita de
claim no plugin agora pode fechar ciclo.

## 00:52 — Tecla T com o cavalo DEBAIXO da carreta: slot vazio - corrigido

TESTE DO USUARIO (sessao 00:45-00:48): tudo funcionou (nada sumiu,
FPS 42-53, desatrelou/reatrelou tudo) - UNICO problema: a tecla T
nao engatou a T0 com o cavalo DEBAIXO da carreta (distancia ~zero).
O botao engatou a 4,44m.

CAUSA (log 00:47:29-37): 4 ticks do despachante, nenhum engate - o
SLOT DO ATOR estava VAZIO. O jogo se recusa a escolher carreta com
flags de estacionado (+0x1168=01, ligadas pelos desengates) como
alvo da tecla T. O botao funciona porque escreve o slot diretamente
(fn172). Nao e distancia - e a recusa do jogo a estacionada.

FIX (instalada 00:52): no despachante com slot vazio, o plugin faz
o que o botao faz - escolhe a carreta sem junta MAIS PROXIMA do
cavalo dentro do limite do hook_far, LIMPA as flags de estacionado
(parked=0, registered=0), escreve o slot e completa o engate na
hora (use_dl=1). Sem carreta por perto: comportamento nativo.
A tecla T agora = o botao.

## 00:55 — REGRA DO USUARIO: a tecla T e SO da T0

O usuario definiu o desenho: a T0 e a conexao PRINCIPAL (a nativa,
a com record). T1 e T2 tem BOTOES proprios no plugin. A tecla T
atrela SOMENTE na T0 e desatrela de qualquer carreta.

FIX (instalada 00:55): o seletor da tecla T (slot vazio) nao pega
mais "a mais proxima" - procura a carreta COM RECORD (a T0 por
definicao), confere junta solta + dentro do limite do hook_far,
limpa as flags de estacionado, escreve o slot e completa na hora.
T0 engatada ou longe: a tecla T nao faz nada (os botoes sao para
T1/T2).

## 09:00 — Tecla T corrigida de vez + trava reposicionada (instalada)

Leitura completa dos arquivos do sistema (INDICE.md, carga/descarga,
loadgame) revelou duas coisas:

1. INTERACAO QUE EU CRIEI SEM VER: a trava de velocidade dentro do
   trailer_manual_detach bloqueava o RESTORE DE CARGA (fn116
   restore_parked -> disconnect_now -> manual detach). Corrigido:
   a trava saiu do manual detach e voltou pros pontos de entrada -
   fn086 (botao, ja tinha) e fn191 (tecla T: os dois caminhos de
   desengate bloqueiam em movimento, inclusive o despachante nativo
   fica retido enquanto anda). O restore interno livre.

2. A TECLA T: a memoria ao vivo (00:58) provou - o slot do ator
   tinha a T1 (o picker do jogo recusa estacionada e escolhe outra),
   o despachante tentava engatar T1 e a geometria nao fechava.
   Corrigido: a tecla T SOBRESCREVE o slot com a T0 (a com record)
   sempre que o slot nao for ela - limpa as flags de estacionado,
   escreve o slot, completa na hora (use_dl=1). Log: "attach: tecla
   T -> T0 (record) ... (slot tinha ...)".

BUILD INSTALADA 09:00 com tudo: varredor de orfas corrigido, guarda
de ciclo no conn_commit, seletor da tecla T = T0, trava de
velocidade nos pontos de entrada do usuario.

## 09:05 — T1/T2 sumindo no engate da T0: direcao da corrente - corrigido

TESTE DO USUARIO (08:56-08:57): a tecla T engatou a T0 PERFEITAMENTE
(a correcao 09:00 funcionou - o log pegou "attach: tecla T -> T0
(record) ... (slot tinha 0x256fac7c508)"). MAS T1 e T2 sumiram.

CAUSA (log 08:57:09-12): 44x "pin: escrita T0->T1 fecharia ciclo -
ignorada". Os claims estavam na direcao CONTRARIA (T1->T2->T0 - a
T0 no FIM com claim de entrada, arranjo do varredor de orfas de um
estado anterior). Com a T0 ENGATADA, estender a corrente pela raiz
(T0->T1) fechava o ciclo T0->T1->T2->T0 - a guarda bloqueou (ela
funcionou!), mas ninguem INVERTEU a corrente. T1/T2 ficaram
alcancaveis so pelo claim de entrada na T0 engatada -> o jogo
escondeu.

FIX (instalada 09:05): o pino, antes de estender a corrente, CORTA
os claims de entrada na raiz (tail) - a direcao vira
truck->T0->T1->T2, a unica valida com a T0 engatada. O guarda de
ciclo permanece como rede de seguranca.

## 09:10 — A guerra consigo mesmo: o SLOT MENTE - corrigido

TESTE DO USUARIO (build 09:05): "piorou" - soltou T2, T1 e T2
piscaram e sumiram. O log pegou a guerra: o corte da raiz (09:05)
cortava T1->T0, o pino re-claimava T0, o guarda cortava de novo -
a cada 120ms, O PINO BRIGAVA COM ELE MESMO.

CAUSA RAIZ (fechada agora): a caminhada do pino comecava no SLOT DO
ATOR - e o slot MENTE (o jogo o consome/troca apos o engate). Com o
slot errado, a caminhada nao enxergava a T0 engatada -> o pino a
marcava "inalcançavel" -> a duas-passadas religava -> o corte da
raiz cortava o claim de volta -> guerra + pisca-pisca.

FIX (instalada 09:10):
1. A cabeca da caminhada = a JUNTA (o rebocado com junta viva via
   truck+0x1580), nao o slot - a junta e a verdade da corrente
2. O corte da raiz so quando a raiz veio do FALLBACK (a caminhada
   nao a alcancou) - nunca cortar o claim que torna a carreta
   alcancavel no arranjo dela

## 09:15 — F2 instrumentado: registrar x desregistrar contados

O caminho documentado no FALHAS pra fechar a questao do FPS: medir
REGISTRAR (0x1412B7A20, 16 callers, sem contador nenhum) x
DESREGISTRAR (0x1412B8190, 12 callers). Se reg >> unreg = vazamento
(achar o caller desbalanceado); se balanceado = acumulo legitimo
(fila de trafego - decisao de design).

Instrumentacao (instalada 09:15):
- Sentinelas 287/288 + hooks hk_lane_registrar/hk_lane_desregistrar
- Contadores por segundo no health: reg=/unreg=/rours= (rours =
  registros cujo ocupante e carreta canon - o ocupante embute a
  handle em +0x80 estatica ou +0x10 dinamica)
- Log de instalacao: "lane_registrar hook ativo" / "lane_desregistrar
  hook ativo"

Proximo teste: o health vai mostrar reg x unreg por segundo - o
dado fecha a questao F2 e aponta a correcao do FPS.

## 10:00 — FPS: o killer era a telemetry.dll (provado pelo teste A/B)

TESTE A/B DO USUARIO (sem plugins x com plugins, mesma fila de
trafego): SEM plugins = GPU 99% saturada, FPS 47-57. COM plugins =
GPU 50-60% ociosa, FPS 15-28. GPU ociosa = a thread do jogo
BLOQUEADA - o padrao classico de callback de telemetria travando o
frame. Isolado: o BR Carretas sozinho MANTVE o FPS (GPU saturada) -
o killer era a telemetry.dll. Ela fica fora pra sempre.

## 10:05 — Sumico da T2 no arranjo T0 engatada: aresta invalida - corrigido

LOG 09:54 pegou o mecanismo completo: a tecla T engatou a T0
(a correcao disparou - o slot tinha a T2, o plugin sobrescreveu),
o acoplamento da T2 no cavalo foi SUBSTITUIDO e a T2 ficou solta
com claim T2->T0 apontando pra T0 ENGATADA. Claim de entrada em
carreta engatada = aresta invalida - o jogo esconde quem a carrega.
O pino re-claimou a T2 pela direcao certa, mas a aresta invalida
sobreviveu (escrita POS-evento - o corte do post-connect ja tinha
rodado). Soltou a T0 -> a aresta virou valida -> a T2 voltou.

FIX (instalada 10:05): o corte de claims de entrada na carreta
ENGATADA agora roda tambem na manutencao do pino (a cada ciclo, uma
escrita por aresta invalida, logada) - a aresta morre no instante
em que aparece, nao so no evento.

## 10:10 — Monitor v2: tres defeitos corrigidos

1. Offsets de junta/pai errados (+0xFB0/+0x1050 -> +0xFE0/+0x1080)
2. Sonda de vida lendo endereco nao-mapeado (0x10000) - reconexao
   em loop sem nunca ler - agora testa a leitura no proprio caminhao
3. O despejo so era lido DEPOIS da sonda (que exige o despejo) -
   loop infinito - agora o despejo carrega na reconexao
O monitor registra a saude dele na propria linha do tempo
(conectado pid=X / enderecos novos / falhas de leitura).

## 10:20 — Mapeamento dono->carreta: o censo agora conta as NOSSAS

O padrao medido (rua lateral, 10:14): a REMONTAGEM do conjunto
dispara a tempestade - registros estaticos 217 -> 1232 em segundos,
o processamento nativo custa 24ms/s, o FPS cai de 68 para 35 - e a
lista esvazia devagar (1045 -> 402) enquanto o FPS "levemente vai
voltando". O jogo registra obstaculo pra cada roda/eixo da
combinacao inteira no engate.

O censo (lane_census) so contava como "nossos" os donos que eram o
CORPO da carreta - os donos reais sao SUB-OBJETOS de veiculo
(ours=0 sempre). FIX: o mapeamento agora varre os campos DA carreta
procurando o dono (direcao trailer->dono), com cache (cada dono
mapeado uma vez, logado: "lane owner X pertence a canonN"). O
health's ours= agora mostra o numero REAL de elementos das nossas
carretas na tempestade.

Proximo passo: se ours= for alto (centenas), bloquear o registro
das nossas carretas na origem (o caminho de registro que a RE
revelar) - o pico da remontagem morre e o FPS nao cai.

## 10:45 — MODO MINIMO (ordem do usuario): padrao nativo = plugin parado

A regra do usuario: tudo conectado (o trem nativo completo) = o
padrao do proprio jogo - o plugin NAO vigia, NAO mantem, NAO toca
a suspensao (depois do save da altura, o jogo se mantem sozinho).
Qualquer desengate (T2, T1, separar T1/T2) = o plugin ativa.

Implementado (instalada 10:45):
- native_pattern(): tudo engatado + nada destacado + sem orfaos
- A suspensao/lift: so roda FORA do padrao nativo
- O pino de visibilidade (refresh_trailer_chain): so roda FORA do
  padrao nativo
O plugin em padrao nativo = custo ~zero por frame.

## 10:50 — A tempestade medida ate o fim: o saldo do jogo

O teste parado do usuario (10:36-37): SEM remontagem nenhuma, a
lista cresceu 21 -> 1773 em 64s - o trafego registra ~4500/s, o
jogo desregistra ~97% (a varredura dele roda), o saldo +27/s
acumula. Os clusters: elementos ESPALHADOS (sem duplicatas) - sao
os conjuntos de pathfinding dos AI's. O saldo positivo e o
balanceamento interno do jogo com trafego denso (o usuario esta
com trafego 1 = o pior caso). O plugin nao pode tocar esse caminho
com seguranca (os hooks quebraram os desengates).

O crash do teste: o game.log mostra "Too many indices in
'traffic_trailer' (241446)" - modelos do MOD de trafego com indice
demais pro buffer do renderizador - crash source classico, NAO o
diagnostico do plugin (tudo guardado por __try, custo desprezivel).

## 11:00 — A FONTE DA TEMPESTADE ERA NOSSA: lane_kill_owner removido

ORDEM DO USUARIO: ler o INDICE, descobrir as funcoes, achar o
problema. O indice apontou: fn273 orphan_teardown_tick, fn274
lane_kill_owner, fn127 find_trailer_provider (a carreta TEM um
provider de obstaculo proprio).

O problema (nosso): o plugin MATAVA os obstaculos de pista da
carreta em dois eventos - o hook break (a junta sumiu) e o un-park
(a carreta com record sai do estacionado). Mas a carreta solta na
rua E um obstaculo que o AI precisa ver - o provider do jogo
RE-REGISTRA o que matamos. Matar -> re-registrar -> matar de novo
no proximo evento = A GUERA que alimentava a tempestade. Cada
engate/desengate adicionava mais registros a lista.

FIX (instalada 11:00): as duas mortes removidas. O provider do
jogo gerencia os proprios registros; a varredura dele limpa o que
envelhece (medido: 2183 -> 552 em 5s). O plugin para de brigar com
o sistema de lane do jogo.

## 11:10 — A ESPIRAL DA MORTE: o guarda de lane nunca desarmava

O teste do usuario decidiu: o jogo SEM plugin + o mod de trafego
funciona; COM plugin despenca na remontagem. A interacao e o
plugin - e o indice apontou: hk_lane_obstacle_insert (fn135), o
hook no caminho de insercao (~4500 inserts/s).

O mecanismo completo (nosso):
1. A guarda de lane arma com qualquer carreta solta e SO desarma
   se o censo mostrar el=0 - mas com trafego el NUNCA e 0 (65-106)
   -> A GUARDA NUNCA DESARMA
2. O proprio guarda: el > 40 -> arma a janela de prune -> com
   trafego SEMPRE > 40 -> janela de prune PERPETUA
3. Com a janela ativa, CADA insert roda o sanitizador (a varredura
   O(container) na thread de fisica) -> 4500 sanitizacoes/s ->
   a thread engasga (sd) -> a GPU starva -> o FPS despenca
4. Espiral: o jogo lento -> os elementos acumulam -> el continua
   >40 -> a janela permanece -> perpetuo

FIX (instalada 11:10): o guarda desarma apos 60 checagens quando
nao ha mortos pendentes (sem exigir el=0) e o gatilho el>40 ->
prune foi REMOVIDO (o prune so em eventos reais). Os inserts
voltam a nativo (sem toll) quando a guarda nao tem trabalho.

## 11:15 — O FURO: o guarda RE-ARMAVA a cada frame com carreta solta

O teste do usuario: "continua a mesma coisa". O furo: a condicao
de armar era (any_loose || prune_frames>0) - "carreta solta" e o
estado NORMAL do uso do plugin -> o guarda re-armava A CADA FRAME,
o desarme das 11:10 era seguido do rearme imediato, a toll do hook
voltava perpetua.

FIX (instalada 11:15): a guarda arma so por EVENTO (arm_lane_guard
no desengate) ou na janela de prune. O estado "solta" nao arma mais.
Entre os eventos, os inserts rodam nativos - zero toll.

## 13:10 — ARQUITETURA DE DOIS SISTEMAS (ordem do usuario)

O usuario definiu: RUA e EMPRESA sao sistemas DIFERENTES que devem
funcionar separadamente - nada de unir tudo (o "lencol pequeno").

CRIADOS:
- DESATRELAMENTO-RUA.md: as funcoes da rua (fn173/174, o pino, a
  tecla T, o modo minimo), as regras provadas, os estados bom
- DESATRELAMENTO-EMPRESA.md: a origem = o commit 6d64f1b
  ("carregamento e descarregamento ok" - o estado provado funcional),
  as funcoes de carga/descarga, as pendencias numeradas

O CRASH MEDIDO (13:01): o ENTER de carga com carretas destacadas -
o caminho de carga do jogo ITERA as carretas (rsi=c0,c1,c2) e chama
exe+0x857458 com rcx=0 (o campo nulo porque destacada) - deref nulo
= crash. O guardia do load e pendencia 3 do arquivo da EMPRESA.

O DISCRIMINADOR DO PINO (instalada): cada tentativa loga
"pin tentativa tail->alvo aresta_antes=X head=Y" - aresta_antes
nao-nulo = caminhada cega; nulo = o jogo cortou (a guerra). O dado
das 12:53: aresta_antes=alvo (o claim ja existia) + head=T0 correto
= DESCONEXAO (o sub-trem solto fora da corrente da T0) - o pino
enraizou o fragmento INVERTIDO (T2->T1) em vez de T0->T1->T2.

FIX instalada: a prioridade do tail = o record-holder SOLTO primeiro
(a ancora que o jogo caminha nas correntes soltas), depois o
engatado, depois o primeiro vivo.

O DIFF DO GIT: 6d64f1b = o estado funcional de carga/descarga;
o trabalho nao commitado = +3253/-438 linhas (as minhas mudancas).
A extracao das variantes da EMPRESA sai do 6d64f1b, funcao por
funcao, sem sobrepor as da rua.

## 13:30 — ORDEM DO USUARIO: carga/descarga = O ESTADO EXATO DO 6d64f1b

O usuario foi claro: o sistema de carregar/descarregar com carretas
separadas JA FUNCIONAVA no GitHub (o commit 6d64f1b). As minhas
mudancas no desatrelamento da rua MODIFICARAM esse sistema. A ordem:

1. As funcoes de carga/descarga voltam ao estado EXATO do 6d64f1b
2. As minhas adicoes nesses caminhos SAEM: o guardia do VEH (o pulo
   do 0x857458), o pin de posicao no load, o que mais eu coloquei
3. O sistema da RUA (trailer_coupling.cpp, o pino, a tecla T) FICA
4. NADA de teste ate estar do jeito do GitHub - sem suposicoes

A EXECUCAO (pendente):
- O diff funcao por funcao entre 6d64f1b e o atual nos arquivos de
  carga (trailer_loadstate.cpp, trailer_loadunload.cpp,
  trailer_loadgame.cpp)
- Cada funcao de carga/descarga restaurada ao corpo do 6d64f1b
- As minhas adicoes removidas desses caminhos
- O build so depois do estado conferido contra o 6d64f1b

## 13:45 — ORDEM: LER TODO O CODIGO, DEPOIS EXECUTAR

O usuario deletou o DESATRELAMENTO-EMPRESA.md (a extracao quebrada)
e RESTAUROU as minhas mudancas no unpark_jointed (a condicao
!parked_rec + o lane_prune de volta). A ordem: LER TODO O CODIGO
dele primeiro, depois executar a ordem (o sistema de empresa do
GitHub rodando separado do da rua).

A LEITURA (em progresso):
- unpark_jointed (fn143): LIDA por completo - o un-park das
  reengatadas: as flags zeradas, o freio solto, o record deslistado,
  o lane prune. O estado atual = a versao do usuario (com a
  condicao do record)
- PENDENTE: os demais arquivos (o loadstate inteiro, o loadunload,
  o loadgame, o coupling, o manipulation, o shared, o detach, o
  lines, o camera, o brake, o core)

## Carga/descarga restaurada ao GitHub — funcao por funcao

Ordem do usuario: a rua/local seguro fica igual; o carregamento e
descarregamento roda IGUAL ao GitHub `6d64f1b`, byte a byte. O restauro
feito funcao por funcao (o metodo do usuario, nao o atalho do checkout):

**trailer_loadunload.cpp** -> ZERO diff vs o GitHub
- Removidos os 36 logs de diagnostico (a variavel `dbg`, os 3 externs e
  todos os `if ( dbg )` espalhados pela funcao)
- Removidos meus gates: o restore imediato no fim do respawn, o retry
  de freeze no reengate, o desfazer de re-engate no teleporte, o gate
  da posicao congelada

**trailer_loadgame.cpp** -> ZERO diff vs o GitHub
- O carry-arm no load ja tinha sido revertido pelo usuario

**trailer_loadstate.cpp** -> funcoes de carga/descarga ZERO diff
- `hk_trailer_state_apply` (fn121): `hk_timer` + wrapper lambda removidos
- O purecall: a checagem de faixa do GitHub restaurada
- O descarte de record orfao: a versao do GitHub restaurada
- `unpark_jointed` (fn143): condicao `!parked_rec` removida, o
  `lane_prune_frames` removido, o `parked_record_unlist` simples de volta
- Os hunks do lane/diagnosticos ficaram (o sistema da rua - a outra
  forma de funcionar)

**A arquitetura das duas formas:**
- Rua/local seguro: o sistema atual (visibilidade, claims, lane) intacto
- Carga/descarga: as funcoes do GitHub, byte a byte - sem gate novo,
  sem mecanismo meu

Documento de apoio criado: `CARREGAMENTO E DESCARREGAMENTO.md`

## 18:20 — Eixos e suspensão mortos: a trava do "padrão nativo" engoliu as features

Sintoma do usuario: painel "Sem dados de eixo" nas 3 carretas (todas com
eixo levantavel real - o botao nativo erguia) e "Salvar altura" sem
efeito (suspw+0 a sessao inteira).

Diagnostico (leitura ao vivo com o jogo aberto - live_axles.py):
- Controlador de eixo presente nas 3 (carreta+0x6C8), estado=3 (eixos
  erguidos), mask=0x1, count=2
- Array de flags VIVO nos offsets antigos: chassi+0x5C8=ptr valido,
  +0x5D0=2, flags = 01 00 (eixo 0 levantavel) nas 3 carretas
- Suspeita anterior (offsets 0x5C8/0x5D0 mortos no 1.62.3.3) era
  FALSA - o scan linear do .text morria no primeiro byte invalido e
  reportava "zero referencias" errado

Causa real: `trailer_manipulation.cpp:706` rodava
`lift_axles_update` + `suspension_update` DENTRO de
`!cargo_freeze() && !native_pattern()`. native_pattern() = verdadeiro
quando tudo esta engatado/nada solto/sem orfaos - ou seja, o estado
normal de jogo matou as duas features (a trava foi feita pra vigia de
corrente, nao pra features). Reparo: gate reduzido a `!cargo_freeze()`.

Suspensão - limites reais extraidos do exe (0x14085cc39):
- O campo trailer+0x4D4 opera em [0.5, 2.0] (comiss vs 0.5 e 2.0 no
  clamp nativo); escrever 0.0 afundava a carreta no chao e a trava
  de sanidade (f<0.3) impedia resgatar (presa pra sempre)
- Régua remapeada: 0->0.5 (piso do jogo), 50->1.0, 100->2.0 (teto)
- Trava de sanidade agora rejeita so valor insano (não-finito,
  <0, >4) - permite resgatar carreta afundada

Teclas (settings.cpp): "Selecionar carreta N p/ engate (T)" estava
errado - a tecla ATRELA direto no cavalo. Renomeado:
"Atrelar carreta 2 no cavalo" / "Atrelar carreta 3 no cavalo".

## 18:25 — FPS despencou (24-35): a LEI DE ESCRITA violada por frame

Depois do reparo acima, o loop de eixos das carretas passou a rodar no
estado normal - e ele REESCREVIA flags[eixo] + mascara do controlador
TODO FRAME, sem checar se mudou. A secao do cavalo no mesmo arquivo ja
fazia certo (so escreve quando difere). Ordem do usuario: o jogo recebe
a decisao dele QUANDO ele decide (clique), nunca por frame.

Reparo: os writes de flags/mascara das carretas agora dentro de
`selection != applied_mask` - escrita unica no evento, identico a
secaoo do cavalo. A deteccao (leituras) continua por frame - e o que
alimenta o painel.

Sinais do periodo ruim no health: sim stall duas vezes (heartbeat
morto 30+ frames), worst=76ms, fps 34.7. Custos medidos do plugin por
frame seguiam pequenos - o churn era no lado do jogo.

## 18:29 — Crash 0xC0000374 (heap) ao desatrelar T0 no posto — RESOLVIDO

Sintoma: clicou Desatrelar T0, o detach rodou (77 elementos de lane
despejados, pin re-encadeou) e o processo morreu no meio. WER: heap
corruption em ntdll - nao uma AV, escrita errada muito antes.

Causa raiz (provada por RE + codigo, nao suposicao):
- `prune_lane_containers` rodava na thread de RENDER fazendo memmove +
  *count + carimbos dentro dos vetores de lane do jogo, enquanto a
  thread de SIM rebuildava os mesmos vetores (ins=722/s). Sem trava:
  o plugin lia o ponteiro do array, o jogo realocava e liberava o
  antigo, o memmove caia no bloco morto -> free-list corrompida ->
  0xC0000374 segundos depois. readable_ptr/__try NAO detectam heap
  liberado - a pagina continua mapeada.
- `lane_kill_owner` em carreta ENGATADA (coupling) ainda alimentava a
  guerra matar/re-registrar - a tempestade de 722 inserts/s que comia
  o FPS antes do clique era isso.
- RE do exe confirmou: insert (0x1412B7DD0) e scan (0x1412B85A0) leem
  el+0x88 -> owner+0x10 sem checar nulo - a protecao e necessaria,
  mas o jogo so desregistra elemento na destruicao do no (vfunc5,
  FALHAS) - elemento morto fica na lista ate o rebuild natural.

Reparo (o defeito deixa de existir, nao e mitigado):
- Mortas: prune_lane_containers, lane_cap_mark, lane_kill_owner,
  lane_dead_pending, lane_owner_refs + os 2 call-sites do kill no
  coupling (orfa + varredura de engatada). Fim de TODA escrita em
  estrutura de lane fora dos hooks - fim da guerra junto.
- lane_sanitize_node virou carimbo unico atomico (el+0x88=dono-falso)
  rodando so dentro dos hooks (serializado com o jogo), com prova de
  vida: so escreve se el+0x00 for vtable dentro do exe - elemento
  liberado falha na checagem e sua memoria nunca e tocada.
- Desarme da guarda nao consulta mais censo de mortos (elemento morto
  nao exige trabalho - o hook carimba na proxima passada do jogo).
- Mantido: tracking (leitura), resolve->elemento-falso, sort guard,
  dump, censo do health.

## 19:23 — Crash 0xC0000025: o teto de lane recriou a guerra — REVERTIDO

O teto de 24 elementos por no chamava a vfunc5 nativa (wipe total) ->
o jogo re-registrava os ~70 elementos -> passava de 24 -> wipe de novo
= ciclo infinito apaga/re-registra. Prova no log: ins=1166/s com 203
elementos tipo=0xFFF - a MESMA tempestade do kill, agora pela via
nativa. Morreu no meio do desatrelamento T2,T1,T0 (det=2 no ultimo
health). Licao: qualquer remocao em massa - nossa ou nativa - vira
guerra contra o re-registro do jogo. Populacao viva de lane e custo
do jogo, nao se toca.

## 19:50 — Tempestade pos desatrelar/atrelar: auto-limitante (NAO e guerra)

ins=1574/s -> 1250/s -> 0 em ~30s. E o rebuild legitimo dos registros de
obstaculo da NOSSA corrente: dump mostrou 66 dos ~85 elementos com o MESMO
dono (nós de junção do combo). Morre sozinha; nao se remove nada (guerra
provada 2x). FPS ancora no residuo ~70 = custo nativo.

## Correções aplicadas (build pendente - jogo aberto)

1. Guarda de lane nao desarma mais por tempo fixo: desarma so quando
   lane_any_dead() nao achar elemento morto nos containers rastreados.
   O crash das 19:37 nasceu 1.2s APOS o desarme, com 2 mortos na lista.
2. crash_tracer resolve o modulo da falha via VirtualQuery->AllocationBase
   ->GetModuleFileNameA: loga "falha 0x... em modulo.dll +0xoff" no lugar
   de "fora do plugin". Proximo crash dira de quem e o codigo que morre.

## Crash 19:37 (rodovia) — assinatura AV no exe

WER: modulo eurotrucks2.exe, 0xC0000005. Codigo nativo caminhou buffer
liberado byte a byte (addr 0x3ba9d302ee..ff, contador rdi 0x12->0x1,
base r8=0x3ba9d2aed8) ~1.2s apos desatrelar T2 + 2 purecalls (vfunc 140
em objeto liberado). NAO era iteracao de lane (byte-walk, nao ptr-walk) -
mortos de lane na hora podem ser coincidentes. Aguardando nome do modulo
na proxima ocorrencia.

## Crash 20:22 (desatrelamento) — DIAGNOSTICADO ao nivel de instrucao

Tracer novo pegou: 2x purecall (exe+0x1295554, +0x12959E2) depois falha
0xC0000005 em eurotrucks2.exe +0x12959E5 — mov rdx,[rax] com rax=0.
Mecanismo PROVADO: o varredor de registros de lane (exe~0x1295xxx) chama
vfunc[8] de um registro cuja fonte morreu no desatrelamento; a purecall
hook engolia e devolvia nullptr; a instrucao seguinte lia [nullptr].
Pilha: 0x1295CFF <- 0x12B724B (mantenedor de nos fisicos) <- 0x129564C
<- 0x1292FFC <- 0x1292950 <- 0x129149E.
FIX instalado: hk_purecall devolve &purecall_fake (vfunc-tipo retorna 0
= "pula esse registro") para rvas 0x1290000-0x1296000 e 0x12B0000-0x12C0000
— cobre o varredor de lane inteiro e o mantenedor de fisica. Mesma
tecnica do elemento-falso da guarda, agora pra qualquer consulta a
objeto morto dentro dessas familias.

## ~20:52 — CARRETA FANTASMA: mecanismo provado e limpeza instalada
Sintoma do usuario: trafego parava pra sempre no ponto exato do
desatrelamento, como se a carreta continuasse la ("carreta fantasma").

Prova ao vivo (experimento desatrelar/reatrelar/mover):
- Os registros de obstaculo NAO carregam ponteiro da carreta (varredura
  de todos os campos do registro: zero refs pra dentro de trailers).
- O fantasma = ELEMENTO APOSENTADO ainda no vetor do no de lane:
  el+0 nao e vtable (slot reciclado) MAS o registro embutido em
  el+0x80 continua com vtable valida, dono valido e posicao valida -
  o trafego le como obstaculo real e para.
- O detector lane_element_dead (dono+registro) nao pega esse caso:
  dono e registro parecem vivos. O teste certo e el+0 sem vtable.
- Remocao manual do vetor (memmove+cnt--+flag +0x74|=9, replicando o
  unregister nativo 0x1412B8190) = trafego destravou na hora. PROVADO.

Implementado (instalado 20:52): lane_sanitize_node agora trata os
dois casos - elemento com vtable morta+dono morto = carimbo (como
antes); elemento aposentado (el+0 sem vtable) com registro embutido
vivo (el+0x80 com vtable do exe) = remove do vetor do no replicando
o unregister nativo, 1 por passada, dentro dos hooks serializados.

RE do unregister nativo (0x1412B8190): unregister(node, link=el+0x80):
acha el+0x80 no vetor +0x20/+0x28, memmove, count--, +0x74|=9; o
jogo so o chama por caminhos que nao cobrem elementos aposentados.

Crash dump (instalado 21:53): crash_tracer ganhou dedupe por
(rip,code) - 24 unicos, repeticoes suprimidas (mata a enxurrada de
2692 linhas vista no shutdown das 21:45). Novo crash_dump_write
(fn236) grava em Documents\Euro Truck Simulator 2\br_carretas_crash.txt
via WriteFile puro (handle pre-aberto, sem heap/CRT): code, rip com
modulo+offset, tipo de acesso + endereco faltoso, 16 regs, dump de
16 qwords de rcx/rbx/rdi/r11 e pilha (rsp scan, exe+/dll+ offsets).

Crashes de hoje (Event Viewer): 21:28 exe+0x12B7ECC = varredura de
obstaculos em velocidade lendo el+0x88 (dono) morto num vetor
r11+8/+0x10 - mesma doenca do fantasma, container diferente.
21:45 exe+0x1E577B = crash no DESLIGAMENTO (indice corrompido em
tabela global), nao relacionado a gameplay.

## 25/11 — Crash no desatrelamento pelo botao + carga reunindo: mesma causa

Crash 12:46:30 (botao Desatrelar C1): detach completou limpo; 40ms
depois AV em exe+0x153E9DB - call [rax+0x60] (vfunc 12 = "meu indice")
num elemento de vetor de 2 entradas cuja vtable era lixo
(0xA6685F3FA6685FC0). A funcao re-arquiva itens registrados numa
tabela por indice ([r14+idx*8+0x18]) e faz OR das flags dos
registros no pai (+0x118 / +0x20). Registro velho na lista do jogo.

Causa raiz (carga + crash): o detach marcava a CORRENTE INTEIRA como
solta (mark_detached/mark_plugin_detached + detached_idx em C1, C2
e C3). Marcadas, C2/C3 entravam nos caminhos de carreta solta:
a carga as tratava como cabeca de secao propria e as reunia; os
pinos/claims de visibilidade gravavam estado nelas, deixando
registro velho na lista que o jogo varre.

Correcao: o laco continua andando a corrente so para zerar +0x25A
(pendente de desengate que a maquina da tecla T liga na corrente
inteira - cancelamento correto), mas mark_detached/
mark_plugin_detached agora so na carreta pedida. Somado ao slot
do ator mantido (correcao do botao: ator continua apontando pra
T0, carretas visiveis).

## 25/11 — Carretas separadas somem apos soltar T0: elo de claim cortado e ninguem re-escreve

Cenario: separou T2, separou T1, soltou T0 ~200m longe -> T1/T2 sumiram
da tela (so T0 visivel). Reatrelar tambem escondia.

Varredura global de memoria (sem valores fixos): os 3 objetos VIVOS.
T0 no slot do ator com rec+parked=1+regd=1 e slave=0 (elo de saida
CORTADO); T1->T2 intacto. Gerenciador de parked-records 0x0, traffic
state 5. Quem corta e o JOGO (modelo provado: zera elo de saida de
carreta estacionada com registro). O elo fica cortado porque:
- conn_commit (que re-escreve a corrente) roda SO NO EVENTO
  (rebuild_parked_claim_links) - nada por frame;
- o pino de visibilidade pula alvo marcado plugin-detached.

Prova: escrevendo T0->T1 direto na memoria as carretas voltaram na
posicao onde estavam e o elo FICOU (jogo aceita, nao zera de novo).

Correcao (fn308 vigia_claims, trailer_manipulation.cpp): manutencao
por frame le os elos registrados em synthetic_edges; elo nosso que
sumiu volta UMA vez; 3 cortes seguidos = conflito, registra no log
e para (padrao pin_retry). Rearme no evento (rebuild_parked_claim_links
zera losses/stopped). Desligado na carga (cargo_freeze) e em chamada
nativa (native_chain_call_active). synthetic_slave_edge ganhou
losses/stopped. INDICE.md: faixa 308 + entrada da funcao.

## 25/11 (2) — Vigia não atuou no teste 14:39: o desengate da tecla T apaga o registro do elo

Teste 14:38-14:39 (DLL nova com fn308): separou T2/T1 pelo botao e soltou
T0 pela TECLA T -> T1/T2 sumiram de novo. Log sem nenhuma linha do
vigia. Causa: o desengate da tecla T e ASSINCRONO - fn191 despacha
(junta ainda viva, rebuild roda cedo demais), o jogo quebra depois
dentro do fn182 (vfunc99), cuja caminhada corta o elo c0->c1 (slave
jointless = "bad") e faz clear_claim_edge - o elo fica cortado E sem
registro; o vigia so conserta elo registrado -> cego. O caminho do
botao (fn284) re-escreve no fim; o da tecla T nao tinha esse fim.

Correcao: fn182 agora termina com rebuild_parked_claim_links() -
a corrente volta a ser escrita e registrada right-after do teardown
assincrono. No caminho do botao o rebuild fica bloqueado pelo guarda
native_chain_call_active (sem trabalho duplicado). Vigia (fn308)
cobre os cortes que o jogo fizer depois.
