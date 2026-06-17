#!/usr/bin/env bash
# init-project.sh — bootstrap a freshly-cloned Aria checkout for macOS dev.
#
# What it does:
#   1. Detects the host platform (must be macOS; Linux partially supported
#      for CLI/CMake bits, but .vscode debug config assumes macOS lldb).
#   2. Auto-detects Qt6 (Homebrew default locations) + prints what it found.
#   3. If any required dep (cmake, Qt6) is missing, prints an install guide
#      and EXITS without writing .vscode/ — keeps you from half-broken state.
#   4. Generates .vscode/{settings,tasks,launch,extensions,c_cpp_properties}.json
#      with absolute paths plugged in for this machine.
#   5. Makes sure build/ exists so VSCode can resolve relative paths early.
#
# Windows counterpart: scripts/init-project.ps1 (separate script).
#
# Usage:
#   ./scripts/init-project.sh              # auto-detect, overwrite .vscode/*
#   QT_DIR=/path/to/qt ./scripts/init-project.sh   # override Qt
#   ./scripts/init-project.sh --no-overwrite  # keep existing .vscode/* untouched
#   ./scripts/init-project.sh --dry-run    # preview, don't write
#   ARIA_NO_QT6=1 ./scripts/init-project.sh   # proceed without Qt (no demo1)
#
# Safe to re-run: .vscode/*.json are 100% generated from the templates below,
# so every run refreshes them to the latest template. Don't hand-edit those
# files — edit the templates in this script instead.

set -euo pipefail

# ── Colors ───────────────────────────────────────────────────────────────────
if [[ -t 1 ]]; then
    C_BOLD=$'\033[1m'; C_GREEN=$'\033[32m'; C_YELLOW=$'\033[33m'
    C_BLUE=$'\033[34m'; C_RED=$'\033[31m'; C_DIM=$'\033[2m'; C_RESET=$'\033[0m'
else
    C_BOLD=""; C_GREEN=""; C_YELLOW=""; C_BLUE=""; C_RED=""; C_DIM=""; C_RESET=""
fi
log()  { echo "${C_BLUE}${C_BOLD}[init]${C_RESET} $*"; }
ok()   { echo "${C_GREEN}${C_BOLD}[ ok]${C_RESET} $*"; }
warn() { echo "${C_YELLOW}${C_BOLD}[ ! ]${C_RESET} $*"; }
err()  { echo "${C_RED}${C_BOLD}[err]${C_RESET} $*" >&2; }
dim()  { echo "${C_DIM}$*${C_RESET}"; }

# ── Parse args ───────────────────────────────────────────────────────────────
# Overwrite policy for .vscode/*.json (these files are 100% generated from
# templates below):
#   default         → overwrite
#   --no-overwrite  → skip if the file already exists
DRY_RUN=0
NO_OVERWRITE=0
for arg in "$@"; do
    case "$arg" in
        --no-overwrite) NO_OVERWRITE=1 ;;
        --dry-run)      DRY_RUN=1 ;;
        -h|--help)
            sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *) err "unknown arg: $arg"; exit 1 ;;
    esac
done

# ── Locate repo root ─────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

# ── Platform guard ───────────────────────────────────────────────────────────
OS="$(uname -s)"
if [[ "$OS" != "Darwin" ]]; then
    warn "This script targets macOS. Detected: $OS"
    warn "For Windows, run:  scripts\\\\init-project.ps1"
    warn "For Linux, most steps will work but .vscode debug assumes lldb."
    read -r -p "Proceed anyway? [y/N] " yn
    [[ "$yn" =~ ^[Yy]$ ]] || exit 1
fi

log "Repo    : $REPO_ROOT"
log "Host OS : $OS ($(uname -m))"

# ── Detect tools ─────────────────────────────────────────────────────────────
have_cmd() { command -v "$1" >/dev/null 2>&1; }

CMAKE_PATH="$(command -v cmake 2>/dev/null || true)"
LLDB_PATH="$(command -v lldb 2>/dev/null || echo /usr/bin/lldb)"
CLANGXX_PATH="$(command -v clang++ 2>/dev/null || echo /usr/bin/clang++)"

