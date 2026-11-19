#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Builds the minimal static FFmpeg (LGPL) the emulator links: only the
# decoders and demuxers PS5 games need (movies: H.264, HEVC, VP9 in MP4/MKV;
# game audio for libSceAjm: AAC, ATRAC9, MP3, Opus). Output:
#   third_party/ffmpeg/<platform>/{include,lib}
#
# Windows: run from an MSYS2 UCRT64 shell (or: MSYSTEM=UCRT64 bash -l tools/build_ffmpeg.sh)
#   pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-nasm make
# Linux: needs gcc, make, nasm.
#
# The source tarball is not part of the repository: put ffmpeg-7.1.2.tar.xz
# (https://ffmpeg.org/releases/) in third_party/ffmpeg/ first.
set -e
VERSION=7.1.2
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DIR="$ROOT/third_party/ffmpeg"
case "$(uname -s)" in
MINGW* | MSYS*) PLATFORM=win64 ;;
*) PLATFORM=linux64 ;;
esac
OUT="$DIR/$PLATFORM"
# FFmpeg's configure cannot build from a path with spaces: work in /tmp
WORK=/tmp/ps5c-ffmpeg-$PLATFORM
SRC="$WORK/ffmpeg-$VERSION"

[ -f "$DIR/ffmpeg-$VERSION.tar.xz" ] || { echo "missing $DIR/ffmpeg-$VERSION.tar.xz"; exit 1; }
mkdir -p "$WORK"
[ -d "$SRC" ] || tar -xf "$DIR/ffmpeg-$VERSION.tar.xz" -C "$WORK"
cd "$SRC"

./configure --prefix="$WORK/install" \
    --enable-static --disable-shared --disable-programs --disable-doc --disable-debug \
    --disable-autodetect --disable-network --disable-everything \
    --disable-avdevice --disable-avfilter --disable-postproc \
    --enable-avcodec --enable-avformat --enable-swresample --enable-swscale \
    --enable-decoder=h264,hevc,vp9,aac,aac_fixed,aac_latm,atrac9,mp3,mp3float,opus,vorbis,pcm_s16le,pcm_f32le \
    --enable-demuxer=mov,matroska,mpegts,mp3,aac,ogg,wav,h264,hevc \
    --enable-parser=h264,hevc,vp9,aac,aac_latm,mpegaudio,opus,vorbis \
    --enable-protocol=file \
    --extra-cflags=-O2
make -j"$(nproc 2>/dev/null || echo 4)"
make install
rm -rf "$OUT"
mkdir -p "$OUT"
cp -r "$WORK/install/include" "$WORK/install/lib" "$OUT/"
echo "FFmpeg $VERSION ($PLATFORM) installed in $OUT"
