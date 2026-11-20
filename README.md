# ZeraPS5

<p>
  <img src="docs/zeraps5-logo.png" alt="ZeraPS5" width="240" align="middle" />
  &nbsp;
  <img src="docs/zeraps5-mascot.png" alt="ZeraPS5 capybara mascot" width="64" align="middle" />
</p>

[![Windows application downloads (latest release)](docs/downloads.svg)](https://github.com/zeraps5/ZeraPS5-Releases/releases/latest)

*Windows application ZIP downloads for the latest release, including updates and tests; not unique users. Updated automatically on an hourly schedule and after a release. GitHub may delay refreshes or cache the image.*

**[Download the latest Windows build](https://github.com/zeraps5/ZeraPS5-Releases/releases/latest)** · **[Browse source](src/)** · **[Build and contribute](CONTRIBUTING.md)** · **[Game compatibility](COMPATIBILITY.md)**

## Download and run

**Current release: v0.1.1-b1080.**

**Vulkan and runtime compatibility, plus direct PFS image loading.** The [b1080 release notes](https://github.com/zeraps5/ZeraPS5-Releases/releases/tag/v0.1.1-b1080) cover Longshot shader and startup changes, R-Type Delta HD audio and input fixes, and read-only loading of supported .ffpfsc images.

**Earlier recorded gameplay:** Sonic Origins Plus reaches Green Hill Zone on Intel Iris Xe (about 23 FPS at 1080p; 35–38 FPS in the dated 720p internal-resolution follow-up). Sonic Superstars reaches Bridge Island (about 15–16 FPS on Iris Xe). Quake II reaches Outer Base with movement and firing (31–53 FPS, median 43 in the final active window on RTX 5060 Ti, b981). These are measured development scenes, not universal performance promises; the compatibility list retains each actual tested revision and configuration.

- [Latest release, downloads, and release notes](https://github.com/zeraps5/ZeraPS5-Releases/releases/latest)
- [Download for Windows x64 — build 1080](https://github.com/zeraps5/ZeraPS5-Releases/releases/download/v0.1.1-b1080/ZeraPS5-v0.1.1-b1080-win64.zip)
- [Download matching source — build 1080](https://github.com/zeraps5/ZeraPS5-Releases/releases/download/v0.1.1-b1080/ZeraPS5-v0.1.1-b1080-win64-source.zip)

1. Download the application ZIP above, rather than GitHub’s automatic source archive.
2. Extract the ZIP to a short, writable folder (for example, `C:\ZeraPS5`) and open **ZeraPS5.exe**. Very long paths can exceed Windows extraction limits during updates.
3. Read the included instructions, then add your own games and required console libraries.

## Game compatibility

**b1080 passed 190 automated Windows checks, including 32 Vulkan GPU checks.** The [release notes](https://github.com/zeraps5/ZeraPS5-Releases/releases/tag/v0.1.1-b1080) describe the package validation. Game records retain their tested revisions.

[Game compatibility and tested builds](COMPATIBILITY.md) — reviewed 15 November 2026, with dated Intel/RTX development and candidate results, each title's actual tested revision, and the preserved startup survey/history.

[Technical changelog](CHANGELOG.md) · [Contribute or report a result](CONTRIBUTING.md) · [Matching source revision and checksums](SOURCE-VERSION.txt)

Public distribution maintained by **Metallrock (metallrockdev)**.

The default branch includes the b1080 emulator source and tests. The [release tag](https://github.com/zeraps5/ZeraPS5-Releases/tree/v0.1.1-b1080) identifies the exact packaged revision. The b984 source import includes the [source changelog](docs/source-changelog-b984.md). Releases published before the source import retain their original tags. For the exact source of a published build, use its versioned `*-source.zip` and the `sources/` materials in the Windows package.

## Experimental software

ZeraPS5 is experimental. Compatibility, stability, and FPS are not guaranteed.
Games, firmware, keys, and console modules are not included in the packages.
Read `LEIA-ME.txt`, `docs/LIMITACOES.txt`, and `docs/RECOMPILAR.txt` inside the ZIP.

## Licensing and credits

See [LICENSE](LICENSE) and [CREDITS.txt](CREDITS.txt). Packages include component licenses,
corresponding source code, FFmpeg sources, and object files for relinking.
Private development is kept separate from this distribution repository.
ZeraPS5 is not affiliated with Sony.
