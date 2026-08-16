#!/usr/bin/env bash
# run.sh — One-shot build & launch for the AppKit demo.
#
# What it does:
#   1. 确保框架 SDK 已安装到 build/dist/sdk/（与 demo1/demo4 共享同一个
#      SDK 树，框架只构建一次；Xcode 工程通过 LIBRARY_SEARCH_PATHS 链接它）。
#   2. xcodebuild 构建 mac-oc-mvvm.app。
#   3. 把 Aria 的 dylib 从 SDK 拷到 app 的 Contents/Frameworks/（app 默认
#      rpath 是 @executable_path/../Frameworks，dylib 是 @rpath/libaria_*.dylib）。
#   4. 重新 ad-hoc 签名（避免 dylib 跟 bundle 签名不一致）。
#   5. open app。
#
# 本 demo 是纯 Xcode 构建：产物（含 demo2.app symlink）在 build/flavors/appkit-demo/，
# 不再需要任何 per-demo CMake 树。
#
# Usage:
#   scripts/run.sh             # Debug，构建 + open
#   scripts/run.sh Release
#   scripts/run.sh Debug --no-launch   # 只构建，不 open（给 VSCode task 用）
#
set -euo pipefail

CONFIG="${1:-Debug}"
LAUNCH=1
for arg in "$@"; do
    if [[ "$arg" == "--no-launch" ]]; then LAUNCH=0; fi
done

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
ARIA_ROOT="$(cd "$PROJECT_DIR/../.." && pwd)"
# Demo's own tree: this is a pure Xcode build, so the tree only holds the
# xcodebuild products + the final .app symlink. The framework dylibs come
# from the shared SDK install (build/flavors/sdk → build/dist/sdk), which
# demo1/demo4 build and install — we reuse it instead of compiling a second
# copy of the framework.
BUILD_DIR="${ARIA_ROOT}/build/flavors/appkit-demo"
SDK_PREFIX="${ARIA_ROOT}/build/dist/sdk"

cyan() { printf '\033[0;36m%s\033[0m\n' "$*"; }
green() { printf '\033[0;32m%s\033[0m\n' "$*"; }

# ── 0. 准备 Xcode 工具链 ───────────────────────────────────────────────
# xcodebuild 需要**完整的 Xcode**；如果机器的 active developer dir 指向
# Command Line Tools (CLT)，xcodebuild 会直接报错。我们不去改系统级的
# `xcode-select`（那需要 sudo），而是按需为本脚本的 xcodebuild 调用设置
# DEVELOPER_DIR —— 这是逐进程覆盖，无需管理员权限，也不影响机器上其它工具。
# （提前到构建 Aria 之前，因为下面要用 xcodebuild 读工程的部署目标。）
ensure_xcode() {
    # 当前 active dir 已经是可用的完整 Xcode → 不动。
    if xcodebuild -version >/dev/null 2>&1; then
        return 0
    fi
    local lic_hit=0 dev cur app out
    local candidates=()
    # 优先顺序：已设置的 DEVELOPER_DIR > xcode-select 当前值（若指向 .app）
    #          > /Applications 下的 Xcode / Xcode-beta。
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
        [[ "$out" == *icense* ]] && lic_hit=1   # 命中 license / License 未接受
    done
    if [[ $lic_hit -eq 1 ]]; then
        cat >&2 <<'MSG'
❌ 找到了 Xcode，但其许可协议尚未接受。请执行一次：
     sudo xcodebuild -license accept
   然后重新运行本脚本。
MSG
    else
        cat >&2 <<'MSG'
❌ 找不到可用的完整 Xcode（xcodebuild 需要它，Command Line Tools 不行）。
   1) 从 App Store 安装 Xcode；
   2) 二选一指向它：
        临时(无需 sudo):  export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
        永久(需 sudo):    sudo xcode-select -s /Applications/Xcode.app/Contents/Developer
   3) 首次使用可能还需：  sudo xcodebuild -runFirstLaunch && sudo xcodebuild -license accept
MSG
    fi
    return 1
}
ensure_xcode || exit 1

