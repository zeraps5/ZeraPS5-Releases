# Compatibilidade

Atualizada em 17/11/2026. Testes num notebook i5-1235U com gráfico Intel Iris Xe, salvo indicação; numa placa de vídeo dedicada os números de FPS mudam.

| Situação | Significado |
| --- | --- |
| **Jogável** | Dá para jogar do início ao fim do que foi testado. |
| **No jogo** | Chega ao gameplay, com defeitos ou lentidão. |
| **Menu** | Chega à tela de título ou ao menu, mas não ao gameplay. |
| **Abertura** | Mostra logos ou vídeos iniciais e para. |
| **Não abre** | Fecha ou para antes de mostrar imagem. |
| **Requer chaves** | A leitura do pacote depende das chaves fornecidas pelo usuário. |
| **Não confirmado** | A rodada não forneceu evidência suficiente para classificar a etapa alcançada; não significa crash confirmado. |

## Regressão da candidata 0.1.1 — RTX, 15/11/2026

Ryzen 7 5700G, RTX 5060 Ti 16 GB, 32 GB RAM, Windows 11, uma instância por vez,
configurações padrão de renderização (PS5 base/saída 1080p), caches/perfis isolados.
Os saves originais foram preservados e conferidos por hash. b974 (`675cb09`) foi
usada nos três primeiros; b981 (`e2feb98`) nos demais. Entre esses códigos entram
F10 por eventos e o ajuste específico do Sonic Origins; o backend Vulkan e as
correções de memória dos três primeiros permanecem iguais. Após a rodada,
main `5a5b994` unificou a seleção de resolução interna (integrado como código
b983); Do jogo, usado nesses testes, conserva seu caminho anterior. b984 fecha
somente documentação/metadados sobre esse código. As novas resoluções
explícitas não foram submetidas a benchmark de FPS desses sete jogos. Sessões curtas não
certificam campanhas inteiras, estabilidade prolongada ou todas as transições.

| Jogo | Build | Sessão | Trecho observado e limites |
| --- | --- | --- | --- |
| Minecraft | 974 | 601 s | Mundo desértico, HUD, movimento/câmera e noite/dia. Aproximadamente 9 FPS em janelas estabilizadas, como a b898 retestada por 601 s; não há ganho percentual comprovado. |
| Grand Theft Auto V | 974 | 842 s | Modo história até o banco, personagens/radar e movimento. Cores/iluminação preservadas no trecho observado; 3–5 FPS (mediana 4), ainda muito lento. |
| Red Dead Redemption | 974 | 902 s | Abertura 3D com cidade, NPCs e trem; não houve controle de gameplay confirmado. NPCs translúcidos também reproduzidos na b898, na mesma tomada da cidade. |
| Skyrim | 981 | 362 s | Novo jogo até a carroça 3D; câmera responde nos dois sentidos, personagens opacos. Mediana 6 FPS no trecho (faixa 1–15, incluindo cargas); há ruído/dithering na vegetação. Sem exploração livre validada. |
| Quake II | 981 | 361 s | Save auto [02] Outer Base carregado, HUD, movimento/câmera e disparo com recuo visível. 31–53 FPS (mediana 43) na janela final com movimentação; não é comparação A/B de ganho. |
| Marvel's Wolverine | 981 | 240 s | Animação/logo da Marvel e depois preto. Mesma limitação da b898, retestada por 181 s com os mesmos arquivos legíveis. Não qualificar como gameplay. |
| Astro Bot | 981 | 302 s | Apenas fundo escuro; sem imagem útil ou gameplay. b898 retestada por 302 s também fica sem cena útil (fundo, partículas/retângulos pretos). Imagem e jogabilidade não aprovadas; tempos frios diferiram, sem conclusão de ganho. |

As sete sessões corretas terminaram de forma controlada com código zero e sem
ERROR/FATAL. No Wolverine, uma primeira montagem misturava ELF com bibliotecas
criptografadas; foi descartada da comparação e repetida com os arquivos legíveis
já existentes. Isso não foi classificado como regressão. Relatório detalhado em
`docs/colaboracao/RELEASE-CANDIDATE-0.1.1-2026-11-15.md`.
Não houve novo teste físico deste pacote no Iris Xe/i5 antigo. Os resultados
históricos do notebook abaixo conservam seus builds e configurações originais.

## Jogos de PS5

