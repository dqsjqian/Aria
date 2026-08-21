#!/usr/bin/env bash
# sync-cmake.sh — Auto-detect module source files that are not referenced
# by the corresponding CMakeLists.txt.
#
# Usage:
#   ./scripts/sync-cmake.sh              # scan and report
#
# This script is READ-ONLY — it reports what needs to be added. CMake
# edits are manual because each module decides its own target_sources()
# layout.
#
# What it scans:
#   - modules/<mod>/src/*.cpp           against modules/<mod>/CMakeLists.txt
#   - modules/<mod>/tests/*.cpp         against modules/<mod>/tests/CMakeLists.txt
#     (falls back to the module CMakeLists if tests/CMakeLists.txt is missing)
#
# Headers are not checked (only .cpp/.mm implementation files).

set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[0;33m'; CYAN='\033[0;36m'; NC='\033[0m'
info()  { echo -e "${CYAN}[INFO]${NC}  $*"; }
ok()    { echo -e "${GREEN}[OK]${NC}    $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }

MODULES_DIR="${PROJECT_DIR}/modules"

# ── Helper: is this .cpp file covered by the given CMakeLists.txt? ──────────
# Coverage rules:
#   1. Explicit filename match (grep "$basename").
#   2. Any GLOB / GLOB_RECURSE line in the CMakeLists is treated as a
#      blanket "everything in this tree is covered" — we don't parse the
#      pattern, we just assume the user knows what they GLOB'd.
is_covered() {
    local cpp_path="$1"
    local cmake_file="$2"
    [[ -f "$cmake_file" ]] || return 1

    local fname
    fname=$(basename "$cpp_path")
    grep -q -F "$fname" "$cmake_file" && return 0

    # GLOB / GLOB_RECURSE means "trust the cmake — everything is picked up".
    grep -qE 'file\s*\(\s*GLOB(_RECURSE)?' "$cmake_file" && return 0

    return 1
}

# ── Scan modules/ ────────────────────────────────────────────────────────────
info "Scanning modules/"

for module_dir in "$MODULES_DIR"/*/; do
    module_name=$(basename "$module_dir")
    [[ "$module_name" == "adapters" ]] && continue   # nested, handled separately

    mod_cmake="${module_dir}CMakeLists.txt"
    test_cmake="${module_dir}tests/CMakeLists.txt"
    [[ ! -f "$mod_cmake" ]] && continue

    missing_src=""
    for f in "${module_dir}src/"*.cpp; do
        [[ ! -f "$f" ]] && continue
        if ! is_covered "$f" "$mod_cmake"; then
            missing_src="${missing_src}  + src/$(basename "$f")"$'\n'
        fi
    done

    missing_test=""
    for f in "${module_dir}tests/"*.cpp; do
        [[ ! -f "$f" ]] && continue
        # tests/CMakeLists.txt owns the test sources; fall back to the module
        # CMakeLists for projects that don't split them out.
        cmake_to_check="$test_cmake"
        [[ -f "$cmake_to_check" ]] || cmake_to_check="$mod_cmake"
        if ! is_covered "$f" "$cmake_to_check"; then
            missing_test="${missing_test}  + tests/$(basename "$f")"$'\n'
        fi
    done

    if [[ -n "$missing_src" || -n "$missing_test" ]]; then
        warn "Module '$module_name':"
        [[ -n "$missing_src" ]]  && printf '%s' "$missing_src"
        [[ -n "$missing_test" ]] && printf '%s' "$missing_test"
        echo "  → Add to $mod_cmake (or ${test_cmake} for tests)"
        echo ""
    else
        ok "Module '$module_name': all sources referenced"
    fi
done

# ── Scan modules/adapters/* ──────────────────────────────────────────────────
if [[ -d "$MODULES_DIR/adapters" ]]; then
    info "Scanning modules/adapters/"
    for adapter_dir in "$MODULES_DIR/adapters"/*/; do
        adapter_name=$(basename "$adapter_dir")
        mod_cmake="${adapter_dir}CMakeLists.txt"
        test_cmake="${adapter_dir}tests/CMakeLists.txt"
        [[ ! -f "$mod_cmake" ]] && continue

        missing=""
        for f in "${adapter_dir}src/"*.cpp "${adapter_dir}src/"*.mm; do
            [[ ! -f "$f" ]] && continue
            if ! is_covered "$f" "$mod_cmake"; then
                missing="${missing}  + src/$(basename "$f")"$'\n'
            fi
        done
        for f in "${adapter_dir}tests/"*.cpp; do
            [[ ! -f "$f" ]] && continue
            cmake_to_check="$test_cmake"
            [[ -f "$cmake_to_check" ]] || cmake_to_check="$mod_cmake"
            if ! is_covered "$f" "$cmake_to_check"; then
                missing="${missing}  + tests/$(basename "$f")"$'\n'
            fi
        done

        if [[ -n "$missing" ]]; then
            warn "Adapter '$adapter_name':"
            printf '%s' "$missing"
            echo "  → Add to $mod_cmake"
            echo ""
        else
            ok "Adapter '$adapter_name': all sources referenced"
        fi
    done
fi

ok "Scan complete."
