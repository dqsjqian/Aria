#!/usr/bin/env bash
# Install the verified upstream macOS binary without changing Homebrew/PATH.
# Homebrew 1.18.0 can intermittently SIGBUS on ARM64:
# https://github.com/doxygen/doxygen/issues/12326
set -euo pipefail

destination="${1:?Usage: install-doxygen.sh DESTINATION}"
case "$(uname -s)/$(uname -m)" in
    Darwin/arm64)
        asset=doxygen-1.18.0-mac-arm.zip
        checksum=31d1c74467a9f6f456b4e6a4eff0c03e93d1ffa92da5fdf2d53a2f14ebaabbd7
        ;;
    Darwin/x86_64)
        asset=doxygen-1.18.0-mac-intel.zip
        checksum=8045d72f6f900fd430042339c1493d6eefcc85a7f990361fb57685682f16a2f9
        ;;
    *)
        echo "install-doxygen: this installer supports macOS ARM64 and Intel" >&2
        exit 1
        ;;
esac

staging="$(mktemp -d)"
trap 'rm -rf "$staging"' EXIT
archive="$staging/$asset"
curl --fail --location --silent --show-error \
    "https://github.com/doxygen/doxygen/releases/download/Release_1_18_0/$asset" \
    --output "$archive"
printf '%s  %s\n' "$checksum" "$archive" | shasum -a 256 --check --status
unzip -q "$archive" -d "$staging/extracted"
mkdir -p "$destination/bin"
install -m 755 "$staging/extracted/doxygen-1.18.0/doxygen" "$destination/bin/doxygen"
"$destination/bin/doxygen" --version
