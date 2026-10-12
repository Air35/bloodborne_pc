# Shadow of the Colossus — PC (port nativo) · v0.0.8

Port nativo do **Shadow of the Colossus** de PS4 (Bluepoint, 2018; CUSA08809, versão EU 1.01)
para **Windows 10/11**. O código x86-64 do próprio jogo roda direto na CPU do PC e a GPU do PS4
é traduzida para Vulkan. Não é um emulador completo: é um carregador com as funções de sistema
de que o jogo precisa, no estilo do [bloodborne_pc](https://github.com/Supermedo/bloodborne_pc).

> **Nenhum arquivo do jogo nem da Sony está incluído.** Você precisa do **seu próprio dump** do
> jogo e, para ter som, de dois módulos do **seu próprio console**. Este projeto não tem
> ligação com a Sony Interactive Entertainment nem com a Bluepoint Games.

*English summary at the end.*

## Download e uso

1. Baixe o `SotC-PC.zip` da página [Releases](../../releases) e descompacte numa pasta.
2. Abra o **`SotC.exe`**. Na primeira vez o lançador procura sozinho, nos discos do PC:
   - a pasta do jogo (o dump CUSA08809, com `eboot.bin`, `sce_module` e `sce_sys`);
   - os módulos de som (`libSceNgs2.sprx` e `libSceUlt.sprx`) de um shadPS4 que você já
     tenha, na pasta `user/sys_modules`, e copia para a pasta de dados do port;
   - um save do jogo no shadPS4, que ele oferece importar.
3. Confira a página **Jogo e saves**. A **biblioteca de jogos** funciona como a do shadPS4:
   adicione as pastas dos seus jogos e escolha o jogo na lista.
4. Clique em **JOGAR**.

Requisitos: Windows 10/11 64-bit, placa de vídeo com **Vulkan 1.3** (NVIDIA, AMD ou Intel
com driver recente). Não precisa instalar Python.

### Os arquivos que você precisa ter

| O quê | De onde | Para quê |
|---|---|---|
| Dump do jogo CUSA08809 (EU 1.01 de preferência) | o seu PS4, pelo GoldHEN | o jogo |
| `libSceNgs2.sprx`, `libSceUlt.sprx` decriptados | o seu PS4 (`/system/common/lib/`, FTP do GoldHEN), ou o `user/sys_modules` do shadPS4 | som (sem eles o jogo roda mudo) |

Os modos de patch (60 FPS, filtro 16x) só valem para a versão EU 1.01; nas outras o jogo roda
normalmente, a 30 FPS.

## O lançador

- **Jogar:** confere a versão do jogo, o som, os saves e a placa de vídeo antes de abrir;
  opções rápidas de saída, upscaler, 60 FPS, filtro 16x e tela cheia.
- **Jogo e saves:** biblioteca de jogos, busca automática no PC, importação de saves do shadPS4.
- **Imagem e FPS:** saída 720p a 4K (o jogo renderiza em 1080p e o FSR 1 + RCAS amplia),
  modo PS4 Pro (o jogo oferece Cinematográfico/Desempenho no próprio menu), efeitos (vinheta,
  névoa, SSAO) e opções de desempenho.
- **Controles:** controle (DualShock 4, DualSense, Xbox) automático; câmera no mouse;
  **troca de teclas** clicando na ação e apertando a tecla nova.
- **Avançado:** cvars do motor Bluepoint, diagnóstico, atalho na área de trabalho (com o ícone
  do seu jogo).

## O que foi feito

| Área | Resultado |
|---|---|
| Boot | O jogo inicia com o carregador do bbport: imports por NID, fibras, patches de CPU (SSE4a), imagem abaixo de 2 GiB (o jogo guarda ponteiros em 31 bits). |
| Vídeo | Núcleo de vídeo do fork shadPS4 SotC dentro do port: logos, abertura, menus, campo aberto sem polígonos pretos. |
| Som | Áudio real com os módulos Ngs2/Ult do próprio usuário. |
| Controles | Teclado e controle juntos, câmera no mouse, troca de teclas no lançador. |
| Modos | 60 FPS (patch de illusion), filtro anisotrópico 16x, modo PS4 Pro, saída até 4K. |
| Desempenho | No demo da tela de título, em modo 60 FPS: média de **37–45 → ~67 FPS**, trecho mais pesado (ponte do cânion) **~17 → 29–34 FPS**. |

As otimizações de desempenho, em resumo (detalhes no [CHANGELOG](CHANGELOG.md)):

- **Menos esperas pela GPU.** Faixas de memória lidas de volta são agrupadas. Leituras "limpas"
  nas tabelas de recursos dos shaders. Um preenchimento de tabelas feito pela GPU passou a ser
  feito na CPU. Proteção do heap do jogo contra escritas da GPU (do Pink-shadPS4).
- **Comandos Vulkan gravados e enviados numa thread própria** (`cp_record_thread` do
  Pink-shadPS4), com trabalho mandado à GPU a cada 0,5 ms.
- **Draws indiretos com argumentos na CPU viram draws diretos**, milhares por quadro.
- **Caches de descrição de imagens, de buscas de texturas e de informações de shader**, e
  verificação de sincronia de buffers sem trava.
- **10× menos falhas de página:** o jogo copia blocos grandes de memória a cada quadro.
- **Cvars de culling do Pink-shadPS4.** Mesma imagem, bem menos trabalho da GPU que a CPU
  precisa esperar.

## Perguntas comuns

- **DirectX 11/12?** Não. O núcleo de vídeo traduz a GPU do PS4 para Vulkan (shaders em SPIR-V).
  Toda placa com DirectX 12 moderno também tem Vulkan 1.3: basta o driver atualizado.
- **AMD e Intel?** Quase todas as otimizações valem para qualquer placa. Duas só ligam
  sozinhas na NVIDIA, por recomendação do Pink-shadPS4: o envio periódico à GPU e as leituras
  antecipadas. Em **Imagem e FPS > Envio de trabalho à GPU**, escolha **Ligado** para usá-las
  numa AMD ou Intel. Ainda não foi testado nessas placas; se a tela ficar preta, volte para
  **Automático**.
- **Por que não vem o som pronto?** Os dois `.sprx` são firmware da Sony; distribuí-los não é
  permitido. O lançador acha e copia os seus automaticamente.

## Compilar a partir do código

Windows com [MSYS2](https://www.msys2.org/) no ambiente CLANG64. Os pacotes estão em
[packaging/windows/README.md](packaging/windows/README.md). Para congelar o `SotC.exe`, é
preciso um Python do Windows com PyInstaller e Pillow (variável `WINPYTHON`).

```
bash build.sh                              # out/bb-probe.exe e a biblioteca de GPU
bash packaging/windows/package_sotc.sh     # dist/SotC-PC.zip
```

O empacotamento recusa qualquer `.sprx`/`.prx`: firmware da Sony nunca entra no pacote.
Ferramentas de teste e medição ficam em `tools/` (`sotc_bench_attract.sh` mede o FPS no demo
da tela de título). Variáveis de diagnóstico estão no [README do pacote](docs/sotc/README-SotC.md).

## Créditos e licença

- [bloodborne_pc](https://github.com/Supermedo/bloodborne_pc) (Supermedo, deadinside28 e
  forks): carregador, runtime nativo e lançador, base deste port. O README original está em
  [docs/bbport](docs/bbport/README.md).
- [shadPS4](https://github.com/shadps4-emu/shadPS4) e o fork shadPS4 SotC: núcleo de vídeo Vulkan.
- [Pink-shadPS4](https://github.com/luizgustavs/Pink-shadPS4) (luizgustavs): cvars de culling,
  proteção do heap BPE, envio periódico, leituras antecipadas, marcador de espera e o gravador de
  comandos em thread própria.
- Patch de 60 FPS: illusion (GoldHEN cheats).

Licença: **GPL-2.0-or-later** (veja [LICENSE](LICENSE)), como os projetos de origem.

## English summary

A native Windows port of the PS4 *Shadow of the Colossus* (CUSA08809, EU 1.01) built on the
bloodborne_pc loader and the shadPS4 Vulkan video core. **No game or Sony files are included.**
Bring your own decrypted game dump and, for sound, your own console's `libSceNgs2.sprx` and
`libSceUlt.sprx`. Download the release zip, run `SotC.exe`; the launcher finds the game, the
sound modules (from an installed shadPS4) and shadPS4 saves on its own. It also offers a
shadPS4-like game library, key remapping, 60 FPS / 16x AF / PS4 Pro modes and 720p–4K output.
In 60 FPS mode the title-screen attract demo averages ~67 FPS on an RTX 2060 SUPER, with
29–34 FPS in its heaviest section.
Vulkan 1.3 only (no DirectX). AMD and Intel can enable the NVIDIA-only submission options in
the launcher (untested).
