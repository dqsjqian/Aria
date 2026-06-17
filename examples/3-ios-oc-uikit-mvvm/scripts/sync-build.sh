#!/usr/bin/env bash
# sync-build.sh — Auto-sync project.pbxproj when source files change.
#
# Usage:
#   ./scripts/sync-build.sh              # scan and update if needed
#   ./scripts/sync-build.sh --dry-run    # show what would change
#
# What it does:
#   1. Scans ios-oc-mvvm/ for *.mm, *.m, *.hpp, *.h, and *Info.plist files
#   2. Compares with current project.pbxproj references
#   3. Adds missing files (subdirectories included)
#   4. Removes orphan files (exist in pbxproj but not on disk)
#   5. Reports what changed
#
# 注意：本脚本不会动 AriaCore 组里的引用（指向工程外 ../../modules/ 的 cpp），
# 那些是固定的 aria 核心源码列表，由 pbxproj 手动维护。

# Disable exit on error for the whole script - handle errors manually
set -uo pipefail
# Note: we deliberately NOT using 'set -e' because many commands
# (like grep) legitimately return non-zero in normal operation.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
SOURCE_DIR="${PROJECT_DIR}/ios-oc-mvvm"
PBXPROJ="${PROJECT_DIR}/ios-oc-mvvm.xcodeproj/project.pbxproj"

DRY_RUN=false
[[ "${1:-}" == "--dry-run" ]] && DRY_RUN=true

