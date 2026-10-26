#!/usr/bin/env bash
# Runs inside the official gcc:<major> image (Debian based, GCC built from the
# release tarball into /usr/local). Installs the build tools plus the supported
# Python, and exports CC / CXX pointing at that GCC rather than Debian's own.
#
# Usage: setup-gcc-container.sh [extra Debian packages...]
set -euo pipefail
: "${GCC_VERSION:?GCC_VERSION must be set}"
: "${PYTHON_VERSION:=3.14}"

export DEBIAN_FRONTEND=noninteractive
apt_get=(apt-get -y -o Acquire::Retries=5 -o Acquire::http::Timeout=30 -o Acquire::https::Timeout=30)
"${apt_get[@]}" update
"${apt_get[@]}" install --no-install-recommends cmake ninja-build git curl ca-certificates "$@"

gxx=/usr/local/bin/g++
installed=$("$gxx" -dumpfullversion)
case "$installed" in
  "$GCC_VERSION".*) ;;
  *) echo "::error::container runs GCC $installed, workflow expects $GCC_VERSION (bump the image tag)"; exit 1 ;;
esac

# Debian ships an older Python than the tooling requires; uv provides the
# supported interpreter as a relocatable build, with pip seeded.
curl -LsSf --retry 5 https://astral.sh/uv/install.sh | env UV_INSTALL_DIR=/usr/local/bin sh
uv python install "$PYTHON_VERSION"
uv venv --seed --python "$PYTHON_VERSION" /opt/python
echo "/opt/python/bin" >> "$GITHUB_PATH"
{
  echo "CC=/usr/local/bin/gcc"
  echo "CXX=$gxx"
  echo "GCOV_TOOL=/usr/local/bin/gcov"
  echo "VIRTUAL_ENV=/opt/python"
} >> "$GITHUB_ENV"
. /etc/os-release
echo "image: gcc:$GCC_VERSION on $PRETTY_NAME ($(uname -m)) | $("$gxx" --version | head -1) | $(/opt/python/bin/python --version)"
