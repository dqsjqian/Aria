#!/usr/bin/env bash
# run.sh — One-shot build & launch for the iOS UIKit demo.
#
# 与 demo2 最大的区别：
#   1) iOS Simulator 不能链接 macOS 的 libaria_*.dylib（SDK 不匹配），
#      所以 demo3 的 Xcode 工程直接把 aria 核心源码 (*.cpp) 内联编译，
#      不再依赖外部 dylib —— 这个脚本也就不需要先跑 CMake 构建 aria。
#   2) iOS 没有 open app，要走 simctl：boot 模拟器 → install → launch。
#
# What it does:
#   1. xcodebuild 构建 ios-oc-mvvm.app (iOS Simulator)
#   2. 找/启动一个合适的 iPhone 模拟器
#   3. simctl install / launch
#
# Usage:
#   scripts/run.sh                       # Debug, 默认 iPhone 17 Pro Max
#   scripts/run.sh Release
#   scripts/run.sh Debug --device "iPhone 17 Pro Max"
#   scripts/run.sh Debug --no-launch     # 只构建，不 install/launch（VSCode task 用）
#
set -euo pipefail

CONFIG="${1:-Debug}"
shift || true

LAUNCH=1
DEVICE_NAME="iPhone 17 Pro Max"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-launch) LAUNCH=0; shift ;;
        --device)    DEVICE_NAME="$2"; shift 2 ;;
        *) echo "unknown arg: $1"; exit 1 ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
ARIA_ROOT="$(cd "$PROJECT_DIR/../.." && pwd)"

cyan()  { printf '\033[0;36m%s\033[0m\n' "$*"; }
green() { printf '\033[0;32m%s\033[0m\n' "$*"; }
red()   { printf '\033[0;31m%s\033[0m\n' "$*" >&2; }

# ── 0. 准备 Xcode 工具链 ───────────────────────────────────────────────
# xcodebuild 和 xcrun simctl 都需要**完整的 Xcode**；如果机器的 active
# developer dir 指向 Command Line Tools (CLT)，两者都会失败。我们不去改系统级
# 的 `xcode-select`（那需要 sudo），而是按需为本脚本设置 DEVELOPER_DIR ——
# 逐进程覆盖，无需管理员权限，也不影响机器上其它工具。
ensure_xcode() {
    if xcodebuild -version >/dev/null 2>&1; then
        return 0   # 当前 active dir 已是可用的完整 Xcode
    fi
    local lic_hit=0 dev cur app out
    local candidates=()
    [[ -n "${DEVELOPER_DIR:-}" ]] && candidates+=("$DEVELOPER_DIR")
    cur="$(xcode-select -p 2>/dev/null || true)"
    [[ "$cur" == *.app/Contents/Developer ]] && candidates+=("$cur")
    for app in /Applications/Xcode.app /Applications/Xcode-beta.app /Applications/Xcode*.app; do
        [[ -d "$app" ]] && candidates+=("$app/Contents/Developer")
    done
    # bash 3.2（macOS 自带）下空数组 + set -u 会报错，用 ${arr[@]+...} 守卫。
    for dev in ${candidates[@]+"${candidates[@]}"}; do
        [[ -x "$dev/usr/bin/xcodebuild" ]] || continue
        if out="$(DEVELOPER_DIR="$dev" xcodebuild -version 2>&1)"; then
            export DEVELOPER_DIR="$dev"
            green "    自动选用 Xcode: $DEVELOPER_DIR"
            return 0
        fi
        [[ "$out" == *icense* ]] && lic_hit=1
    done
    if [[ $lic_hit -eq 1 ]]; then
        red "❌ 找到了 Xcode，但其许可协议尚未接受。请执行一次：sudo xcodebuild -license accept，然后重新运行。"
    else
        red "❌ 找不到可用的完整 Xcode（xcodebuild / simctl 需要它，Command Line Tools 不行）。"
        red "   1) 从 App Store 安装 Xcode；"
        red "   2) 临时(无需 sudo): export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer"
        red "      或永久(需 sudo): sudo xcode-select -s /Applications/Xcode.app/Contents/Developer"
        red "   3) 首次使用可能还需: sudo xcodebuild -runFirstLaunch && sudo xcodebuild -license accept"
    fi
    return 1
}
ensure_xcode || exit 1

# ── 1. xcodebuild (iOS Simulator) ─────────────────────────────────────────
cyan "[1/4] xcodebuild ios-oc-mvvm ($CONFIG, iphonesimulator) …"

# 挑一个真实存在的 simulator 作为 destination。先找 Booted 的；
# 没有就按名字精确匹配一个。
DESTINATION="platform=iOS Simulator,name=${DEVICE_NAME}"

