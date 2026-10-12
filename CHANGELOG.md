# Changelog

## 0.0.9 — 2026-10-12
- **Configuração inicial automática:** na primeira vez o `SotC.exe` abre numa tela de boas-vindas
  que só pede a pasta do jogo (ou já oferece a que encontrou no PC). Depois ele configura sozinho
  as DLCs (dentro da pasta do jogo e ao lado dela), os saves (copia um save do shadPS4 se o port
  ainda não tiver) e o som, e mostra o resultado. **Configurar de novo** repete o processo.
- **Pasta principal como no shadPS4:** você aponta a pasta principal do jogo e ele acha as
  subpastas do jogo base (CUSA08809), da atualização e das DLCs.
- **Atualização:** a pasta `CUSA08809-UPDATE` ou `CUSA08809-patch` ao lado do jogo é usada como no
  shadPS4: os arquivos dela têm prioridade sobre os do jogo base (`SOTC_UPDATE_DIR`). Se ela tiver
  `eboot.bin`, é ele que roda.
- Tela principal mais limpa: o registro do jogo virou um link discreto; a verificação mostra as DLCs.
- **Pasta de DLC** no lançador (Jogo e saves), como no shadPS4. Também é encontrada na busca
  automática: a pasta `addcont` do shadPS4 ou uma pasta com uma subpasta por DLC.
- O runtime lista ao jogo as DLCs instaladas (`sceAppContentGetAddcontInfoList`), lendo o
  `CONTENT_ID` do `param.sfo` de cada uma (`SOTC_DLC_DIR`).
- **DLC em .pkg sem instalar:** os pacotes de DLC (tipo `0x1b`) são lidos direto do cabeçalho,
  sem extrair. Isso basta para as DLCs do jogo, que só desbloqueiam itens (arco antigo, capa,
  espada, cavalos). A busca olha na pasta escolhida, na pasta do jogo e nas pastas vizinhas.
- **Modo PS4 Pro:** o registro do jogo mostra se ele foi aberto como PS4 Pro
  (`sceKernelIsNeoMode -> 1`). A opção só libera o Modo Grafismo do PS4 Pro: é preciso escolher
  **Cinematográfico** ou **Desempenho** no jogo, em Opções > Apresentação.

## 0.0.8 — 2026-10-12

Primeira versão pública.

### Lançador (`SotC.exe`)
- **Busca automática** ao abrir: encontra nos discos do PC o jogo (CUSA08809), os módulos de som
  do usuário num shadPS4 instalado (copiados para a pasta de dados do port) e saves do shadPS4
  (pergunta antes de importar). **Procurar no PC** repete a busca.
- **Biblioteca de jogos** como a do shadPS4: pastas de jogos, lista com ícone, título, ID e versão.
- **Troca de teclas:** clique na ação e aperte a tecla nova (gravado como `key.<entrada>=` no
  `sotc.ini`).
- **Ícone novo** (arte original). A janela e o atalho na área de trabalho usam o `icon0.png` do
  seu dump.
- **Envio de trabalho à GPU:** Automático (NVIDIA), Ligado em qualquer placa (AMD/Intel,
  experimental) ou Desligado.

### Desempenho
Demo da tela de título, modo 60 FPS, i9-13900K + RTX 2060 SUPER: média de 37–45 → **~67 FPS**,
trecho mais pesado ~17 → **29–34 FPS**.
- Comandos Vulkan gravados e enviados numa thread própria (`cp_record_thread` do Pink-shadPS4).
- Envio periódico de trabalho à GPU por tempo (a cada 0,5 ms), em vez de por número de comandos.
- Draws indiretos com argumentos na CPU viram draws diretos.
- Caches: descrição de imagens por T#, buscas de imagens, informações de binário de shader e
  ponteiro de memória por thread.
- Verificação de sincronia de buffers sem trava quando nada mudou; `RangeSet::Add` não reinsere
  intervalos já marcados.
- Falhas de escrita: a página que falhou e as seguintes, sem dados da GPU, são liberadas juntas
  (~50 mil → ~4 mil falhas por segundo).
- Readbacks:
  - faixas lidas recentemente são baixadas juntas;
  - leituras "limpas" nas tabelas de recursos dos shaders, com o bitmap acompanhando a proteção
    real das páginas;
  - o preenchimento de tabelas de descritores (compute `0x50784801`) é feito na CPU;
  - leituras antecipadas.
- Do Pink-shadPS4: cvars `doIndexBufferCulling=0 doSunShadowIndexBufferCulling=0` e a proteção
  do heap BPE contra escritas da GPU.
- Opcionais, para teste: readback assíncrono (`SOTC_RB_ASYNC=1`) e marcador de espera
  (`SOTC_WAIT_MARKER=2`).

### Correções
- Travamento da fila da GPU com a gravação em thread própria: a apresentação espera o quadro
  chegar à fila.
- Morte silenciosa por proteção de página na pilha de uma fibra.
- Tela preta no boot; crash no modo PS4 Pro (`alt_tile` nos buffers de vídeo).

## Antes (desenvolvimento, 07–08/10/2026)
- Boot do jogo no carregador do bbport (imports por NID, fibras, SSE4a, imagem abaixo de 2 GiB).
- Núcleo de vídeo do fork shadPS4 SotC; som com os módulos do usuário.
- Modos 60 FPS e 16x AF como patches do carregador; lançador Tkinter congelado (`SotC.exe`).
- Teclado e controle juntos, câmera no mouse, saída 720p–4K, modo PS4 Pro, efeitos do motor.
