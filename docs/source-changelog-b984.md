# Changelog

## 0.1.1 build 984 — Windows x64 experimental

- Vulkan: solicita 1.4 quando disponível, detecta recursos por placa/driver e usa renderização dinâmica, mais estados dinâmicos, push descriptors e semáforo de linha do tempo. Preserva os caminhos anteriores; não exige Vulkan 1.4 de todas as placas.
- Cache de shaders: `s_getpc_b64` usa o endereço de carregamento dos uniformes, permitindo reaproveitar traduções entre execuções. Preenchimentos grandes usam SSE2 e escritas não temporais com barreira; a reserva de memória da fila Videodec2 cai de 16 para 1 MiB.
- Memória: mapeamentos diretos contíguos, arredondamento de regiões e split privado sem descartar escritas concorrentes de páginas vizinhas. A janela física do host é liberada ao destruir a memória. Fibers que migram restauram o dono HLE da thread atual, inclusive no retorno ao dispatcher.
- O rastreamento COW volta a ser explicitamente experimental e desligado por padrão: os testes concorrentes reproduziram perda de escritas quando ativado automaticamente. Não é uma correção do algoritmo COW nem promessa de ganho universal de FPS.
- Resolução interna unificada no launcher, global ou por jogo: Do jogo, 1440p, 1080p, 900p, 720p e 540p, somente nos títulos compatíveis (Unreal, Sonic Origins Plus e TMNT: Splintered Fate). A escala Unreal avançada explícita tem prioridade; Do jogo preserva o comportamento anterior. Não reduz automaticamente todos os jogos. Uma seleção interna 720p já gravada passa a valer também nos Unreal compatíveis se a escala avançada estiver em Do jogo; selecione Do jogo na resolução interna para remover esse ajuste.
- CPU: compatibilidade automática de instruções guest com Ivy Bridge, com opções automático/teste/desligado no launcher; executáveis host sem AVX2 obrigatório. O teste físico recente deste pacote numa máquina antiga permanece separado da validação RTX.
- F10: painel compacto com CPU, sistema, uso de CPU, RAM do sistema/processo, memória comprometida e uso/orçamento de memória da GPU. Apertos rápidos agora ficam na fila mesmo entre quadros lentos; repetição automática não alterna o painel várias vezes.
- Entrada nativa opcional por mouse/teclado, desligada por padrão. Clique captura o mouse e F8 o libera/captura; perda de foco libera entradas. O teclado nativo requer controle físico conectado.
- HLE: callbacks, pipes/poll, cancelamento de pthread, informação de canais de áudio e saída sem som. O slot correto em SetSaveDataMemory2 permite ao Sonic Origins Plus avançar; sua exibição acompanha a saída em vez de ficar fixa em 4K.
- Quake III homebrew: bibliotecas companheiras, libc e callbacks, com perfil de memória/renderização específico. Menu e partida local são resultados de desenvolvimento identificados na compatibilidade; multiplayer externo e sessões longas não validados.
- Quake 1: área de patches de compatibilidade em memória, com título/versão/SHA-256/bytes originais conferidos; transporte IPv4 UDP opt-in usado nos experimentos LAN. Não representa validação de PSN, menu online ou co-op entre PCs físicos.
- Diagnósticos técnicos completos nos 15 idiomas e verificação de cobertura do catálogo. Fontes, objetos de religação, FFmpeg, licenças e hashes continuam acompanhando a distribuição.
- Validação desta candidata: 182/182 testes (28 GPU com validação Vulkan), 28/28 GPU no fallback e 52/52 testes de distribuição/atualização. Regressão RTX em sete jogos, com builds e trechos registrados; Minecraft e b898 ficaram perto de 9 FPS em janelas estabilizadas, sem ganho percentual comprovado. Wolverine e Astro continuam sem gameplay nesta rodada; TLOU não é anunciado como jogável.

## 0.1.0 build 898 — Windows distribution update

- Restore executable-embedded Noto Sans and startup video resources used by the public b838 package.
- Restore the font and video fallback paths while retaining the b897 runtime improvements.
- Keep the update payload within the paths supported by existing public installers, with the OFL notice in licenses/.
- Refresh the corresponding-source README download links for this release.

## 0.1.0 build 897 — experimental Windows x64

- **Jogos direto do .pkg:** pacotes fake de PS5 (fPKG) rodam sem extrair; o
  launcher lista os .pkg das pastas de jogos. Jogos que consultam o PlayGo
  recebem o estado de instalação do próprio pacote (The Last of Us Part I
  passa dessa etapa). Leitura inicial de pacotes fake de PS4, com chaves que o
  usuário coloca em `user/ps4_pkg_keys.txt` (o emulador não traz chaves);
  ainda não testada com um pacote real.
