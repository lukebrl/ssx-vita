#!/usr/bin/env bash
#
# Builds the VPKs (on Linux or WSL).
#
#   ./build.sh            Debug build: logs to ux0:data/ssx/log.txt, [PERF]
#                         lines; installs as "SSX (2012) Debug" (SSXD00001)
#   ./build.sh release    Release build: -O3, no logging; installs as
#                         "SSX (2012)" (SSXV00001)
#   ./build.sh both       Both, copied with their ELFs (to read crash dumps)
#                         to $OUT_DIR (default: dist/)
#
# Both keep the on-screen FPS counter and share the data folder. Extra CMake
# options (e.g. -DSSX_FNPROF=ON, see docs/DEBUGGING.md) go in $CMAKE_ARGS.
#
# Needs VitaSDK-softfp (Android .so files use the softfp float ABI, so the
# regular hardfp VitaSDK can't be used) and CMake >= 3.14.

set -euo pipefail

: "${VITASDK:=$HOME/vitasdk-softfp}"
export VITASDK
export PATH="$VITASDK/bin:$HOME/opt/cmake-3.31.6-linux-x86_64/bin:$PATH"

cd "$(dirname "$0")"

build() {
    local type=$1 dir=$2
    # shellcheck disable=SC2086 # CMAKE_ARGS is a list of options
    cmake -S . -B "$dir" -DCMAKE_BUILD_TYPE="$type" -DDUMP_COMPILED_SHADERS=OFF ${CMAKE_ARGS:-}
    cmake --build "$dir" -j"$(nproc)"
}

case "${1:-}" in
    release)
        build Release build-release
        echo; echo "Built: build-release/ssx_vita.vpk"
        ;;
    both)
        build Debug build
        build Release build-release
        out="${OUT_DIR:-dist}"
        mkdir -p "$out"
        cp build/ssx_vita_debug.vpk build-release/ssx_vita.vpk "$out/"
        # ELFs with symbols, to read crash dumps
        cp build/ssx_vita "$out/ssx_vita_debug.elf"
        cp build-release/ssx_vita "$out/ssx_vita.elf"
        echo; echo "Built: $out/ssx_vita_debug.vpk (debug), $out/ssx_vita.vpk (release)"
        ;;
    *)
        build Debug build
        echo; echo "Built: build/ssx_vita_debug.vpk"
        ;;
esac
