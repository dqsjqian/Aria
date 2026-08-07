#!/usr/bin/env bash
# examples/4-web-mvvm/scripts/run.sh — build & run the Web MVVM demo
#
# 用法：
#   examples/4-web-mvvm/scripts/run.sh                        # Debug, HTTP, 19090, 自动开浏览器
#   examples/4-web-mvvm/scripts/run.sh Release                # Release, HTTP
#   examples/4-web-mvvm/scripts/run.sh Debug --tls            # Debug, HTTPS（自动生成自签名证书）
#   examples/4-web-mvvm/scripts/run.sh Debug --no-launch      # 只构建不运行
#   examples/4-web-mvvm/scripts/run.sh Debug --no-open        # 跑但不开浏览器
#   examples/4-web-mvvm/scripts/run.sh Debug --probe          # 启动 → 探测端点 → 退出（CI 用）
#
# 环境变量：
#   ARIA_DEMO4_PORT      端口（默认 19090）
#   ARIA_DEMO4_CERT      已存在的 cert（与 --tls 一起用，跳过自签）
#   ARIA_DEMO4_KEY       已存在的 key
#   JOBS                 并行任务数（默认 CPU 核心数）
#
# Build dir : build/flavors/web-demo/   (与其他 demo 完全隔离)

set -euo pipefail

# ── 颜色 ──────────────────────────────────────────────────────────────────────
if [[ -t 1 ]]; then
    C_BOLD=$'\033[1m'; C_GREEN=$'\033[32m'; C_YELLOW=$'\033[33m'
    C_BLUE=$'\033[34m'; C_RED=$'\033[31m'; C_RESET=$'\033[0m'
else
    C_BOLD=""; C_GREEN=""; C_YELLOW=""; C_BLUE=""; C_RED=""; C_RESET=""
fi

log()  { echo "${C_BLUE}${C_BOLD}[demo4]${C_RESET} $*"; }
ok()   { echo "${C_GREEN}${C_BOLD}[demo4]${C_RESET} $*"; }
warn() { echo "${C_YELLOW}${C_BOLD}[demo4]${C_RESET} $*"; }
err()  { echo "${C_RED}${C_BOLD}[demo4]${C_RESET} $*"; }

# ── 参数 ──────────────────────────────────────────────────────────────────────
BUILD_TYPE="${1:-Debug}"
USE_TLS=0
NO_RUN=0
NO_OPEN=0
PROBE=0

for arg in "$@"; do
    case "$arg" in
        --tls)                USE_TLS=1 ;;
        --no-launch|--no-run) NO_RUN=1 ;;
        --no-open)            NO_OPEN=1 ;;
        --probe)              PROBE=1; NO_OPEN=1 ;;
    esac
done

PORT="${ARIA_DEMO4_PORT:-19090}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

# ── 定位仓库根 + 隔离的 build 目录 ────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEMO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$DEMO_ROOT/../.." && pwd)"
# Demo's own build tree (standalone: only this demo's objects, not the
# framework). The framework SDK is built once into build/flavors/sdk/ and
# installed to build/dist/tree/; the demo links it via find_package.
BUILD_DIR="$REPO_ROOT/build/flavors/web-demo"
SDK_TREE="$REPO_ROOT/build/flavors/sdk"
SDK_PREFIX="$REPO_ROOT/build/dist/tree"
APP_PATH="$BUILD_DIR/example_4_web_mvvm"

log "仓库根  : $REPO_ROOT"
log "构建目录: $BUILD_DIR"
log "构建类型: $BUILD_TYPE"
log "端口    : $PORT"
log "TLS     : $([[ $USE_TLS -eq 1 ]] && echo 启用 || echo 关闭)"
log "并行度  : $JOBS"

command -v cmake >/dev/null 2>&1 || { err "cmake 未安装"; exit 1; }

# ── 确保框架 SDK 已构建并安装 ────────────────────────────────────────────────
# 与 demo1 的 run.sh 共用同一个 SDK 树（build/flavors/sdk → build/dist/tree），
# 框架只构建一次。任何框架源码比已安装的 SDK 新就重建。
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
TLS_FLAG="OFF"
if [[ "$USE_TLS" == "1" ]]; then TLS_FLAG="ON"; fi

log "配置 CMake..."
cmake -S "$DEMO_ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DARIA_USE_INSTALLED=ON \
    -DARIA_HTTP_ENABLE_TLS=$TLS_FLAG \
    -DCMAKE_PREFIX_PATH="$SDK_PREFIX" \
    >/dev/null

