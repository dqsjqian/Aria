# Third-Party Notices

Aria bundles or depends on the following third-party components. Each is
distributed under its own license, reproduced alongside the component and
summarized below. Aria itself is licensed under the MIT License (see `LICENSE`).

---

## cpp-httplib

- **Version:** v0.53.1 (bundled single-header `third_party/cpp-httplib/httplib.h`)
- **Copyright:** Copyright (c) 2026 Yuji Hirose. All rights reserved.
- **License:** MIT — see `third_party/cpp-httplib/LICENSE`
- **Upstream:** https://github.com/yhirose/cpp-httplib

## doctest

- **Version:** bundled single-header (`third_party/doctest/doctest.h`)
- **Copyright:** Copyright (c) 2016-2023 Viktor Kirilov
- **License:** MIT — see `third_party/doctest/LICENSE`
- **Upstream:** https://github.com/doctest/doctest

## nlohmann/json

- **Version:** 3.11.3 (`third_party/nlohmann_json/include/nlohmann/json.hpp`)
- **Copyright:** Copyright (c) 2013-2023 Niels Lohmann
- **License:** MIT — see `third_party/nlohmann_json/LICENSE.MIT`
- **Upstream:** https://github.com/nlohmann/json

## OpenSSL

- **Form:** git submodule (`third_party/openssl`)
- **License:** Apache License 2.0 — see `third_party/openssl/LICENSE.txt`
- **Upstream:** https://github.com/openssl/openssl

---

All bundled MIT-licensed components retain their original copyright notices in
their source headers and accompanying `LICENSE` files, as required by the MIT
License. OpenSSL is consumed as a pinned submodule and is not redistributed in
source form within this repository.
