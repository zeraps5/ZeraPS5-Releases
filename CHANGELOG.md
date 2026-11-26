# Release changelog

## v0.1.1-b1214 - 26 November 2026

- Simplifies Vulkan shader waterfall loops with one provable iteration per emulated lane.
- Adds a regression case for the optimization and checks that overlapping registers keep the loop intact.
- Grand Theft Auto VI reached the loading screen in a development test.

[Release assets and validation](https://github.com/zeraps5/ZeraPS5-Releases/releases/tag/v0.1.1-b1214).

## v0.1.1-b1192 - 25 November 2026

- Fixed duplicate PKG and exFAT listings for the same file. Mounted .pkg files appear once as PKG. Game copies in separate folders remain visible.


## v0.1.1-b1191 — 25 November 2026

- Vulkan presentation can locate the rendered image when it was registered through another mapping of the same memory.
- Compute batches flush a full writeback list before recording new stores. CPU writeback baselines come from the exact GPU upload snapshot.
- Compute pipelines require 32-lane subgroups only when their shaders depend on subgroup width. Supported Unreal profiles can be read from PKG and exFAT applications.
- Added PlayStation semaphore and Audio3D handling, extended AMM/AMPR memory and file handling, and added a PSML fallback outside PS5 Pro PSSR.
- The launcher displays the game and system library counts even when no system libraries are installed. It identifies required console modules when a game reports them missing.
- The Windows package includes source, licenses and relinkable objects. Console firmware and shaders extracted from it are excluded.

All 196 Windows checks passed. [Release assets and validation](https://github.com/zeraps5/ZeraPS5-Releases/releases/tag/v0.1.1-b1191).

## v0.1.1-b1152 — 24 November 2026 UTC

- **System libraries (HLE).** Added **125 function exports** across the emulated libraries, counted by library and symbol ID. This includes full or partial implementations and compatibility stubs. Highlights include **5 zlib exports** for decompression and result handling, **4 JPEG encoding exports**, **5 JSON2 exports**, and **5 AJM exports** for MP3 frame parsing and batched audio jobs. The kernel paths also gained flexible-memory release and page-table statistics.
- **Experimental PS4 compatibility.** Added **21 PS4 Gnm driver exports** within that HLE count. The graphics path now handles selected legacy GCN geometry shaders through an export-shader, geometry-shader, and copy-vertex-shader pipeline on Vulkan.
- **Camera support on Windows.** Added **7 `libSceCamera` exports**, covering device detection, opening, configuration, capture and frame delivery. The launcher can select a webcam through Windows Media Foundation or a compatible **PS4 Camera** through Sony's USB adapter. The PS4 Camera path can deliver separate feeds from its two lenses. A standard webcam supplies a single feed to both views. Some PS4 Camera setups require WinUSB and user-provided firmware.
- **Vulkan and shader translation.** Updated **18 graphics source files**, including **6 files in the shader translator**. Changes include relative VGPR addressing (`V_MOVREL`), scalar register analysis for relocation, and wave64 execution-mask handling. The Vulkan writeback path compares GPU results against the exact input snapshot to preserve unrelated CPU writes. Vertex buffers also receive tighter index-range bounds and snapshot handling.
- **Launcher and updates.** Added an Accessories panel with camera selection and shortcuts to keyboard, mouse and DualSense settings. The Windows updater now retries temporary file-sharing locks when replacing files.

The Windows test suite grew from 190 to 196 checks. The three new Vulkan cases cover vertex snapshots, shader execution masks, and relative register addressing. All 196 checks passed, including 35 GPU checks.

[Release assets and validation](https://github.com/zeraps5/ZeraPS5-Releases/releases/tag/v0.1.1-b1152).

## v0.1.1-b1080 — 20 November 2026 UTC

- **Vulkan and shader compatibility.** Includes fixes for RADV-generated shaders, texture modes 25 and 26, 8-bit index streams, sub-dword typed buffer loads, and render targets that share guest memory. GPU-only pixels can be restored from R32 to an RGBA8 view without changing their bytes.
- **Longshot.** Adds startup and memory support for the targeted build and shader fixes for the black patches, flicker and ghosting seen in development.
- **R-Type Delta HD.** Adds AJM control handling, an ATRAC9 fallback and a title-specific input timing fix.
- **Game images.** Adds direct, read-only access to supported .ffpfsc images, including nested PFS and PFSC-compressed data.
- **Runtime and services.** Improves memory pools, file I/O, thread cleanup, IME text handling, JPEG decoding and audio compatibility. Includes engine-side rendering and internal-resolution changes from development.

[Release assets and validation](https://github.com/zeraps5/ZeraPS5-Releases/releases/tag/v0.1.1-b1080).

## v0.1.1-b984 — 16 November 2026 UTC

Published Windows x64 release, source `0d269bb`. [Full release changes, dated game observations and validation](docs/release-candidate-b984.md).

- Modern Vulkan paths with capability checks and fallbacks: dynamic rendering/state, push descriptors and timeline semaphores.
- Reusable relocated shader translations, large-fill transfer optimizations and a smaller Videodec2 queue reservation.
- Native memory/thread safety fixes and optional older-CPU guest-instruction compatibility.
- Unified supported-game internal-resolution controls; compact host diagnostics with reliable short F10 presses.
- Sonic Origins Plus save-slot/display fixes, optional native input and experimental Quake homebrew/LAN work.
- New dated compatibility records for Sonic Origins Plus, Sonic Superstars, Quake II, Quake III homebrew and Commandos: Origins; seven RTX candidate checks retain b974/b981 labels.
- 182 runtime checks, 28 fallback GPU checks and 52 distribution checks passed. The actual public b898 launcher also downloaded and installed the frozen b984 payload over isolated loopback HTTP, restarted and reported current; all 48 payload files and synthetic user data were checked, including rollback/recovery.
- Six published assets match their reviewed digests. The original public b898 launcher downloaded b984 directly from GitHub using its built-in production endpoint and installed the verified ZIP. The updated launcher opened normally and a second check reported the latest version. All 48 payload files, five synthetic user-data markers, previous executable backups and the committed journal were verified in an isolated installation. Known game, modal-automation and experimental-COW limits are documented in the full review.

## v0.1.0-b898 — 11 November 2026

Earlier Windows x64 release. Runtime base b897 (`a1e7371`), distribution integration `7f993bd`.

- **Open supported PS5 fPKG games directly.** The launcher discovers .pkg files in game folders. Package-aware PlayGo handling reports the installation state from the package and verifies that each chunk's data is present.
- **Reduce CPU/GPU synchronization work.** Small command-processor writes, DCC-clear prediction and lock-free memory pinning reduce waits. Interleaved vertex buffers use one copy, and shader-evaluation caching costs less work.
- **Improve graphics and text.** Fix DCC fast-clear colors, anisotropic sampling, LOD bias, border colors and min/max reduction. Scan newly uploaded large textures so accented text can appear correctly. Handle null shader-call targets used by Elliot's color lookup table.
- **Expand launcher settings.** Add play-time tracking, window/fullscreen/maximized modes, mod-folder selection, desktop shortcuts, PS4 library entries and internal-resolution settings for supported Unreal Engine games.
- **Improve system emulation.** Update flexible-memory accounting, mprotect across gaps, x87 precision, WaitEqueue, 10-bit VideoOut formats, AGR command submission, ACM and IME parameters.
- **Extend experimental PS4 handling.** Add fPKG reading with user-provided keys, GCN 2D macro tiling, 38-bit addressing and SI texture-pitch rules. Keys are supplied by the user.
- **Improve TMNT Splintered Fate presentation.** Restore full-screen opening images and support a 1280×720 internal-resolution mode.
- **Make crash reports more informative.** Include the involved operating-system or graphics-driver module.
- **Complete the new interface and diagnostic text catalogs.** Add translations for new package, mod and shortcut messages, preserve numerical/path data, and synchronize the translated help in the 15 interface languages.
- **Keep font and startup video self-contained.** Restore the public distribution's embedded assets and their fallback paths, and keep the package compatible with the existing b838 installer.


### Compatibility and validation

- Updated [compatibility records](COMPATIBILITY.md), with actual tested revisions and separate Intel/RTX results.
- 145 CTest checks, including 12 native GPU fixtures, and 52 distribution/update checks passed.
- After publication, the public b838 launcher downloaded and installed b898 from GitHub and automatically restarted. All 47 installed payload files, five synthetic user-data markers and previous-program backups were checked in an isolated installation.
- Matching source, credits and component licenses remain included. Executables are unsigned.

See the [full b898 release notes](https://github.com/zeraps5/ZeraPS5-Releases/releases/tag/v0.1.0-b898) for detailed observations and known limitations.

## Earlier releases

- [b838 — 6 November 2026](https://github.com/zeraps5/ZeraPS5-Releases/releases/tag/v0.1.0-b838)
- [b786 — 3 November 2026 UTC](https://github.com/zeraps5/ZeraPS5-Releases/releases/tag/v0.1.0-b786)
- [b705 — 1 November 2026](https://github.com/zeraps5/ZeraPS5-Releases/releases/tag/v0.1.0-b705)
- [b571](https://github.com/zeraps5/ZeraPS5-Releases/releases/tag/v0.1.0-b571) · [b561](https://github.com/zeraps5/ZeraPS5-Releases/releases/tag/v0.1.0-b561) · [b555](https://github.com/zeraps5/ZeraPS5-Releases/releases/tag/v0.1.0-b555) · [b553](https://github.com/zeraps5/ZeraPS5-Releases/releases/tag/v0.1.0-b553)

Historical notes describe their own release. New compatibility observations are maintained in the current compatibility list and dated history.
