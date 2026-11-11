# ZeraPS5 — experimental releases

<p>
  <img src="docs/zeraps5-logo.png" alt="ZeraPS5" width="240" align="middle" />
  &nbsp;
  <img src="docs/zeraps5-mascot.png" alt="ZeraPS5 capybara mascot" width="64" align="middle" />
</p>

[![Windows application downloads (latest release)](docs/downloads.svg)](https://github.com/zeraps5/ZeraPS5-Releases/releases/latest)

*Windows application ZIP downloads for the latest release, including updates and tests; not unique users. Updated automatically on an hourly schedule and after a release. GitHub may delay refreshes or cache the image.*

## Download and run

**Current release: v0.1.0-b898.**

**Direct PKG loading, CPU/GPU optimizations, graphics fixes and more launcher options.** See the [b898 technical release notes and latest game tests](https://github.com/zeraps5/ZeraPS5-Releases/releases/tag/v0.1.0-b898).

Highlights included in this release:

- Discover and open supported PS5 fPKG games directly, with package-aware PlayGo handling.
- Reduce CPU/GPU synchronization work, improve vertex-buffer copies and lower shader-evaluation overhead.
- Correct DCC fast-clear colors, texture sampling and accented text rendering.
- Add play-time tracking, screen modes, mod-folder selection, desktop shortcuts and internal-resolution settings for supported Unreal Engine games.
- Keep startup video and fonts embedded, with the public b838-to-b898 launcher update tested after publication.

**Recorded gameplay highlights:** Dolphin Spirit Ocean Mission reached underwater gameplay at approximately **30–56 FPS on an RTX 5060 Ti** (tested on b897). Alone in the Dark 2, the PS1 classic, reached movement and combat at approximately **60 FPS on Intel Iris Xe** (tested on b861). These figures describe the recorded scenes and hardware. Each compatibility entry retains its actual tested build.

- [Latest release, downloads, and release notes](https://github.com/zeraps5/ZeraPS5-Releases/releases/latest)
- [Download for Windows x64 — build 898](https://github.com/zeraps5/ZeraPS5-Releases/releases/download/v0.1.0-b898/ZeraPS5-v0.1.0-b898-win64.zip)
- [Download source code — build 898](https://github.com/zeraps5/ZeraPS5-Releases/releases/download/v0.1.0-b898/ZeraPS5-v0.1.0-b898-win64-source.zip)

1. Download the application ZIP above, rather than GitHub’s automatic source archive.
2. Extract the ZIP to a short, writable folder (for example, `C:\ZeraPS5`) and open **ZeraPS5.exe**. Very long paths can exceed Windows extraction limits during updates.
3. Read the included instructions, then add your own games and required console libraries.

## Game compatibility

[Game compatibility and tested builds](COMPATIBILITY.md) — see the latest recorded test for each game, the result reached, and earlier progress where relevant.

Public distribution maintained by **Metallrock (metallrockdev)**.

This repository hosts packaged releases and their corresponding source code.
GitHub’s automatically generated “Source code” archives do not replace the versioned
`*-source.zip` asset or the `sources/` materials included in the application package.

## Experimental software

ZeraPS5 is experimental. Compatibility, stability, and FPS are not guaranteed.
Games, firmware, keys, and console modules are not included in the packages.
Read `LEIA-ME.txt`, `docs/LIMITACOES.txt`, and `docs/RECOMPILAR.txt` inside the ZIP.

## Licensing and credits

See [LICENSE](LICENSE) and [CREDITS.txt](CREDITS.txt). Packages include component licenses,
corresponding source code, FFmpeg sources, and object files for relinking.
Private development is kept separate from this distribution repository.
ZeraPS5 is not affiliated with Sony.

