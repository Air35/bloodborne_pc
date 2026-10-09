# Shadow of the Colossus — PC (port nativo)

Port nativo do **Shadow of the Colossus** de PS4 (CUSA08809, versão EU 1.01) para Windows.
O código do jogo roda direto na CPU. Isto não é um emulador completo: há um carregador e as
funções de sistema de que o jogo precisa. A GPU usa o núcleo de vídeo Vulkan do fork
shadPS4 SotC. A base é o [bloodborne_pc](https://github.com/Supermedo/bloodborne_pc) (GPL-2.0).

Este pacote **não contém o jogo nem firmware da Sony**. Você precisa ter os seus.

## Requisitos

- Windows 10/11 64-bit e GPU com Vulkan 1.3.
- Python não é necessário para o `SotC.exe`; só o `SotC.bat` precisa de
  [Python 3](https://www.python.org/downloads/).
- O **seu dump** do jogo CUSA08809 (pasta com `eboot.bin`, `sce_module`, `sce_sys`...).
- Para ter som, `libSceNgs2.sprx` e `libSceUlt.sprx` do **seu próprio console**, decriptados.
  São os mesmos arquivos da pasta `user/sys_modules` do shadPS4; num PS4 com GoldHEN ficam em
  `/system/common/lib/` (FTP na porta 2121). Sem eles o jogo funciona, mas sem som.

## Como jogar

1. Abra `SotC.exe`, o lançador.
2. Em **Jogo e saves**, escolha:
   - a pasta do jogo;
   - a pasta com os dois `.sprx`, para ter som;
   - se quiser, importe um save do shadPS4 pelo botão **Importar save do shadPS4**.
3. Clique em **JOGAR**.

A tela **Jogar** confere tudo antes: a versão do jogo, os módulos de som, os saves e a placa de vídeo.
Na primeira vez a imagem do jogo é preparada, o que leva alguns segundos. Isso só se repete
quando o jogo ou os módulos mudam. Os saves e o cache de shaders ficam em `user/`.

Sem o lançador, `SotC.bat` faz o mesmo lendo `sotc.ini`. `SotC.exe --play` abre o jogo direto
com as últimas opções do lançador, o que serve para atalhos e para o Steam.

## Imagem e desempenho

Tudo isto está no lançador, na página **Imagem e FPS** (e os principais em **Jogar**):

- **Saída** 720p, 1080p, 1440p ou 4K. O jogo renderiza em 1080p, como no PS4, e a imagem é
  ampliada para a saída com **FSR 1** + nitidez (RCAS) ou bilinear. A janela nunca passa de 90%
  da tela; a tela cheia usa o monitor inteiro.
- **60 FPS** (experimental): tira o limite de 30 FPS do jogo (patch GoldHEN de illusion), com
  vblank a 120 Hz. Mediu 38 FPS no campo aberto e ~25 no templo.
- **Texturas nítidas**: filtro anisotrópico 16x, quase sem custo.
- **Modo PS4 Pro**: o jogo se vê num PS4 Pro e oferece no próprio menu (Opções > Apresentação)
  o Modo Grafismo **Cinematográfico** (1440p, mais detalhe) ou **Desempenho**. É mais pesado:
  ~25 FPS no campo aberto.
- **Efeitos**: vinheta, névoa atmosférica e oclusão ambiente (SSAO) podem ser desligados.
  O motion blur, o brilho e os filtros ficam no menu do próprio jogo.
- **Cvars do motor** (Avançado): variáveis do motor Bluepoint, `NOME=valor`, para quem quer
  experimentar.

Os modos de patch só valem para a versão EU 1.01; em outra versão eles não são aplicados.

## Controles

**Mouse:** clique na janela do jogo para capturar o mouse, que passa a girar a câmera; Esc ou
trocar de janela solta o mouse. Botão esquerdo = R1 (agarrar), direito = □ (atacar),
meio = L1. Sensibilidade e inversão ficam em **Controles** no lançador. Para remapear os botões
do mouse, use `mouse.<entrada>=left,right,middle,x1,x2`.

Um controle (DualShock 4, DualSense, Xbox...) é usado automaticamente. Com vários conectados,
escolha um em **Controles** no lançador (ou `BB_GAMEPAD=` no `sotc.ini`). O teclado funciona
junto com o controle:

| Teclado | PS4 |
|---|---|
| W A S D | analógico esquerdo |
| setas | analógico direito (câmera) |
| Espaço | ✕ |
| Shift esquerdo | ○ |
| E | □ |
| Q | △ |
| 1 / 3 | L1 / R1 |
| R / F | L2 / R2 |
| Z / C | L3 / R3 |
| Enter | Options |
| I J K L | direcional |
| Tab / Backspace | touchpad (esquerda / direita) |

Para remapear, ponha linhas no `sotc.ini`, por exemplo `key.cross=Space,J` ou
`pad.circle=b`. As entradas são:

- botões: `cross circle square triangle l1 r1 l2 r2 l3 r3 options touchpad touchpad_right`;
- direcional: `up down left right`;
- no teclado, os analógicos: `move_up` `move_down` `move_left` `move_right` e
  `look_up` `look_down` `look_left` `look_right`.

## Estado

Funcionam: logos, abertura, menus com texto, carregar save, exploração a cavalo,
som (com os módulos do usuário). O jogo roda travado em 30 FPS, como no PS4; durante as
cargas cai para 22–25.

## Créditos

- [bloodborne_pc](https://github.com/Supermedo/bloodborne_pc) (bbport, deadinside28 e forks): carregador,
  runtime nativo e lançador, base deste port.
- [shadPS4](https://github.com/shadps4-emu/shadPS4) e o fork shadPS4 SotC: núcleo de vídeo Vulkan.
- [Pink-shadPS4](https://github.com/luizgustavs/Pink-shadPS4) (luizgustavs): as cvars de culling, a proteção
  do heap do jogo contra leituras da GPU (`bpe_heap_guard`), o flush periódico, as leituras antecipadas,
  o marcador de espera e o gravador de comandos Vulkan em thread própria (`cp_record_thread`).

## Diagnóstico

Estas variáveis de ambiente ajudam a investigar problemas:

| Variável | Efeito |
|---|---|
| `SOTC_FPS=1` | taxa de quadros a cada 5 s |
| `BB_FAULT_RING=arquivo` | grava as últimas exceções (sobrevive a quedas) |
| `BB_STACKS_TRIGGER=arquivo` | criar o arquivo despeja as pilhas das threads |
| `BB_READBACKS=0/1/2` | leituras da GPU; o padrão 2 é o exigido pelo jogo |
| `SOTC_RECORD_THREAD=0` | grava os comandos Vulkan na própria thread de comandos (padrão: thread própria) |
| `SOTC_INDIRECT_DIRECT=0` | mantém indiretos os draws cujos argumentos estão na CPU |
| `SOTC_WAIT_MARKER=1/2` | esperas pela GPU consultam um marcador escrito por ela (experimental) |
| `SOTC_CMD_STATS=1/2` | draws e dispatches por quadro (2: shaders de compute mais usados) |