Novos títulos na RTX em 17/11/2026: main `3a96267`, build 1024, Ryzen 7 5700G,
RTX 5060 Ti 16 GB, 32 GB RAM, PS5 base, saída 1080p e resolução interna do jogo.
Sessões iniciais com perfis/cache privados, sem benchmark A/B. Prioridade do
processo BelowNormal a partir de Venba para manter o Windows responsivo;
Silksong começou em normal e mudou no fim, Iron Lung usou normal. Capturas
feitas pelo renderer, com limites descritos no
`docs/colaboracao/RELATORIO-CODEX-novos-jogos-2026-11-17.md`.

| Jogo | ID | Situação | Observações |
| --- | --- | --- | --- |
| Quake III Arena (homebrew PS5_Quake3e v1.4.0) | PPSA99319 | No jogo | RTX 5060 Ti, 12/11/2026, build experimental 906 (`6c44020` com alterações locais): menu e partida local em q3dm1, um cliente humano, movimento/câmera/disparo por roteiro. As correções foram integradas posteriormente à 0.1.1; isso não representa novo teste completo do jogo. Bots, tela dividida, áudio audível, sessões longas e multiplayer externo não validados. Consulte docs/QUAKE3-NATIVE-INPUT.md. |
| New Joe & Mac: Caveman Ninja | PPSA02801 | Jogável | 60 FPS; em 01/11/2026 os textos de todos os itens dos menus aparecem. |
| Sonic Frontiers | PPSA03831 | Jogável | Confirmado pelo usuário em 30/10/2026; vídeo do logo sem imagem. |
| Sonic Superstars | PPSA06888 | No jogo | Iris Xe, main `9bb4367`, 15/11/2026: logos, título, menu, seleção de personagem, o vídeo de abertura (com imagem) e a primeira fase (Bridge Island) com HUD e cenário corretos, a ~15–16 FPS; menus a ~60 FPS. |
| Sonic Origins Plus | PPSA05324 | No jogo | Iris Xe, 15/11/2026: Sonic 1 (Modo Aniversário) jogável em Green Hill Zone a ~23 FPS em 1080p e ~28-38 FPS com resolução interna 720p (28 FPS medido no main em 15/11/2026). O jogo usava 4K fixo; uma correção agora monta a tela no tamanho da saída (saída 4K mantém o 4K). |
| The Grinch: Christmas Adventures | PPSA07656 | Abertura | Iris Xe, 15/11/2026, .pkg (fPKG backport): abre direto do pacote depois da correção da leitura de inodes; a tela de carregamento aparece correta, mas ao entrar na fase o jogo para (desenhos lentos com o rastreio de escrita ligado; compilação de pipeline que não termina). |
| Dead Cells: Return to Castlevania Edition | PPSA15552 | No jogo | Com som. Desde 01/11/2026 o menu não pisca mais (camada da interface sumia em quadros soltos). |
| KAGE: Shadow of The Ninja | PPSA20450 | No jogo | Stage 1 jogável e renderizado corretamente (01/11/2026). |
| Ratchet & Clank: Size Matters (clássico de PSP) | PPSA14431 | No jogo | Iris Xe, branch `claude/ratchet-psp`, 02/11/2026: logos, abertura, menu com texto (~29 FPS) e New Game até o gameplay em Pokitaru, com texturas e cores corretas, a ~11–13 FPS (limitado pela GPU). START é o clique na metade direita do touch pad; o menu do emulador (OPTIONS) abre sem texto. As correções estão no candidato ao main no merge `d5872a5`; no build desse merge o gameplay ficou em ~14–15 FPS. Iris Xe, main `64f19a6` (03/11/2026): gameplay em Pokitaru a ~17–18 FPS (os dois shaders de pixel do PSPHD deixaram o laço de despacho). Branch `claude/font-layout` (03/11/2026): o menu do emulador (OPTIONS) abre com texto. |
| Disney's Atlantis: The Lost Empire (clássico de PS1) | PPSA14421 | No jogo | Iris Xe, branch `claude/ratchet-psp`, 02/11/2026. Emulador PS1HD da Sony: logos, abertura, menu a 60 FPS e a primeira fase (Whitmore's Mansion) a ~6–7 FPS, limitada pela GPU. START é o clique na metade direita do touch pad; as dicas do emulador aparecem sem texto (libSceFont). No main `1c4893f` fica sem imagem; as correções estão no candidato ao main no merge `d5872a5`, ainda sem reteste nesse build. Branch `claude/font-layout` (03/11/2026): o menu do emulador (OPTIONS) e as dicas aparecem com texto. Branch `claude/ps1hd-perf` (03/11/2026): menu a ~30 FPS e a primeira fase a ~22–24 FPS (as cópias da VRAM para os desenhos que a amostram levam só o que mudou). |
| BUBBLE BOBBLE Sugar Dungeons | PPSA26356 | No jogo | Iris Xe, main `d9d6194`, 06/11/2026: o eboot de `decrypted/` também abre (chega ao aviso de salvamento automático). RTX 5060 Ti, main `1c4893f` (build 756): Bub responde ao controle na área inicial; pelo Adventure Gate chega ao tutorial Basic Operations da Training Room, com movimento e bolhas visíveis. Usa o eboot da raiz da pasta do jogo. FPS de gameplay não medido. |
| Planet of Lana | PPSA19450 | No jogo | RTX 5060 Ti, main `1c4893f` (build 756): menu em português, Novo Jogo e tutorial de movimento na vila; Lana atravessa a passarela com o analógico. FPS de gameplay não medido. |
| Fatal Frame / Project Zero: Maiden of Black Water | PPSA04333 | No jogo | RTX 5060 Ti, main `1c4893f` (build 756): depois da cinemática, chega ao tutorial de câmera de Miu Hinasaki no prólogo Lurking in the Deep; o diálogo avança com X e a câmera responde ao analógico. A cena fica bastante escura. FPS de gameplay não medido. |
| Return to Monkey Island | PPSA10122 | No jogo | Iris Xe, 07/11/2026: menu, Novo jogo, Prelúdio e a primeira cena no parque com personagens. As letras acentuadas das legendas aparecem desde a varredura de texturas recém-enviadas ("máscara"). |
| Wobbly Life | PPSA29413 | No jogo | Iris Xe, main `8ffa708`, 07/11/2026: menu, Selecione um Mundo e a história inicial; o "P" de "Pressione ✕ para continuar" sai escuro em vez de branco. |
| The Rogue Prince of Persia | PPSA28155 | No jogo | Iris Xe, main `628cb73`, 08/11/2026: título (confirma com ○), aviso dos servidores da Ubisoft, salvamentos (✕ carrega), "Carregando" em ~15 s, a história inicial, o logo da Ubisoft, a cena em quadrinhos e a primeira fase com o Príncipe e diálogo, a ~11–12 FPS (limitado pelo processador). Antes parecia parado no "Carregando" porque o teste terminava cedo demais. |
| Record of Lodoss War: Deedlit in Wonder Labyrinth | — | No jogo | Iris Xe, main `df464cf`, 16/11/2026: intro a ~57 FPS (antes ~28; duas texturas grandes eram rechecadas inteiras a cada quadro). |
| Arkanoid: Eternal Battle | PPSA06328 | No jogo | Modo RETRO. |
| Rugrats: Adventures in Gameland | PPSA23396 | No jogo | |
| Void Terrarium | — | No jogo | Com som. |
| Minecraft | PPSA17221 | No jogo | Menu e mundo; travamentos ocasionais. |
| Grand Theft Auto V | — | No jogo | Cena 3D com personagem e radar; ainda com defeitos de texto e transições. |
| Dragon Ball: Sparking! ZERO | PPSA15211 | No jogo | Chega à luta; imagem escura. |
| Crash Bandicoot 4: It's About Time | PPSA02433 | Menu | Iris Xe, main `bbecd5a`, 10/11/2026: logos e tela "PRESS START" com imagem correta, ~31 FPS com pausas de até 0,6 s. Não testado além do título. |
| XIII | PPSA02775 | Menu | Iris Xe, main `bbecd5a`, 10/11/2026: vídeos de logo (Microids, PlayMagic) e a tela "Press any button to start" com imagem correta, ~36 FPS. Não testado além do título. |
| Poppy Playtime: Chapter 2 | PPSA21631 | Menu | Iris Xe, main `bbecd5a`, 10/11/2026: menu principal em português (Jogar / Configurações), ~7–12 FPS. Não testado além do menu. |
| No Man's Sky | PPSA02110 | Menu | Menus, escolha de modo e viagem pela galáxia do jogo novo. |
| Little Nightmares Enhanced Edition | — | Menu | Tela de título a ~3,5–4 FPS (limite da GPU Iris Xe). O ray tracing vem desligado; para testar, `PS5C_SHADER_CALLS=1` e `PS5C_BVH=1`. |
| Wizard with a Gun | PPSA06416 | Menu | ~42 FPS; o primeiro quadro tem blocos azuis e vermelhos. |
| The Mortuary Assistant | PPSA16110 | Menu | Iris Xe, main `1c4893f`, 02/11/2026: logos da DreadXP e aviso inicial com texto a ~40–60 FPS; o menu principal aparece (Novo turno, Somente embalsamento, Opções), mas a ~1 FPS. Iris Xe, main `f4e3795` (03/11/2026): menu a ~3 FPS (o shader principal do menu deixou de cair no laço de despacho). |
| Dying Light: The Beast | PPSA24002 | Menu | Tela de título. Com `[psn] conta=1` passa do login e fica no carregamento. |
| LEGO Bricktales | PPSA07430 | Menu | Roda em 4K. |
| Os Smurfs | — | Menu | |
| Resident Evil 2 | — | Menu | |
| Peppa Pig | PPSA03706 | Menu | Os vídeos tocam. |
| Commandos: Origins | PPSA14708 | Menu | Iris Xe, branch `claude/commandos` (03/11/2026): o vídeo de abertura toca com som e o jogo chega à escolha de idioma (~3 min), a menos de 1 FPS: o Unreal renderiza em 4K e alguns compute shaders levam mais de 1 s na GPU. Com o idioma salvo, o menu principal abre em ~2 min; toques curtos no X (não segurar) entram em New Game, dificuldade e na tela de carregamento da primeira missão, onde o teste de 10 min terminou compilando pipelines. Antes da branch, travava no fim do vídeo (neblina volumétrica no laço de despacho) e, depois disso, fechava por volta de 80 s (memória de vídeo esgotada). |
| Super Bomberman R 2 | — | Abertura | Passa do pedido de login da PSN; estado atual não verificado. |
| Little Nightmares II | — | Abertura | Logos a ~5 FPS, depois tela preta. |
| Marvel's Wolverine | — | Abertura | Passa pelas aberturas (Sony, PlayStation Studios, Marvel Games), depois tela preta. |
| Ghost of Yōtei | PPSA26344 | Abertura | Splash inicial. |
| Darwin's Paradox | PPSA28608 | Abertura | Iris Xe, branch `claude/darwin`, 23/11/2026, .pkg: logo da ZDT a ~2,4 FPS com o back buffer em 1080p (era 4K, ~0,7 FPS). Um compute shader com tabela de descritores por lane ainda é pulado. A primeira execução compila shaders por vários minutos. |
| Astro Bot | PPSA21567 | Abertura | Objetos e personagens não aparecem. |
| God of War Sons of Sparta | — | Abertura | Para em "Sony presents". |
| TMNT: Splintered Fate | PPSA25582 | No jogo | Iris Xe, branch `claude/tmnt-tela`, 08/11/2026: logos, arte da abertura e termos em tela cheia (antes só um quarto da tela); menu e tutorial ("MOVIMENTO") a ~14 FPS em 1080p e ~23 FPS com Resolução interna 720p, cena iluminada (antes muito escura: a tabela de cor saía preta). Ao entrar no jogo, 8–15 s sem imagem compilando shaders. |
| Gothic 1 | PPSA09846 | Não abre | O dump testado está incompleto ("Global shader library missing"). |
| Tetris Effect: Connected | PPSA07923 | Abertura | Iris Xe, main `d9d6194`, 06/11/2026: passa da alocação de 11 GiB (contagem de troféus do `sceNpTrophy2GetGameInfo`) e mostra o aviso de fotossensibilidade; não testado com controle. |
| Double Dragon Gaiden: Rise of the Dragons | PPSA16018 | Menu | Iris Xe, main `d9d6194`, 06/11/2026: tela de título a ~50 FPS (plugins de Media/Plugins iniciam quando o jogo os carrega). |
| Jurassic Park Classic Games Collection | PPSA17217 | Menu | Iris Xe, main `d9d6194`, 06/11/2026: vídeo de abertura e tela "PRESS ✕ TO START" a ~40 FPS (`sceKernelMapDirectMemory2`, libScePngDec, AvPlayer sem callback, métricas de fonte). |
| TMNT Arcade: Wrath of the Mutants | PPSA18571 | Menu | Iris Xe, main `d9d6194`, 06/11/2026: chega à seleção de personagem (módulos de Media/Modules iniciam quando o jogo os carrega). |
| Diablo II: Resurrected | PPSA03912 | Menu | Iris Xe, main `d9d6194`, 06/11/2026: tela "Pressione ✕ para começar" a ~20 FPS; o carregamento até ela leva cerca de 60 s. |
| Alone in the Dark (2024) | PPSA08240 | Menu | Iris Xe, main `d9d6194`, 06/11/2026: menu principal (Continuar, Novo Jogo, Opções). Antes, o driver da Intel travava num shader com 175 samplers. |
| Alone in the Dark 2 (clássico de PS1) | PPSA24515 | Jogável | Iris Xe, main `540ebaf`, 08/11/2026: emulador PS1HD da Sony; escolha de idioma, "Jack is back", carregamento, a abertura ("Two days later...") e o jogo com o personagem andando e lutando, a ~60 FPS (o usuário confirmou que funcionou perfeito). Precisava das funções de texto da libSceFont. |
| Trifox | PPSA06408 | Abertura | Iris Xe, main `d9d6194`, 06/11/2026: logos iniciais (dump backport com a pasta `decrypted/`). |
| Hello Kitty Island Adventure | PPSA18176 | Menu | Iris Xe, main `d9d6194`, 06/11/2026: tela de título (dump backport com a pasta `decrypted/`). |
| Bob Esponja: Titãs da Maré | PPSA26893 | Abertura | Iris Xe, main `d9d6194`, 06/11/2026: imagem exFAT; logos THQ Nordic e Purple Lamp e aviso de epilepsia em português. |
| Grand Theft Auto: San Andreas – The Definitive Edition | PPSA03524 | Abertura | Iris Xe, main `d9d6194`, 06/11/2026: logo inicial (751 quadros em 60 s); antes parava no primeiro quadro. |
| Silent Hill Townfall | PPSA33286 | Abertura | Iris Xe, main `d9d6194`, 06/11/2026: não fecha mais, mas faz só ~5 quadros em 60 s na Iris Xe (criação de shaders de compute e cópia de buffers). |
| Arcade Game Zone | PPSA19015 | Não abre | RTX 5060 Ti, main `1c4893f` (build 756): quadros pretos; erro ao carregar os textos e encerramento na inicialização de `ContentManagement`. |
| The Last of Us Part I | PPSA07642 | Não abre | Iris Xe, main `bbecd5a`, 10/11/2026, direto do .pkg (v02.002.000): passa da memória flexível e do PlayGo, carrega os arquivos e escolhe o idioma, mas os quadros saem pretos (~0,4 FPS). |
| Dolphin Spirit: Ocean Mission | PPSA13711 | No jogo | RTX 5060 Ti 16 GB, Ryzen 7 5700G, 32 GB RAM, b897 `a1e7371`, 10/11/2026: gameplay subaquático com movimento e câmera respondendo. Rodada de 181 s, aproximadamente 30–56 FPS no trecho observado e 55,9 FPS nos últimos 20 s registrados. Saída 1080p, resolução interna do jogo. |
| R-Type Delta HD Boosted | PPSA26414 | Menu | RTX 5060 Ti, b897 `a1e7371`, 10/11/2026: título e demonstração 3D a cerca de 45 FPS no trecho final. A abertura apresenta defeitos geométricos e de cor. Partida controlada ainda não foi confirmada. |
| Balan Wonderworld | PPSA02058 | Menu | RTX 5060 Ti, b897 `a1e7371`, 10/11/2026: menu e seleção de idioma em português. Novo jogo ficou preto e repetiu erros de áudio ATRAC9 durante a rodada de 182 s. |
| Halloween: The Game | PPSA29340 | Abertura | RTX 5060 Ti, b897 `a1e7371`, 10/11/2026: abertura com logo e exceção do convidado na thread de carregamento, ao escrever em 0x28. |
| Age of Mythology: Retold | PPSA18653 | Não abre | RTX 5060 Ti, b897 `a1e7371`, 10/11/2026: o leitor do pacote apresentou eboot.bin com zero bytes. A causa ainda está em investigação. |
| Immortals of Aveum | PPSA08236 | Abertura | Iris Xe, 16/11/2026, teste de 150 s: logos (Ascendant Studios, EA Originals) e fecha aos 33 s com acesso inválido à memória no executável do jogo. |
| Demon's Souls | — | Abre | Iris Xe, 16/11/2026, direto do .pkg: a abertura travava ~130 s na criação da porta de áudio (dispositivo de som do Windows abrindo na thread do jogo); com o som abrindo em thread própria (main `ad284c9`) o primeiro quadro sai aos ~57 s. Tela preta a ~1,6 FPS: shaders usam image_store_mip com mip variável e image_gather4_c_lz_o (não suportados). |
| Stray | PPSA02100 | Abertura | Iris Xe, 16/11/2026, teste de 150 s: tela preta com o ícone de carregamento, para aos ~490 quadros. |
| Sonic Unleashed Recompiled (homebrew) | PPSA99902 | Abre | Iris Xe, 16/11/2026: o RADV embutido só aceita memória da GPU entre 8 e 12 GiB; com a janela de 32 bits do perfil do Quake III (estendida a este título) o Vulkan encontra a GPU, desenha e apresenta (~2-3 quadros/s no carregamento). Tela preta: a imagem final lê uma textura em SW_64KB_D_X (modo 26), ainda não suportado. round/ldexp/__isfinite implementados. |
| Tomb Raider I-III Remastered | PPSA16901 | No jogo | Iris Xe, main `df464cf`, 16/11/2026: contrato (passa com direcional para a direita), menu em anel, Novo jogo, vídeo de abertura com legendas em português e a fase das Cavernas do TR1 com HUD. Na fase roda a 60 quadros por segundo (antes ~22: uma textura de 90 MB era rechecada inteira a cada quadro), mas a Lara ainda não responde ao controle. |
| Tomb Raider IV-VI Remastered | PPSA18154 | Menu | Iris Xe, 16/11/2026, teste de 150 s: a seleção de jogos funciona e chega a uma cena 3D de Roma (provável início do Chronicles), ~15 FPS; gameplay a confirmar. |
| Atari 50: The Anniversary Celebration | PPSA08601 | Menu | Iris Xe, 16/11/2026, teste de 150 s: linha do tempo interativa a ~54 FPS, imagem correta. |
| Pumpkin Jack | PPSA03446 | Menu | Iris Xe, 16/11/2026, teste de 150 s: tela de título. |
| Hamster Playground | PPSA06653 | Abertura | Iris Xe, 16/11/2026, teste de 150 s: logo da Unity e tela de carregamento. |
| Propagation: Paradise Hotel | PPSA15878 | Não abre | Iris Xe, 16/11/2026, teste de 150 s: fecha aos 10 s (escrita no endereço 0x28 dentro da libc). |
| TMNT: Mutants Unleashed | PPSA17245 | Abertura | Iris Xe, 16/11/2026, teste de 150 s: alerta de fotossensibilidade, depois tela preta. |
| Resident Evil 4 | PPSA07411 | Abertura | Iris Xe, 16/11/2026: o PlayGo sem playgo-chunk.dat passou a relatar 128 chunks (com 1000 a camada PlayGo do Unity estourava a tabela e a thread PlayGo fechava aos 14 s); passa do logo do RE Engine e chega à tela de consentimento de coleta de dados (main `45011c5`: as sombras rodavam um laço até o limite, ~270 ms por desenho); essa tela roda a ~2 FPS. |
| Goosebumps: Terror in Little Creek | PPSA27305 | Abertura | Iris Xe, 16/11/2026, teste de 150 s: aviso de salvamento automático, depois tela preta. |
| Venba | PPSA13225 | No jogo | RTX, b1049, 17/11/2026, PKG v02.000.000: Play, abertura, personagens e cena inicial do quarto; diálogo respondeu a X, aproximadamente 60 FPS sem instrumentação. 199 s; cozinha e capítulo completo não testados. Resultado anterior da b1024 preservado na regressão. |
| A Game About Digging a Hole | PPSA30552 | No jogo | RTX, b1024, 17/11/2026, PKG v01.100.000: Novo jogo, cenário 3D parcial, HUD e pausa respondendo a Options. 182 s; capturas quase pretas, câmera e escavação não confirmadas. Imagem e jogabilidade ainda pendentes. |
| Hollow Knight: Silksong | PPSA12544 | Menu | RTX, b1024, 17/11/2026, PKG v01.000.010: título com Iniciar o jogo/Opções/Extras, cerca de 4–5 FPS e uso muito alto de CPU. 242 s; gameplay não confirmado. |
| Iron Lung | PPSA21755 | Menu | RTX, b1024, 17/11/2026, PKG v01.000.100: título New/Continue/Settings a cerca de 5 FPS, uso muito alto de CPU. 181 s; gameplay não confirmado. |
| Death Howl | PPSA30896 | Menu | RTX, b1024, 17/11/2026, PKG v01.000.004: menu; após Start New Save as capturas ficam quase pretas, cerca de 9 FPS. 211 s; sem gameplay confirmado. A primeira medição, interrompida por falha do gravador, foi descartada e repetida. |
| The Beast Inside | PPSA10720 | Menu | RTX, b1024, 17/11/2026, exFAT v01.000.000: menu em português, Novo jogo, dificuldade Normal e carga do Capítulo 1. 271 s; sem gameplay confirmado antes do limite, ainda compilando pipelines a frio. |
| The Walking Dead: Streets of Survival | PPSA38510 | Abertura | RTX, b1024, 17/11/2026, PKG v01.000.001: arte inicial; contador parou em 15 flips até o fim, mesmo após X. 181 s; não chegou ao menu/gameplay, sem erro fatal registrado. |
| Invincible VS | PPSA30568 | Abertura | RTX, b1024, 17/11/2026, PKG v01.016.000: aviso inicial, logo Unreal e cena de tráfego da abertura, cerca de 1–2 FPS no fim. 301 s; sem menu/luta confirmado, sem erro fatal registrado. |
| Survival: Fountain of Youth | PPSA20142 | Não confirmado | RTX, b1024, 17/11/2026, pasta v01.007.000: seis flips, depois sem avanço até 241 s; somente captura uniforme escura, sem título/menu útil confirmado. Uso muito alto de CPU, sem erro fatal registrado. |
| Death Stranding Director’s Cut | PPSA01969 | Menu | RTX, b1049, 17/11/2026, PKG v01.005.000: menu interativo, seleção de Novo jogo/Normal e carregamento; depois cai no executável +0x858ecd, lendo 0x4. 324 s; gameplay não confirmado. Corrigidos eventos APR, temporizador e I/O vetorial que impediam a abertura na b1024. |
| Quake | PPSA01880 | No jogo | Iris Xe, 23/11/2026, teste de 150 s: logos, menu principal (o aviso "Connection error" é do modo online), carregamento e gameplay no mapa inicial com HUD e arma, ~33–37 FPS. |
| Plants vs. Zombies: Replanted | PPSA29802 | Menu | Iris Xe, 23/11/2026, teste de 150 s: logo da EA, tela de título com "Carregando" e as telas de contrato de usuário e política de privacidade com texto legível, ~58 FPS. Só com ✕ a rodada alternou entre as duas telas sem aceitar os termos. |
| LEGO Party | PPSA29141 | Menu | Iris Xe, 23/11/2026, teste de 150 s (backport 4.xx+): arte de carregamento e tela de título "LET'S PARTY!", ~7–16 FPS. |
| The House of the Dead: Remake | PPSA04649 | Abertura | Iris Xe, 23/11/2026, teste de 150 s: arte inicial e depois tela preta, ~17 quadros por segundo. |
| Devil May Cry 5 Special Edition | PPSA01442 | Abertura | Iris Xe, 23/11/2026, teste de 150 s (v1.002): ~30 quadros por segundo, mas a imagem fica preta; só um brilho fraco no fim. |
| Beyond a Steel Sky | PPSA03979 | Abertura | Iris Xe, 23/11/2026, teste de 150 s: quadros pretos a ~58 por segundo; a fila de comandos da GPU ficou bloqueada 10 s e `AkMotion.prx` não carregou. |
| Ultrawings 2 (PS VR2) | PPSA13481 | Não abre | Iris Xe, 23/11/2026, teste de 120 s: nenhum quadro; a thread principal espera um mutex. Jogo de VR (óculos não emulados). |
| Alien: Rogue Incursion Evolved Edition (PS VR2) | PPSA30391 | Não abre | Iris Xe, 23/11/2026, teste de 120 s, direto do .pkg: um quadro, sem imagem. Jogo de VR (óculos não emulados). |
| The Messenger | PPSA24643 | Menu | Iris Xe, 25/11/2026, teste de 90 s: abertura com texto em português e a tela de título "Aperte ✕" a ~50 FPS. Antes ficava preta depois da abertura: o jogo mapeia de novo a memória das telas e a apresentação não achava a imagem desenhada (branch `claude/jogos-20261125`). |
| Goodnight Universe | PPSA29727 | Menu | Iris Xe, 25/11/2026, .pkg (backport 4.xx): menu inicial (mesma correção do The Messenger), ~14 FPS. Em duas de três rodadas o driver perdeu a GPU (device lost) entre 24 e 88 s, também sem a correção. |
| What Remains of Edith Finch | PPSA06207 | Abertura | Iris Xe, 25/11/2026, teste de 90 s: logos da Annapurna e da Giant Sparrow em 4K, ~8 FPS; 126 pipelines compilados em 38 s. |
| Octopath Traveler II | PPSA09809 | Menu | Iris Xe, 25/11/2026, teste de 90 s: tela inicial de idioma e brilho (sem entrada na rodada). |
| Grendizer: The Feast of the Wolves | PPSA14376 | Abertura | Iris Xe, 25/11/2026, teste de 90 s: cena de abertura com imagem correta, ~21 FPS. |
| Harvest Days | PPSA14677 | Menu | Iris Xe, 25/11/2026, teste de 90 s: menu principal com cena 3D, ~50 FPS. |
| Enchanted Portals | PPSA17144 | Menu | Iris Xe, 25/11/2026, teste de 90 s: menu em português em 4K, ~4 FPS. |
| Aeterna Noctis | PPSA04109 | Menu | Iris Xe, 25/11/2026, teste de 90 s: logos e menu em português, ~47 FPS. |
| Asterix & Obelix XXL 3 | PPSA07498 | Menu | Iris Xe, 25/11/2026, teste de 90 s: aviso inicial e "Press the cross button" com cena 3D, ~26 FPS. |
| Ghost Song | PPSA09232 | Menu | Iris Xe, 25/11/2026, teste de 90 s (backport 4.xx): créditos iniciais e menu principal, ~21 FPS. |
| POSTAL: Brain Damaged | PPSA18144 | Abertura | Iris Xe, 25/11/2026, .pkg (backport 4.xx): logos e cenas 3D da abertura com imagem, ~41 FPS. |
| The Exit 8 / Platform 8 | PPSA23276 | Não abre | Iris Xe, 25/11/2026, .pkg (backport 4.xx): um quadro em 90 s. Unreal 5 em 4K: a thread da GPU passa segundos em cada compute shader pesado e a fila de compute espera por ela (não é trava). |
| Q.U.B.E. 2 | PPSA06246 | Menu | Iris Xe, 25/11/2026, .pkg: logo da Unreal; a tela de boas-vindas fica preta, só com o ícone ✕. O ✕ leva ao menu principal (o log do jogo confirma), mas a cena e os textos continuam pretos; o jogo não acha a fonte Roboto-Regular no pak. |

