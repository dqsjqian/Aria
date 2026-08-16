#!/usr/bin/env bash
# Unified build & package script for macOS / Linux.
#
# Default behaviour (no args): build aria framework + tests + ctest +
# release package -> build/flavors/release/
#
# Usage:
#   scripts/build.sh                # default: Release framework + tests + ctest + package
#   scripts/build.sh debug          # Debug framework + tests (no ctest, no package)
#   scripts/build.sh tests          # alias for default (was: build/tests/ — now reuses build/flavors/release)
#   scripts/build.sh asan           # Debug + AddressSanitizer + UBSan
#   scripts/build.sh tsan           # Debug + ThreadSanitizer
#   scripts/build.sh pack-zip       # Default flow plus build/dist/archives/aria-*.tar.gz
#   scripts/build.sh tsan-gate      # Pre-release TSan gate (CHANGELOG promises every release is TSan-clean)
#   scripts/build.sh android        # Android NDK cross-build (JNI adapter)
#   scripts/build.sh clean          # Wipe build/
#
# Notes:
#   * This script only builds the aria framework + tests; it does not
#     build examples. To run a demo use examples/<demo>/scripts/run.sh
#     (Qt / AppKit / UIKit each have their own).
#   * To override the compiler: CC=clang CXX=clang++ scripts/build.sh

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

MODE="${1:-default}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

# Unified build-tree layout (since 2026-06-08):
#
#   build/
#     ├── ide/                  VSCode CMake Tools workspace (Debug + tests)
#     ├── flavors/              command-line build flavors (this script)
#     │     ├── release/        scripts/build.sh         (default)
#     │     ├── debug/          scripts/build.sh debug
#     │     ├── asan/           scripts/build.sh asan    (ASan + UBSan)
#     │     ├── tsan/           scripts/build.sh tsan
#     │     ├── tsan-gate/      scripts/build.sh tsan-gate
#     │     │
#     │     │── CI-only flavors (created by .github/workflows, not
#     │     │      by this script; listed so the layout stays the single
#     │     │      source of truth for where build trees may appear) ──
#     │     ├── qt/             CI: Qt6-enabled build
#     │     ├── docs/           CI: Doxygen reference
#     │     ├── abi-smoke/      CI: shared-lib cross-dylib ABI smoke
#     │     ├── tidy/           CI: clang-tidy gate
#     │     ├── bench/          nightly: benchmark + P99 ceiling check
#     │     └── fuzz/           nightly: fuzzers at 1M iterations
#     │     │
#     │     │── Demo SDK + standalone demo trees. The framework SDK is
#     │     │      built ONCE into sdk/ and installed to dist/tree/;
#     │     │      every CMake demo builds STANDALONE against it
#     │     │      (ARIA_USE_INSTALLED=ON + find_package), so the demo
#     │     │      tree holds only the demo's own objects ──
#     │     ├── sdk/             shared framework SDK build (run.sh scripts)
#     │     ├── qt-demo/         1-qt-showcase, standalone (Qt6)
#     │     ├── qt-demo-msvc/    1-qt-showcase, standalone MSVC (run-msvc.ps1)
#     │     ├── web-demo/        4-web-mvvm, standalone (HTTP)
#     │     ├── web-demo-msvc/   4-web-mvvm, standalone MSVC
#     │     └── appkit-demo/     2-macos-appkit-mvvm products (.app symlink)
#     ├── platforms/            cross-compilation targets
#     │     └── android/       scripts/build.sh android (NDK cross-build)
#     ├── examples/             MIRROR of the main build's add_subdirectory
#     │     examples (NOT standalone trees — never configure into them),
#     │     plus raw product output from the non-CMake demos
#     │     (3-ios-oc-uikit-mvvm via xcodebuild; 2-macos-appkit-mvvm now
#     │     builds standalone too and keeps products in flavors/appkit-demo)
#     └── dist/                 release artefacts (only when packaging)
#           ├── tree/           the installed SDK (demand-built by demo
#           │                   run scripts; also the standalone link target)
#           └── archives/       .tar.gz / .zip output (was packages/)
#
# Per-flavor sub-directories under a single build/ root: `rm -rf build/`
# wipes everything; CMake caches never cross-contaminate. `tests` mode
# is now an alias for the default flow (it reuses build/flavors/release/
# instead of duplicating the build).

