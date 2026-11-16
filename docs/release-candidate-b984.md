# ZeraPS5 0.1.1 b984 — release review

**Published 16 November 2026 UTC. [Download b984](https://github.com/zeraps5/ZeraPS5-Releases/releases/tag/v0.1.1-b984). Game observations and pre-publication validation were reviewed on 15 November 2026.**

**Modern Vulkan paths with fallbacks, unified internal-resolution controls, older-CPU instruction compatibility and native memory/thread safety fixes.**

Experimental Windows x64 release, maintained by **metallrockdev**. Build **984**, source **0d269bb8de1cdc4dce31f4d689d8fef0d675d700**. Contributors' credits and component licenses are preserved.

## Changes since b898

- **Vulkan capabilities and fallbacks.** Request Vulkan 1.4 when the loader and device support it. Use dynamic rendering, additional dynamic state, push descriptors and timeline semaphores where available; retain the earlier fallback paths. Vulkan 1.4 is not a universal hardware requirement.
- **Shader and transfer work.** Reuse `s_getpc_b64` shader translations when load addresses change. Use SSE2 and fenced non-temporal stores for large CPU fills. Reduce the Videodec2 compute-queue reservation from 16 MiB to 1 MiB.
- **Memory and thread correctness.** Fix contiguous/partial direct mappings, concurrent writes to neighboring pages, host physical-window teardown and HLE ownership when fibers migrate between threads. These fixes have native reproductions and tests.
- **Unified internal resolution.** Offer Game default, 1440p, 1080p, 900p, 720p and 540p globally or per game for supported Unreal titles, Sonic Origins Plus and TMNT: Splintered Fate. An explicit advanced Unreal scale takes precedence. This is not a universal resolution scaler.
- **Older CPU compatibility and F10 diagnostics.** Automatically replace supported guest instructions unavailable on pre-AVX2 hosts, with Automatic/Always/Off controls. The host executables do not require AVX2. Show compact CPU, OS, RAM, commit and GPU-memory diagnostics; retain short F10 presses between slow frames.
- **Game and system handling.** Correct Sonic Origins Plus save-slot/display handling, audio configuration, callbacks, pipes/poll and pthread cancellation. Add optional native mouse/keyboard input, disabled by default. The native keyboard path requires a physical controller.
- **Homebrew support.** Integrate Quake III companion-library/runtime work and guarded in-memory Quake compatibility patches. Experimental LAN transport does not establish PSN or external multiplayer compatibility.
- **15 interface languages.** Include translated diagnostics, update/recovery text, matching source, FFmpeg source, relink objects, credits and licenses.

## New development compatibility records

These results keep their actual tested revision and configuration; they are not relabeled as fresh benchmarks of the final b984 ZIP.

| Game | Hardware / record | Observed result |
| --- | --- | --- |
| Sonic Origins Plus | Core i5-1235U / Intel Iris Xe, 15 November developer record; 1080p revision `71db681` | Sonic 1 Anniversary Mode, Green Hill Zone: about 23 FPS at 1080p and 35–38 FPS with 720p internal resolution in the dated developer follow-up. The exact per-run 720p revision was not recorded. |
| Sonic Superstars | Core i5-1235U / Intel Iris Xe, `9bb4367`, 15 November | Bridge Island with HUD and scenery, about 15–16 FPS; menu about 60 FPS. |
| Quake II | Ryzen 7 5700G / RTX 5060 Ti 16 GB, b981 / `e2feb98`, 15 November | Outer Base save with HUD, movement, camera and firing. About 31–53 FPS, median 43 in the final active observation window of the 361-second run. |
| Quake III Arena homebrew | PS5_Quake3e v1.4.0; earlier experimental b906 / `6c44020` + local changes, RTX, 12 November | Menu and a local q3dm1 match with one human client and scripted movement/camera/firing. Bots, split screen, audible output, external multiplayer and long sessions remain outside the test. |
| Commandos: Origins | Intel Iris Xe, dated 3 November development-branch record | Opening video with sound, language/main menus and first-mission loading. Branch fixes are included; first-mission gameplay was not reached in that record. |

## Seven-game RTX candidate check

Ryzen 7 5700G, RTX 5060 Ti 16 GB, 32 GB RAM, Windows 11. One emulator at a time, 1080p output, Game default internal resolution, isolated profiles/caches and hash-checked save copies. b974 / `675cb09` was used for the first three runs; b981 / `e2feb98` for the others. The final resolution integration preserves the default path used in those sessions; its explicit resolution choices were not benchmarked across these seven games.

| Game | Tested build / duration | Observed result |
| --- | --- | --- |
| Minecraft | b974 / 601 s | World, movement/camera and day/night. Settled windows around 9 FPS, also observed in the public b898 comparison; no measured percentage gain. |
| Grand Theft Auto V | b974 / 842 s | Bank prologue, characters/radar and forward movement; colors preserved in the examined section. Around 3–5 FPS. |
| Red Dead Redemption | b974 / 902 s | Opening cinematic with city, NPCs and train. Translucent NPCs also occur in b898. Player-controlled gameplay was not established. |
| Skyrim | b981 / 362 s | Cart introduction with responding camera and opaque characters. Median around 6 FPS in the recorded later window; vegetation stippling remains. |
| Quake II | b981 / 361 s | Outer Base gameplay with movement, camera and firing; 31–53 FPS in the final active window. |
| Marvel's Wolverine | b981 / 240 s | Animated opening/Marvel logo, then black output. The b898 comparison has the same observed limitation. |
| ASTRO BOT | b981 / 302 s | No useful scene. The b898 comparison also has a broken opening; this does not certify gameplay or complete graphical equivalence. |

All seven corrected setups ended at their controlled limits with exit code zero and no logged ERROR/FATAL. A zero exit code is not a gameplay result. The stripped distributed b984 executable separately passed a 211-second Quake II opening/demo smoke test. There was no new physical notebook/older-CPU test of this ZIP and no whole-game or universal FPS-gain certification.

## Validation and update test

- **182/182 CTest checks**, including **28 GPU checks with Vulkan validation**, repeated after final code integration; no VUID messages recorded.
- **28/28 GPU fallback checks**, with the new paths explicitly disabled on the same RTX. This does not simulate a physical older GPU.
- **52/52 distribution/update checks**, including 15 languages, hashes, rollback, interrupted installation and user-data preservation.
- The **original public b898 launcher** downloaded the actual b984 ZIP from an isolated loopback HTTP server, verified SHA-256, installed it and automatically restarted as b984. A second check reported the current version. All **48 payload files**, five synthetic save/settings/game/module markers, previous executable backups and the committed journal were verified. Real-payload rollback and interrupted-install recovery also passed.
- **Public update verification:** The original public b898 launcher downloaded b984 directly from GitHub using its built-in production endpoint and installed the verified ZIP. The updated launcher opened normally and a second check reported the latest version. All 48 payload files, five synthetic user-data markers, previous executable backups and the committed journal were verified in an isolated installation.
- The six published assets match GitHub's SHA-256 digests and sizes. All 528 packaged emulator source files were checked against the recorded Git content, allowing the documented Windows CRLF conversions. Source, relink objects, FFmpeg, licenses, icons and embedded font/video were checked.
- The internal-resolution choices are visible in the portable launcher. Saving/reopening that modal was not completed by UI automation; no successful save round-trip is claimed.

A first public-update attempt encountered a Windows file-sharing violation (`0x80070020`). The updater rolled back; all 50 baseline files and five synthetic user-data markers were independently verified restored. The repeated installation passed. This records the observed interruption and recovery; the process responsible for the lock was not identified.

## Current limitations

Copy-on-write tracking remains experimental and disabled by default after a concurrent-write reproduction. Disabling its automatic activation does not fix that experimental algorithm. TLOU-specific diagnostic/shader experiments are excluded from this release. The Last of Us Part I and Diablo II have no confirmed gameplay record here. ASTRO BOT and Wolverine are not presented as playable. Some package/exFAT entries can still appear twice. Graphical defects, slowdowns, high memory use and crashes remain possible.

## Installation and source

Verify `SHA256SUMS`, extract the Windows ZIP into a writable folder and run `ZeraPS5.exe`. The versioned `*-source.zip` and the same archive inside `sources/` contain the matching emulator source. FFmpeg 7.1.2 source and relink objects are included. GitHub's automatic Source code archives represent this distribution repository.

Games, personal saves, console libraries, firmware, keys and credentials are not included. Reports remain manual by default. Executables are unsigned. ZeraPS5 is not affiliated with Sony.

**Windows ZIP SHA-256:** `46b513f07ddb78c3def09a3e80038f50468968ee122db2b1922b7ed2c8171eb0`  
**Source ZIP SHA-256:** `814bbadf56ab3064d279076df90d69823e5730ff8b43aa24dc66c6675ed7ec81`
