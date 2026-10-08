# Caminho B: otimizações de GPU do bbport no shadPS4 SotC

Pedido original: levar para o nosso shadPS4 as otimizações de GPU do bbport, como o
pipeline de desenho em duas threads e o FSR.

## O que os testes mostraram (07–08/10/2026)

O port nativo rodou primeiro com o núcleo de vídeo do bbport, que traz ~40 otimizações
desligáveis por `BB_TOGGLE_FILE`, listadas em `gpu/shim/bbport_toggles.h` do bbport.

| Otimização do bbport | Efeito no Shadow of the Colossus |
|---|---|
| `PendingFenceWaits` (bit 39) | tela preta intermitente no boot (50–75% das execuções) |
| `DrawPipeline`, `DrawPreparation` e afins (preparo adiantado e diferido) | estado de GPU velho ou lixo durante o streaming ao carregar o save: programas de shader inválidos, `Unpack10_11_11`, device lost |
| leituras de GPU relaxadas (padrão do bbport) | o jogo trava; o SotC precisa de readbacks precisos (`BB_READBACKS=2`) |

Com o núcleo de vídeo do fork no lugar do do bbport (mantendo o carregador e o runtime do
bbport), o jogo ficou correto: menus, save e mundo aberto, 10 min sem queda. Essas
otimizações supõem que o jogo não lê, na CPU, o que a GPU acabou de escrever. Isso vale
para o Bloodborne, mas não para o SotC.

## Medição atual (port nativo, i9-13900K + RTX 2060 SUPER, 1080p)

- Padrão: 30 FPS travado, como no PS4; 22–25 em cargas e no templo.
- Modo `fps60` (vblank 120 Hz): 38 FPS no campo aberto, ~25 no templo.
- Gargalo: a thread `GpuCommandProcessor` fica 72% ocupada. O tempo vai no driver Vulkan e
  em esperas de fence das leituras precisas. Os job workers do jogo ficam em 36–43%.
- O fork no shadPS4 rodava a 17–18 FPS (documento consolidado), então o port nativo já tem
  ~1,7x de desempenho.

## Conclusão

Copiar o pipeline em duas threads do bbport para o fork, como está, quebraria o SotC
pelos motivos da tabela. O FSR não traz ganho porque o jogo já renderiza em 1080p nativo.

O port nativo e o fork agora usam **o mesmo núcleo de vídeo** (os arquivos de
`gpu/shadps4/video_core` são os do fork). Por isso, qualquer otimização feita aqui vale
para os dois. O caminho viável é otimizar esse núcleo comum, uma peça por vez, com teste
A/B: `tools/sotc_hangtest.py` e a rota que carrega o save em `tools/sotc_play.py`.
Candidatos, em ordem de risco:

1. Caches puros do bbport, sem diferimento: `RegionCache`, `FetchShaderCache`,
   `ImageDescCache`, `FindImageCache`, `TextureBindingMemo`, `RenderStateMemo`.
   Atacam o custo da thread de comandos.
2. `ThreadedRecording` (gravação Vulkan noutra thread), mantendo a ordem estrita em relação
   às leituras precisas.
3. Readbacks precisos mais baratos: copiar só as páginas lidas e esperar a fence certa em vez
   de esperar a GPU ociosa.

Cada item é uma fusão manual entre núcleos que divergiram em ~17 mil linhas. O ganho
esperado está no modo 60 FPS: o modo padrão já atinge os 30 do PS4.
