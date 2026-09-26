# Third-Party Notices

Aria depends on the following third-party components. None of them are
vendored into this repository: since 3.0, every dependency is downloaded
once, verified against a SHA256 pinned at its `cmake/ariaFetchPinned.cmake`
call site, and cached under `build/_deps` (see `docs/migration-3.0.md`).
Each component is distributed under its own license, summarized below.
Aria itself is licensed under the MIT License (see `LICENSE`).

---

## Continuo

- **Version:** 0.1.5 (hash-pinned release archive, pinned in `modules/adapters/http/CMakeLists.txt`)
- **Copyright:** Copyright (c) 2026 dqsjqian
- **License:** MIT — see `LICENSE` upstream
- **Upstream:** https://github.com/dqsjqian/continuo
- **Used by:** `aria::http` transport (event loop, TCP, TLS, HTTP/1.1 serving)

## nlohmann/json

- **Version:** 3.12.0 (hash-pinned `json.tar.xz` release asset)
- **Copyright:** Copyright (c) 2013-2023 Niels Lohmann
- **License:** MIT — reproduced in the release archive as `LICENSE.MIT`
- **Upstream:** https://github.com/nlohmann/json
- **Used by:** `aria::http` JSON encode/decode

## doctest

- **Version:** 2.5.3 (hash-pinned single header, tests only — not distributed with the installed library)
- **Copyright:** Copyright (c) 2016-2023 Viktor Kirilov
- **License:** MIT — reproduced in the upstream repository
- **Upstream:** https://github.com/doctest/doctest
- **Note:** consumed with one reviewed patch (MSVC pragma guard), documented in the root `CMakeLists.txt`

## OpenSSL

- **Version:** 4.0.2 (hash-pinned release archive, built from source via `cmake/BuildOpenSSL.cmake` when TLS is enabled)
- **License:** Apache License 2.0 — reproduced in the release archive as `LICENSE.txt`
- **Upstream:** https://github.com/openssl/openssl
- **Used by:** Continuo's TLS module (server/client certificates, ALPN, mTLS)

---

## Removed in 3.0

- **cpp-httplib** (v0.53.1–0.54.1, bundled single-header with a local
  `SO_EXCLUSIVEADDRUSE` patch) was the HTTP adapter transport until 2.x and is
  no longer distributed with, or depended on by, Aria. Its MIT notice applied
  to the versions previously bundled here. The transport is now Continuo.
- **CPM.cmake** was used as an offline fallback for doctest and is no longer
  part of the build.

---

All MIT-licensed components retain their original copyright notices in their
source headers and accompanying license files, as the MIT License requires.
OpenSSL is consumed as a pinned source archive and is not redistributed
within this repository.
