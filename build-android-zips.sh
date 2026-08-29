#!/bin/bash
# build-android-zips.sh — Build a Kodi binary addon for Android (arm64-v8a +
# armeabi-v7a + armv8a32), strip the .so, and package installable zips.
#
# Usage: build-android-zips.sh <addon-repo-dir> [addon-id]
#   <addon-repo-dir>  e.g. /path/to/pvr.iptvsimple
#   [addon-id]        defaults to the repo dir basename
#
# Requires: cmake, zip, Android NDK r28c at
# /usr/lib/android-sdk/ndk/28.2.13676358, and a Kodi source tree as a sibling
# directory named "xbmc" (used as the superbuild source).
#
# Labels built can be restricted via LABELS="..." (default: arm64 armv7):
#   LABELS="armv8a32" ./build-android-zips.sh /path/to/pvr.iptvsimple
# armv8a32 = armeabi-v7a ABI compiled with ARMv8-A tuning (+crc+crypto,
# neon-fp-armv8) for 32-bit-kernel TV boxes on ARMv8 CPUs.
#
# Cached downloads: set CACHED_DOWNLOADS to a directory of pre-fetched tarballs
# (with .sha512 sidecar files) so the superbuild never hits the network.
# By default, the script looks for a sibling "android-tarballs" dir next to
# the addon repo (i.e. <kodi-workspace>/android-tarballs).  This folder should
# be populated once (when network is available) and reused for all subsequent
# builds.  See verify-downloads.sh for integrity checking.

set -euo pipefail

REPO_DIR="$(cd "$1" && pwd)"
ADDON_ID="${2:-$(basename "$REPO_DIR")}"
XBMC_DIR="$(dirname "$REPO_DIR")/xbmc"
NDK_ROOT="/usr/lib/android-sdk/ndk/28.2.13676358"
NDK_BIN="$NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin"
NDK_TOOLCHAIN="$NDK_ROOT/build/cmake/android.toolchain.cmake"
STRIP="$NDK_BIN/llvm-strip"
API=24
ADDON_MANIFEST="$REPO_DIR/$ADDON_ID/addon.xml.in"
CACHED_DOWNLOADS="${CACHED_DOWNLOADS:-$(dirname "$REPO_DIR")/android-tarballs}"

if [[ ! -f "$NDK_TOOLCHAIN" ]]; then
  echo "ERROR: NDK toolchain not found at $NDK_TOOLCHAIN" >&2
  exit 1
fi
if [[ ! -f "$ADDON_MANIFEST" ]]; then
  echo "ERROR: addon manifest not found at $ADDON_MANIFEST" >&2
  exit 1
fi