case "$MODE" in
    default)   BUILD_DIR="build/flavors/release" ;;   # default invocation == release
    release)   BUILD_DIR="build/flavors/release" ;;
    debug)     BUILD_DIR="build/flavors/debug" ;;
    tests)     BUILD_DIR="build/flavors/release" ;;   # alias: ctest on the release tree
    asan)      BUILD_DIR="build/flavors/asan" ;;
    tsan)      BUILD_DIR="build/flavors/tsan" ;;
    pack-zip|pack|zip|tar|package) BUILD_DIR="build/flavors/release" ;;
    tsan-gate) BUILD_DIR="build/flavors/tsan-gate" ;;
    android)    BUILD_DIR="build/platforms/android" ;;
    clean)     BUILD_DIR="build" ;;
    *)         BUILD_DIR="build/flavors/$MODE" ;;
esac

# ── tsan-gate / clean: dedicated branches ────────────────────────────────────
case "$MODE" in
    clean)
        echo "▶ wiping ${BUILD_DIR}/"
        rm -rf "${BUILD_DIR}"
        exit 0
        ;;
    tsan-gate)
        # Release-verification gate: reconfigure into a side build dir
        # under TSan, build the test binaries, run all of them. Halts
        # on the first race report. CHANGELOG promises every release
        # is TSan-clean; this is the script that enforces it.
        echo "▶ release-gate: TSan stress"
        TSAN_BUILD_DIR="${BUILD_DIR}"
        mkdir -p "${TSAN_BUILD_DIR}"
        cmake -S . -B "${TSAN_BUILD_DIR}" \
            -DCMAKE_BUILD_TYPE=Debug \
            -DARIA_ENABLE_TSAN=ON \
            -DARIA_ENABLE_ASAN=OFF \
            -DARIA_ENABLE_UBSAN=OFF \
            -DARIA_BUILD_QT6=OFF \
            -DARIA_BUILD_EXAMPLES=OFF
        cmake --build "${TSAN_BUILD_DIR}" \
            --target test_async test_binding test_core test_abi test_runtime \
            -j "${JOBS}"
        export TSAN_OPTIONS="halt_on_error=1 history_size=7"
        # Since fc87b2a all executables land in <build>/bin/; the older
        # modules/.../tests/ paths are obsolete.
        "./${TSAN_BUILD_DIR}/bin/test_async"
        "./${TSAN_BUILD_DIR}/bin/test_binding"
        "./${TSAN_BUILD_DIR}/bin/test_core"
        "./${TSAN_BUILD_DIR}/bin/test_abi"
        "./${TSAN_BUILD_DIR}/bin/test_runtime"
        echo "✓ TSan release-gate green"
        exit 0
        ;;
    android)
        # Android NDK cross-build: configure with Android toolchain,
        # build the JNI adapter + core libraries. Requires ANDROID_NDK_ROOT
        # or a standard SDK install at ~/Library/Android/sdk (macOS) or
        # $ANDROID_SDK_ROOT.
        echo "▶ android NDK cross-build"
        ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}"
        if [[ ! -d "$ANDROID_SDK_ROOT" ]]; then
            echo "✗ ANDROID_SDK_ROOT not found: $ANDROID_SDK_ROOT" >&2
            echo "  Set ANDROID_SDK_ROOT or install Android SDK" >&2
            exit 1
        fi
        # NDK: prefer ANDROID_NDK_ROOT, else auto-detect latest under SDK
        ANDROID_NDK_ROOT="${ANDROID_NDK_ROOT:-$ANDROID_SDK_ROOT/ndk/$(ls "$ANDROID_SDK_ROOT/ndk/" 2>/dev/null | sort -V | tail -1)}"
        ANDROID_NDK_ROOT="${ANDROID_NDK_ROOT%/}"
        if [[ ! -d "$ANDROID_NDK_ROOT" ]]; then
            echo "✗ Android NDK not found at: $ANDROID_NDK_ROOT" >&2
            exit 1
        fi
        # Use SDK-bundled CMake (system CMake may be incompatible with NDK)
        ARIA_ANDROID_CMAKE="${ARIA_ANDROID_CMAKE:-$ANDROID_SDK_ROOT/cmake/$(ls "$ANDROID_SDK_ROOT/cmake/" 2>/dev/null | sort -V | tail -1)/bin/cmake}"
        if [[ ! -x "$ARIA_ANDROID_CMAKE" ]]; then
            ARIA_ANDROID_CMAKE="$(command -v cmake 2>/dev/null || true)"
        fi
        if [[ -z "$ARIA_ANDROID_CMAKE" ]]; then
            echo "✗ CMake not found. Install CMake or set ARIA_ANDROID_CMAKE" >&2
            exit 1
        fi
        # Ninja: prefer the SDK-bundled ninja (same dir as its cmake), else PATH.
        # CMake won't auto-discover ninja if it isn't on PATH, and the Android
        # demo (5-android-jni-mvvm) links the STATIC libraries produced here.
        ARIA_ANDROID_NINJA="${ARIA_ANDROID_NINJA:-$(dirname "$ARIA_ANDROID_CMAKE")/ninja}"
        if [[ ! -x "$ARIA_ANDROID_NINJA" ]]; then
            ARIA_ANDROID_NINJA="$(command -v ninja 2>/dev/null || true)"
        fi
        if [[ -z "$ARIA_ANDROID_NINJA" ]]; then
            echo "✗ ninja not found. Install ninja or set ARIA_ANDROID_NINJA" >&2
            exit 1
        fi
        echo "  NDK   : $ANDROID_NDK_ROOT"
        echo "  CMake : $ARIA_ANDROID_CMAKE"
        echo "  Ninja : $ARIA_ANDROID_NINJA"
        mkdir -p "${BUILD_DIR}"
        # ARIA_BUILD_SHARED=OFF: Android consumers (5-android-jni-mvvm) link
        # STATIC .a archives; binding/runtime default to SHARED (.so) which
        # would break them.
        "$ARIA_ANDROID_CMAKE" -S . -B "${BUILD_DIR}" \
            -G Ninja \
            -DCMAKE_MAKE_PROGRAM="$ARIA_ANDROID_NINJA" \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake" \
            -DANDROID_ABI=arm64-v8a \
            -DANDROID_PLATFORM=android-21 \
            -DARIA_BUILD_JNI=ON \
            -DARIA_BUILD_SHARED=OFF \
            -DARIA_BUILD_TESTS=ON \
            -DARIA_BUILD_EXAMPLES=OFF \
            -DARIA_BUILD_QT6=OFF
        "$ARIA_ANDROID_CMAKE" --build "${BUILD_DIR}" -j "${JOBS}"
        echo "✓ android cross-build done"
        exit 0
        ;;
