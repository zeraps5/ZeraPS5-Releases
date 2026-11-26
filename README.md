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

**Current release: v0.1.1-b1214.**

**b1214 shader compiler update.** Vulkan shader translation simplifies proven single-iteration waterfall loops. [Release notes](https://github.com/zeraps5/ZeraPS5-Releases/releases/tag/v0.1.1-b1214).

**KAGE: Shadow of the Ninja reached 60 FPS on a third-generation Intel Core i5 (Ivy Bridge)** in a project test reported on 24 November 2026. [Older CPU test and instruction compatibility](COMPATIBILITY.md#older-cpu-test).

**Latest recorded gameplay:** Quake reached the first map at approximately 33–37 FPS on Intel Iris Xe in a 23 November development session. The compatibility list gives the scope and hardware of every record, including earlier runs of Sonic Origins Plus, Sonic Superstars and Quake II.

- [Latest release, downloads, and release notes](https://github.com/zeraps5/ZeraPS5-Releases/releases/latest)
- [Download for Windows x64 — build 1214](https://github.com/zeraps5/ZeraPS5-Releases/releases/download/v0.1.1-b1214/ZeraPS5-v0.1.1-b1214-win64.zip)
- [Download matching source — build 1214](https://github.com/zeraps5/ZeraPS5-Releases/releases/download/v0.1.1-b1214/ZeraPS5-v0.1.1-b1214-win64-source.zip)

1. Download the application ZIP above, rather than GitHub’s automatic source archive.
2. Extract the ZIP to a short, writable folder (for example, `C:\ZeraPS5`) and open **ZeraPS5.exe**. Very long paths can exceed Windows extraction limits during updates.
3. Read the included instructions, then add your own games and required console libraries.

## Game compatibility

**b1214 passed 196 automated Windows checks.** [Package and updater validation](https://github.com/zeraps5/ZeraPS5-Releases/releases/tag/v0.1.1-b1214). Game records retain their tested revisions.

[Game compatibility and tested builds](COMPATIBILITY.md) — reviewed 25 November 2026, with dated Intel and RTX sessions, tested revisions where recorded, and the startup survey.

[Technical changelog](CHANGELOG.md) · [Contribute or report a result](CONTRIBUTING.md) · [Matching source revision and checksums](SOURCE-VERSION.txt)

Public distribution maintained by **Metallrock (metallrockdev)**.

The default branch includes the b1214 emulator source and tests. The [release tag](https://github.com/zeraps5/ZeraPS5-Releases/tree/v0.1.1-b1214) identifies the exact packaged revision. The b984 source import includes the [source changelog](docs/source-changelog-b984.md). Releases published before the source import retain their original tags. For the exact source of a published build, use its versioned `*-source.zip` and the `sources/` materials in the Windows package.

## Experimental software

ZeraPS5 is experimental. Compatibility, stability, and FPS are not guaranteed.
Games, firmware, keys, and console modules are not included in the packages.
Read `LEIA-ME.txt`, `docs/LIMITACOES.txt`, and `docs/RECOMPILAR.txt` inside the ZIP.

## Licensing and credits

See [LICENSE](LICENSE) and [CREDITS.txt](CREDITS.txt). Packages include component licenses,
corresponding source code, FFmpeg sources, and object files for relinking.
Private development is kept separate from this distribution repository.
ZeraPS5 is not affiliated with Sony.