# ── Build ─────────────────────────────────────────────────────────────────────
log "编译 example_4_web_mvvm..."
cmake --build "$BUILD_DIR" --target example_4_web_mvvm --config "$BUILD_TYPE" -j "$JOBS"

if [[ ! -x "$APP_PATH" ]]; then
    err "构建成功但找不到可执行文件：$APP_PATH"
    exit 1
fi
ok "构建完成：$APP_PATH"

if [[ "$NO_RUN" == "1" ]]; then
    log "--no-launch 已指定，不启动"
    exit 0
fi

# ── 自签名证书（如启用 TLS 且未提供）──────────────────────────────────────────
CERT_PATH=""
KEY_PATH=""
if [[ "$USE_TLS" == "1" ]]; then
    CERT_PATH="${ARIA_DEMO4_CERT:-}"
    KEY_PATH="${ARIA_DEMO4_KEY:-}"
    if [[ -z "$CERT_PATH" || -z "$KEY_PATH" ]]; then
        CERT_DIR="$BUILD_DIR/_certs"
        mkdir -p "$CERT_DIR"
        CERT_PATH="$CERT_DIR/cert.pem"
        KEY_PATH="$CERT_DIR/key.pem"
        if [[ ! -s "$CERT_PATH" || ! -s "$KEY_PATH" ]]; then
            log "生成自签名证书 → $CERT_DIR/"
            openssl req -x509 -newkey rsa:2048 -nodes \
                -keyout "$KEY_PATH" -out "$CERT_PATH" \
                -days 365 -subj "/CN=localhost" 2>/dev/null
        fi
    fi
    log "证书    : $CERT_PATH"
    log "私钥    : $KEY_PATH"
fi

SCHEME="http"
if [[ "$USE_TLS" == "1" ]]; then SCHEME="https"; fi
URL="$SCHEME://127.0.0.1:$PORT"

# ── 启动服务器 ────────────────────────────────────────────────────────────────
# Demo 始终启用 static_root，把 example 目录交给 server，浏览器同源访问 / 即可。
# 这样 SDK 用 location.origin 自动定位 API，无 CORS 麻烦。
log "启动 → $URL  (static_root = $DEMO_ROOT)"
if [[ "$USE_TLS" == "1" ]]; then
    "$APP_PATH" "$DEMO_ROOT" "$CERT_PATH" "$KEY_PATH" &
else
    "$APP_PATH" "$DEMO_ROOT" &
fi
SERVER_PID=$!
trap "kill $SERVER_PID 2>/dev/null || true; wait $SERVER_PID 2>/dev/null || true" EXIT INT TERM

# 等监听就绪（最多 5s）
LISTENER_OK=0
for _ in 1 2 3 4 5 6 7 8 9 10; do
    if curl -sk --max-time 1 "$URL/aria/health" > /dev/null 2>&1; then
        LISTENER_OK=1
        break
    fi
    sleep 0.5
done

if [[ "$LISTENER_OK" != "1" ]]; then
    err "服务器启动失败：$URL/aria/health 无响应"
    exit 1
fi
ok "服务器已就绪：$URL"

# ── probe 模式 ────────────────────────────────────────────────────────────────
if [[ "$PROBE" == "1" ]]; then
    log "probe 模式：curl 各端点 → 退出"
    PROBE_OK=1
    for ep in "/aria/health" "/aria/views"; do
        out=$(curl -sk --max-time 2 "$URL$ep" 2>/dev/null || echo "")
        if [[ -z "$out" ]]; then
            err "probe 失败：$URL$ep 无响应"
            PROBE_OK=0
        else
            ok "$ep → $out"
        fi
    done
    if [[ "$PROBE_OK" != "1" ]]; then exit 1; fi
    ok "probe 通过"
    exit 0
fi

# ── 开浏览器 ──────────────────────────────────────────────────────────────────
DEMO_URL="$URL/index.html"
if [[ "$NO_OPEN" != "1" ]]; then
    log "打开浏览器 → $DEMO_URL"
    case "$(uname)" in
        Darwin) open "$DEMO_URL" ;;
        Linux)  xdg-open "$DEMO_URL" >/dev/null 2>&1 || warn "无法自动打开浏览器，请手动访问 $DEMO_URL" ;;
        *)      log "（请手动打开 $DEMO_URL）" ;;
    esac
else
    log "（手动访问 $DEMO_URL）"
fi

log "（Ctrl-C 退出）"
wait $SERVER_PID