VERSION="$(sed -n 's/^[[:space:]]*version="\([^"]*\)".*/\1/p' "$ADDON_MANIFEST" | head -1)"
if [[ -z "$VERSION" ]]; then
  echo "ERROR: could not read version from $ADDON_MANIFEST" >&2
  exit 1
fi
echo "=== Building $ADDON_ID $VERSION for Android (arm64-v8a + armeabi-v7a + armv8a32) ==="

# Kodi's depends layer keys off CPU names ("arm64", "armeabi-v7a"), not ABI names.
# armv8a32 maps to armeabi-v7a CPU (32-bit ARMv8 on 32-bit kernel TV boxes).
declare -A CPU_MAP=( [arm64]=arm64 [armv7]=armeabi-v7a [armv8a32]=armeabi-v7a )

# ARMv8-A optimization flags for 32-bit builds:
#   -march=armv8-a+crc+crypto : enable ARMv8-A instructions with CRC32 and crypto
#   -mfpu=neon-fp-armv8       : NEONv2 / fp-armv8 (Cortex-A53's native FPU)
#   -mfloat-abi=softfp        : mandated by Android armeabi-v7a ABI
#   -mtune=cortex-a53         : schedule for Cortex-A53 (dominant in budget ARMv8 TV boxes)
TUNED_CFLAGS="-march=armv8-a+crc+crypto -mfpu=neon-fp-armv8 -mfloat-abi=softfp -mtune=cortex-a53"

# CMake override for Release optimization + dead-code elimination (--gc-sections)
OPTIMIZATION_CMAKE="$REPO_DIR/cmake/optimization.cmake"

# Copy cached download tarballs into the build's download directory so
# ExternalProject's download step finds them locally instead of hitting the
# network (ftp.gnu.org is frequently unreachable in restricted environments).
# Uses cp -n so existing downloads in the build dir are never overwritten.
preseed_downloads() {
  local label="$1"
  local dl_dir="$REPO_DIR/build-android-$label/build/download"
  mkdir -p "$dl_dir"
  if [[ -d "$CACHED_DOWNLOADS" ]]; then
    cp -n "$CACHED_DOWNLOADS"/*.tar.gz "$CACHED_DOWNLOADS"/*.tar.xz \
         "$CACHED_DOWNLOADS"/*.tar.bz2 "$CACHED_DOWNLOADS"/*.zip "$dl_dir/" 2>/dev/null || true
    cp -n "$CACHED_DOWNLOADS"/*.sha512 "$dl_dir/" 2>/dev/null || true
  fi
}

build_one() {
  local label="$1" abi="$2" ziparch="$3"
  local cpu="${CPU_MAP[$label]}"
  local build_dir="$REPO_DIR/build-android-$label"

  # Preseed download cache BEFORE configure so the download directory exists
  # with cached tarballs before ExternalProject tries to fetch anything.
  preseed_downloads "$label"

  if [[ ! -f "$build_dir/CMakeCache.txt" ]]; then
    echo "--- Configuring $label ($abi) ---"
    local extra_flags=()
    if [[ "$label" == armv8a32 ]]; then
      extra_flags=(-DCMAKE_C_FLAGS="$TUNED_CFLAGS" \
                   -DCMAKE_CXX_FLAGS="$TUNED_CFLAGS")
    fi
    if ! cmake -S "$XBMC_DIR/cmake/addons" -B "$build_dir" \
      -DCMAKE_TOOLCHAIN_FILE="$NDK_TOOLCHAIN" \
      -DANDROID_ABI="$abi" \
      -DANDROID_PLATFORM="android-$API" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_USER_MAKE_RULES_OVERRIDE="$OPTIMIZATION_CMAKE" \
      -DADDON_SRC_PREFIX="$(dirname "$REPO_DIR")" \
      -DADDONS_TO_BUILD="$ADDON_ID" \
      -DCPU="$cpu" \
      "${extra_flags[@]}"; then
      echo "ERROR: configure failed for $label" >&2
      exit 1
    fi
  fi

  # Export cross-compilation env for autoconf-based dependency recipes
  # (iconv, gmp, nettle, gnutls): their ./configure runs rely on CC/HOST from
  # the environment and silently build for the host without them. CMake-based
  # deps are unaffected — the toolchain file overrides env CC/CXX.
  local api="$API"
  if [[ "$abi" == arm64-v8a ]]; then
    export CC="$NDK_BIN/aarch64-linux-android${api}-clang"
    export CXX="$NDK_BIN/aarch64-linux-android${api}-clang++"
    export HOST="aarch64-linux-android"
  else
    export CC="$NDK_BIN/armv7a-linux-androideabi${api}-clang"
    export CXX="$NDK_BIN/armv7a-linux-androideabi${api}-clang++"
    export HOST="arm-linux-androideabi"
  fi

  # pkg-config discovery for dependency .pc files (FindFFMPEG et al rely on
  # it; official CI exports this globally from tools/depends)
  export PKG_CONFIG_PATH="$build_dir/build/depends/lib/pkgconfig"

  echo "--- Building $label ($abi) ---"
  # Wipe the superbuild's ExternalProject work area for THIS addon only, so it
  # always gets a fresh addon re-checkout/rebuild. Dependency stamps (under
  # build/<dep>/src/<dep>-stamp/) are untouched — they survive and prevent
  # re-downloads. The download dir (build/download/) is also untouched thanks
  # to the preseed above.
  rm -rf "$build_dir/$ADDON_ID-prefix" "$build_dir/.install"
  cmake --build "$build_dir" -j"$(nproc)" > /dev/null

  # Stage from THIS arch's private .install tree, never from any shared output
  # dir. Layout varies between builds (either flat .install/<id>/ or split
  # lib/+share/), so locate the pieces instead of assuming paths.
  local so_src data_dir
  so_src="$(find "$build_dir/.install" -name "$ADDON_ID.so" | head -1)"
  data_dir="$(dirname "$(find "$build_dir/.install" -name addon.xml -path "*$ADDON_ID*" | head -1)")"
  if [[ -z "$so_src" || -z "$data_dir" ]]; then
    echo "ERROR: could not locate built addon pieces under $build_dir/.install" >&2
    exit 1
  fi

  local stage
  stage="$(mktemp -d)/$ADDON_ID"
  mkdir -p "$stage"
  cp -r "$data_dir/." "$stage/"
  cp "$so_src" "$stage/"

  local so="$stage/$ADDON_ID.so"
  # Verify we staged the right architecture before stripping
  if [[ "$abi" == arm64-v8a && ! "$(file -b "$so")" == *"ARM aarch64"* ]]; then
    echo "ERROR: staged $label .so is not aarch64: $(file -b "$so")" >&2
    exit 1
  fi
  if [[ "$abi" == armeabi-v7a && ! "$(file -b "$so")" == *"32-bit"*"ARM"* ]]; then
    echo "ERROR: staged $label .so is not armv7: $(file -b "$so")" >&2
    exit 1
  fi
  # Aggressive strip: --strip-all removes all symbols (more than --strip-unneeded)
  "$STRIP" --strip-all "$so"

  local zip_name="addon-$ADDON_ID-$VERSION-android-$ziparch.zip"
  (cd "$(dirname "$stage")" && zip -9 -qr "$REPO_DIR/$zip_name" "$ADDON_ID")
  rm -rf "$stage"

  echo "--- Packaged $zip_name ($(du -h "$REPO_DIR/$zip_name" | cut -f1)) ---"
}

want() { [[ " ${LABELS:-} " == *" $1 "* ]]; }

LABELS="${LABELS:-arm64 armv7}"

if want arm64; then    build_one arm64    arm64-v8a    aarch64;   fi
if want armv7; then    build_one armv7    armeabi-v7a  armv7;     fi
if want armv8a32; then build_one armv8a32 armeabi-v7a  armv8a32;  fi

echo "=== Done: $ADDON_ID $VERSION ==="