- **Desempenho:** menos esperas entre a CPU e a GPU (escritas pequenas do
  processador de comandos, previsão das limpezas DCC, fixação de memória sem
  trava), cópia única de buffers de vértices intercalados e cache de
  avaliações mais barato. Rogue Prince of Persia: 7,6 → 9,5 FPS na fase.
- **Gráficos:** texturas DCC com cor de limpeza correta, amostragem com
  anisotropia, viés de LOD, cor de borda e redução min/max; varredura de
  texturas grandes recém-enviadas (acentos do Monkey Island); correção de
  shaders que pulam com `s_swappc_b64` nulo (LUT do Elliot).
- **Unreal:** opção de resolução interna (por jogo e global no launcher).
- **Launcher:** tempo de jogo, modo de tela (janela, tela cheia, maximizada),
  pasta de mods, atalhos na área de trabalho e lista de jogos de PS4.
- **Kernel e bibliotecas:** mprotect sobre buracos, precisão x87, WaitEqueue,
  formatos de 10 bits no VideoOut, envio de comandos AGR, ACM, parâmetros do
  IME; o executável principal conta na memória flexível.
- **PS4 (experimental):** tiling macro 2D do GCN, endereços de 38 bits e
  regras de pitch do SI nas texturas.
- **TMNT Splintered Fate:** abertura em tela cheia e modo 1280x720.
- Relatórios de falha mostram a DLL do sistema ou do driver envolvida.
- Compatibilidade atualizada (Rogue Prince, Alone in the Dark 2, Crash
  Bandicoot 4, XIII, Poppy Playtime 2, The Last of Us Part I). Consulte
  docs/COMPATIBILIDADE.md.

## 0.1.0 build 786 — experimental Windows x64

- Reaproveitamento de avaliações de shaders com cópia esparsa dos uniformes,
  validação dos dados de origem e suporte a constantes realocadas.
- Correção das trocas BC1/BC3 no mesmo endereço e dos uploads de texturas
  3D de 8/16 bits convertidas para RGBA8.
- Correções de espelhos da memória PSP, busca de símbolos por módulo e
  estado inicial da apresentação. Suporte a geometria de sprites dos clássicos.
- Correção do acesso relativo a registradores vetoriais e de limpezas de
  profundidade por camada. Geometria NGG experimental permanece opt-in.
- Opções NGG usam o cache central de ambiente, evitando ponteiros inválidos
  após reinicialização do cache nos testes.
- Lista de compatibilidade inclui nove títulos testados na RTX 5060 Ti e
  três no Iris Xe, com etapas alcançadas e limitações de cada teste.
- Compatibilidade e FPS dos jogos conservam a identificação dos builds
  em que foram observados. Consulte docs/COMPATIBILIDADE.md.

## 0.1.0 build 705 — experimental Windows x64

- Correct GTA V texture ownership and image-store channel handling, restoring
  colors/materials in the tested development prologue without disabling fast fills.
- Wait for a queued display buffer before drawing into it again, addressing
  disappearing menu layers. GTA's title was also observed by the user without
  perceived flicker in a development run; this does not establish every transition.
- Correct quad DPP activity and full native32 EXEC masks; missing image-store
  DMASK components are zero, including alpha.
- Integrate published shader-call, coherent-buffer and workgroup improvements
  used in Wolverine/ASTRO development. Experimental mesh/NGG/BVH, native32 and
  memory modes remain opt-in; complete gameplay is not claimed for either title.
- Update kernel, offline-service, audio-filter and VideoOut handling from main.
- Preserve the public launcher's keyboard hints/remapping, 15 languages and RTL,
  GitHub updates and manual private reports. Automatic uploads remain disabled.
- Preserve synchronous public video callbacks by default. Deferred delivery with
  the current event-thread implementation remains available as an explicit opt-in.

Validation of the final package is recorded separately. Development-build game
results retain their original build/settings labels. TMNT reached its menus and
introduction but later encountered Vulkan device-lost; its correction is deferred.
No universal FPS increase or newly completed game is claimed by this release.


## 0.1.0 build 571

- Wolverine: passa pelas aberturas (Sony, PlayStation Studios, Marvel Games); depois
  ainda fica em tela preta.
