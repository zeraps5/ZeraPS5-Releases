#!/usr/bin/env bash
set -euo pipefail
source_dir="$(cygpath -u "$ZERA_SOURCE_DIR")"
review_dir="$(cygpath -u "$ZERA_REVIEW_DIR")"
runner_temp="$(cygpath -u "$RUNNER_TEMP")"
test "$source_dir" = "$runner_temp/zeraps5-signpath/source"
test "$review_dir" = "$runner_temp/zeraps5-signpath"
cd "$source_dir"
pacman -Q > "$review_dir/packages.txt"
sh tools/build_ffmpeg.sh
cmake -S . -B "$review_dir/build" -G "MinGW Makefiles" \
  -DCMAKE_BUILD_TYPE=Release -DPS5C_RUN_GPU_TESTS=OFF \
  -DPS5C_MARCH_V3=OFF -DCMAKE_EXE_LINKER_FLAGS="-static -static-libgcc"
cmake --build "$review_dir/build" --parallel 2
ctest --test-dir "$review_dir/build" --output-on-failure --parallel 2
