# Game compatibility and tested builds

From 2D gameplay on integrated graphics to experimental PS5 scene rendering, these records show the progress reached in ZeraPS5. Each entry keeps its own tested build or dated development assessment and hardware.

Updated: **2026-11-06**. Current candidate package: **b838**. A development result is not automatically a b786 result. Where a per-run build was not recorded, the dated assessment is shown. Playable segments do not imply an entire game was completed.

**Intel integrated-graphics spotlight:** New Joe & Mac: Caveman Ninja is reported at **60 FPS on Core i5-1235U / Intel Iris Xe** in the development assessment. This result is specific to that game/profile; it was not remeasured in the final b705 package.

**Wolverine and ASTRO BOT:** development has advanced in menu/save handling and visible hub rendering. Their detailed stages and remaining work are listed below.

## New records accompanying build 786

The nine RTX 5060 Ti sessions used **build 756 (`1c4893f`)**. The three notebook results used **Core i5-1235U / Intel Iris Xe** and the revisions named below. The classic-game fixes are included in b786; Atlantis was not retested on the merge, and neither classic was retested on RTX for this release. None of these sessions is relabeled as a b786 benchmark. All twelve titles were tested on 2 November 2026.

| Game | Hardware and tested build | Observed result |
| --- | --- | --- |
| BUBBLE BOBBLE Sugar Dungeons | RTX 5060 Ti · build 756, `1c4893f` | Bub responds to controls in the starting area and reaches the Training Room movement/bubble tutorial. Gameplay FPS not measured. |
| Planet of Lana | RTX 5060 Ti · build 756, `1c4893f` | New Game reaches the village movement tutorial; Lana crosses the walkway with analog input. Gameplay FPS not measured. |
| Fatal Frame / Project Zero: Maiden of Black Water | RTX 5060 Ti · build 756, `1c4893f` | Reaches Miu Hinasaki's camera tutorial in the prologue; dialog and camera respond. The scene is very dark. Gameplay FPS not measured. |
| TMNT: Splintered Fate | RTX 5060 Ti · build 756, `1c4893f` | Opening image stays black and stops at 196 frames; X does not reach the menu within 80 seconds. |
| Tetris Effect: Connected | RTX 5060 Ti · build 756, `1c4893f` | Exits before the menu; memory mapping fails with `0x8002000d` in two runs. |
| Double Dragon Gaiden: Rise of the Dragons | RTX 5060 Ti · build 756, `1c4893f` | Exits during initialization before the menu, on a null-address read. |
| Jurassic Park Classic Games Collection | RTX 5060 Ti · build 756, `1c4893f` | Exits before game selection, after an unimplemented memory call and a null-address write. |
| TMNT Arcade: Wrath of the Mutants | RTX 5060 Ti · build 756, `1c4893f` | Black screen and exit before the menu; a PSN-module version warning precedes an invalid access. |
| Arcade Game Zone | RTX 5060 Ti · build 756, `1c4893f` | Black frames and exit during text/content initialization. |
| Ratchet & Clank: Size Matters — PSP classic | Core i5-1235U / Iris Xe · `claude/ratchet-psp`, then merge candidate `d5872a5` | Reaches gameplay in Pokitaru with correct textures/colors. About 11–13 FPS on the test branch and 14–15 FPS in the merge-candidate run; game menu about 29 FPS. Emulator Options menu text is missing. |
| Disney's Atlantis: The Lost Empire — PS1 classic | Core i5-1235U / Iris Xe · `claude/ratchet-psp` | Reaches Whitmore's Mansion at about 6–7 FPS; menu about 60 FPS. Emulator hint text is missing. Not retested on the merged build or RTX. |
| The Mortuary Assistant | Core i5-1235U / Iris Xe · `1c4893f` | Logos/warning at about 40–60 FPS; main menu visible at about 1 FPS. Gameplay not confirmed. |

TMNT: Splintered Fate, TMNT Arcade: Wrath of the Mutants and TMNT: Mutants Unleashed are separate games. Reaching a short playable segment does not establish complete-game compatibility.

## Earlier recorded results

