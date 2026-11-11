# Release changelog

## v0.1.0-b898 — 11 November 2026

Current Windows x64 release. Runtime base b897 (`a1e7371`), distribution integration `7f993bd`.

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
