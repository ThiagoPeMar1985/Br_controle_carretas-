# BR Controle de Carretas (ETS2/ATS 1.61)

> [!WARNING]
> Plugin experimental / em desenvolvimento. Pode causar crashes,
> principalmente após atualizações do jogo. Somente singleplayer — **NÃO**
> recomendado para multiplayer (TruckersMP).

Plugin criado por **ThiagoPeMar1985**, baseado em
[dariowouters/ts-extra-utilities](https://github.com/dariowouters/ts-extra-utilities)
(MIT), focado unicamente no **engate e desengate individual de
carretas** — ideal para rodotrem, bitrem e conjuntos brasileiros. O recurso de
eixo direcional do plugin original foi removido.

## Recursos

- **Engatar/desengatar cada carreta individualmente** (suporte a multi-carretas)
   - Desengate a última carreta num doca, siga com o resto do conjunto, volte e
     recolha depois
   - Botões Engatar/Desengatar por carreta, além de teclas configuráveis:
     engatar/desengatar as carretas 2 e 3 no veículo anterior, ou engatá-las
     direto no cavalo (a primeira carreta continua na tecla T do jogo)
   - **Engatar no cavalo**: conecta uma carreta traseira direto na quinta roda
     enquanto o resto do conjunto fica estacionado
   - Carretas reengatadas soltam o freio e rolam normalmente — o plugin repete
     a sequência completa de engate do jogo (registro de reboque, flag de
     estacionada, manilha, mangueiras de ar, carga nas rodas, estado de acoplamento)
   - Carretas desengatadas pelo plugin nunca são reengatadas sozinhas pelo
     fluxo nativo; a tecla T continua funcionando na primeira carreta
   - `g_trailer_cables_mode` acompanha o comboio automaticamente: `1` com a
     primeira carreta engatada, `2` quando a última engata, `0` sem carretas
   - Carretas desengatadas ficam estacionadas onde você as deixou durante
     carga/descarga no doca — o conjunto volta à posição original depois da
     cinemática (entrou de ré, sai de ré)

- **Eixos eleváveis por carreta**
   - Escolha quais eixos eleváveis de cada carreta respondem à tecla de lift
     do jogo (switches por eixo na janela)
   - **"Erguer 1º eixo na ré"**: engatou a ré com os eixos no chão → sobe
     automaticamente o primeiro eixo elevável de cada carreta; saiu da ré →
     desce e volta a seleção salva

- **Teclas de atalho configuráveis**
   - Remapeie na própria janela do plugin (coluna "Teclas") — clique na ação,
     depois aperte a tecla desejada
   - Aceita **combos com modificador** (`Ctrl+J`, `Shift+F5`, `Alt+Delete`) e
     **botões de joystick/volante** (aperte o botão com a captura aberta)
   - Binds salvos em `%APPDATA%\br_controle_de_carretas\settings.ini`

- **Controles da UI**
   - `Delete` abre/fecha a janela do plugin
   - `Insert` libera o cursor do mouse (centralizado) para clicar na UI

> [!NOTE]
> O engate/desengate é singleplayer apenas e assume que o cavalo já está
> posicionado sob o ponto de engate da carreta — o joint é criado no gancho
> fixo, então alinhe primeiro (limite de ~7 m, como no engate nativo).

## Como usar

Funciona apenas com **DirectX 11**.

Copie `br_controle_de_carretas.dll` para
`<pasta_do_jogo>/bin/win_x64/plugins` (crie a pasta `plugins` se não existir).

No jogo: `Delete` abre a janela, `Insert` libera o mouse. Na coluna
**Teclas** da janela você binda as ações de engate — clique na ação e aperte
a tecla, combo (`Ctrl+J`) ou botão do volante/joystick desejado.

### Rebindando pelo arquivo

Todos os binds ficam em `%APPDATA%\br_controle_de_carretas\settings.ini` —
incluindo as teclas de janela e cursor, que **não** podem ser remapeadas no
jogo (são a única forma de abrir a UI, então uma bind ruim trancaria o
plugin). Edite o arquivo com o jogo fechado.

Distribuidores: o `br-settings.ini` incluso também pode ficar ao lado da dll
na pasta plugins — numa instalação limpa ele define os binds abaixo até o
usuário salvar os próprios no jogo.

```ini
[keybinds]
toggle_ui=DELETE
toggle_cursor=INSERT
attach_detach_2=CTRL+DELETE   ; combo com modificador
attach_detach_3=JOY0:B3       ; joystick 0, botão 3
```

Os valores aceitam código de virtual-key (`46`), nome da tecla (`A`-`Z`,
`0`-`9`, `F1`-`F12`, `INSERT`, `DELETE`, `HOME`, `END`, `PAGEUP`, `PAGEDOWN`,
`UP`, `DOWN`, `LEFT`, `RIGHT`, `SPACE`, `TAB`, `ENTER`, `BACKSPACE`,
`NUMPAD0`-`NUMPAD9`, `MINUS`, `EQUALS`, `COMMA`, `PERIOD`), combos com
`CTRL+`/`SHIFT+`/`ALT+` e botões de joystick no formato `JOY<n>:B<botão>`.
Valor vazio, inválido ou duplicado volta para `Delete`/`Insert`.

> Teclados atuais vão só até **F12** — F13 a F24 existiam só em teclados
> antigos padrão IBM.

## Compilação

```sh
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target br_controle_de_carretas
```

Saída: `build/Release/br_controle_de_carretas.dll`.

> [!CAUTION]
> O `build/` guarda caminhos absolutos no CMakeCache — depois de
> copiar/mover a pasta do projeto, apague `build/` e reconfigure, senão ele
> compila a árvore antiga.

## Créditos

- Autor: [ThiagoPeMar1985](https://github.com/ThiagoPeMar1985) — adaptação PT-BR,
  bitrem/rodotrem, câmera 2 customizada e demais recursos novos
- Código-base (MIT): [dariowouters/ts-extra-utilities](https://github.com/dariowouters/ts-extra-utilities)