# Colors (no mapfile, bash 3 compatible)
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[0;33m'; CYAN='\033[0;36m'; NC='\033[0m'
info()  { echo -e "${CYAN}[INFO]${NC}  $*"; }
ok()    { echo -e "${GREEN}[OK]${NC}    $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }

# ── Step 1: Find all source files (recursively) ──────────────────────────────
info "Scanning ${SOURCE_DIR} for source files..."

ALL_FILES=""
# Skip vendored third-party sources (e.g. Masonry); those are managed by
# scripts/add-masonry-to-pbxproj.rb (一次性脚本)，不走本脚本。
while IFS= read -r f; do
    [[ -f "$f" ]] && ALL_FILES="${ALL_FILES}${f}"$'\n'
done < <(find "${SOURCE_DIR}" \( -path "${SOURCE_DIR}/ThirdParty" -prune \) -o \
         \( -name '*.mm' -o -name '*.m' -o -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*Info.plist' \) -type f -print 2>/dev/null)

FILE_COUNT=$(echo "$ALL_FILES" | grep -c '.' || true)
info "Found ${FILE_COUNT} source files"

# ── Step 2: Find files currently in pbxproj ───────────────────────────────────
NOT_IN_PBX=""
ORPHAN=""

while IFS= read -r fpath; do
    [[ -z "$fpath" ]] && continue
    fname=$(basename "$fpath")
    # Check if file name exists in pbxproj (covers both flat and subdirectory references)
    if ! grep -q "${fname}" "$PBXPROJ" 2>/dev/null; then
        # Get relative path from SOURCE_DIR for the message
        rel="${fpath#${SOURCE_DIR}/}"
        NOT_IN_PBX="${NOT_IN_PBX}${rel}"$'\n'
    fi
done <<< "$ALL_FILES"

# Check for files in pbxproj that no longer exist on disk
# （跳过 aria_* 命名的 PBXFileReference —— 那些指向 ../../modules/，不在 SOURCE_DIR 里）
while IFS= read -r line; do
    # 只看 name=xxx 或 path=xxx 里的文件名；aria 引用是 name = signal.cpp; path = ../.../signal.cpp
    # 这里抓 path 字段里的文件名（或纯路径）
    raw=$(echo "$line" | sed -n 's/.*path = \([^;]*\);.*/\1/p' | tr -d ' "')
    [[ -z "$raw" ]] && continue
    fname=$(basename "$raw")
    [[ "$fname" != *.mm && "$fname" != *.m && "$fname" != *.cpp && "$fname" != *.hpp && "$fname" != *.h && "$fname" != *Info.plist ]] && continue
    # 工程外引用（path 以 ../ 开头）跳过
    [[ "$raw" == ../* ]] && continue
    # Try to find the file anywhere under SOURCE_DIR
    if ! find "${SOURCE_DIR}" -name "${fname}" -type f 2>/dev/null | grep -q .; then
        ORPHAN="${ORPHAN}${fname}"$'\n'
    fi
done < <(grep -E 'path = .*\.(mm|m|cpp|hpp|h|plist)' "$PBXPROJ" 2>/dev/null || true)

# ── Step 3: Report ────────────────────────────────────────────────────────────
CHANGES=0

NOT_COUNT=$(echo "$NOT_IN_PBX" | grep -c '.' || true)
ORPHAN_COUNT=$(echo "$ORPHAN" | grep -c '.' || true)

if [[ "$NOT_COUNT" -gt 0 ]]; then
    warn "${NOT_COUNT} file(s) NOT in project.pbxproj:"
    echo "$NOT_IN_PBX" | while IFS= read -r f; do
        [[ -n "$f" ]] && echo "  + $f (missing from project)"
    done
    CHANGES=1
else
    ok "All source files are in project.pbxproj"
fi

if [[ "$ORPHAN_COUNT" -gt 0 ]]; then
    warn "${ORPHAN_COUNT} orphan file(s) in project.pbxproj (no longer on disk):"
    echo "$ORPHAN" | while IFS= read -r f; do
        [[ -n "$f" ]] && echo "  - $f (file deleted but still in project)"
    done
    CHANGES=1
else
    ok "No orphan files in project.pbxproj"
fi

if [[ "$CHANGES" -eq 0 ]]; then
    ok "Project is in sync. Nothing to do."
    exit 0
fi

# ── Step 4: Auto-update pbxproj (使用 xcodeproj gem，通过 Ruby 脚本安全修改) ──
if $DRY_RUN; then
    warn "Dry run — not modifying project.pbxproj"
    exit 0
fi

RUBY_SYNC="${SCRIPT_DIR}/pbxproj-sync.rb"
if [[ ! -f "$RUBY_SYNC" ]]; then
    warn "缺少 ${RUBY_SYNC}，无法自动修改 pbxproj"
    exit 1
fi

# Step 4a: 先移除 orphans
if [[ "$ORPHAN_COUNT" -gt 0 ]]; then
    info "Removing orphan files via Ruby helper..."
    # 把所有孤儿文件名收集成参数传给 Ruby 脚本
    ORPHAN_ARGS=()
    while IFS= read -r fname; do
        [[ -n "$fname" ]] && ORPHAN_ARGS+=("$fname")
    done <<< "$ORPHAN"
    if [[ ${#ORPHAN_ARGS[@]} -gt 0 ]]; then
        ruby "$RUBY_SYNC" remove "${ORPHAN_ARGS[@]}" || {
            warn "移除 orphan 失败"
            exit 1
        }
    fi
fi

# Step 4b: 再添加 missing
if [[ "$NOT_COUNT" -gt 0 ]]; then
    info "Adding missing files via Ruby helper..."
    ADD_ARGS=()
    while IFS= read -r relpath; do
        [[ -z "$relpath" ]] && continue
        # pbxproj-sync.rb 期待以工程根为起点的相对路径，需要加上 "ios-oc-mvvm/"
        ADD_ARGS+=("ios-oc-mvvm/${relpath}")
    done <<< "$NOT_IN_PBX"
    if [[ ${#ADD_ARGS[@]} -gt 0 ]]; then
        ruby "$RUBY_SYNC" add "${ADD_ARGS[@]}" || {
            warn "添加 missing 失败"
            exit 1
        }
    fi
fi

ok "Done. project.pbxproj updated."
echo ""
info "Rebuild to verify:"
echo "  xcodebuild -project ios-oc-mvvm.xcodeproj -scheme ios-oc-mvvm -sdk iphonesimulator build"