# 部署目标：CMake 构建的 Aria dylib 必须与 Xcode 工程的
# MACOSX_DEPLOYMENT_TARGET 对齐，否则链接时会出现
#   "building for macOS-X.0, but linking with dylib ... built for newer version Y.0"
# 的告警。从工程动态读取（读不到则回退到 13.0）。
DEPLOY_TARGET="$(xcodebuild -project "${PROJECT_DIR}/mac-oc-mvvm.xcodeproj" \
    -scheme mac-oc-mvvm -configuration "$CONFIG" -showBuildSettings 2>/dev/null \
    | awk -F'= ' '/ MACOSX_DEPLOYMENT_TARGET =/ {print $2; exit}')"
DEPLOY_TARGET="${DEPLOY_TARGET//[[:space:]]/}"
DEPLOY_TARGET="${DEPLOY_TARGET:-13.0}"

# ── 1. 确保框架 SDK 已构建并安装 ────────────────────────────────────────
# 与 demo1/demo4 的 run.sh 共用同一个 SDK 树（build/flavors/sdk →
# build/dist/sdk），框架只构建一次。任何框架源码比已安装的 SDK 新就重建
# ——否则 Xcode 链接旧 dylib 会报 undefined symbol（见 0f142ec 的教训）。
ensure_sdk() {
    local sdk_config="$SDK_PREFIX/lib/cmake/aria/ariaConfig.cmake"
    local newest_src newest_lib
    if [[ ! -f "$sdk_config" ]]; then
        need_sdk=1
    else
        newest_src="$(find "${ARIA_ROOT}/modules" \
            -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.mm' \) \
            -not -path '*/tests/*' -not -path '*/fuzz/*' \
            -exec stat -f '%m' {} + 2>/dev/null | sort -rn | head -1)"
        # Compare against the actual build artifact, not ariaConfig.cmake:
        # CMake install keeps the source mtime, and configure_package_config_file
        # only regenerates when its template changes — the config's mtime can be
        # arbitrarily older than the real build, which would force a rebuild
        # on every run. A relinked dylib always gets a fresh mtime.
        newest_lib="$(stat -f '%m' "$SDK_PREFIX/lib/libaria_runtime.dylib" 2>/dev/null)"
        need_sdk=0
        if [[ -n "$newest_src" && -n "$newest_lib" && "$newest_src" -gt "$newest_lib" ]]; then
            need_sdk=1
        fi
    fi
    if [[ "$need_sdk" == "1" ]]; then
        cyan "[1/4] 构建框架 SDK → $SDK_PREFIX …"
        # UIKit adapter is NOT built here: it requires an iOS toolchain,
        # and demo3 (Xcode) compiles it directly without the installed SDK.
        cmake -S "${ARIA_ROOT}" -B "${ARIA_ROOT}/build/flavors/sdk" \
            -DCMAKE_BUILD_TYPE="$CONFIG" \
            -DARIA_BUILD_QT6=ON \
            -DARIA_BUILD_HTTP=ON \
            -DARIA_BUILD_APPKIT=ON \
            -DARIA_BUILD_EXAMPLES=OFF \
            -DARIA_BUILD_TESTS=OFF \
            -DARIA_BUILD_BENCHMARK=OFF \
            > /dev/null
        cmake --build "${ARIA_ROOT}/build/flavors/sdk" -j > /dev/null
        # Full re-install: clear lib/ + include/ first so CMake never sees
        # up-to-date dylibs and skips its install-time rpath rewrite. On a
        # partial rebuild, skipping the rewrite leaves stale rpaths in the
        # freshly linked dylibs and install_name_tool then errors on the
        # next run ("no LC_RPATH ... required for -delete_rpath").
        cmake -E rm -rf "$SDK_PREFIX/lib" "$SDK_PREFIX/include"
        cmake --install "${ARIA_ROOT}/build/flavors/sdk" --prefix "$SDK_PREFIX" > /dev/null
        green "    框架 SDK 就绪：$SDK_PREFIX"
    else
        green "[1/4] 框架 SDK 已就绪：$SDK_PREFIX"
    fi
}
ensure_sdk

