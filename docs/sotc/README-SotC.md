# Shadow of the Colossus — PC (port nativo)

Port nativo do **Shadow of the Colossus** de PS4 (CUSA08809, versão EU 1.01) para Windows.
O código do jogo roda direto na CPU. Isto não é um emulador completo: há um carregador e as
funções de sistema de que o jogo precisa. A GPU usa o núcleo de vídeo Vulkan do fork
shadPS4 SotC. A base é o [bloodborne_pc](https://github.com/Supermedo/bloodborne_pc) (GPL-2.0).

Este pacote **não contém o jogo nem firmware da Sony**. Você precisa ter os seus.

## Requisitos

- Windows 10/11 64-bit e GPU com Vulkan 1.3.
- [Python 3](https://www.python.org/downloads/) instalado, com a opção "Add to PATH" marcada.
- O **seu dump** do jogo CUSA08809 (pasta com `eboot.bin`, `sce_module`, `sce_sys`...).
- Para ter som, `libSceNgs2.sprx` e `libSceUlt.sprx` do **seu próprio console**, decriptados.
  São os mesmos arquivos da pasta `user/sys_modules` do shadPS4; num PS4 com GoldHEN ficam em
  `/system/common/lib/` (FTP na porta 2121). Sem eles o jogo funciona, mas sem som.

## Como jogar

1. Edite `sotc.ini`:
   - `SOTC_GAME_DIR=` a pasta do jogo;
   - `SOTC_SYS_MODULES=` a pasta com os dois `.sprx`, para ter som.
2. Clique duas vezes em `SotC.bat`.

Na primeira vez a imagem do jogo é preparada, o que leva alguns segundos. Isso só se repete
quando o jogo ou os módulos mudam. Os saves e o cache de shaders ficam em `user/`.

Para usar um save do shadPS4, copie
`shadPS4/user/home/1000/savedata/CUSA08809` para `user/savedata/1/CUSA08809`.

## Controles

Um controle (DualShock 4, DualSense, Xbox...) é usado automaticamente. Sem controle,
o teclado fica assim:

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

## Estado

Funcionam: logos, abertura, menus com texto, carregar save, exploração a cavalo,
som (com os módulos do usuário). O jogo roda travado em 30 FPS, como no PS4; durante as
cargas cai para 22–25.

## Diagnóstico

Estas variáveis de ambiente ajudam a investigar problemas:

| Variável | Efeito |
|---|---|
| `SOTC_FPS=1` | taxa de quadros a cada 5 s |
| `BB_FAULT_RING=arquivo` | grava as últimas exceções (sobrevive a quedas) |
| `BB_STACKS_TRIGGER=arquivo` | criar o arquivo despeja as pilhas das threads |
| `BB_READBACKS=0/1/2` | leituras da GPU; o padrão 2 é o exigido pelo jogo |
