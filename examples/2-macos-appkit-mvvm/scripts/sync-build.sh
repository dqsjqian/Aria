#!/usr/bin/env bash
# sync-build.sh — Auto-sync project.pbxproj when source files change.
#
# Usage:
#   ./scripts/sync-build.sh              # scan and update if needed
#   ./scripts/sync-build.sh --dry-run    # show what would change
#
# What it does:
#   1. Scans mac-oc-mvvm/ for *.mm, *.m, *.hpp, *.h, and *Info.plist files
#   2. Compares with current project.pbxproj references
#   3. Adds missing files (subdirectories included)
#   4. Removes orphan files (exist in pbxproj but not on disk)
#   5. Reports what changed

# Disable exit on error for the whole script - handle errors manually
set -uo pipefail
# Note: we deliberately NOT using 'set -e' because many commands
# (like grep) legitimately return non-zero in normal operation.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
SOURCE_DIR="${PROJECT_DIR}/mac-oc-mvvm"
PBXPROJ="${PROJECT_DIR}/mac-oc-mvvm.xcodeproj/project.pbxproj"

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
while IFS= read -r f; do
    [[ -f "$f" ]] && ALL_FILES="${ALL_FILES}${f}"$'\n'
done < <(find "${SOURCE_DIR}" \( -name '*.mm' -o -name '*.m' -o -name '*.hpp' -o -name '*.h' -o -name '*Info.plist' \) -type f 2>/dev/null)

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
while IFS= read -r line; do
    fname=$(echo "$line" | sed -n 's/.*path = \([^;]*\);.*/\1/p' | tr -d ' ')
    [[ -z "$fname" ]] && continue
    [[ "$fname" != *.mm && "$fname" != *.m && "$fname" != *.hpp && "$fname" != *.h && "$fname" != *Info.plist ]] && continue
    # Try to find the file anywhere under SOURCE_DIR
    if ! find "${SOURCE_DIR}" -name "${fname}" -type f 2>/dev/null | grep -q .; then
        ORPHAN="${ORPHAN}${fname}"$'\n'
    fi
done < <(grep -E 'path = .*\.(mm|m|hpp|h|plist);' "$PBXPROJ" 2>/dev/null || true)

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

# ── Step 4: Auto-update pbxproj ───────────────────────────────────────────────
if $DRY_RUN; then
    warn "Dry run — not modifying project.pbxproj"
    exit 0
fi

# ── Step 4a: Remove orphan files from pbxproj ─────────────────────────────────
if [[ "$ORPHAN_COUNT" -gt 0 ]]; then
    info "Removing orphan files from project.pbxproj..."
    while IFS= read -r fname; do
        [[ -z "$fname" ]] && continue
        info "  Removing: $fname"
        
        # Find the file reference ID (e.g., BB000010)
        # Line format: 		BB000010 /* DataModel.hpp */ = {isa = PBXFileReference; ... };
        file_ref_id=$(grep "PBXFileReference" "$PBXPROJ" | grep "$fname" | head -1 | awk '{print $1}' || true)
        
        if [[ -z "$file_ref_id" ]]; then
            warn "    Could not find fileRef ID for $fname, skipping"
            continue
        fi
        
        ok "    Found fileRef ID: $file_ref_id"
        
        # Find build file ID (e.g., CC488579) if it's a source file (.m/.mm)
        # Line format: 		CC488579 /* DataModel.mm in Sources */ = {isa = PBXBuildFile; ... };
        build_file_id=$(grep "PBXBuildFile" "$PBXPROJ" | grep "$fname" | head -1 | awk '{print $1}' || true)
        
        # 1. Remove from PBXFileReference section
        # Use sed to delete lines containing the file_ref_id and PBXFileReference
        sed -i '' "/${file_ref_id}.*PBXFileReference/d" "$PBXPROJ" || true
        
        # 2. Remove from PBXBuildFile section (if exists)
        if [[ -n "$build_file_id" ]]; then
            sed -i '' "/${build_file_id}.*PBXBuildFile/d" "$PBXPROJ" || true
        fi
        
        # 3. Remove from PBXSourcesBuildPhase (the reference to build file)
        if [[ -n "$build_file_id" ]]; then
            sed -i '' "/${build_file_id}.*in Sources/d" "$PBXPROJ" || true
        fi
        
        # 4. Remove from group children (references like "${file_ref_id} /* ${fname} */,")
        sed -i '' "/${file_ref_id}.*${fname} \*\//d" "$PBXPROJ" || true
        
        ok "    Removed: $fname"
    done <<< "$ORPHAN"
fi

# ── Step 4b: Add missing files to pbxproj ─────────────────────────────────────
counter=100
echo "$NOT_IN_PBX" | while IFS= read -r relpath; do
    [[ -z "$relpath" ]] && continue
    fname=$(basename "$relpath")
    ext="${fname##*.}"

    case "$ext" in
        mm)    ftype="sourcecode.cpp.objcpp" ;;
        m)     ftype="sourcecode.c.objc" ;;
        hpp)   ftype="sourcecode.cpp.h" ;;
        h)     ftype="sourcecode.c.h" ;;
        plist) ftype="text.plist.xml" ;;
        *)     ftype="sourcecode.c.objc" ;;
    esac

    file_ref_id=$(printf "CC%06X" "$counter")
    build_file_id=$(printf "DD%06X" "$((counter + 1000))")
    counter=$((counter + 1))

    # Add to PBXFileReference section
    sed -i '' "/\/\* End PBXFileReference section \*\//i\\
\	\	${file_ref_id} /\* ${fname} \*\/ = {isa = PBXFileReference; lastKnownFileType = ${ftype}; path = ${fname}; sourceTree = \"<group>\"; };" \
        "$PBXPROJ"

    # Add to Source group children (after Info.plist line)
    sed -i '' "/path = Info.plist;/a\\
\	\	\	${file_ref_id} /\* ${fname} \*\/," \
        "$PBXPROJ"

    # Add build file for .mm and .m files (they go in Sources phase)
    if [[ "$ext" == "mm" || "$ext" == "m" ]]; then
        sed -i '' "/\/\* End PBXBuildFile section \*\//i\\
\	\	${build_file_id} /\* ${fname} in Sources \*\/ = {isa = PBXBuildFile; fileRef = ${file_ref_id}; };" \
            "$PBXPROJ"

        # Add to Sources build phase (last entry before closing paren)
        sed -i '' "/runOnlyForDeploymentPostprocessing = 0;/i\\
\	\	\	${build_file_id} /\* ${fname} in Sources \*\/," \
            "$PBXPROJ"
    fi

    ok "Added: ${relpath} (fileRef=${file_ref_id})"
done

ok "Done. project.pbxproj updated."
echo ""
info "Rebuild to verify:"
echo "  xcodebuild -project mac-oc-mvvm.xcodeproj -scheme mac-oc-mvvm build"
