#!/usr/bin/env bash
# init-project.sh — bootstrap a freshly-cloned Aria checkout for macOS/Linux.
# Generates machine-local VSCode configuration for framework development,
# tests, documentation, benchmarks, sanitizers, and platform builds.
#
# Usage:
#   ./scripts/init-project.sh
#   QT_DIR=/path/to/qt ./scripts/init-project.sh
#   ARIA_NO_QT6=1 ./scripts/init-project.sh
#   ./scripts/init-project.sh --no-overwrite
#   ./scripts/init-project.sh --dry-run

set -euo pipefail

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

DRY_RUN=0
NO_OVERWRITE=0
for arg in "$@"; do
    case "$arg" in
        --no-overwrite) NO_OVERWRITE=1 ;;
        --dry-run)      DRY_RUN=1 ;;
        -h|--help)
            sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *) err "unknown arg: $arg"; exit 1 ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

OS="$(uname -s)"
if [[ "$OS" != "Darwin" && "$OS" != "Linux" ]]; then
    err "Unsupported host: $OS. On Windows run scripts\\init-project.ps1"
    exit 1
fi

CMAKE_PATH="$(command -v cmake 2>/dev/null || true)"
CLANGXX_PATH="$(command -v clang++ 2>/dev/null || command -v g++ 2>/dev/null || true)"
LLDB_PATH="$(command -v lldb 2>/dev/null || true)"
[[ -n "$CMAKE_PATH" ]] || { err "cmake not found"; exit 1; }
[[ -n "$CLANGXX_PATH" ]] || { err "C++ compiler not found"; exit 1; }

QT_DIR_FINAL=""
if [[ "${ARIA_NO_QT6:-}" != "1" ]]; then
    if [[ -n "${QT_DIR:-}" && -d "$QT_DIR" ]]; then
        QT_DIR_FINAL="$QT_DIR"
    else
        for candidate in /opt/homebrew/opt/qt /opt/homebrew/opt/qt@6 /usr/local/opt/qt /usr/local/opt/qt@6; do
            if [[ -d "$candidate" ]]; then QT_DIR_FINAL="$candidate"; break; fi
        done
    fi
fi

log "Repo     : $REPO_ROOT"
log "Host OS  : $OS ($(uname -m))"
log "cmake    : $CMAKE_PATH"
log "compiler : $CLANGXX_PATH"
if [[ -n "$QT_DIR_FINAL" ]]; then ok "Qt6      : $QT_DIR_FINAL"; else warn "Qt6 adapter disabled"; fi

write_file() {
    local dest="$1"
    local content="$2"
    if [[ -e "$dest" && $NO_OVERWRITE -eq 1 ]]; then
        dim "  skip (exists) : $dest"
        return
    fi
    if [[ $DRY_RUN -eq 1 ]]; then
        echo "  would write   : $dest"
        return
    fi
    mkdir -p "$(dirname "$dest")"
    if [[ -e "$dest" ]] && printf '%s\n' "$content" | cmp -s - "$dest"; then
        dim "  unchanged     : $dest"
    else
        printf '%s\n' "$content" > "$dest"
        ok "  wrote         : $dest"
    fi
}

if [[ -n "$QT_DIR_FINAL" ]]; then
    QT_SETTINGS_ARGS="        \"-DCMAKE_PREFIX_PATH=$QT_DIR_FINAL\",
        \"-DARIA_BUILD_QT6=ON\","
    QT_TASK_ARGS="                \"-DCMAKE_PREFIX_PATH=$QT_DIR_FINAL\",
                \"-DARIA_BUILD_QT6=ON\","
else
    QT_SETTINGS_ARGS='        "-DARIA_BUILD_QT6=OFF",'
    QT_TASK_ARGS='                "-DARIA_BUILD_QT6=OFF",'
fi