## Jogos de PS4

A execução de jogos de PS4 está no começo.

| Jogo | ID | Situação | Observações |
| --- | --- | --- | --- |
| Cloudberry Kingdom | CUSA57754 | Jogável | Primeiro jogo de PS4: logo, vídeo, menus e fases com imagem correta. Texturas com tiling 2D do PS4 ainda aparecem pretas. |
| LEGO City Undercover (PS4) | CUSA06549 | No jogo | Iris Xe, 21/11/2026: roda direto do `.pkg` de PS4 (com o arquivo de chaves do usuário). Logos, título 3D, cutscenes, menu principal, Capítulo 1 e o gameplay na cidade com imagem e cores corretas e HUD. Lento no gameplay: ~3,5 FPS (~2500 desenhos por quadro). |
| The Mortuary Assistant (PS4) | CUSA43772 | Requer chaves | b897 `a1e7371`, 10/11/2026: a leitura do pacote parou na exigência das chaves de PS4 fornecidas pelo usuário. O executável do jogo não chegou a ser carregado. Este registro é separado do teste da versão de PS5 PPSA16110. |
| Sonic X Shadow Generations | CUSA44422 | Não abre | Iris Xe, 16/11/2026, teste de 150 s, direto do .pkg PS4: fecha aos 16 s (acesso inválido no executável), nenhum quadro. |
