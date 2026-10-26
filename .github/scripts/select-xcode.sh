#!/usr/bin/env bash
# Selects the newest released Xcode on the image. Betas and release candidates
# are skipped even when a plain-named symlink (Xcode_27.2.app) points at them.
set -euo pipefail
xcode=""
for candidate in $(ls -d /Applications/Xcode_*.app | grep -E '/Xcode_[0-9]+(\.[0-9]+)*\.app$' | sort -V); do
  target=$(cd "$candidate" && pwd -P)
  case "$(basename "$target")" in
    *[Bb]eta*|*[Rr]elease_[Cc]andidate*|*_RC*) continue ;;
  esac
  xcode=$target
done
[ -n "$xcode" ] || { echo "::error::no released Xcode found"; exit 1; }
sudo xcode-select -s "$xcode"
echo "macOS $(sw_vers -productVersion) | $(xcodebuild -version | head -1) | $(clang++ --version | head -1)"