- Peppa Pig (PPSA03706): os vídeos tocam (antes ficavam parados no primeiro quadro).
- No Man's Sky: carrega sem travar; shaders são compilados em segundo plano.
- Atualizações: o jogo usa a pasta do update (user\patch\<TITLEID> ou a pasta -patch
  ao lado do jogo) por cima dos arquivos originais.
- PS5 Pro: a lista de jogos mostra quais aceitam o modo Pro; nas configurações de cada
  jogo dá para pedir o modo PS5 Pro (experimental).
- O nome do jogo na janela segue o idioma escolhido (antes podia sair em árabe).
- Funções de sistema novas: memória AMM, compressão PNG, PlayGo sem metadados e AJM.
- Correções do Codex no tempo de execução (rede local, caminhos de módulos, memória sob
  demanda) e testes novos.
- Astro Bot ainda não é jogável: objetos e personagens não aparecem. Os testes de
  geometria do Codex (NGG mesh, oclusão) vêm desligados.


## Candidata local — verificação de atualizações na tela principal

- Botão Verificar atualizações disponível sem selecionar jogo. A consulta
  acontece em segundo plano; o botão fica desativado enquanto ela está ativa.
- Busca ao iniciar explicitamente ligada por padrão, respeitando a preferência
  existente. A instalação de uma atualização continua pedindo confirmação.
- Ajuda e botão traduzidos nos 15 idiomas. Logs continuam em user/logs.

## Candidata local — coleção acumulativa de bibliotecas

- Bibliotecas locais reconhecidas agora são copiadas para conjuntos completos
  em system/modules/collections. Cópias idênticas são reaproveitadas e conjuntos
  diferentes são acrescentados sem sobrescrever arquivos ou misturar origens.
- A última coleção completa pode ser usada sem o disco de origem. Se a cópia
  falhar, a interface avisa e continua usando a pasta original disponível.
- Bibliotecas proprietárias continuam fora do Git e dos pacotes distribuídos.

## Candidata local — descoberta de bibliotecas

- O launcher e o núcleo localizam bibliotecas já presentes em instalações
  próximas e guardam apenas o caminho no perfil do usuário. A pasta escolhida
  em Configurações continua prioritária; os módulos não são copiados nem baixados.
- Reconhecimento de cabeçalhos ELF e SELF, busca limitada e ajuda atualizada
  nos 15 idiomas. Não há mudança no renderizador ou anúncio de ganho de FPS.
- Análise da AGC do AnyPS5 registrada para orientar uma futura substituição
  gradual da biblioteca original; a dependência ainda existe nesta versão.

## 0.1.0 build 481

- DLC: coloque cada DLC numa pasta em user\dlc\<TITLEID>\<pasta>\ e o jogo o vê instalado.
- Multijogador local: Configurações > Controle > Jogadores locais (1 a 4); do jogador 2 em
  diante, um controle de Xbox para cada.
- Painel de desempenho no jogo (tecla F10 ou Configurações): FPS, tempo de quadro e placa.
- O launcher toca a música do jogo selecionado (dá para desligar nas Configurações).
- Troféus desbloqueados aparecem na tela (por número) e ficam salvos.
- Gráficos: stencil (máscaras de desenho que o Unreal usa muito), contas de 64 bits nos
  shaders e tamanho de pontos; correções do Codex para o GTA V (profundidade e cobertura).
- Funções de sistema novas: data e hora completas (Rtc) e outras de rede e kernel.

## 0.1.0 build 469

