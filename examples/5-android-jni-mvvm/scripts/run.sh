#!/usr/bin/env bash
# examples/5-android-jni-mvvm/scripts/run.sh — build & install the Android JNI + Compose demo
#
# 用法：
#   examples/5-android-jni-mvvm/scripts/run.sh                        # Debug, 安装到设备
#   examples/5-android-jni-mvvm/scripts/run.sh Release               # Release
#   examples/5-android-jni-mvvm/scripts/run.sh Debug --no-launch     # 只构建不安装
#   examples/5-android-jni-mvvm/scripts/run.sh Debug --probe          # 构建 → 安装 → 启动 → 验证
#
# 环境变量：
#   ANDROID_SDK_ROOT   Android SDK 路径（默认自动探测）
#   ANDROID_NDK_ROOT   Android NDK 路径（默认自动探测）
#   ARIA_JNI_CMAKE     CMake 可执行文件路径（默认用 SDK 自带的 cmake）
#   JOBS               并行任务数（默认 CPU 核心数）
#
# Build dir : examples/5-android-jni-mvvm/app/build/   (Gradle 管理)

set -euo pipefail

# ── 颜色 ──────────────────────────────────────────────────────────────────────
if [[ -t 1 ]]; then
    C_BOLD=$'\033[1m'; C_GREEN=$'\033[32m'; C_YELLOW=$'\033[33m'
    C_BLUE=$'\033[34m'; C_RED=$'\033[31m'; C_RESET=$'\033[0m'
else
    C_BOLD=""; C_GREEN=""; C_YELLOW=""; C_BLUE=""; C_RED=""; C_RESET=""
fi

log()  { echo "${C_BLUE}${C_BOLD}[demo5]${C_RESET} $*"; }
ok()   { echo "${C_GREEN}${C_BOLD}[demo5]${C_RESET} $*"; }
warn() { echo "${C_YELLOW}${C_BOLD}[demo5]${C_RESET} $*"; }
err()  { echo "${C_RED}${C_BOLD}[demo5]${C_RESET} $*"; }

# ── 参数 ──────────────────────────────────────────────────────────────────────
BUILD_TYPE="${1:-Debug}"
NO_RUN=0
PROBE=0

for arg in "$@"; do
    case "$arg" in
        --no-launch|--no-run) NO_RUN=1 ;;
        --probe)              PROBE=1 ;;
    esac
done

JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

# ── 定位项目根 ────────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEMO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$DEMO_ROOT/../.." && pwd)"

log "仓库根  : $REPO_ROOT"
log "Demo 根 : $DEMO_ROOT"
log "构建类型: $BUILD_TYPE"

# ── 探测 Android SDK / NDK ───────────────────────────────────────────────────
ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}"
if [[ ! -d "$ANDROID_SDK_ROOT" ]]; then
    err "ANDROID_SDK_ROOT 不存在: $ANDROID_SDK_ROOT"
    exit 1
fi
export ANDROID_SDK_ROOT

# NDK: 优先用环境变量，否则在 SDK 下找最新版本
ANDROID_NDK_ROOT="${ANDROID_NDK_ROOT:-$ANDROID_SDK_ROOT/ndk/$(ls "$ANDROID_SDK_ROOT/ndk/" 2>/dev/null | sort -V | tail -1)}"
ANDROID_NDK_ROOT="${ANDROID_NDK_ROOT%/}"  # 去掉尾部 /
export ANDROID_NDK_ROOT

# ── 验证 NDK ────────────────────────────────────────────────────────────────
if [[ ! -d "$ANDROID_NDK_ROOT" ]]; then
    err "ANDROID_NDK_ROOT 不存在: $ANDROID_NDK_ROOT"
    exit 1
fi

# ── 定位 ndk-build 脚本 ──────────────────────────────────────────────────────
NDK_BUILD="$ANDROID_NDK_ROOT/ndk-build"
if [[ ! -x "$NDK_BUILD" ]]; then
    err "ndk-build 不存在或不可执行: $NDK_BUILD"
    exit 1
fi

# ── CMake（用 SDK 自带的，系统 CMake 可能与 NDK 不兼容）──────────────────────
ARIA_JNI_CMAKE="${ARIA_JNI_CMAKE:-$ANDROID_SDK_ROOT/cmake/$(ls "$ANDROID_SDK_ROOT/cmake/" 2>/dev/null | sort -V | tail -1)/bin/cmake}"
if [[ ! -x "$ARIA_JNI_CMAKE" ]]; then
    ARIA_JNI_CMAKE="$(command -v cmake 2>/dev/null || true)"
fi
if [[ -z "$ARIA_JNI_CMAKE" ]]; then
    err "找不到 CMake。请安装 CMake 或设置 ARIA_JNI_CMAKE"
    exit 1
fi

log "CMake   : $ARIA_JNI_CMAKE"

# ── Gradle ──────────────────────────────────────────────────────────────────
GRADLE_CMD="$DEMO_ROOT/gradlew"
if [[ ! -x "$GRADLE_CMD" ]]; then
    err "gradlew 不可执行: $GRADLE_CMD"
    exit 1
fi

# ── 构建 ────────────────────────────────────────────────────────────────────
log "开始 Gradle 构建..."

cd "$DEMO_ROOT"
"$GRADLE_CMD" clean assembleDebug --info 2>&1 | tee /tmp/demo5-gradle.log

if [[ "$NO_RUN" == "1" ]]; then
    ok "构建完成（--no-launch）"
    exit 0
fi

# ── 安装到设备 ──────────────────────────────────────────────────────────────
APK_DIR="app/build/outputs/apk/debug"
APK_PATH="$(ls "$DEMO_ROOT/$APK_DIR/"*.apk 2>/dev/null | head -1 || true)"
if [[ -z "$APK_PATH" ]]; then
    err "APK 不存在"
    exit 1
fi

log "安装 APK: $APK_PATH"

adb install -r "$APK_PATH"

# ── 启动应用 ────────────────────────────────────────────────────────────────
PACKAGE_NAME="com.example.aria.demo5"
ACTIVITY_NAME=".MainActivity"

log "启动应用: $ACTIVITY_NAME"
adb shell am start -n "$PACKAGE_NAME/$ACTIVITY_NAME"

# ── 验证 ────────────────────────────────────────────────────────────────────
if [[ "$PROBE" == "1" ]]; then
    sleep 2
    FRONT=$(adb shell dumpsys window windows 2>/dev/null | grep -E 'mCurrentFocus' | head -1)
    if [[ "$FRONT" == *"$PACKAGE_NAME"* ]]; then
        ok "应用已启动并处于前台"
    else
        err "应用可能未成功启动"
        exit 1
    fi
fi

ok "完成"
