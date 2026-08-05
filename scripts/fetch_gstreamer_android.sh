#!/usr/bin/env bash
# Fetch the official GStreamer Android universal prebuilts and prune each ABI
# to the static-link closure vkrender actually uses (~130 MB per ABI instead
# of ~3.5 GB), placing the result where the Gradle/CMake build expects it:
# android/gstreamer-android/<abi>/.
#
# Requirements: curl, tar (with xz), pkg-config. Idempotent; skips ABIs that
# are already in place. Used by CI; equally fine for a dev machine (on
# Windows run it from Git Bash / WSL, or just extract the tarball unpruned as
# docs/ANDROID.md describes).
#
# Env overrides: GST_VERSION, ABIS ("arm64-v8a x86_64"), WORK_DIR.
set -euo pipefail

VER="${GST_VERSION:-1.26.11}"
ABIS=(${ABIS:-arm64-v8a x86_64})
REPO="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$REPO/android/gstreamer-android"
WORK="${WORK_DIR:-${TMPDIR:-/tmp}/gst-android-fetch}"
URL="https://gstreamer.freedesktop.org/data/pkg/android/${VER}/gstreamer-1.0-android-universal-${VER}.tar.xz"

# Union of the static-link closures the Android CMakeLists files pull in
# (renderer/decoder, airplay, webrtc). The pruned tree must be a superset of
# every module's needs, so keep this in sync with the pkg_check_modules lists.
MODULES="gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 \
gstcoreelements gstapp gstrtsp gstrtp gstrtpmanager gstudp gsttcp \
gstplayback gsttypefindfunctions gstvideoparsersbad \
gstwebrtc gstnice gstdtls gstsrtp gstrtpmanagerbad gstrswebrtc gstrsrtp \
gstwebrtchttp"

# Tarball top-level dirs use NDK-ish names, not Android ABI names.
tardir() { case "$1" in arm64-v8a) echo arm64 ;; armeabi-v7a) echo armv7 ;; *) echo "$1" ;; esac; }

mkdir -p "$WORK" "$DEST"
tarball="$WORK/gstreamer-1.0-android-universal-${VER}.tar.xz"

for abi in "${ABIS[@]}"; do
    out="$DEST/$abi"
    if [ -f "$out/lib/pkgconfig/gstreamer-1.0.pc" ]; then
        echo "== $abi already present, skipping"
        continue
    fi

    if [ ! -f "$tarball" ]; then
        echo "== downloading $URL"
        curl -fL --retry 3 -o "$tarball" "$URL"
    fi

    src="$WORK/$(tardir "$abi")"
    if [ ! -d "$src" ]; then
        echo "== extracting $(tardir "$abi") from tarball"
        tar -C "$WORK" -xJf "$tarball" "$(tardir "$abi")"
    fi

    echo "== pruning $abi to link closure"
    export PKG_CONFIG_LIBDIR="$src/lib/pkgconfig:$src/lib/gstreamer-1.0/pkgconfig"
    libs=$(pkg-config --static --libs $MODULES)

    mkdir -p "$out/lib/gstreamer-1.0"
    cp -r "$src/include" "$out/include"
    cp -r "$src/lib/pkgconfig" "$out/lib/pkgconfig"
    cp -r "$src/lib/gstreamer-1.0/pkgconfig" "$out/lib/gstreamer-1.0/pkgconfig"
    # Generated per-arch headers live under lib/.
    [ -d "$src/lib/glib-2.0" ] && cp -r "$src/lib/glib-2.0" "$out/lib/glib-2.0"
    [ -d "$src/lib/gstreamer-1.0/include" ] && cp -r "$src/lib/gstreamer-1.0/include" "$out/lib/gstreamer-1.0/include"

    missing=""
    for flag in $libs; do
        case "$flag" in
        -l*) name="lib${flag#-l}.a" ;;
        *) continue ;;
        esac
        if [ -f "$src/lib/$name" ]; then
            cp "$src/lib/$name" "$out/lib/$name"
        elif [ -f "$src/lib/gstreamer-1.0/$name" ]; then
            cp "$src/lib/gstreamer-1.0/$name" "$out/lib/gstreamer-1.0/$name"
        else
            missing="$missing ${flag#-l}" # NDK system libs (m, dl, log, ...)
        fi
    done
    # M18: the AirPlay APK links OpenSSL from these prebuilts; keep it even if
    # it is not part of the GStreamer static-link closure (headers arrive with
    # the full include/ copy above).
    for name in libssl.a libcrypto.a; do
        [ -f "$src/lib/$name" ] && cp "$src/lib/$name" "$out/lib/$name"
    done

    echo "   kept $(du -sh "$out" | cut -f1); system libs resolved by NDK:$missing"
done

echo "== done: $DEST"