SETTINGS_JSON=$(cat <<JSON
{
    "cmake.sourceDirectory": "\${workspaceFolder}",
    "cmake.buildDirectory": "\${workspaceFolder}/build/ide",
    "cmake.configureArgs": [
        "-DCMAKE_BUILD_TYPE=Debug",
$QT_SETTINGS_ARGS
        "-DARIA_BUILD_TESTS=ON",
        "-DARIA_BUILD_DOCS=ON"
    ],
    "cmake.parallelJobs": 8,
    "C_Cpp.default.compileCommands": "\${workspaceFolder}/build/ide/compile_commands.json",
    "C_Cpp.default.cppStandard": "c++20"
}
JSON
)

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
$QT_TASK_ARGS
                "-DARIA_BUILD_TESTS=ON",
                "-DARIA_BUILD_DOCS=ON"
            ],
            "group": "build",
            "problemMatcher": []
        },
        {
            "label": "aria: build all",
            "type": "shell",
            "command": "cmake",
            "args": ["--build", "\${workspaceFolder}/build/ide", "-j", "8"],
            "dependsOn": ["aria: configure (Debug)"],
            "group": { "kind": "build", "isDefault": true },
            "problemMatcher": ["\$gcc"]
        },
        {
            "label": "aria: ctest",
            "type": "shell",
            "command": "ctest",
            "args": ["--test-dir", "\${workspaceFolder}/build/ide", "--output-on-failure"],
            "dependsOn": ["aria: build all"],
            "group": "test",
            "problemMatcher": []
        },
        {
            "label": "aria: configure (ASan+UBSan)",
            "type": "shell",
            "command": "cmake",
            "args": ["-S", "\${workspaceFolder}", "-B", "\${workspaceFolder}/build/flavors/asan", "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Debug", "-DARIA_ENABLE_ASAN=ON", "-DARIA_ENABLE_UBSAN=ON", "-DARIA_BUILD_TESTS=ON"],
            "group": "build",
            "problemMatcher": []
        },
        {
            "label": "aria: ctest (ASan+UBSan)",
            "type": "shell",
            "command": "bash",
            "args": ["-c", "cmake --build build/flavors/asan -j && ctest --test-dir build/flavors/asan --output-on-failure"],
            "dependsOn": ["aria: configure (ASan+UBSan)"],
            "problemMatcher": []
        },
        {
            "label": "aria: configure (TSan)",
            "type": "shell",
            "command": "cmake",
            "args": ["-S", "\${workspaceFolder}", "-B", "\${workspaceFolder}/build/flavors/tsan", "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Debug", "-DARIA_ENABLE_TSAN=ON", "-DARIA_BUILD_TESTS=ON"],
            "group": "build",
            "problemMatcher": []
        },
        {
            "label": "aria: ctest (TSan)",
            "type": "shell",
            "command": "bash",
            "args": ["-c", "cmake --build build/flavors/tsan -j && TSAN_OPTIONS='halt_on_error=1' ctest --test-dir build/flavors/tsan --output-on-failure --timeout 600"],
            "dependsOn": ["aria: configure (TSan)"],
            "problemMatcher": []
        },
        {
            "label": "aria: docs",
            "type": "shell",
            "command": "cmake",
            "args": ["--build", "\${workspaceFolder}/build/ide", "--target", "aria_docs"],
            "dependsOn": ["aria: configure (Debug)"],
            "problemMatcher": []
        },
        {
            "label": "aria: benchmark",
            "type": "shell",
            "command": "bash",
            "args": ["-c", "cmake -S . -B build/flavors/bench -DCMAKE_BUILD_TYPE=Release -DARIA_BUILD_BENCHMARK=ON && cmake --build build/flavors/bench -j"],
            "group": "build",
            "problemMatcher": []
        },
        {
            "label": "aria: build (Android NDK)",
            "type": "shell",
            "command": "bash",
            "args": ["-c", "scripts/build.sh android"],
            "options": { "cwd": "\${workspaceFolder}" },
            "group": "build",
            "problemMatcher": []
        },
        {
            "label": "aria: clang-tidy (all headers)",
            "type": "shell",
            "command": "bash",
            "args": ["-c", "shopt -s globstar nullglob; cmake -S . -B build/ide -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null && cmake --build build/ide -j >/dev/null && files=(modules/*/include/aria/**/*.hpp); clang-tidy -p build/ide --warnings-as-errors='*' \"\${files[@]}\""],
            "problemMatcher": []
        }
    ]
}
JSON
)

LAUNCH_JSON=$(cat <<'JSON'
{
    "version": "0.2.0",
    "configurations": [
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

INTELLISENSE_MODE="linux-gcc-x64"
[[ "$OS" == "Darwin" && "$(uname -m)" == "arm64" ]] && INTELLISENSE_MODE="macos-clang-arm64"
[[ "$OS" == "Darwin" && "$(uname -m)" != "arm64" ]] && INTELLISENSE_MODE="macos-clang-x64"
C_CPP_PROPERTIES_JSON=$(cat <<JSON
{
    "version": 4,
    "configurations": [
        {
            "name": "Aria framework",
            "compileCommands": "\${workspaceFolder}/build/ide/compile_commands.json",
            "compilerPath": "$CLANGXX_PATH",
            "cStandard": "c17",
            "cppStandard": "c++20",
            "intelliSenseMode": "$INTELLISENSE_MODE",
            "includePath": ["\${workspaceFolder}/modules/**"]
        }
    ]
}
JSON
)

log "Generating .vscode/ ..."
write_file ".vscode/settings.json" "$SETTINGS_JSON"
write_file ".vscode/extensions.json" "$EXTENSIONS_JSON"
write_file ".vscode/tasks.json" "$TASKS_JSON"
write_file ".vscode/launch.json" "$LAUNCH_JSON"
write_file ".vscode/c_cpp_properties.json" "$C_CPP_PROPERTIES_JSON"

if [[ $DRY_RUN -eq 0 ]]; then
    mkdir -p build/platforms
fi

echo
ok "Project initialized."
echo "  Open this folder in VSCode, install the recommended extensions, then run 'aria: build all'."
echo "  The flagship sample application is AriaTools."