| Game / title ID | Last recorded test or assessment | Stage reached | Hardware | Context |
| --- | --- | --- | --- | --- |
| Grand Theft Auto V / PPSA04263 | Public b705, 2026-11-01 | In-game: bank prologue room and characters | RTX 5060 Ti | Exact packaged executable, RTX 5060 Ti, ~332-second isolated smoke test. Wood, red clothing, yellow notice and reflections visible; intentionally stopped. Longer stability/flicker checks remain relevant. |
| Minecraft / PPSA17221 | Public b705, 2026-11-01 | In-game: copied world, terrain, sky, shovel and HUD | RTX 5060 Ti | Exact packaged executable, RTX 5060 Ti, ~292-second isolated smoke test; intentionally stopped after capture. Cloned settings/saves and explicit host-memory layout; not a benchmark or long-session certification. |
| The Elder Scrolls V: Skyrim / PPSA03747 | Development b703 / 319d150, 2026-11-01 | In-game introduction: cart, characters and forest | RTX 5060 Ti | No major black cutouts in examined frames; slow. Opening segment only. |
| Red Dead Redemption / PPSA30528 | Development b703 / 319d150, 2026-11-01 | Opening scenes: boat, port and characters | RTX 5060 Ti | Material irregularities and Atrac9 warnings remain. Earlier 31-Oct development tests hit device loss; one normal b703 run does not establish a fix. This is not RDR2. |
| ASTRO BOT / PPSA21567 | Development b703 / 319d150, 2026-11-01 | Hub rendered: ship, character and objects | RTX 5060 Ti | Experimental direct-hub profile; corrupt distant terrain, incomplete lighting/UI, very low speed and slow shutdown. Earlier scripted movement/jump records retain their own snapshots. Practical playability is not claimed. |
| Marvel's Wolverine / PPSA03671 | Development work through a1b4e1a, 2026-10-31 | Menus, save selection and incomplete scene | RTX 5060 Ti | Navigation and save handling progressed beyond logos in specific profiles. Character control/gameplay remains unconfirmed. Private 1080p logo-video measurements are not package performance. |
| Teenage Mutant Ninja Turtles: Mutants Unleashed / Title ID pending record | Development b703 / 319d150, 2026-11-01 | Menus / introduction / initial-level loading | RTX 5060 Ti | VK_ERROR_DEVICE_LOST at about 568 seconds in the first run. Diagnostic repeat paused; no fix implemented and gameplay not confirmed. |
| Sonic Frontiers / PPSA03831 | User confirmation 2026-10-30; development assessment 2026-11-01 | User-reported playable segment | Core i5-1235U / Intel Iris Xe (developer assessment) | Initial-logo video lacks image in the assessment. Separate RTX b498 record reached Cyber Space with movement/rings. Do not assign either result to b703. |
| New Joe & Mac: Caveman Ninja / PPSA02801 | Development assessment dated 2026-11-01 / 7377348 | Developer-reported playable segment | Core i5-1235U / Intel Iris Xe (developer assessment) | 60 FPS reported on Core i5-1235U / Iris Xe in the development assessment. Unselected menu text can disappear. This earlier result is not a b705 benchmark or whole-game completion claim. |
| Dead Cells: Return to Castlevania Edition / PPSA15552 | Development assessment dated 2026-11-01 / 7377348 | In-game | Core i5-1235U / Intel Iris Xe (developer assessment) | Sound reported; assessment says menu-layer flicker improved after presentation wait handling. No final-package run yet. |
| KAGE: Shadow of The Ninja / PPSA20450 | Development assessment dated 2026-11-01 / 7377348 | In-game: Stage 1 | Core i5-1235U / Intel Iris Xe (developer assessment) | Triangular black area remains. Public b571 run reached the PSN ranking prompt; older gameplay records retain their original builds. |
| Record of Lodoss War: Deedlit in Wonder Labyrinth / Title ID pending record | Development assessment dated 2026-11-01 / 7377348 | In-game | Core i5-1235U / Intel Iris Xe (developer assessment) | Developer-reported in-game progress; full-game stability is outside this record. |
| Arkanoid: Eternal Battle / PPSA06328 | Development assessment dated 2026-11-01 / 7377348 | In-game: RETRO mode | Core i5-1235U / Intel Iris Xe (developer assessment) | RETRO mode only; other modes are outside this record. |
| Rugrats: Adventures in Gameland / PPSA23396 | Development assessment dated 2026-11-01 / 7377348 | In-game | Core i5-1235U / Intel Iris Xe (developer assessment) | Earlier public record was the Start Game screen on b484. The newer developer assessment reports in-game progress. |
| void tRrLM(); //Void Terrarium / Title ID pending record | Development assessment dated 2026-11-01 / 7377348 | In-game | Core i5-1235U / Intel Iris Xe (developer assessment) | In-game progress with sound reported in the dated development assessment. |
| Dragon Ball: Sparking! ZERO / PPSA15211 | Development assessment dated 2026-11-01 / 7377348 | In-game: combat | Core i5-1235U / Intel Iris Xe (developer assessment) | Dark image remains. Earlier RTX opening/QTE results are a separate hardware profile. |
| No Man's Sky / PPSA02110 | RTX development record 2026-10-28; final per-run build not supplied | Planet and inventory in a development run | RTX 5060 Ti | Planet rendered with very low performance and texture/lighting defects. Separate Intel assessment dated 01-Nov covers menus/mode selection/galaxy sequence; preserve both profiles. |
| Little Nightmares Enhanced Edition / Title ID pending record | Development assessment dated 2026-11-01 / 7377348 | Main menu | Core i5-1235U / Intel Iris Xe (developer assessment) | Ray-tracing paths remain experimental opt-ins. This menu record is distinct from the older original Little Nightmares logo record. |
| Wizard with a Gun / PPSA06416 | Development assessment dated 2026-11-01 / 7377348 | Main menu | Core i5-1235U / Intel Iris Xe (developer assessment) | First frame has blue/red blocks; no gameplay claim. |
| Dying Light: The Beast / PPSA24002 | Development assessment dated 2026-11-01 / 7377348 | Title / loading after local account handling | Core i5-1235U / Intel Iris Xe (developer assessment) | Local account emulation is not a real PSN login. Loading beyond title does not prove gameplay. Final video-event integration needs regression checks. |
| LEGO Bricktales / PPSA07430 | Development assessment dated 2026-11-01 / 7377348 | Menu | Core i5-1235U / Intel Iris Xe (developer assessment) | Developer assessment reports a 4K output. No final-package gameplay claim. |
| The Smurfs / PPSA02876 (earlier public record) | Development assessment dated 2026-11-01 / 7377348 | Menu | Core i5-1235U / Intel Iris Xe (developer assessment) | Localized release/title naming should be confirmed against the tested copy. |
| Resident Evil 2 / Title ID pending record | Development assessment 2026-11-01; cinematic report 2026-10-24 | Menus / New Game / cinematic | Core i5-1235U / Intel Iris Xe (developer assessment) | Earlier cinematic report had a black 3D scene. New assessment supplies no stronger gameplay result. |
| Peppa Pig / PPSA03706 | b571 change record / development assessment 2026-11-01 | Menus and opening videos | Core i5-1235U / Intel Iris Xe (developer assessment) | Videos reported to play; exact latest run build is not supplied. |
| Super Bomberman R 2 / Title ID pending record | Earlier b117 fix; development assessment 2026-11-01 | Opening; passed PSN prompt | Core i5-1235U / Intel Iris Xe (developer assessment) | Assessment retains opening progress; do not infer a new gameplay test. |
| Little Nightmares II / Title ID pending record | Development assessment dated 2026-11-01 / 7377348 | Opening logos | Core i5-1235U / Intel Iris Xe (developer assessment) | Black screen after logos; no gameplay result. |
| Ghost of Yōtei / PPSA26344 | b498 local RTX test, 2026-10-23; newer Intel assessment 2026-11-01 | Splash / partial calibration images | RTX 5060 Ti / Core i5-1235U / Intel Iris Xe (developer assessment) | RTX record showed partial bonus/calibration screens without scenery; Intel assessment lists initial splash. Keep the hardware outcomes distinct. |
| God of War Sons of Sparta / Title ID pending record | Development assessment dated 2026-11-01 / 7377348 | Opening: Sony presents | Core i5-1235U / Intel Iris Xe (developer assessment) | Stops during the opening. |
| Gothic 1 / PPSA09826 | Development assessment dated 2026-11-01 / 7377348 | Startup stopped | Core i5-1235U / Intel Iris Xe (developer assessment) | Tested dump reported Global shader library missing; this incomplete-data case does not establish a failure with a complete game copy. |
| Diablo II: Resurrected / PPSA03912 | Visual b542, 2026-10-26; execution/normal-exit b555, 2026-10-27 | Title / incomplete class selection | RTX 5060 Ti | Preserved historical result. Missing characters/scenery and memory pressure in the visual run; b555 log alone is not newer visual proof. |
| Little Nightmares (original record) / Title ID pending record | Development b484, 2026-10-20 | Unreal logo | Core i5-1235U / Intel Iris Xe (developer assessment) | Preserved historical logo record; do not replace it with the separate Enhanced Edition assessment. |
| Cloudberry Kingdom (PS4) / CUSA57754 | Development assessment dated 2026-11-01 / 7377348 | libc initialization; graphics unavailable | Core i5-1235U / Intel Iris Xe (developer assessment) | PS4 execution is early; Gnm functions remain missing in this record. |