esac

# ── Mode → CMake options ─────────────────────────────────────────────────────
# Defaults shared by every non-package mode: framework + tests, no examples,
# no Qt adapter. Examples live in their own per-demo scripts.
COMMON_OPTS=(
    -DARIA_BUILD_EXAMPLES=OFF
)

DO_CTEST="NO"
DO_PACKAGE="NO"
DO_ARCHIVE="NO"

case "$MODE" in
    default)
        # Default: framework + tests + ctest + release package
        CMAKE_OPTS=(-DCMAKE_BUILD_TYPE=Release -DARIA_BUILD_TESTS=ON)
        DO_CTEST="YES"
        DO_PACKAGE="YES"
        ;;
    release)
        # Alias: identical to default
        CMAKE_OPTS=(-DCMAKE_BUILD_TYPE=Release -DARIA_BUILD_TESTS=ON)
        DO_CTEST="YES"
        DO_PACKAGE="YES"
        ;;
    debug)
        CMAKE_OPTS=(-DCMAKE_BUILD_TYPE=Debug -DARIA_BUILD_TESTS=ON)
        ;;
    asan)
        CMAKE_OPTS=(-DCMAKE_BUILD_TYPE=Debug
                    -DARIA_BUILD_TESTS=ON
                    -DARIA_ENABLE_ASAN=ON
                    -DARIA_ENABLE_UBSAN=ON)
        ;;
    tsan)
        CMAKE_OPTS=(-DCMAKE_BUILD_TYPE=Debug
                    -DARIA_BUILD_TESTS=ON
                    -DARIA_ENABLE_TSAN=ON)
        ;;
    tests)
        CMAKE_OPTS=(-DCMAKE_BUILD_TYPE=Release -DARIA_BUILD_TESTS=ON)
        DO_CTEST="YES"
        ;;
    pack-zip|pack|zip|tar|package)
        # Default flow + extra archive step
        CMAKE_OPTS=(-DCMAKE_BUILD_TYPE=Release -DARIA_BUILD_TESTS=ON)
        DO_CTEST="YES"
        DO_PACKAGE="YES"
        DO_ARCHIVE="YES"
        ;;
    *)
        echo "unknown mode: $MODE"
        echo "valid: (none) | release | debug | tests | asan | tsan | pack-zip | tsan-gate | android | clean"
        exit 1
        ;;