log "cmake   : ${CMAKE_PATH:-not found}"
log "clang++ : $CLANGXX_PATH"
log "lldb    : $LLDB_PATH"

# ── Detect Qt6 ───────────────────────────────────────────────────────────────
# Priority: $ARIA_NO_QT6=1 → skip; $QT_DIR env → use if valid; Homebrew defaults
QT_DIR_FINAL=""
if [[ "${ARIA_NO_QT6:-}" == "1" ]]; then
    :   # user forced off
elif [[ -n "${QT_DIR:-}" && -d "$QT_DIR" ]]; then
    QT_DIR_FINAL="$QT_DIR"
else
    for c in "/opt/homebrew/opt/qt" "/opt/homebrew/opt/qt@6" "/usr/local/opt/qt" "/usr/local/opt/qt@6"; do
        if [[ -d "$c" ]]; then QT_DIR_FINAL="$c"; break; fi
    done
fi

if [[ -n "$QT_DIR_FINAL" ]]; then
    ok "Qt6     : $QT_DIR_FINAL"
else
    warn "Qt6     : not found"
fi

# ── Dependency gate ──────────────────────────────────────────────────────────
# If any required dep is missing, bail out with an actionable guide. We do
# NOT generate a half-broken .vscode/ in that case.
MISSING_DEPS=()
[[ -z "$CMAKE_PATH" ]] && MISSING_DEPS+=("cmake")
[[ -z "$QT_DIR_FINAL" && "${ARIA_NO_QT6:-}" != "1" ]] && MISSING_DEPS+=("qt6")

