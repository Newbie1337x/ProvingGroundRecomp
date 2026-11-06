#!/usr/bin/env bash
# Cross-compiles Proving Ground Recompiled for Linux on ARM64 from an x86-64 Linux host
# (WSL Ubuntu works). See docs/PORTING.md for the packages it needs.
#
#   REXSDK_SOURCE=/path/to/patched/rexglue-sdk tools/build-linux-arm64.sh [Release|RelWithDebInfo]
#
# The generated code is the same on every architecture, so this script does not run the code
# generator: generated/ must already exist (from build.sh or build.ps1 on a PC). The build tree
# goes to $PG_ARM64_BUILD (default ~/pg-build/linux-arm64-<config>) because building on a
# Windows drive mounted in WSL is several times slower.
set -euo pipefail

CONFIG="${1:-Release}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="${REXSDK_SOURCE:-$REPO/thirdparty/rexglue-sdk}"
BUILD="${PG_ARM64_BUILD:-$HOME/pg-build/linux-arm64-$(echo "$CONFIG" | tr '[:upper:]' '[:lower:]')}"
JOBS="${JOBS:-$(nproc)}"
SUFFIX="${REX_CLANG_SUFFIX:-}"
if [[ -z "$SUFFIX" ]]; then
    for v in 22 21 20; do command -v "clang++-$v" >/dev/null 2>&1 && { SUFFIX="-$v"; break; }; done
fi

[[ -f "$SDK/CMakeLists.txt" ]] || { echo "No ReXGlue SDK at $SDK (set REXSDK_SOURCE)." >&2; exit 1; }
[[ -f "$REPO/generated/sources.cmake" ]] || { echo "generated/ is missing; run the code generator on a PC first." >&2; exit 1; }

cmake -S "$REPO" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$REPO/cmake/toolchains/linux-arm64.cmake" -DREX_CLANG_SUFFIX="$SUFFIX" \
    -DCMAKE_BUILD_TYPE="$CONFIG" -DREXSDK_DIR="$SDK" -DPG_BUILD_GAME=ON \
    -DREXGLUE_USE_D3D12=OFF -DREXGLUE_USE_VULKAN=ON -DREXGLUE_BUILD_TESTS=OFF \
    -DREXGLUE_ENABLE_FIDELITYFX=OFF \
    -DCMAKE_C_FLAGS=-march=armv8.2-a -DCMAKE_CXX_FLAGS=-march=armv8.2-a
cmake --build "$BUILD" --target pg --parallel "$JOBS"

DIST="$REPO/out/dist/ProvingGroundRecomp-linux-arm64"
rm -rf "$DIST"
mkdir -p "$DIST"
cp "$BUILD/ProvingGround" "$DIST/"
find "$BUILD" -maxdepth 1 -name '*.so*' -exec cp -P {} "$DIST/" \;
find "$SDK/out/linux-arm64" -name 'librexruntime*.so*' -exec cp -P {} "$DIST/" \; 2>/dev/null || true
cp "$REPO/LICENSE" "$REPO/THIRD_PARTY_NOTICES.md" "$DIST/"
echo "Built: $DIST/ProvingGround"
