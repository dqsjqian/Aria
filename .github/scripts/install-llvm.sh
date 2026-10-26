#!/usr/bin/env bash
# Installs LLVM $LLVM_VERSION from apt.llvm.org (the LLVM project's own apt
# repository, which publishes every release line for both Ubuntu images that
# ubuntu-latest can land on) and exports CC / CXX / CLANG_TIDY.
#
# Usage: install-llvm.sh [libcxx] [runtime] [tidy]
#   libcxx   libc++ and libc++abi
#   runtime  compiler-rt: sanitizer runtimes and libFuzzer
#   tidy     clang-tidy
set -euo pipefail
: "${LLVM_VERSION:?LLVM_VERSION must be set}"
v=$LLVM_VERSION

packages=("clang-$v")
for component in "$@"; do
  case "$component" in
    libcxx)  packages+=("libc++-$v-dev" "libc++abi-$v-dev") ;;
    runtime) packages+=("libclang-rt-$v-dev") ;;
    tidy)    packages+=("clang-tidy-$v") ;;
    *) echo "::error::unknown component '$component'"; exit 1 ;;
  esac
done

. /etc/os-release
apt_get=(sudo apt-get -y -o Acquire::Retries=5 -o Acquire::http::Timeout=30
         -o Acquire::https::Timeout=30 -o DPkg::Lock::Timeout=300)
sudo install -d -m 0755 /etc/apt/keyrings
curl -fsSL --retry 5 https://apt.llvm.org/llvm-snapshot.gpg.key \
  | sudo gpg --batch --yes --dearmor -o /etc/apt/keyrings/apt.llvm.org.gpg
echo "deb [signed-by=/etc/apt/keyrings/apt.llvm.org.gpg] https://apt.llvm.org/$VERSION_CODENAME/ llvm-toolchain-$VERSION_CODENAME-$v main" \
  | sudo tee "/etc/apt/sources.list.d/llvm-$v.list" > /dev/null
"${apt_get[@]}" update
"${apt_get[@]}" install --no-install-recommends "${packages[@]}"

installed=$("clang++-$v" -dumpversion)
case "$installed" in
  "$v".*) ;;
  *) echo "::error::expected LLVM $v, found $installed"; exit 1 ;;
esac
{
  echo "CC=clang-$v"
  echo "CXX=clang++-$v"
  echo "CLANG_TIDY=clang-tidy-$v"
} >> "$GITHUB_ENV"
echo "image: $PRETTY_NAME ($(uname -m)) | $("clang++-$v" --version | head -1)"
