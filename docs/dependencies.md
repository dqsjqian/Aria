# Dependency versions and reproducible builds

Aria manages nlohmann/json, doctest, Mira and OpenSSL through
[`dependencies.json`](../dependencies.json) and
[`dependencies.lock.json`](../dependencies.lock.json). The manifest omits a
version by default: the resolver selects the highest stable upstream release
on its first resolution. The lock records the selected version, full commit,
download URL, SHA256 and original manifest entry. Subsequent configurations
reuse that selection; they do not automatically move to a newer release.

The checked-in lock provides the initial selection for a fresh checkout.
Review and commit lock changes when updating a project. No third-party source
or Git submodule is checked into Aria. Downloads and extraction stay in ignored
build directories, and both cached archives and extracted sources are checked
before reuse.

## Select or update versions

The resolver uses Python 3.10+ and its standard library. A matching lock can be
read by CMake without Python. GitHub's `gh` CLI is optional; when available the
resolver can use its authenticated API access.

From the repository root:

```bash
# Fill missing entries, preserving every matching selection.
python3 scripts/dependencies.py resolve --manifest dependencies.json --lock dependencies.lock.json

# Explicitly discover current stable releases and update the lock.
python3 scripts/update_dependencies.py

# Update one dependency only.
python3 scripts/update_dependencies.py --only json

# Override selected versions for this update; other entries use their manifest requests.
python3 scripts/update_dependencies.py --version json=3.12.0 --version openssl=4.0.3
```

For a mixed policy, set `version` on each dependency that must stay fixed and
leave it absent on dependencies that should advance during updates. Running
the updater with no arguments then applies that policy to the whole manifest.
`--only mira` updates Mira while leaving every other lock record unchanged;
if combined with `--version`, every override must also appear in `--only`.
The updater changes selection metadata. The next normal configure/build
fetches or rebuilds the selected libraries; run the project tests to establish
compatibility before committing the new lock.

For a shared explicit requirement, add `"version": "3.12.0"` to the `json`
manifest entry and run `resolve`. Explicit versions take precedence over the
default stable-release selection. Upstream compatibility still matters:
choosing a version does not establish that it supports the APIs used by Aria.

A local CMake override has precedence over the manifest's version:

```bash
cmake -S . -B build/flavors/custom -DARIA_BUILD_HTTP=ON -DARIA_DEP_JSON_VERSION=3.12.0
```

The other source-library options are `ARIA_DEP_DOCTEST_VERSION`,
`ARIA_DEP_MIRA_VERSION` and `ARIA_DEP_OPENSSL_VERSION`. Clear a cached option
with `-DARIA_DEP_JSON_VERSION=` to return to the manifest and base lock.
Equivalent resolver overrides use `--version json=3.12.0` and can be repeated.
The CLI records its selected version in the lock: a later plain `resolve`
preserves it. Store a version in the manifest when it should also constrain
future `update` commands. With no explicit version, an existing valid lock
always takes precedence over discovering the latest release.

CMake writes missing or overridden selections to
`<binary-dir>/deps/aria-<identity>/dependencies.lock.json`, using the source
lock as a base. It never edits the checked-in lock during configuration.
This effective lock is reused on subsequent configurations. To deliberately
update the project's default selection, use the CLI `update` command above.
Changing or deleting the source lock invalidates its old effective selections;
they cannot shadow a newer source lock or revive a selection after deletion.

## Offline use and local sources

`--offline` makes the resolver require a matching lock without release lookup.
`-DARIA_DEPENDENCIES_OFFLINE=ON` additionally makes CMake require existing
cached downloads. Populate the cache during a normal build first; override its
location with `-DARIA_DEPS_CACHE_DIR=/path/to/cache` if necessary.

`-DARIA_PIN_JSON_SOURCE_DIR=/path/to/json` uses that source directory directly.
The same archive override is available for Mira and OpenSSL. It intentionally
bypasses resolution and checksum verification; the caller owns the contents
and compatibility of the supplied tree. A parent may also provide complete
dependency targets, in which case Aria reuses those targets. Source-version
options do not replace an already supplied target or a source-directory
override.

## Share a consumer's lock

A consumer can place the managed dependency entries alongside its own entries
in a single manifest and lock. Set `ARIA_DEPENDENCY_MANIFEST` and
`ARIA_DEPENDENCY_LOCK` before adding Aria to the build. Every managed dependency
used by that configuration must appear in the selected manifest.

The CMake interface is available directly to source consumers:

```cmake
include("${ARIA_DIR}/cmake/ariaDependencies.cmake")
aria_resolve_dependency(NAME json VERSION json_version URL json_url SHA256 json_sha)

include("${ARIA_DIR}/cmake/ariaFetchPinned.cmake")
aria_fetch_pinned_archive(NAME json)
# ARIA_PINNED_JSON_SOURCE_DIR now names the checked source directory.
```

`NAME` is case insensitive for resolution. `REVISION` and `TAG` optionally
name output variables, and `MANIFEST`/`LOCK` arguments override the global
paths for an individual lookup. The existing fetch helpers also accept
explicit `VERSION`, `URL` and `SHA256` values for unrelated dependencies.

## Qt is an installed SDK

Qt is discovered on the machine, never downloaded or installed by this
resolver. Aria requests natural descending version-folder order within
CMake's package search locations. Explicit `Qt6_DIR`, toolchain settings and
`CMAKE_PREFIX_PATH` retain their normal precedence, so the chosen SDK remains
compatible with the selected compiler.

Use `-DARIA_DEP_QT_VERSION=6.10.0` for an exact installed version. An unavailable
version fails configuration instead of silently selecting another SDK. Source
builds and installed Aria packages apply this option through
`aria_find_qt6(...)` in `cmake/ariaFindQt.cmake`.
