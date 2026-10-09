# Dependency versions and reproducible builds

Aria uses one [`dependencies.json`](../dependencies.json). Each dependency has
source fields, an optional requested `version`, and a generated `resolved`
object. Sources are recorded once. A missing/empty version or `"latest"` means
latest stable when first resolved or deliberately updated; normal builds reuse
the persisted result. Commit this file to reproduce the selection in CI.

Complete usage: [English](dependency-updates.en.md) ·
[中文](dependency-updates.zh.md). These guides cover mixed fixed/latest policies,
selective updates, command-line overrides, offline operation and rollback.

## CLI and selection

Python 3.10+ with its standard library is required only for new resolution.
CMake can consume valid embedded results without Python or network access.
The optional authenticated `gh` CLI can supply GitHub API access.

```bash
python3 scripts/dependencies.py update --file dependencies.json
python3 scripts/dependencies.py update --file dependencies.json --only mira
python3 scripts/dependencies.py update --file dependencies.json --version json=3.12.0 --version openssl=4.0.3
python3 scripts/dependencies.py resolve --file dependencies.json --offline
```

An invocation-specific version overrides an explicit declaration. A declaration
with a fixed version is restored by the next resolution without that override.
If the declaration uses the default/latest policy, ordinary resolution keeps a
previous command-line selection until another explicit request or update.
`--only` limits writes to selected entries; include every override in that list
when combining the two arguments.

Resolution is atomic across the selected entries. It records complete GitHub
commits and SHA256 values for downloads. The result's `request_hash` binds the
source fields and requested policy; editing either invalidates the old result.
The hash is a consistency check, not an upstream signature. Editing `resolved`
by hand is unsupported. Cached downloads and extracted sources are independently
verified before reuse. A successful update still requires compatibility testing.

## CMake source integration

```cmake
set(ARIA_DEPENDENCIES_FILE "${CMAKE_SOURCE_DIR}/dependencies.json" CACHE FILEPATH "")
include("${ARIA_DIR}/cmake/ariaDependencies.cmake")
aria_resolve_dependency(NAME json VERSION json_version URL json_url SHA256 json_sha)

include("${ARIA_DIR}/cmake/ariaFetchPinned.cmake")
aria_fetch_pinned_archive(NAME json)
# ARIA_PINNED_JSON_SOURCE_DIR names the verified source directory.
```

`NAME` is case insensitive. `FILE` selects another complete dependency file for
one lookup. `REVISION` and `TAG` optionally name output variables. Parents must
include declarations for every dependency used by their selected Aria features.
Fetch helpers also retain their explicit VERSION/URL/SHA256 interface for
unrelated dependencies.

Source-library version options: `ARIA_DEP_JSON_VERSION`,
`ARIA_DEP_DOCTEST_VERSION`, `ARIA_DEP_MIRA_VERSION`, `ARIA_DEP_OPENSSL_VERSION`.
For example:

```bash
cmake -S . -B build/flavors/custom -DARIA_BUILD_HTTP=ON -DARIA_DEP_JSON_VERSION=3.12.0
```

Overrides and missing results are resolved to an ignored build-directory
`dependencies.json`, leaving the source file unchanged. The effective file
binds the complete source-file fingerprint; updating source requirements or
results invalidates older build selections. Clear a cached version option,
for example `-DARIA_DEP_JSON_VERSION=`, to return to project policy and results.
Use the updater to persist a shared selection instead of editing build caches.

## Offline builds and local sources

`resolve --offline` requires matching embedded metadata. CMake's
`-DARIA_DEPENDENCIES_OFFLINE=ON` additionally requires cached source downloads.
Use `-DARIA_DEPS_CACHE_DIR=/path/to/cache` to select a populated cache.

`-DARIA_PIN_JSON_SOURCE_DIR=/path/to/json` supplies a source directory directly;
Mira and OpenSSL have equivalent source overrides. Such explicit directories
bypass resolver/checksum checks, so the caller owns their contents and
compatibility. Preexisting dependency targets supplied by a parent also take
precedence over source-version requests.

## Installed Qt SDK

Qt is discovered, not installed by the updater. `aria_find_qt6` requests natural
descending version-directory order within CMake-visible locations while
preserving explicit `Qt6_DIR`, `CMAKE_PREFIX_PATH` and toolchain precedence.
`-DARIA_DEP_QT_VERSION=6.10.0` requires that exact installed SDK; an unavailable
version fails rather than selecting another one. The helper restores the
parent's package-sort settings after its lookup.
