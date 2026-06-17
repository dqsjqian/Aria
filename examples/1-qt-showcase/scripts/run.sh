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
BUILD_DIR="$REPO_ROOT/build/examples/1-qt-showcase"
# Per-demo isolated build dir under build/examples/<name>/ so each demo's
# cmake cache does not collide with framework or other demos' configs.
APP_PATH_NEW="$BUILD_DIR/bin/ex_qt_showcase.app/Contents/MacOS/ex_qt_showcase"
APP_PATH_OLD="$BUILD_DIR/examples/1-qt-showcase/ex_qt_showcase.app/Contents/MacOS/ex_qt_showcase"

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

# ── Configure ─────────────────────────────────────────────────────────────────
log "配置 CMake..."
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_PREFIX_PATH="$QT_DIR" \
    -DARIA_BUILD_QT6=ON \
    -DARIA_BUILD_EXAMPLES=ON \
    -DARIA_BUILD_TESTS=OFF \
    >/dev/null

# ── Build ─────────────────────────────────────────────────────────────────────
log "编译 ex_qt_showcase..."
cmake --build "$BUILD_DIR" --target ex_qt_showcase --config "$BUILD_TYPE" -j "$JOBS"

# ── 找到最终产物（fc87b2a 之后用 build/bin/，老 build 缓存可能在 examples/.../） ─
APP_PATH=""
for p in "$APP_PATH_NEW" "$APP_PATH_OLD"; do
    if [[ -x "$p" ]]; then APP_PATH="$p"; break; fi
done
if [[ -z "$APP_PATH" ]]; then
    warn "构建成功但没找到可执行文件，候选路径："
    warn "  $APP_PATH_NEW"
    warn "  $APP_PATH_OLD"
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
