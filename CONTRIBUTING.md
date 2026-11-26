# Contributing to ZeraPS5

Bug reports, compatibility results, focused code fixes and documentation improvements are welcome.

## Get the matching emulator source

- [b1214 source archive](https://github.com/zeraps5/ZeraPS5-Releases/releases/download/v0.1.1-b1214/ZeraPS5-v0.1.1-b1214-win64-source.zip)
- [b1214 Windows package](https://github.com/zeraps5/ZeraPS5-Releases/releases/download/v0.1.1-b1214/ZeraPS5-v0.1.1-b1214-win64.zip), including the same emulator source, FFmpeg source and relink materials under sources/.
- [Exact revision and SHA-256](SOURCE-VERSION.txt)

Fork or clone this repository to work on the emulator source, tests, or documentation. Create a branch from `main` and open a focused pull request with the affected revision, steps to reproduce, and tests you ran. Preserve upstream credits and component licenses. Do not upload full game or console files.

## Build from source

The [rebuild and relink instructions](dist/docs/RECOMPILAR.txt) specify Windows x64, MSYS2 UCRT64 GCC/binutils/make, CMake 3.21 or newer, and NASM for FFmpeg. FFmpeg's matching source is included in the Windows package.

1. Clone this repository. For the original build identity, extract the versioned emulator source archive outside any Git checkout.
2. Place the supplied ffmpeg-7.1.2.tar.xz under third_party/ffmpeg/ and run `sh tools/build_ffmpeg.sh` from MSYS2 UCRT64.
3. With UCRT64/bin on PATH, configure, build and run the relevant checks:

```sh
cmake -S . -B build/distribution -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build/distribution --parallel 2
ctest --test-dir build/distribution --output-on-failure
```

These commands are the packaged instructions. Identical output bytes across different toolchains have not been certified.

The launcher compares the published build number to the installed build number. The public Git history differs from the original build history, so its commit count must not be used as a release sequence. Verify that a future stable package has a build number higher than the current public manifest before publishing it.

## Compatibility reports

[Open a compatibility issue](https://github.com/zeraps5/ZeraPS5-Releases/issues/new) and provide:

- Game name, platform, title ID and game version.
- Emulator build and source revision, if available.
- CPU, GPU, RAM, Windows version and graphics-driver version.
- Resolution, relevant settings and whether the test used a special profile.
- Scene reached, actions performed, run duration and observed problems.
- FPS only with the measured scene, hardware and settings. A menu/video FPS is separate from gameplay FPS.

Screenshots and short clips help identify the reached stage. Do not include your personal paths, account information, keys, game files, firmware or proprietary console modules in a public issue.

## Private diagnostic reports

After closing the game, the launcher's **Report a problem** option can send a filtered ZIP to the private report receiver, or save a local draft. Describe the actions and problem and retain the returned report code. Automatic report uploads remain disabled in this release. The package's docs/REPORTING.txt explains contents, limits and retention.

When opening a public issue, include the report code and a non-sensitive summary. Review every attachment before making it public.

## Updating the public compatibility list

Current English records are stored in docs/compatibility-records.json. Each entry keeps its actual test revision/date, hardware, stage, result and evidence source. The complete dated startup survey and older observations are retained in COMPATIBILITY.md and the linked history. A documentation correction does not relabel old game tests as tests of the newest executable.
