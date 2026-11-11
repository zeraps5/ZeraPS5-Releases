# Contributing to ZeraPS5

Bug reports, compatibility results, focused code fixes and documentation improvements are welcome.

## Get the matching emulator source

- [b898 source archive](https://github.com/zeraps5/ZeraPS5-Releases/releases/download/v0.1.0-b898/ZeraPS5-v0.1.0-b898-win64-source.zip)
- [b898 Windows package](https://github.com/zeraps5/ZeraPS5-Releases/releases/download/v0.1.0-b898/ZeraPS5-v0.1.0-b898-win64.zip), including the same emulator source, FFmpeg source and relink materials under sources/.
- [Exact revision and SHA-256](SOURCE-VERSION.txt)

GitHub's automatic Source code ZIP and a clone of this repository contain distribution documentation. Use the versioned source archive above for emulator code. Pull requests here can update the public documentation, compatibility records and repository tooling.

For an emulator-code change, extract the matching source, prepare a focused patch and [open an issue](https://github.com/zeraps5/ZeraPS5-Releases/issues/new) with the patch, affected revision, problem, proposed fix and validation. Preserve upstream credits and component licenses. Do not upload full game or console files.

## Build from source

The included dist/docs/RECOMPILAR.txt is the build/relink reference. It specifies Windows x64, MSYS2 UCRT64 GCC/binutils/make, CMake 3.21 or newer, and NASM for FFmpeg. FFmpeg's matching source is included in the Windows package.

1. Extract the versioned emulator source archive.
2. Place the supplied ffmpeg-7.1.2.tar.xz under third_party/ffmpeg/ and run `sh tools/build_ffmpeg.sh` from MSYS2 UCRT64.
3. With UCRT64/bin on PATH, configure, build and run the relevant checks:

```sh
cmake -S . -B build/distribution -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build/distribution --parallel 2
ctest --test-dir build/distribution --output-on-failure
```

These commands are the packaged instructions. Identical output bytes across different toolchains have not been certified.

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