esac

CMAKE_OPTS+=("${COMMON_OPTS[@]}")

# ── Adapter auto-enable (adapter conformance tests are framework-core) ───────
# qt6_tests / appkit_conformance and friends are part of the framework's
# core conformance suite, but the examples that USE the adapter are not in
# this script's scope. Set ARIA_NO_QT6=1 / ARIA_NO_APPKIT=1 to disable.

# Qt6 adapter — probe Homebrew Qt (macOS) or QT_DIR
if [[ "${ARIA_NO_QT6:-}" != "1" ]]; then
    QT_DIR_DEFAULT=""
    if [[ "$(uname)" == "Darwin" ]]; then
        for c in "/opt/homebrew/opt/qt" "/usr/local/opt/qt" "/opt/homebrew/opt/qt@6"; do
            if [[ -d "$c" ]]; then QT_DIR_DEFAULT="$c"; break; fi
        done
    fi
    QT_DIR_FINAL="${QT_DIR:-$QT_DIR_DEFAULT}"
    if [[ -n "$QT_DIR_FINAL" && -d "$QT_DIR_FINAL" ]]; then
        CMAKE_OPTS+=(-DARIA_BUILD_QT6=ON -DCMAKE_PREFIX_PATH="$QT_DIR_FINAL")
        echo "▶ Qt6 detected at ${QT_DIR_FINAL} — adapter + qt6_tests enabled"
    fi
fi

# AppKit adapter — host-side conformance tests on macOS
if [[ "$(uname)" == "Darwin" && "${ARIA_NO_APPKIT:-}" != "1" ]]; then
    CMAKE_OPTS+=(-DARIA_BUILD_APPKIT=ON)
    echo "▶ macOS — AppKit adapter + appkit_conformance enabled"
fi

# On MSYS2 / MinGW prefer Ninja if present (avoids accidental MSVC pickup).
GENERATOR_ARGS=()
if [[ -n "${MSYSTEM:-}" ]] && command -v ninja >/dev/null 2>&1; then
    GENERATOR_ARGS=(-G Ninja)
    echo "▶ MSYS2 (${MSYSTEM}) detected — using Ninja generator"
fi

echo "▶ configuring (${MODE}) with ${JOBS} jobs"
cmake -S . -B "${BUILD_DIR}" ${GENERATOR_ARGS[@]+"${GENERATOR_ARGS[@]}"} "${CMAKE_OPTS[@]}"

echo "▶ building"
cmake --build "${BUILD_DIR}" -j "${JOBS}"

if [[ "$DO_CTEST" == "YES" ]]; then
    echo "▶ running ctest"
    ctest --test-dir "${BUILD_DIR}" --output-on-failure
fi

if [[ "$DO_PACKAGE" == "YES" ]]; then
    echo "▶ packaging release"
    cmake --build "${BUILD_DIR}" --target package-release
fi

if [[ "$DO_ARCHIVE" == "YES" ]]; then
    echo "▶ creating archive"
    cmake --build "${BUILD_DIR}" --target package-archive
    echo ""
    echo "✓ Archive created:"
    ls -lh build/dist/archives/*.* 2>/dev/null || true
fi

echo "✓ ${MODE} done"
