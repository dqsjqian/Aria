#!/usr/bin/env bash
# examples/1-qt-showcase/scripts/run.sh — build & run the Qt showcase demo
#
# 用法：
#   examples/1-qt-showcase/scripts/run.sh                    # Debug, 弹 GUI
#   examples/1-qt-showcase/scripts/run.sh Release            # Release, 弹 GUI
#   examples/1-qt-showcase/scripts/run.sh Debug probe        # Debug, probe 模式（跑完 9 tab 自动退出，适合 CI）
#   examples/1-qt-showcase/scripts/run.sh Debug --no-launch  # 只构建不运行
#
# 环境变量：
#   QT_DIR   自定义 Qt 路径（默认 /opt/homebrew/opt/qt）
#   JOBS     并行任务数（默认 8）

set -euo pipefail

# ── 颜色 ──────────────────────────────────────────────────────────────────────
if [[ -t 1 ]]; then
    C_BOLD=$'\033[1m'; C_GREEN=$'\033[32m'; C_YELLOW=$'\033[33m'
    C_BLUE=$'\033[34m'; C_RESET=$'\033[0m'
else
    C_BOLD=""; C_GREEN=""; C_YELLOW=""; C_BLUE=""; C_RESET=""
fi

log()  { echo "${C_BLUE}${C_BOLD}[demo1]${C_RESET} $*"; }
ok()   { echo "${C_GREEN}${C_BOLD}[demo1]${C_RESET} $*"; }
warn() { echo "${C_YELLOW}${C_BOLD}[demo1]${C_RESET} $*"; }

# ── 参数 ──────────────────────────────────────────────────────────────────────
BUILD_TYPE="${1:-Debug}"
MODE="${2:-normal}"           # normal | probe
NO_RUN=0
for arg in "$@"; do
    if [[ "$arg" == "--no-launch" || "$arg" == "--no-run" ]]; then NO_RUN=1; fi
done

QT_DIR="${QT_DIR:-/opt/homebrew/opt/qt}"
JOBS="${JOBS:-8}"

# ── 定位仓库根 ────────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEMO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$DEMO_ROOT/../.." && pwd)"
# Demo's own build tree (standalone: only this demo's objects, not the
# framework). The framework SDK is built once into build/flavors/sdk/ and
# installed to build/dist/tree/; the demo links it via find_package.
BUILD_DIR="$REPO_ROOT/build/flavors/qt-demo"
SDK_TREE="$REPO_ROOT/build/flavors/sdk"
SDK_PREFIX="$REPO_ROOT/build/dist/tree"
# The .app lands directly under the standalone build tree's root on macOS
# (CMAKE_RUNTIME_OUTPUT_DIRECTORY is only set by the *framework* CMakeLists,
# which is not involved in standalone mode).
APP_PATH="$BUILD_DIR/ex_qt_showcase.app/Contents/MacOS/ex_qt_showcase"

log "仓库根  : $REPO_ROOT"
log "构建类型: $BUILD_TYPE"
log "Qt 路径 : $QT_DIR"
log "并行度  : $JOBS"

# ── 预检 ──────────────────────────────────────────────────────────────────────
if [[ ! -d "$QT_DIR" ]]; then
    warn "Qt 目录不存在：$QT_DIR"
    warn "请用  brew install qt  或设置  QT_DIR=/path/to/qt"
    exit 1
fi
command -v cmake >/dev/null 2>&1 || { warn "cmake 未安装"; exit 1; }

# ── 确保框架 SDK 已构建并安装 ────────────────────────────────────────────────
# 框架只构建一次（build/flavors/sdk/），install 到 build/dist/tree/，所有
# CMake demo 共享。任何框架源码比已安装的 SDK 新就重建——否则 demo 会链接
# 旧 dylib 报 undefined symbol（见 0f142ec 的教训）。
ensure_sdk() {
    local sdk_config="$SDK_PREFIX/lib/cmake/aria/ariaConfig.cmake"
    local newest_src newest_lib
    if [[ ! -f "$sdk_config" ]]; then
        need_sdk=1
    else
        newest_src="$(find "${REPO_ROOT}/modules" \
            -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.mm' \) \
            -not -path '*/tests/*' -not -path '*/fuzz/*' \
            -exec stat -f '%m' {} + 2>/dev/null | sort -rn | head -1)"
        newest_lib="$(stat -f '%m' "$sdk_config" 2>/dev/null)"
        need_sdk=0
        if [[ -n "$newest_src" && -n "$newest_lib" && "$newest_src" -gt "$newest_lib" ]]; then
            need_sdk=1
        fi
    fi
    if [[ "$need_sdk" == "1" ]]; then
        log "构建框架 SDK → $SDK_PREFIX ..."
        # UIKit adapter is NOT built here: it requires an iOS toolchain,
        # and demo3 (Xcode) compiles it directly without the installed SDK.
        cmake -S "$REPO_ROOT" -B "$SDK_TREE" \
            -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
            -DARIA_BUILD_QT6=ON \
            -DARIA_BUILD_HTTP=ON \
            -DARIA_BUILD_APPKIT=ON \
            -DARIA_BUILD_EXAMPLES=OFF \
            -DARIA_BUILD_TESTS=OFF \
            -DARIA_BUILD_BENCHMARK=OFF \
            >/dev/null
        cmake --build "$SDK_TREE" -j "$JOBS" >/dev/null
        # Full re-install: clear lib/ + include/ first so CMake never sees
        # up-to-date dylibs and skips its install-time rpath rewrite. On a
        # partial rebuild, skipping the rewrite leaves stale rpaths in the
        # freshly linked dylibs and install_name_tool then errors on the
        # next run ("no LC_RPATH ... required for -delete_rpath").
        cmake -E rm -rf "$SDK_PREFIX/lib" "$SDK_PREFIX/include"
        cmake --install "$SDK_TREE" --prefix "$SDK_PREFIX" >/dev/null
        ok "框架 SDK 就绪：$SDK_PREFIX"
    fi
}
ensure_sdk

# ── Configure（standalone：只配 demo 自己，链接已安装 SDK）────────────────────
log "配置 CMake..."
cmake -S "$DEMO_ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DARIA_USE_INSTALLED=ON \
    -DCMAKE_PREFIX_PATH="$SDK_PREFIX;$QT_DIR" \
    >/dev/null

# ── Build ─────────────────────────────────────────────────────────────────────
log "编译 ex_qt_showcase..."
cmake --build "$BUILD_DIR" --target ex_qt_showcase --config "$BUILD_TYPE" -j "$JOBS"

# ── 找到最终产物 ──────────────────────────────────────────────────────────────
if [[ ! -x "$APP_PATH" ]]; then
    warn "构建成功但没找到可执行文件：$APP_PATH"
    exit 1
fi
ok "构建完成：$APP_PATH"

# ── Run ───────────────────────────────────────────────────────────────────────
if [[ "$NO_RUN" == "1" ]]; then
    log "--no-launch 已指定，不启动应用"
    exit 0
fi

if [[ "$MODE" == "probe" ]]; then
    log "以 probe 模式启动（跑完 9 tab 自动退出）..."
    ARIA_PROBE=1 "$APP_PATH"
else
    log "启动 GUI..."
    "$APP_PATH"
fi
