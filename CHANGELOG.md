BR Controle de Carretas 1.0
    Autor: ThiagoPeMar1985 — baseado em ts-extra-utilities 2.0 (MIT, Dario Wouters)

  == Identidade e escopo ==

    - Plugin brasileiro independente: UI, documentação e mensagens ao
      usuário inteiramente em PT-BR
    - Foco exclusivo em conjuntos brasileiros (bitrem, rodotrem, cadeia
      Cavalo -> C0 -> Dolly -> C2) e carretas especiais de carga
      indivisivel/oversize com eixos direcionais
    - Renomeado: br_controle_de_carretas.dll, binds em
      %APPDATA%\br_controle_de_carretas\settings.ini, preset br-settings.ini
    - Diagnósticos de Hooks ficam internos — não aparecem na UI do usuário

  == Engate / desengate ==

    - A primeira carreta (C0) continua 100% na tecla T do jogo — o engate
      nativo da SCS não é substituído, só estendido
    - Engatar/desengatar individual para as carretas seguintes (botões na
      janela e teclas configuráveis), com suporte a bitrem e rodotrem
    - "Engatar no cavalo": conecta uma carreta traseira direto na quinta
      roda — estacione o conjunto, leve uma carreta, volte e remonte
    - Carretas desatreladas continuam no mundo: visíveis, com física,
      apoio baixado e estado de estacionada — não desaparecem nem são
      puxadas de volta pelo jogo
    - O reengate repete a sequência nativa completa (registro de reboque,
      flag de estacionada, apoio, mangueiras de ar, carga nas rodas,
      estado de acoplamento): cabos, luzes e freios voltam a funcionar
      como num engate normal
    - Guarda de proximidade no engate (~7 m, como no nativo) com aviso em
      PT-BR quando a carreta está longe demais
    - Som da manivela do apoio ao desengatar — único som adicionado

  == Camera 2 ==

    - Com qualquer carreta desatrelada: a camera ancora no cavalo e a roda
      do mouse controla a distancia sem limite — scroll pra frente
      aproxima, pra tras afasta, com suavizacao nativa
    - Conjunto completo engatado: comportamento 100% nativo da SCS —
      a camera mede o comboio a partir da primeira carreta da cadeia,
      que e restaurada como ancora pelo setter nativo do jogo

  == Eixos elevaveis ==

    - Selecao por eixo em cada carreta: so os eixos marcados respondem a
      tecla de lift do jogo
    - "Eixo na re (vanderleia/4o eixo)": engatou a re, sobe o primeiro
      eixo elevavel de cada carreta; saiu da re, desce e volta a selecao
      salva

  == Oversize — carretas de eixos direcionais ==

    - Painel proprio na janela ("Oversize"), ao lado das teclas, para
      carretas especiais de carga pesada/indivisivel
    - Virar esquerda/direita: angulo dos eixos direcionais por tecla
      (segurar move continuamente) ou pelas setas/slider da UI
    - Centralizar: retorna os eixos suavemente ao zero na velocidade
      configurada — nunca da um tranco no volante
    - Bloquear/desbloquear direcionamento automatico: bloqueado, a carreta
      obedece ao seu angulo; desbloqueado, volta a copiar o cavalo como no
      jogo original. Virar uma carreta a bloqueia automaticamente
    - Velocidade de resposta configuravel (0.1 a 2.0) — controla o ritmo
      das teclas e do retorno ao centro, salva em settings.ini
    - O hook so substitui o angulo em carretas bloqueadas e sempre chama o
      setter nativo — a fisica por roda continua sendo do jogo
    - Ponteiros de steering republicados a cada frame via atomicos —
      trocar/recarregar carretas nao deixa ponteiro velho (evita crash)
    - Carretas sem eixos direcionais sao ignoradas com aviso na UI

  == Freio — distribuicao carreta/cavalo ==

    - Slider "Carreta freia mais" (0 a 200% extra): o canal de freio que o
      cavalo transmite pras carretas e amplificado, entao a carreta recebe
      mais do que o cavalo no mesmo pedal — como na regulagem real de
      carreta brasileira
    - Na frenagem parcial a carreta segura primeiro: no piso escorregadio
      ela trava e arrasta os eixos em linha reta em vez do cavalo dobrar
      em L
    - O canal do proprio cavalo nunca e alterado — a autoridade de
      frenagem do conjunto inteiro e preservada
    - So afeta carretas acopladas recebendo o broadcast do cavalo;
      carreta solta mantem o spring brake nativo
    - No pedal a fundo nao muda: os dois ja estao em 100% — o recurso age
      na frenagem parcial/modulada, onde o L acontece
    - Valor salvo em settings.ini ([brake] trailer_boost)

  == Janela ==

    - Painel de teclas redimensionavel (arraste a borda direita) — largura
      salva em settings.ini, sem texto de bind cortado
    - Painel Oversize recolhivel numa faixa fina com seta; estado salvo
    - Colunas de carretas se ajustam automaticamente ao espaco restante

  == Estabilidade e correcoes (o que faz rodar liso, sem crashes) ==

    - Corrupcao de refcount na camera: liberar ponteiros que o jogo
      escreveu direto destruia objetos e congelava a camera no reengate.
      Corrigido usando o setter nativo com o protocolo correto e escrita
      crua apenas para o estado "sem carreta"
    - Guerra de escrita na ancora da camera (milhares de escritas por
      segundo) que travava rotacao e zoom: corrigido com escritas somente
      nas transicoes de estado, nunca por frame
    - Camera "ancorada no cavalo" apos reengatar: a causa real era a
      ancora apontando para a ultima carreta — a camera mede o comboio
      a partir da cabeca da cadeia. Corrigido restaurando a primeira
      carreta (mesma escolha do resolver nativo)
    - Zoom da camera limitado apos reengatar: o recompute nativo passou a
      ser chamado uma unica vez por transicao, devolvendo o conjunto de
      distancias correto
    - Camera seguindo carreta desatrelada: o setter nativo e hookeado e
      troca qualquer alvo com junta morta por "sem carreta" (seguir o
      cavalo)
    - Medicao do comboio (combo_length) contando carretas desatreladas:
      a cadeia slave agora para na primeira junta morta, entao camera e
      demais consumidores medem so o conjunto vivo
    - Freio de servico/progressivo nao chegando nas carretas reengatadas:
      os canais de freio sao propagados enquanto o trailer esta no comboio
    - Conjunto estacionado freando carreta engatada no cavalo: escritas de
      freio vindas do conjunto parado sao engolidas pelo hook do setter
    - Jogo re-engatando sozinho carretas desatreladas pelo plugin:
      connect_slave e o dispatcher de engate recusam trailers marcados
    - Carreta estacionada sendo teleportada/reaplicada pelo fluxo nativo:
      hooks de state-apply e teleport pulam trailers desatrelados
    - Trailer sumindo depois da cinematica de carga no doca: o conjunto e
      restaurado na posicao original (entrou de re, sai de re)
    - Varredura de trafego batendo em obstaculo morto: entradas de lane
      obstacle com dono destruido sao podadas
    - Trafego: a carreta solta vira obstaculo registrado na via; densidade
      de trafego e restaurada quando o conjunto remonta
    - ZZ_fix_ai_truck_defs.scs: bridge de defs de caminhao de IA para
      Rotas Brasil / packs AM29 (defs antigos quebravam o spawn de
      trafego na 1.61)

