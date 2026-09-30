# Updating dependencies

Run these commands from the **Aria repository root**. Use Python 3.10+ with its standard library; substitute `python3` when required on macOS/Linux. Online resolution can use an authenticated `gh` CLI, or the public GitHub API. Install the compiler, CMake and platform SDKs listed in the README separately.

`dependencies.json` declares sources and optional persistent `version` requirements. `dependencies.lock.json` records selected versions, complete Git commits and download SHA256 values. Commit both files when they change. Normal builds reuse matching locks; a missing selection resolves the latest stable release once. Updating is explicit and excludes prereleases and development branches.

## Commands

```bash
python scripts/update_dependencies.py --help
python scripts/update_dependencies.py
python scripts/update_dependencies.py --only mira
python scripts/update_dependencies.py --only json --only mira
python scripts/update_dependencies.py --version json=3.12.0 --version openssl=4.0.3
python scripts/update_dependencies.py --only json --only mira --version json=3.12.0
```

The available, case-sensitive names are: `json`, `doctest`, `mira`, `openssl`. Repeat `--only` for multiple selections and `--version` for different overrides. When using both, select every overridden name with `--only`. Unknown names, duplicate overrides and overrides outside the selection are errors.

To keep two dependencies fixed while updating the third, add `"version": "3.12.0"` to the existing JSON entry and `"version": "4.0.3"` to the existing OpenSSL entry; preserve all their source fields. Leave `mira` without a `version` field. A plain updater run then respects the two fixed versions and selects the latest stable release for each unpinned dependency. `--only mira` instead changes only Mira, leaving every other record untouched. No script edits are needed.

Precedence: this invocation's `--version` overrides the manifest; explicit manifest versions override defaults. Without an explicit request, ordinary resolution reuses the lock and deliberate updating discovers the latest stable version. A command-line selection remains locked for normal builds, but does not constrain a later update unless repeated or placed in the manifest. To unpin a library, remove its manifest `version` and deliberately update it.

## Fetch, build, verify and commit

The updater atomically saves selection metadata; it does **not** compile the entire application or establish API compatibility. After it succeeds:

```bash
cmake -S . -B build/flavors/dependency-check -DARIA_BUILD_TESTS=ON -DARIA_BUILD_HTTP=ON -DARIA_HTTP_ENABLE_TLS=ON
cmake --build build/flavors/dependency-check --config Release --parallel 3
ctest --test-dir build/flavors/dependency-check -C Release --output-on-failure --no-tests=error
```

Follow the [README](../README.en.md) for platform SDK selection and additional probes. Use the same configuration for build and CTest. Review `git diff -- dependencies.json dependencies.lock.json` and commit changed declarations and locks only after the build/tests succeed. Do not commit ignored downloads, source caches or build-directory effective locks. CI and releases consume checked-in selections.

## Existing locks, offline operation and overrides

```bash
python scripts/dependencies.py resolve --manifest dependencies.json --lock dependencies.lock.json
python scripts/dependencies.py resolve --manifest dependencies.json --lock dependencies.lock.json --offline
```

`resolve` fills missing/mismatched entries and preserves valid selections. Its offline mode requires a matching lock; successful offline resolution does not ensure source archives are cached. `update --offline` cannot discover new releases. A deleted lock causes fresh resolution, but loses prior selections; prefer the updater for controlled upgrades.



Aria source builds support CMake library overrides such as `-DARIA_DEP_JSON_VERSION=3.12.0`; these use a build-directory lock and preserve the project lock. Clear a cached override to return to the project selection. Explicit source directories or preexisting dependency targets take precedence and remain the caller's responsibility. Qt is an installed SDK: `-DARIA_DEP_QT_VERSION=6.10.0` selects an exact installed version; otherwise CMake searches its visible locations while respecting `Qt6_DIR`, `CMAKE_PREFIX_PATH`, and toolchains. This updater does not install Qt.

## Errors and rollback

Resolution failures leave the existing lock unchanged. Correct invalid names/versions, network failures or API limits and retry; do not disable integrity checks. A successful resolution followed by a failed build means compatibility still needs work. Restore the manifest and lock from a known-good Git revision after preserving local changes, then repeat fetch/build/tests. Restoring metadata alone does not restore binaries. Never edit checksums to accept changed bytes.

Additional arguments: `--manifest PATH`, `--lock PATH`, `--base-lock PATH`, `--cache-dir PATH`, and `--offline`. Custom output locks do not automatically reconfigure consumers: pass consistent manifest/lock arguments to fetch/build tools and `-DARIA_DEPENDENCY_MANIFEST=... -DARIA_DEPENDENCY_LOCK=...` to Aria CMake source integrations. Run `--help` for the parser's complete interface.