# 部署目标一致性检查（不再需要 CMake 缓存：dylib 的 min-os 版本由 SDK 树
# 决定；SDK 用默认部署目标构建，Xcode 工程读到的目标若更高会链接告警。
# 这里只打印提示，不阻断——SDK 树与 demo 的部署目标都在 macOS 13 档位）。

# 强制用机器的**真实硬件架构**（不是 shell 架构 —— 如果终端以 Rosetta 打开，
# `uname -m` 会返回 x86_64 即使机器是 Apple Silicon）。避免 xcodebuild 在
# destination 列表里挑错 arch 导致跟 CMake 产出的 dylib 架构不匹配。
# ONLY_ACTIVE_ARCH=YES 让它尊重 -arch 并跳过其它。
if [[ "$(sysctl -n hw.optional.arm64 2>/dev/null)" == "1" ]]; then
    HOST_ARCH="arm64"
else
    HOST_ARCH="x86_64"
fi
cyan "[2/4] xcodebuild mac-oc-mvvm ($CONFIG, arch=$HOST_ARCH) …"
xcodebuild -project "${PROJECT_DIR}/mac-oc-mvvm.xcodeproj" \
    -scheme mac-oc-mvvm -configuration "$CONFIG" \
    -arch "$HOST_ARCH" \
    ONLY_ACTIVE_ARCH=YES \
    -quiet build

# 找到构建产物
DERIVED=$(xcodebuild -project "${PROJECT_DIR}/mac-oc-mvvm.xcodeproj" \
    -scheme mac-oc-mvvm -configuration "$CONFIG" \
    -arch "$HOST_ARCH" \
    -showBuildSettings 2>/dev/null \
    | awk -F'= ' '/^ *BUILT_PRODUCTS_DIR/ {print $2; exit}')
APP="${DERIVED}/mac-oc-mvvm.app"

if [[ ! -d "$APP" ]]; then
    echo "❌ 找不到构建产物: $APP" >&2
    exit 1
fi
green "    → $APP"

# ── 3. 把 Aria 的 dylib 拷到 app 的 Frameworks/ ───────────────────────
cyan "[3/4] 拷贝 Aria 动态库到 app/Contents/Frameworks/ …"
FW="${APP}/Contents/Frameworks"
mkdir -p "$FW"

# dylib 的 install name 是 @rpath/libaria_xxx.<SOVERSION>.dylib，
# 真文件是 .<full-version>.dylib，两者都要在 Frameworks 里（symlink + 实体）。
# SDK install 把所有共享库放在 ${SDK_PREFIX}/lib/。
# 用 cp -P 跟着 dylib 全家（实体 + 各级 symlink）一起拷过来。
SRC_DIR="${SDK_PREFIX}/lib"
for mod in binding runtime; do
    # 把 libaria_<mod>{.dylib,.<n>.dylib,.<n>.<m>.<p>.dylib} 全拷一遍
    for f in "${SRC_DIR}/libaria_${mod}".dylib "${SRC_DIR}/libaria_${mod}".*.dylib; do
        [[ -e "$f" ]] || continue
        cp -P "$f" "$FW/"
    done
done

ls "$FW/" | sed 's/^/    /'

# ── 4. 重新签名 + 启动 ────────────────────────────────────────────────
cyan "[4/4] codesign & launch …"
codesign --force --deep --sign - "$APP" 2>&1 | tail -1 || true

green "启动: $APP"

# 维护一个固定 symlink，方便 VSCode / 其它工具指向最新产物
LINK="${BUILD_DIR}/demo2.app"
mkdir -p "${BUILD_DIR}"
ln -sfn "$APP" "$LINK"
green "Symlink: $LINK -> $APP"

if [[ $LAUNCH -eq 1 ]]; then
    open "$APP"
else
    green "跳过 open（--no-launch）"
fi

# 让 VSCode debugger 能从 stdout 解析到可执行路径
echo "APP_EXECUTABLE=$APP/Contents/MacOS/mac-oc-mvvm"