xcodebuild -project "${PROJECT_DIR}/ios-oc-mvvm.xcodeproj" \
    -scheme ios-oc-mvvm \
    -configuration "$CONFIG" \
    -destination "$DESTINATION" \
    -sdk iphonesimulator \
    ONLY_ACTIVE_ARCH=YES \
    -quiet build

# 找到构建产物
DERIVED=$(xcodebuild -project "${PROJECT_DIR}/ios-oc-mvvm.xcodeproj" \
    -scheme ios-oc-mvvm \
    -configuration "$CONFIG" \
    -destination "$DESTINATION" \
    -sdk iphonesimulator \
    -showBuildSettings 2>/dev/null \
    | awk -F'= ' '/^ *BUILT_PRODUCTS_DIR/ {print $2; exit}')
APP="${DERIVED}/ios-oc-mvvm.app"

if [[ ! -d "$APP" ]]; then
    red "❌ 找不到构建产物: $APP"
    exit 1
fi
green "    → $APP"

# 维护一个固定 symlink，方便 VSCode / 其它工具指向最新产物
DEMO3_BUILD="${ARIA_ROOT}/build/examples/3-ios-oc-uikit-mvvm"
LINK="${DEMO3_BUILD}/demo3.app"
mkdir -p "${DEMO3_BUILD}"
ln -sfn "$APP" "$LINK"
green "    Symlink: $LINK -> $APP"

if [[ $LAUNCH -eq 0 ]]; then
    green "跳过 install/launch（--no-launch）"
    echo "APP_BUNDLE=$APP"
    exit 0
fi

# ── 2. 选一个模拟器 UDID ──────────────────────────────────────────────────
cyan "[2/4] 查找 iOS Simulator: $DEVICE_NAME …"

# 取名称精确匹配 "<DEVICE_NAME> ("（防止 "iPhone 17" 误中 "iPhone 17 Pro"）的 iOS 设备。
# 注意：macOS 默认 awk 是 BSD awk，不支持 gawk 的 {36} 计数量词和 match() 第三参数，
#       所以这里：awk 只做「过滤 iOS 段里的设备行」，UDID 的提取交给 sed -E 做。
# xcrun simctl list devices available 输出示例：
#   -- iOS 17.5 --
#       iPhone 17 Pro Max (A63E7294-C3A9-4A26-B531-F903A3E9124B) (Shutdown)
#   -- watchOS 10.5 --
#       Apple Watch ...
UDID=$(xcrun simctl list devices available \
    | awk -v target="$DEVICE_NAME (" '
        /^-- iOS/              { in_ios = 1; next }
        /^-- /                 { in_ios = 0; next }
        in_ios && index($0, target) { print }
    ' \
    | sed -E 's/.*\(([0-9A-Fa-f-]+)\).*/\1/' \
    | head -1)

if [[ -z "$UDID" ]]; then
    red "❌ 找不到名为 '$DEVICE_NAME' 的 iOS Simulator"
    red "   可用设备列表:"
    xcrun simctl list devices available | grep -E '^\s+iPhone|^\s+iPad' | head -10 >&2
    exit 1
fi
green "    UDID = $UDID"

# ── 3. Boot 模拟器 ───────────────────────────────────────────────────────
cyan "[3/4] Boot 模拟器 (如已 booted 会跳过) …"
STATE=$(xcrun simctl list devices | grep "$UDID" | sed -E 's/.*\((Booted|Shutdown)\).*/\1/' | head -1)
if [[ "$STATE" != "Booted" ]]; then
    xcrun simctl boot "$UDID"
    # 打开 Simulator.app 让界面可见
    open -a Simulator --args -CurrentDeviceUDID "$UDID"
    # 等待 boot 完成（最多 30s）
    for i in {1..30}; do
        S=$(xcrun simctl list devices | grep "$UDID" | sed -E 's/.*\((Booted|Shutdown)\).*/\1/' | head -1)
        [[ "$S" == "Booted" ]] && break
        sleep 1
    done
else
    # 已 Booted 也确保 Simulator.app 前台可见
    open -a Simulator
fi
green "    Booted: $UDID"

# ── 4. Install & Launch ──────────────────────────────────────────────────
cyan "[4/4] install & launch …"
xcrun simctl install "$UDID" "$APP"

BUNDLE_ID=$(defaults read "$APP/Info" CFBundleIdentifier 2>/dev/null || echo "com.example.ios-oc-mvvm")
green "    Bundle ID: $BUNDLE_ID"

xcrun simctl launch --console-pty "$UDID" "$BUNDLE_ID" || true

green "完成: $APP"
# 让 VSCode debugger 能从 stdout 解析到 app bundle
echo "APP_BUNDLE=$APP"
echo "SIMULATOR_UDID=$UDID"
echo "BUNDLE_ID=$BUNDLE_ID"