if [[ ${#MISSING_DEPS[@]} -gt 0 ]]; then
    echo
    err "Missing required dependencies: ${MISSING_DEPS[*]}"
    echo "       Skipping .vscode/ generation — fix the deps below, then re-run this script."
    echo
    # Detect Homebrew
    HAS_BREW=0
    command -v brew >/dev/null 2>&1 && HAS_BREW=1

    if [[ $HAS_BREW -eq 0 ]]; then
        echo "  ▶ Install Homebrew first (one-liner):"
        echo "      /bin/bash -c \"\$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)\""
        echo
    fi

    echo "  ▶ Install missing dependencies:"
    for dep in "${MISSING_DEPS[@]}"; do
        case "$dep" in
            cmake) echo "      brew install cmake" ;;
            qt6)   echo "      brew install qt              # or set QT_DIR=/path/to/your/qt" ;;
        esac
    done
    echo
    echo "  Once installed, re-run:"
    echo "      ./scripts/init-project.sh"
    echo
    echo "  To opt out of Qt6 (and skip demo1):"
    echo "      ARIA_NO_QT6=1 ./scripts/init-project.sh"
    exit 1
fi

# ── Helper: write file with overwrite/dry-run checks ─────────────────────────
# Default policy: overwrite .vscode/*.json so re-running this script pulls in
# the latest template. No backup — these files are 100% generated, don't
# hand-edit them (edit the templates in this script instead).
# If the existing file is byte-identical we silently no-op to keep mtime
# stable and avoid pestering VSCode to reload.
# --no-overwrite flips to "skip if exists" for the rare case you want it.
write_file() {
    local dest="$1"
    local content="$2"

    if [[ -e "$dest" && $NO_OVERWRITE -eq 1 ]]; then
        dim "  skip (exists) : $dest"
        return 0
    fi

    if [[ $DRY_RUN -eq 1 ]]; then
        if [[ -e "$dest" ]]; then
            echo "  would update  : $dest ($(printf '%s\n' "$content" | wc -l | tr -d ' ') lines)"
        else
            echo "  would write   : $dest ($(printf '%s\n' "$content" | wc -l | tr -d ' ') lines)"
        fi
        return 0
    fi

    mkdir -p "$(dirname "$dest")"

    if [[ -e "$dest" ]] && printf '%s\n' "$content" | cmp -s - "$dest"; then
        dim "  unchanged     : $dest"
        return 0
    fi

    if [[ -e "$dest" ]]; then
        printf '%s\n' "$content" > "$dest"
        ok "  updated       : $dest"
    else
        printf '%s\n' "$content" > "$dest"
        ok "  wrote         : $dest"
    fi
}

# ── Generate .vscode/ ────────────────────────────────────────────────────────
log "Generating .vscode/ …"

# ── settings.json ───────────────────────────────────────────────────────────
SETTINGS_JSON=$(cat <<'JSON'
{
    "cmake.sourceDirectory": "${workspaceFolder}",
    "cmake.buildDirectory": "${workspaceFolder}/build/ide",
    "cmake.generator": "Unix Makefiles",
    "cmake.configureArgs": [
        "-DCMAKE_BUILD_TYPE=Debug",
__QT_CONFIGURE_ARGS__
        "-DARIA_BUILD_TESTS=ON",
        "-DARIA_BUILD_EXAMPLES=OFF"
    ],
    "cmake.parallelJobs": 8,
    "C_Cpp.default.compileCommands": "${workspaceFolder}/build/ide/compile_commands.json",
    "C_Cpp.default.cppStandard": "c++20",
    "C_Cpp.default.intelliSenseMode": "macos-clang-arm64",
    "files.associations": {
        "*.hpp": "cpp",
        "*.h": "cpp",
        "*.mm": "objective-cpp",
        "*.inl": "cpp"
    }
}
JSON
)

if [[ -n "$QT_DIR_FINAL" ]]; then
    QT_ARGS="        \"-DCMAKE_PREFIX_PATH=$QT_DIR_FINAL\",
        \"-DARIA_BUILD_QT6=ON\","
else
    QT_ARGS=""
fi
SETTINGS_JSON="${SETTINGS_JSON//__QT_CONFIGURE_ARGS__/$QT_ARGS}"

# ── extensions.json ─────────────────────────────────────────────────────────
EXTENSIONS_JSON=$(cat <<'JSON'
{
    "recommendations": [
        "ms-vscode.cpptools",
        "ms-vscode.cmake-tools",
        "llvm-vs-code-extensions.vscode-clangd",
        "twxs.cmake"
    ]
}
JSON
)

# ── tasks.json ──────────────────────────────────────────────────────────────
if [[ -n "$QT_DIR_FINAL" ]]; then
    QT_CONFIGURE_ARG="                \"-DCMAKE_PREFIX_PATH=$QT_DIR_FINAL\",
                \"-DARIA_BUILD_QT6=ON\","
else
    QT_CONFIGURE_ARG=""
fi

TASKS_JSON=$(cat <<JSON
{
    "version": "2.0.0",
    "tasks": [
        {
            "label": "aria: configure (Debug)",
            "type": "shell",
            "command": "cmake",
            "args": [
                "-S", "\${workspaceFolder}",
                "-B", "\${workspaceFolder}/build/ide",
                "-DCMAKE_BUILD_TYPE=Debug",
                "-DARIA_BUILD_TESTS=ON",
                "-DARIA_BUILD_EXAMPLES=OFF",
                "-DARIA_BUILD_QT6=OFF"
            ],
            "group": "build",
            "problemMatcher": []
        },
        {
            "label": "aria: build demo1 (Debug, no launch)",
            "type": "shell",
            "command": "\${workspaceFolder}/examples/1-qt-showcase/scripts/run.sh",
            "args": ["Debug", "--no-launch"],
            "group": {
                "kind": "build",
                "isDefault": true
            },
            "problemMatcher": ["\$gcc"]
        },
        {
            "label": "aria: build all",
            "type": "shell",
            "command": "cmake",
            "args": [
                "--build", "\${workspaceFolder}/build/ide",
                "-j", "8"
            ],
            "dependsOn": ["aria: configure (Debug)"],
            "problemMatcher": ["\$gcc"]
        },
        {
            "label": "aria: run demo1",
            "type": "shell",
            "command": "\${workspaceFolder}/examples/1-qt-showcase/scripts/run.sh",
            "args": ["Debug"],
            "presentation": {
                "reveal": "always",
                "panel": "new"
            },
            "problemMatcher": []
        },
        {
            "label": "aria: probe demo1",
            "type": "shell",
            "command": "\${workspaceFolder}/examples/1-qt-showcase/scripts/run.sh",
            "args": ["Debug", "probe"],
            "problemMatcher": []
        },
        {
            "label": "aria: build demo4 (Debug, no launch)",
            "type": "shell",
            "command": "\${workspaceFolder}/examples/4-web-mvvm/scripts/run.sh",
            "args": ["Debug", "--no-launch"],
            "problemMatcher": ["\$gcc"]
        },
        {
            "label": "aria: run demo4 (HTTP)",
            "type": "shell",
            "command": "\${workspaceFolder}/examples/4-web-mvvm/scripts/run.sh",
            "args": ["Debug"],
            "presentation": {
                "reveal": "always",
                "panel": "new"
            },
            "problemMatcher": []
        },
        {
            "label": "aria: run demo4 (HTTPS)",
            "type": "shell",
            "command": "\${workspaceFolder}/examples/4-web-mvvm/scripts/run.sh",
            "args": ["Debug", "--tls"],
            "presentation": {
                "reveal": "always",
                "panel": "new"
            },
            "problemMatcher": []
        },
        {
            "label": "aria: probe demo4",
            "type": "shell",
            "command": "\${workspaceFolder}/examples/4-web-mvvm/scripts/run.sh",
            "args": ["Debug", "--probe"],
            "problemMatcher": []
        },
        {
            "label": "aria: ctest",
            "type": "shell",
            "command": "ctest",
            "args": ["--output-on-failure"],
            "options": {
                "cwd": "\${workspaceFolder}/build/ide"
            },
            "dependsOn": ["aria: build all"],
            "problemMatcher": []
        },
        {
            "label": "aria: build demo2 (Debug, no launch)",
            "type": "shell",
            "command": "\${workspaceFolder}/examples/2-macos-appkit-mvvm/scripts/run.sh",
            "args": ["Debug", "--no-launch"],
            "group": "build",
            "problemMatcher": {
                "owner": "cpp",
                "fileLocation": ["autoDetect", "\${workspaceFolder}"],
                "pattern": {
                    "regexp": "^(.*?):(\\\\d+):(\\\\d+):\\\\s+(warning|error|fatal error):\\\\s+(.*)\$",
                    "file": 1, "line": 2, "column": 3, "severity": 4, "message": 5
                }
            }
        },
        {
            "label": "aria: build demo2 (Release, no launch)",
            "type": "shell",
            "command": "\${workspaceFolder}/examples/2-macos-appkit-mvvm/scripts/run.sh",
            "args": ["Release", "--no-launch"],
            "problemMatcher": []
        },
        {
            "label": "aria: run demo2",
            "type": "shell",
            "command": "\${workspaceFolder}/examples/2-macos-appkit-mvvm/scripts/run.sh",
            "args": ["Debug"],
            "problemMatcher": []
        },
        {
            "label": "aria: build demo3 (Debug, no launch)",
            "type": "shell",
            "command": "\${workspaceFolder}/examples/3-ios-oc-uikit-mvvm/scripts/run.sh",
            "args": ["Debug", "--no-launch"],
            "group": "build",
            "problemMatcher": {
                "owner": "cpp",
                "fileLocation": ["autoDetect", "\${workspaceFolder}"],
                "pattern": {
                    "regexp": "^(.*?):(\\\\d+):(\\\\d+):\\\\s+(warning|error|fatal error):\\\\s+(.*)\$",
                    "file": 1, "line": 2, "column": 3, "severity": 4, "message": 5
                }
            }
        },
        {
            "label": "aria: build demo3 (Release, no launch)",
            "type": "shell",
            "command": "\${workspaceFolder}/examples/3-ios-oc-uikit-mvvm/scripts/run.sh",
            "args": ["Release", "--no-launch"],
            "problemMatcher": []
        },
        {
            "label": "aria: run demo3",
            "type": "shell",
            "command": "\${workspaceFolder}/examples/3-ios-oc-uikit-mvvm/scripts/run.sh",
            "args": ["Debug"],
            "problemMatcher": []
        },
        {
            "label": "aria: build demo5 (Debug, no launch)",
            "type": "shell",
            "command": "\${workspaceFolder}/examples/5-android-jni-mvvm/scripts/run.sh",
            "args": ["Debug", "--no-launch"],
            "group": "build",
            "problemMatcher": ["\$gcc"]
        },
        {
            "label": "aria: run demo5 (Android JNI + Compose)",
            "type": "shell",
            "command": "\${workspaceFolder}/examples/5-android-jni-mvvm/scripts/run.sh",
            "args": ["Debug"],
            "presentation": {
                "reveal": "always",
                "panel": "new"
            },
            "problemMatcher": []
        },
        {
            "label": "aria: configure (ASan+UBSan)",
            "type": "shell",
            "command": "cmake",
            "args": [
                "-S", "\${workspaceFolder}",
                "-B", "\${workspaceFolder}/build/flavors/asan",
                "-G", "Ninja",
                "-DCMAKE_BUILD_TYPE=Debug",
                "-DARIA_ENABLE_ASAN=ON",
                "-DARIA_ENABLE_UBSAN=ON",
                "-DARIA_BUILD_TESTS=ON",
                "-DARIA_BUILD_EXAMPLES=OFF"
            ],
            "group": "build",
            "problemMatcher": []
        },
        {
            "label": "aria: ctest (ASan+UBSan)",
            "type": "shell",
            "command": "bash",
            "args": [
                "-c",
                "cmake --build build/flavors/asan -j && ctest --test-dir build/flavors/asan --output-on-failure"
            ],
            "dependsOn": ["aria: configure (ASan+UBSan)"],
            "problemMatcher": []
        },
        {
            "label": "aria: build (Android NDK)",
            "type": "shell",
            "command": "bash",
            "args": [
                "-c",
                "scripts/build.sh android"
            ],
            "options": {
                "cwd": "\${workspaceFolder}"
            },
            "group": "build",
            "problemMatcher": []
        },
        {
            "label": "aria: configure (TSan)",
            "type": "shell",
            "command": "cmake",
            "args": [
                "-S", "\${workspaceFolder}",
                "-B", "\${workspaceFolder}/build/flavors/tsan",
                "-G", "Ninja",
                "-DCMAKE_BUILD_TYPE=Debug",
                "-DARIA_ENABLE_TSAN=ON",
                "-DARIA_BUILD_TESTS=ON",
                "-DARIA_BUILD_EXAMPLES=OFF"
            ],
            "group": "build",
            "problemMatcher": []
        },
        {
            "label": "aria: ctest (TSan)",
            "type": "shell",
            "command": "bash",
            "args": [
                "-c",
                "cmake --build build/flavors/tsan -j && TSAN_OPTIONS='halt_on_error=1' ctest --test-dir build/flavors/tsan --output-on-failure --timeout 600"
            ],
            "dependsOn": ["aria: configure (TSan)"],
            "problemMatcher": []
        },
        {
            "label": "aria: clang-tidy (all headers)",
            "type": "shell",
            "command": "bash",
            "args": [
                "-c",
                "shopt -s globstar nullglob; cmake -S . -B build/ide -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null && cmake --build build/ide -j >/dev/null && files=(modules/*/include/aria/**/*.hpp); clang-tidy -p build/ide --warnings-as-errors='*' \"\${files[@]}\""
            ],
            "problemMatcher": [
                {
                    "owner": "clang-tidy",
                    "fileLocation": ["autoDetect", "\${workspaceFolder}"],
                    "pattern": {
                        "regexp": "^(.*?):(\\\\d+):(\\\\d+):\\\\s+(warning|error):\\\\s+(.*)\$",
                        "file": 1, "line": 2, "column": 3, "severity": 4, "message": 5
                    }
                }
            ]
        }
    ]
}
JSON
)

# ── launch.json ─────────────────────────────────────────────────────────────
LAUNCH_JSON=$(cat <<'JSON'
{
    "version": "0.2.0",
    "configurations": [
        {
            "name": "Run demo1 (Qt showcase)",
            "type": "node-terminal",
            "request": "launch",
            "command": "./examples/1-qt-showcase/scripts/run.sh Debug",
            "cwd": "${workspaceFolder}"
        },
        {
            "name": "Debug demo1 (Qt showcase, lldb)",
            "type": "cppdbg",
            "request": "launch",
            "program": "${workspaceFolder}/build/examples/1-qt-showcase/bin/ex_qt_showcase.app/Contents/MacOS/ex_qt_showcase",
            "args": [],
            "stopAtEntry": false,
            "cwd": "${workspaceFolder}",
            "environment": [],
            "externalConsole": false,
            "MIMode": "lldb",
            "preLaunchTask": "aria: build demo1 (Debug, no launch)"
        },
        {
            "name": "Debug demo1 — probe mode (lldb)",
            "type": "cppdbg",
            "request": "launch",
            "program": "${workspaceFolder}/build/examples/1-qt-showcase/bin/ex_qt_showcase.app/Contents/MacOS/ex_qt_showcase",
            "args": [],
            "stopAtEntry": false,
            "cwd": "${workspaceFolder}",
            "environment": [
                { "name": "ARIA_PROBE", "value": "1" }
            ],
            "externalConsole": false,
            "MIMode": "lldb",
            "preLaunchTask": "aria: build demo1 (Debug, no launch)"
        },
        {
            "name": "Run demo2 (AppKit)",
            "type": "node-terminal",
            "request": "launch",
            "command": "./examples/2-macos-appkit-mvvm/scripts/run.sh Debug",
            "cwd": "${workspaceFolder}"
        },
        {
            "name": "Run demo3 (UIKit / iOS Simulator)",
            "type": "node-terminal",
            "request": "launch",
            "command": "./examples/3-ios-oc-uikit-mvvm/scripts/run.sh Debug",
            "cwd": "${workspaceFolder}"
        },
        {
            "name": "Run demo4 (Web MVVM, HTTP)",
            "type": "node-terminal",
            "request": "launch",
            "command": "./examples/4-web-mvvm/scripts/run.sh Debug",
            "cwd": "${workspaceFolder}"
        },
        {
            "name": "Run demo4 (Web MVVM, HTTPS)",
            "type": "node-terminal",
            "request": "launch",
            "command": "./examples/4-web-mvvm/scripts/run.sh Debug --tls",
            "cwd": "${workspaceFolder}"
        },
        {
            "name": "Run demo4 — probe mode",
            "type": "node-terminal",
            "request": "launch",
            "command": "./examples/4-web-mvvm/scripts/run.sh Debug --probe",
            "cwd": "${workspaceFolder}"
        },
        {
            "name": "Run demo5 (Android JNI + Compose)",
            "type": "node-terminal",
            "request": "launch",
            "command": "./examples/5-android-jni-mvvm/scripts/run.sh Debug",
            "cwd": "${workspaceFolder}"
        },
        {
            "name": "Debug test (pick binary, lldb)",
            "type": "cppdbg",
            "request": "launch",
            "program": "${workspaceFolder}/build/ide/bin/test_${input:testModule}",
            "args": [],
            "stopAtEntry": false,
            "cwd": "${workspaceFolder}",
            "MIMode": "lldb",
            "preLaunchTask": "aria: build all"
        }
    ],
    "inputs": [
        {
            "id": "testModule",
            "type": "pickString",
            "description": "Which test module?",
            "options": ["abi", "core", "async", "runtime", "binding"],
            "default": "core"
        }
    ]
}
JSON
)

# ── c_cpp_properties.json ───────────────────────────────────────────────────
if [[ "$(uname -m)" == "arm64" ]]; then
    INTELLISENSE_MODE="macos-clang-arm64"
else
    INTELLISENSE_MODE="macos-clang-x64"
fi

C_CPP_PROPERTIES_JSON=$(cat <<JSON
{
    "version": 4,
    "configurations": [
        {
            "name": "Aria (CMake — framework & demo1)",
            "compileCommands": "\${workspaceFolder}/build/ide/compile_commands.json",
            "compilerPath": "$CLANGXX_PATH",
            "cStandard": "c17",
            "cppStandard": "c++20",
            "intelliSenseMode": "$INTELLISENSE_MODE"
        },
        {
            "name": "demo2 (AppKit / Xcode)",
            "compilerPath": "$CLANGXX_PATH",
            "cStandard": "c17",
            "cppStandard": "c++20",
            "intelliSenseMode": "$INTELLISENSE_MODE",
            "includePath": [
                "\${workspaceFolder}/examples/2-macos-appkit-mvvm/mac-oc-mvvm/App",
                "\${workspaceFolder}/examples/2-macos-appkit-mvvm/mac-oc-mvvm/Adapter",
                "\${workspaceFolder}/examples/2-macos-appkit-mvvm/mac-oc-mvvm/Business/Root/Views",
                "\${workspaceFolder}/examples/2-macos-appkit-mvvm/mac-oc-mvvm/Business/Root/ViewModels",
                "\${workspaceFolder}/examples/2-macos-appkit-mvvm/mac-oc-mvvm/Business/Root/Models",
                "\${workspaceFolder}/examples/2-macos-appkit-mvvm/mac-oc-mvvm/Business/Playground/Views",
                "\${workspaceFolder}/modules/core/include",
                "\${workspaceFolder}/modules/abi/include",
                "\${workspaceFolder}/modules/binding/include",
                "\${workspaceFolder}/modules/runtime/include",
                "\${workspaceFolder}/modules/async/include"
            ],
            "defines": ["DEBUG=1"]
        },
        {
            "name": "demo3 (UIKit / Xcode)",
            "compilerPath": "$CLANGXX_PATH",
            "cStandard": "c17",
            "cppStandard": "c++20",
            "intelliSenseMode": "$INTELLISENSE_MODE",
            "includePath": [
                "\${workspaceFolder}/examples/3-ios-oc-uikit-mvvm/ios-oc-mvvm/App",
                "\${workspaceFolder}/examples/3-ios-oc-uikit-mvvm/ios-oc-mvvm/Adapter",
                "\${workspaceFolder}/examples/3-ios-oc-uikit-mvvm/ios-oc-mvvm/Business/Root/Views",
                "\${workspaceFolder}/examples/3-ios-oc-uikit-mvvm/ios-oc-mvvm/Business/Root/ViewModels",
                "\${workspaceFolder}/examples/3-ios-oc-uikit-mvvm/ios-oc-mvvm/Business/Root/Models",
                "\${workspaceFolder}/examples/3-ios-oc-uikit-mvvm/ios-oc-mvvm/Business/Playground/Views",
                "\${workspaceFolder}/modules/core/include",
                "\${workspaceFolder}/modules/abi/include",
                "\${workspaceFolder}/modules/binding/include",
                "\${workspaceFolder}/modules/runtime/include",
                "\${workspaceFolder}/modules/async/include"
            ],
            "defines": ["DEBUG=1", "TARGET_OS_IPHONE=1"]
        },
        {
            "name": "demo5 (Android JNI / Gradle)",
            "compilerPath": "$CLANGXX_PATH",
            "cStandard": "c17",
            "cppStandard": "c++20",
            "intelliSenseMode": "$INTELLISENSE_MODE",
            "includePath": [
                "\${workspaceFolder}/modules/adapters/jni/include",
                "\${workspaceFolder}/modules/core/include",
                "\${workspaceFolder}/modules/abi/include",
                "\${workspaceFolder}/modules/binding/include",
                "\${workspaceFolder}/modules/runtime/include",
                "\${workspaceFolder}/modules/async/include"
            ],
            "defines": ["DEBUG=1", "ANDROID=1"]
        }
    ]
}
JSON
)

write_file ".vscode/settings.json"           "$SETTINGS_JSON"
write_file ".vscode/extensions.json"         "$EXTENSIONS_JSON"
write_file ".vscode/tasks.json"              "$TASKS_JSON"
write_file ".vscode/launch.json"             "$LAUNCH_JSON"
write_file ".vscode/c_cpp_properties.json"   "$C_CPP_PROPERTIES_JSON"

# ── Make sure build/ exists so VSCode paths resolve early ────────────────────
if [[ $DRY_RUN -eq 0 ]]; then
    mkdir -p build
    mkdir -p build/platforms
fi

# ── Summary ──────────────────────────────────────────────────────────────────
echo
ok "Project initialized."
echo "  Next steps:"
echo "    • Open this folder in VSCode"
echo "    • Install recommended extensions when prompted"
echo "    • Press F5 → pick 'Debug demo1 (Qt showcase)' to run/debug"
echo "    • Or run:  ./examples/1-qt-showcase/scripts/run.sh  /  ./examples/2-macos-appkit-mvvm/scripts/run.sh  /  ./examples/3-ios-oc-uikit-mvvm/scripts/run.sh  /  ./examples/5-android-jni-mvvm/scripts/run.sh"