- Menos esperas pela placa de vídeo: Dead Cells até 75% mais rápido, Minecraft chega ao
  menu bem mais rápido, Rugrats e KAGE também ganharam. Jogos que já estavam no limite da
  placa de vídeo (No Man's Sky na galáxia, LEGO Bricktales em 4K) ficam iguais.
- Minecraft: o narrador de acessibilidade não gera mais avisos (continua sem voz).

## 0.1.0 build 467

- No Man's Sky: chega aos menus, à escolha de modo e à viagem pela galáxia do jogo novo
  (antes: tela preta e fechava aos 16 s); não perde mais a placa de vídeo no meio do jogo.
- Sonic Frontiers: os raios de neon somem como no console (ficavam marcados na tela);
  iluminação do cenário corrigida.
- Dragon Ball Sparking ZERO: fim da imagem roxa e dos pontos pretos no brilho (bloom).
- GTA V: cena e iluminação corrigidas em vários pontos; KAGE: personagem visível.
- Astro Bot e outros: áudio com reamostragem; Ghost of Yotei avança mais na inicialização.
- Bibliotecas do sistema: todas as funções que os jogos testados chamavam e faltavam
  agora existem (JSON, serviços de rede/PSN offline, gestos, compartilhamento, kernel).
- Shaders: 944 de 1007 instruções da GPU do PS5 traduzidas (antes ~770), incluindo
  amostragem de textura em todas as variantes; o ray tracing ainda não é suportado.

## 0.1.0 build 117

- Super Bomberman R 2: a janela de login da PSN fecha como "cancelada" (o jogo ficava
  esperando por ela aos ~110 s); a pergunta "tem conta PSN?" responde que não.

## 0.1.0 build 115

- Minecraft: endereços de rede escritos como texto ("192.168.0.1") passam a ser
  convertidos (antes o jogo recebia "endereço inválido"); o mundo abre e o jogador
  conecta ao servidor local da partida.

## 0.1.0 build 113

- Super Bomberman R 2: não fecha mais aos ~22 s (o endereço do servidor online não
  era separado em partes e o jogo usava lixo como ponteiro).
- Contra: Operation Galuga: a busca por saves antigos do PS4 responde "não existe",
  como num PS5 sem save de PS4.
- Login na PSN responde "sem conta conectada" em vez de um "ok" vazio. Nada sai do
  PC: os jogos continuam sem acesso à internet.
- Relatórios: mostram a placa de vídeo escolhida e todas as placas do PC (antes saía
  a do monitor principal, que pode não ser a usada).

## 0.1.0 build 106

- GTA V entra no jogo: passa da tela inicial (aperte X quando aparecer o logo) e
  carrega o modo história; o menu de pausa abre (botão Options). Os textos ainda saem
  com letras trocadas.
- Relógio de rede, licença do jogo (versão completa) e troféus respondem como num
  console sem PSN.

## 0.1.0 build 103

- Astro Bot e outros jogos: menos esperas da CPU pela placa de vídeo e cópia mais
  rápida dos resultados de volta (Astro ~20% mais rápido nos menus; KAGE também).
- Shaders com acesso FLAT à memória passam a funcionar (tela de carregamento do GTA V).
- Threads que esperam um mutex não perdem mais a vez para quem o solta e pega de novo.

## 0.1.0 build 100

- GTA V passa do ponto em que fechava (~30 s) e chega à tela do logo; o carregamento
  segue a partir dali. Os arquivos lidos chegavam vazios à descompressão (memória
  remapeada pelo jogo perdia o conteúdo, e a leitura direta nos buffers do jogo
  não era permitida), e o chat de voz dividia por zero.
- Astro Bot: não fecha mais ao abrir o vídeo do logo da PlayStation Studios (o
  emulador escrevia 8 bytes além da estrutura de informações do vídeo).

## 0.1.0 build 96

- Astro Bot: a imagem final passa a ser desenhada (a GPU salvava e restaurava o estado
  de desenho com um comando que o emulador ignorava) e os contadores de culling na GPU
  (GDS) funcionam. A fase ainda não aparece: teste com o jogo no disco local (pela rede
  o carregamento leva vários minutos).
- Leitura de arquivos dos jogos mais rápida (índice de arquivos e arquivos mantidos
  abertos).

## 0.1.0 build 94

- Astro Bot inicia e roda (antes fechava na hora). A imagem ainda fica preta e os
  textos não aparecem: próximos passos.
- Som: reverb (ambiência) e filtros do mixer de som do PS5 são aplicados.
- Jogos que trocam de pilha (fibras/tarefas) não travam mais sem mensagem no log.

## 0.1.0 build 91

- Corrige a build 88, que não abria jogos ("libwinpthread-1.dll não foi encontrado").
- Mixer de som do PS5 (libSceNgs2): Dead Cells e Void Terrarium passam a ter som;
  Little Nightmares II fica bem mais audível. Reverb e filtros ainda não são aplicados.

## 0.1.0 build 88

- Som dos jogos: o decodificador de áudio do PS5 (ATRAC9, AAC, MP3, Opus) foi
  implementado. Jogos que ficavam mudos passam a ter som (Little Nightmares).
- O emulador inclui o FFmpeg (versão enxuta, licença LGPL, aviso em licenses\):
  vídeos decodificados mais rápido e em mais formatos.

## 0.1.0 build 84

- Vídeos: o player de vídeo do PS5 (libSceAvPlayer) foi implementado, com imagem e
  som, usando o decodificador do próprio Windows. Jogos que pulavam os vídeos devem
  mostrá-los agora. Se algum jogo travar num vídeo, desligue em user\config com
  [video] e filmes=0.
- Pastas de jogos com acentos ou travessões ("–") no nome abrem normalmente
  (God of War Sons of Sparta não abria).

## 0.1.0 build 80

- Sonic Frontiers carrega as fases e shaders e segue rodando (antes recusava os
  pacotes com "packfile ver.3 is not supported" e fechava aos 50 s).
  Precisa do libSceAmpr.sprx novo em system\modules.
- Bibliotecas de system\modules têm preferência sobre as da pasta fakelib do jogo;
  a fakelib só cobre o que falta no sistema.

## 0.1.0 build 78

- O título da janela do jogo mostra a versão do emulador e o nome do jogo
  (ex.: "ZeraPS5 0.1.0 build 78 - Shadow of the Ninja (PPSA20450)").
- Texturas em array com mais de 8 camadas: a partir da 9ª camada o jogo recebia a
  primeira no lugar (texturas trocadas).

## 0.1.0 build 75

- Nova opção nas Configurações: "Enviar o log para análise quando o jogo fechar"
  (marcada). O envio é em segundo plano; a barra de status avisa quando termina.

## 0.1.0 build 73

- Vários monitores em placas diferentes: a janela do jogo abre num monitor ligado à
  placa de vídeo escolhida (passar a imagem de uma placa para outra deixava o início
  lento ou travado). O log lista cada monitor e a placa dele.

## 0.1.0 build 71

- O log mostra quanto a placa de vídeo levou para iniciar e para ligar a imagem à
  janela (lento quando o monitor está em outra placa).

## 0.1.0 build 69

- Quando nenhum quadro aparece por 30 s, o log registra onde cada thread do jogo
  está esperando: os relatórios de travamento passam a mostrar a causa.

## 0.1.0 build 67

- Sonic Frontiers: os arquivos das fases carregam (comandos de leitura que o
  emulador não conhecia); antes ficava parado no quadro 0. Achado pelo primeiro
  relatório enviado pelo launcher.

## 0.1.0 build 65

- Reportar problema: escreva o que aconteceu e envie direto para análise (vai para o
  servidor de atualizações; uma cópia fica em user\reports).
- Opção "Verificar atualizações ao iniciar" nas Configurações (marcada por padrão).
- O launcher informa a versão instalada ao servidor, para sabermos quem tem qual build.
- Dragon Ball Sparking ZERO em português: em user\config\PPSA15211.ini ponha
  [jogo] e userDefinedParam1=2 (região americana; o dump diz Europa).
- Shaders com desvios (if/else e laços) são traduzidos como código estruturado em
  vez de uma máquina de estados: a GPU os executa mais rápido (Little Nightmares
  chega ao logo ~50% mais rápido; compute shaders do Dragon Ball ~3x menos tempo de GPU).
- Análise dos shaders antes de cada desenho mais leve (pula instruções vetoriais).
- Parâmetros do jogo (userDefinedParam do param.json) chegam ao jogo: Dragon Ball
  Sparking ZERO usa a região certa e sai do japonês (vai para o inglês).

## 0.1.0 build 58

- Minecraft: as telas novas (a partir de "Começar", como MODO DE JOGO) mostram
  texto, imagens e ícones; antes apareciam só os cartões vazios.

## 0.1.0 build 57

- O endereço de atualização vem embutido no launcher: não precisa configurar.

## 0.1.0 build 56

- A versão do launcher aparece no título da janela e sempre no canto direito da
  barra de status.

## 0.1.0 build 55

- Primeira atualização entregue pelo próprio launcher.
- A pergunta de atualização mostra a versão uma vez só.

## 0.1.0 build 53

- Launcher: atualização automática (endereço http:// da rede ou pasta), escolha da
  placa de vídeo, pastas de jogos em qualquer disco, teclas configuráveis,
  botão ↻ Recarregar.
- Configurações em user\config\zeraps5.ini (e por jogo), nunca sobrescritas.
- Aviso claro quando faltam bibliotecas de sistema do console.

## 0.1.0 (primeira build de teste)

- Executa jogos de PS5 nativamente em PCs x86-64 com Vulkan.
- Chegam ao gameplay: KAGE Shadow of The Ninja, Dead Cells, Record of Lodoss War,
  Joe & Mac, Arkanoid Eternal Battle (modo RETRO), Rugrats, Void Terrarium, Minecraft
  (menu e mundo; crashes intermitentes).
- Chegam às telas iniciais ou ao menu: Little Nightmares I e II, Os Smurfs, LEGO,
  Resident Evil 2, Dragon Ball Sparking ZERO.
- Som (AudioOut/AudioOut2), saves por jogo em `user/`, console em português do Brasil.
- Texturas cubemap e em array (panorama do menu do Minecraft).
