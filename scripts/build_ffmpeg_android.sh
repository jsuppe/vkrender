#!/bin/bash
# Cross-compiles a minimal static FFmpeg (RTSP demux + parsers, no decoders)
# for Android. Run on a Linux box.
#
# Env overrides:
#   NDK_DIR  - existing NDK install (skips the download)
#   OUT_DIR  - artifact prefix root   (default: <repo>/android/ffmpeg-android)
#   WORK_DIR - scratch for NDK+source (default: ~/vkrender-deps)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR="${OUT_DIR:-$SCRIPT_DIR/../android/ffmpeg-android}"
WORK_DIR="${WORK_DIR:-$HOME/vkrender-deps}"
NDK_VER=r27c
API=29
mkdir -p "$WORK_DIR"
cd "$WORK_DIR"

if [ -z "$NDK_DIR" ]; then
  if [ ! -d "android-ndk-$NDK_VER" ]; then
    echo "== downloading NDK $NDK_VER =="
    curl -sLo ndk.zip "https://dl.google.com/android/repository/android-ndk-$NDK_VER-linux.zip"
    unzip -q ndk.zip && rm ndk.zip
  fi
  NDK_DIR=$WORK_DIR/android-ndk-$NDK_VER
fi
TC=$NDK_DIR/toolchains/llvm/prebuilt/linux-x86_64
echo "== NDK: $NDK_DIR =="

if [ ! -d "$WORK_DIR/ffmpeg-src" ]; then
  echo "== cloning FFmpeg n6.1 =="
  git clone -q --depth 1 -b n6.1.2 https://git.ffmpeg.org/ffmpeg.git "$WORK_DIR/ffmpeg-src"
fi

build_abi() {
  local ABI=$1 ARCH=$2 CPU=$3 CCPREFIX=$4
  echo "== configuring $ABI =="
  cd "$WORK_DIR/ffmpeg-src"
  make distclean >/dev/null 2>&1 || true
  ./configure \
    --prefix="$OUT_DIR/$ABI" \
    --target-os=android --arch=$ARCH --cpu=$CPU \
    --cc=$TC/bin/${CCPREFIX}${API}-clang \
    --cxx=$TC/bin/${CCPREFIX}${API}-clang++ \
    --ar=$TC/bin/llvm-ar --nm=$TC/bin/llvm-nm --ranlib=$TC/bin/llvm-ranlib \
    --strip=$TC/bin/llvm-strip \
    --enable-cross-compile --enable-pic --disable-asm \
    --enable-static --disable-shared \
    --disable-programs --disable-doc --disable-debug \
    --disable-avdevice --disable-swscale --disable-swresample --disable-postproc \
    --disable-avfilter --disable-encoders --disable-decoders --disable-muxers \
    --disable-devices --disable-filters --disable-hwaccels \
    --disable-everything \
    --enable-network \
    --enable-protocol=tcp,udp,rtp,http \
    --enable-demuxer=rtsp,sdp,rtp \
    --enable-parser=h264,hevc \
    --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,extract_extradata \
    >/dev/null
  echo "== building $ABI =="
  make -j$(nproc) >/dev/null 2>&1
  make install >/dev/null
  echo "== $ABI done =="
}

build_abi x86_64 x86_64 x86-64 x86_64-linux-android
build_abi arm64-v8a aarch64 armv8-a aarch64-linux-android

echo "ALL_DONE: $OUT_DIR"
