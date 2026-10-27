# Game compatibility and tested builds

See what has been reached in ZeraPS5, which build was tested, and the conditions of each recorded run.

Results apply to the listed build, hardware, and settings. When a newer run covered only startup, previously demonstrated gameplay is shown with its own build. Results may differ between versions.

Last assessment update: **2026-10-27**.

## Games with test records

| Game / title ID | Latest recorded test | Result of that test | Previously demonstrated progress | Notes and recorded hardware |
| --- | --- | --- | --- | --- |
| **Minecraft**<br>PPSA17221 | b554 · 2026-10-27 | Startup and AGC libraries | In-game on b521 · 2026-10-26: world rendering, camera and small controlled movements. | The b554 run covered startup and normal exit. The gameplay record belongs to b521. Roughly 10–11 FPS at the original view distance; 38–44 FPS used a reduced distance.<br>**Hardware:** Iris Xe in the earlier Claude report; RTX 5060 Ti in the later Codex records |
| **Sonic Frontiers**<br>PPSA03831 | b498 (local changes) · 2026-10-22 | In-game: Cyber Space | Movement and ring collection documented on b498. | Roughly 7–9 FPS, compilation pauses and color fringes. The record covers part of a stage.<br>**Hardware:** RTX 5060 Ti; driver not recorded for this session |
| **KAGE: Shadow of The Ninja**<br>PPSA20450 | b484 · 2026-10-20 | Scene recorded without input | b339 · 2026-10-07: Stage 1, blue ninja, jump animation, HUD and pause. | The more recent run may show an automatic demo. The initial-stage record is from 2026-10-07 on GTX 1050 Ti.<br>**Hardware:** Iris Xe for the 2026-10-20 report; GTX 1050 Ti for the 2026-10-07 record |
| **Dragon Ball: Sparking! ZERO**<br>PPSA15211 | Development bisection on Iris Xe · 2026-10-26; final build label not recorded | Combat and regression reported on Iris Xe | Development snapshot: scripted fight at roughly 2 FPS (Claude). RTX: opening/QTE on b498 on 2026-10-23. | Keep hardware profiles separate: Intel visual defects and lighting regression; RTX purple/black introduction. The final bisection build was not identified in the supplied assessment.<br>**Hardware:** Core i5-1235U / Iris Xe; RTX 5060 Ti in a separate run |
| **Grand Theft Auto V**<br>PPSA04263 | b524 (local changes) · 2026-10-26 | Menus; flickering recorded | b498 · 2026-10-22: bank prologue, characters and HUD. | The recent test covered menus. The prologue record belongs to an earlier build; missing-resource warnings and freezes were reported.<br>**Hardware:** RTX 5060 Ti 16 GB; driver 617.42 recorded only for the 2026-10-22 run |
| **The Elder Scrolls V: Skyrim**<br>PPSA03747 | b521 (local changes) · 2026-10-26 | Cart introduction | New Game, characters, geometry and the introduction route rendered. | Low performance; measurements varied with the scene and experimental settings. The documented segment is the introduction.<br>**Hardware:** RTX 5060 Ti |
| **Diablo II: Resurrected**<br>PPSA03912 | b555 · 2026-10-27 | Normal exit recorded in the log | b542 · 2026-10-26: menus and incomplete class selection. | The latest documented visual outcome is from b542: title/class selection returned, with missing characters/scenery and memory pressure. The b555 record covers execution and exit.<br>**Hardware:** RTX 5060 Ti |
| **ASTRO BOT**<br>PPSA21567 | b498 / BVH candidate · 2026-10-22 | Startup; BVH run limited by memory | Baseline showed a black background and lines; the ray-tracing candidate was evaluated separately. | The supervisor stopped the BVH candidate after 2.30 seconds due to memory headroom. The run needs continuation to evaluate the next segment.<br>**Hardware:** RTX 5060 Ti |
| **Dead Cells**<br>PPSA15552 | b484 · 2026-10-20 | Main menu | Claude record on Iris Xe. | The documented stage is main menu. An additional session can extend the result.<br>**Hardware:** Core i5-1235U / Iris Xe |
| **Ghost of Yōtei**<br>PPSA26344 | b498 (local changes) · 2026-10-23 | Partial menus/calibration | Bonus/calibration images appeared after the earlier loading-only tests. | Scenery was missing in the recorded result; several profiles used experimental features.<br>**Hardware:** RTX 5060 Ti |
| **Joe & Mac: Caveman Ninja**<br>PPSA02801 | Development snapshot · 2026-10-26; build number not recorded | Title screen | Title screen also recorded on 2026-10-24 on an earlier development snapshot. | An HTILE regression was fixed in a development revision. The stage described by Claude is the title screen.<br>**Hardware:** Core i5-1235U / Iris Xe |
| **Little Nightmares**<br>Title ID to be recorded | b484 · 2026-10-20 | Unreal logo | Claude record on Iris Xe. | The documented stage is unreal logo. Ray-tracing shaders were unsupported in that run.<br>**Hardware:** Core i5-1235U / Iris Xe |
| **Little Nightmares II**<br>Title ID to be recorded | Development snapshot · 2026-10-21; build number not recorded | Bandai Namco logo | Run g_ln2 reported by Claude on Iris Xe. | The record covers the opening logo; the exact executable still needs to be matched to the run.<br>**Hardware:** Core i5-1235U / Iris Xe |
| **No Man’s Sky**<br>PPSA02110 | b498 (local changes) · 2026-10-22 | Logos/loading; USB reads interrupted | Earlier Claude report on development revision: menus and the initial galaxy sequence. | USB resets affected the later run. Distinguish the initial galaxy sequence from effective control on a planet.<br>**Hardware:** RTX 5060 Ti in the USB-affected run |
| **The Smurfs (localized title: Os Smurfs Missão Florrorosa)**<br>PPSA02876 | b310 (local changes) · 2026-10-07 | Startup and exit recorded in the log | A run was recorded on GTX 1050 Ti. | The available material is a log. A new capture would help record the visual stage reached.<br>**Hardware:** GTX 1050 Ti |
| **Red Dead Redemption**<br>PPSA30528 | b538 (local changes) · 2026-10-26 | Menus and opening scenes | Scenes were also recorded on b537; some runs were automatic demos. | Visual dropouts persisted. The b538 run recorded Vulkan device loss at 191.998 seconds.<br>**Hardware:** RTX 5060 Ti |
| **Resident Evil 2**<br>Title ID to be recorded | Development snapshot · 2026-10-24; build number not recorded | Menus, New Game and cinematic | Subtitled cinematic reported in runs re2_d/re2_long. | The 3D scene was black. Claude reported the run on Iris Xe; folder version FR 01.000.003.<br>**Hardware:** Core i5-1235U / Iris Xe |
| **Rugrats: Adventures in Gameland**<br>PPSA23396 | b484 · 2026-10-20 | Start Game screen | Claude record on Iris Xe. | The documented stage is start game screen. An additional session can extend the result.<br>**Hardware:** Core i5-1235U / Iris Xe |
| **void tRrLM(); //Void Terrarium**<br>Title ID to be recorded | b484 · 2026-10-20 | New Game menu | Claude record on Iris Xe. | The documented stage is new game menu. An additional session can extend the result.<br>**Hardware:** Core i5-1235U / Iris Xe |

“In-game” describes the segment reached; it does not imply a completed game or sustained performance. A normal exit or startup log describes execution, not a visual gameplay result.

## Help expand the list

Try a recent build and share the game version, ZeraPS5 build, hardware, settings, stage reached, and any remaining issues. Screenshots or a short video help compare results. Please review logs for personal information before sharing them.

First detailed reports are welcome for **Arkanoid**, **Lodoss** (full title to be confirmed), and **LEGO Bricktales**.

## Reading the build labels

- Build numbers refer to the emulator used for the recorded run, not the game version.
- Some tests used development builds with local changes. Where a build number was not recorded, the snapshot date is retained instead of inventing a version.
- Reported hardware and performance figures belong to that run. Vulkan API version is not the same field as the vendor driver version.
- This list combines existing Claude Code and Codex test records. Entries with different hardware or routes are kept separate in their notes.

[Download ZeraPS5](https://github.com/zeraps5/ZeraPS5-Releases/releases/latest) · [Back to README](README.md)