## Help expand the list

Try a recent build and share the emulator build, game version, hardware, settings and scene reached. Screenshots and short videos help compare results. Review exported logs for personal information before sharing.

Records combine dated developer assessments and isolated test runs. Intel Iris Xe and RTX results remain separate. Experimental profiles, private resolution experiments and optional performance modes are identified in their notes; their results do not establish default-package performance.

[Download b786 and read the technical changelog](https://github.com/zeraps5/ZeraPS5-Releases/releases/tag/v0.1.0-b786) · [Back to README](README.md)


## External-drive startup survey — 2026-11-06, build 837

Hardware: Ryzen 7 5700G, RTX 5060 Ti 16 GB, Windows. Exact source: `913a5ec`. 28 short sessions, normally 90 s with selected extensions. Capture and staging work were enabled: this was not an FPS comparison. Original saves were preserved. Several windows were manually closed, so normal code-0 closures are not classified as crashes. A title, video, preview or attract scene is not a gameplay confirmation. Short new sessions do not replace earlier verified gameplay records.

| Game | ID | Version | Stage observed | Observation |
|---|---|---|---|---|
| Shadow of the Ninja | PPSA20450 | 01.000.000 | Title | Opening animation and PRESS ANY BUTTON. Gameplay not tested. |
| Planet of Lana | PPSA19450 | 01.010.000 | Loading | Loading icon, then black frames during the 90 s window. Earlier gameplay record is not invalidated by this short load test. |
| Atari 50: The Anniversary Celebration | PPSA08601 | 01.009.000 | Gallery/menu | Pong cabinet and text rendered in the gallery. Minigame controls not tested. |
| Goosebumps: Terror in Little Creek | PPSA27305 | 01.000.002 | Loading | Publisher logos and street/loading screen. No gameplay in 182 s. |
| PROJECT ZERO: MAIDEN OF BLACK WATER | PPSA04333 | 01.000.005 | In-game tutorial | 3D prologue scene with Miu and a movement tutorial. Scene is dark; analog movement was not verified in this session. |
| Tomb Raider IV-VI Remastered | PPSA18154 | 01.000.005 | License agreement | Agreement displayed; not accepted automatically. Gameplay pending user acceptance. |
| Resident Evil 4 | PPSA07411 | 01.000.000 | Initialization fault | Memory access fault in PlayGo thread, libc+0x479b, after about 7 s. Root cause not established. |
| TMNT: Splintered Fate | PPSA25582 | 01.017.000 | Terms / UI defect | Terms panel displayed, displaced/cropped at the lower right. Agreement not accepted automatically. |
| Arcade Game Zone | PPSA19015 | 01.003.000 | Rendered scene | Air-hockey court, paddles, puck and score rendered. May be attract/demo mode; player control not verified. |
| The Smurfs: Mission Vileaf | PPSA02876 | 01.000.001 | Opening | Splash/title artwork. Menu and gameplay not reached in this session. |
| Grand Theft Auto: San Andreas – The Definitive Edition | PPSA03524 | 01.003.000 | Opening | San Andreas logo displayed. No gameplay in this session. |
| SONIC FRONTIERS | PPSA03831 | 01.041.000 | Opening cutscene | 3D Eggman cutscene and Skip prompt displayed. Gameplay not tested. |
| Diablo® II: Resurrected™ | PPSA03912 | 01.039.000 | Title | Title screen with fire colors and Press X to start. Gameplay not tested. |
| Trifox | PPSA06408 | 01.002.000 | Opening, then black | FMOD logo followed by black frames until the 90 s limit. No spontaneous crash recorded. |
| SUPER BOMBERMAN R 2 | PPSA07190 | 01.003.001 | Initial settings | Language/region settings displayed. Navigation attempt did not advance; gameplay not verified. |
| Tetris® Effect: Connected | PPSA07923 | 02.000.021 | Initial warning | Photosensitivity warning displayed. Menu/gameplay not verified. |
| Alone in the Dark | PPSA08240 | 01.000.000 | Loading | Dark mansion/loading screen. No gameplay within 182 s. |
| Double Dragon Gaiden: Rise of the Dragons | PPSA16018 | 01.000.013 | Title | Double Dragon Gaiden title and credit counter displayed. Gameplay not tested. |
| Jurassic Park Classic Games Collection | PPSA17217 | 01.000.000 | Game selection | Jurassic Park 8-bit selection and preview video displayed. Preview is not proof that the minigame was started. |
| TMNT: Mutants Unleashed | PPSA17245 | 01.000.000 | Opening | Last captured image was Unity. Logs show additional rendering during the final seconds; no documented gameplay in 91 s. |
| Hello Kitty Island Adventure | PPSA18176 | 02.000.000 | Main menu | New Game / save-slot selection displayed after Cross. Window closed with exit code 0; session interrupted before starting a game. |
| Teenage Mutant Ninja Turtles Arcade: Wrath of the Mutants | PPSA18571 | 01.000.002 | Title | Warning followed by Press X to start. Window closed with code 0; gameplay not tested. |
| Alone in the Dark 2 (1996) | PPSA24515 | 01.000.003 | Controls / UI defect | Controller/START screen displayed with poorly legible or missing elements. Window closed with code 0 in 35 s; gameplay not tested. |
| Ghost of Yōtei | PPSA26344 | 01.512.000 | Vulkan failure | No image presented; VK_ERROR_DEVICE_LOST while waiting for a batch fence, exit code 1 after 13 s. |
| BUBBLE BOBBLE SUGAR DUNGEONS | PPSA26356 | 01.002.000 | Autosave notice | Autosave notice and Yes button displayed. Window closed with code 0 in 33 s; gameplay not tested. |
| SILENT HILL: Townfall | PPSA33286 | 02.003.000 | Opening | SCREEN logo displayed after a long initial pipeline/resource load. No menu/gameplay in 152 s. |
| Sonic Unleashed Recompiled | PPSA99902 | 01.003.000 | Guest abort | Homebrew/recompiled port. Calls abort and executes UD2 at eboot+0x33c17d9; exits with code 3 after 5 s. This is not proof of a missing host CPU instruction. |
| SpongeBob SquarePants: Titans of the Tide | PPSA26893 | not read from image | Autosave notice / loading | Read directly from the exFAT image. Portuguese autosave notice displayed; process ended with code 0 during resource loading at 127 s. Gameplay and exact closure cause not established. |

Three PKG-only folders were excluded by choice. The survey used 27 extracted game folders and one exFAT image. Sonic Unleashed Recompiled is a homebrew/recompiled port. No license agreement was accepted automatically.