--- histórico upstream (ts-extra-utilities) ---

2.0
    - Individually detachable trailers, ported to 1.61.1.9: per-trailer
      Connect/Disconnect buttons and keybinds for trailers 2-3 (the first trailer
      stays on the game's own T key). Disconnect drops the landing gear and the
      joint; Connect re-attaches the trailer to whatever is ahead of it in the
      chain. Singleplayer only
    - "Attach trailer to truck": connect a rear trailer straight to the fifth wheel
      — park a trailer at a dock, haul the rest, come back and pick it up. Refused
      while the fifth wheel is occupied
    - Manual connect/disconnect replays the full native sequence: towed-trailer
      bookkeeping, parked flag, brace, air line rebuild and wheel-load
      distribution; the truck's coupling state (+0x14E0) is marked coupled so the
      game releases the trailer's brakes
    - The parked set can no longer re-brake a trailer sitting on the truck: the
      brake setter is hooked and positive writes aimed at a truck-towed trailer
      are swallowed, while the pedal brake still works
    - connect_slave stays permanently hooked but only refuses trailers detached
      through this plugin — the game no longer rejoins them when the head trailer
      is detached with T, and normal attach/spawn keeps working
    - g_trailer_cables_mode follows the coupled convoy automatically: 1 while the
      first trailer is coupled, 2 once the last trailer is coupled, 0 with nothing
    - Overlay font slightly enlarged for 4K readability; Insert releases a
      centered cursor
    - Diagnostic dump per trailer (joint, brake channels, connector, truck towed
      pointer) and brake setter call tracing in game.log.txt

1.2.2
    - Trailer steering offsets updated for game version 1.61.1.9 (the trailer object grew by
      0x28 bytes: steering angle 0x690 -> 0x6B8, steering data 0x698 -> 0x6C0, slave trailer
      0x1060 -> 0x1088; first trailer in game_actor is still 0xC8)

1.2.0
    - Configurable keybinds (Keybinds window), saved to %APPDATA%\ts-extra-utilities\settings.ini
    - New hotkeys: steer left / right, center, lock / unlock trailer steering (unbound by default)
    - Adjustable steering speed for the hotkeys

1.1.1
    - DirectX 11 Present is now inline-hooked (plus Present1) instead of vtable-swapped, so the overlay also shows on flip-model swapchains
    - Logs "Present hook is live, overlay initialized" once the overlay is up

1.1.0
    - Updated manual trailer steering for game version 1.61 (ETS2 + ATS)
    - Trailer steering now only overrides the steering angle instead of skipping the whole steering update
    - Detachable trailers and lockable joints are disabled until their offsets are re-verified

1.0.1
    - Fixed DirectX hook not working on some devices

1.0.0
    - Initial Version
