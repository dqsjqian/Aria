# Third-Party Notices

Aria's own code is licensed under the [MIT License](LICENSE). Third-party
components retain their own licenses; the root license does not replace them.

The versions below describe the current `resolved` entries in
[dependencies.json](dependencies.json). Downloads are verified and cached outside
tracked source. An explicit source, target, SDK, or version override can select
different components; distributors must check what their build actually uses.
See the [dependency guide](docs/dependencies.md) and update instructions
([中文](docs/dependency-updates.zh.md), [English](docs/dependency-updates.en.md)).

| Component | Current resolution | License | Use |
|---|---|---|---|
| [Mira](https://github.com/dqsjqian/Mira) | 1.0.0 | MIT | Optional HTTP adapter transport; TLS when enabled. WebSocket, HTTP/2 and HTTP/3 are disabled in this integration. |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 | MIT | Optional HTTP adapter JSON implementation; compiled template code can be present in binaries. |
| [OpenSSL](https://github.com/openssl/openssl) | 4.0.3 | Apache-2.0 | HTTP TLS when enabled; the bundled build produces static libraries. |
| [doctest](https://github.com/doctest/doctest) | 2.5.3 | MIT; embedded portions under Boost-1.0 | Tests only; not part of the installed Aria library. |
| [Qt Core, Gui and Widgets](https://doc.qt.io/qt-6/licensing.html) | Selected installed Qt 6 SDK | LGPL-3.0-only, with GPL/commercial alternatives as specified upstream | Optional Qt adapter; Qt plugins and bundled third-party components have their own notices. |

## Source notices

- Mira: Copyright (c) 2026 dqsjqian. See the selected source's `LICENSE`
  ([current revision](https://github.com/dqsjqian/Mira/blob/9386d89d2a259303a0a2c3b1539a5cb0e3fc0be5/LICENSE)).
- nlohmann/json: Copyright (c) 2013–2025 Niels Lohmann. See `LICENSE.MIT`
  ([current revision](https://github.com/nlohmann/json/blob/55f93686c01528224f448c19128836e7df245f72/LICENSE.MIT)).
  Embedded MIT portions also credit Evan Nemerson (2016–2021), in
  `include/nlohmann/thirdparty/hedley/hedley.hpp`, and Florian Loitsch (2009), in
  `include/nlohmann/detail/conversions/to_chars.hpp`. Other embedded MIT portions
  credit Björn Hoehrmann (2008–2009) and The Abseil Authors (2018). Installation
  extracts all selected JSON header SPDX attributions into `json-ATTRIBUTIONS.txt`.
- OpenSSL: see `LICENSE.txt`
  ([current revision](https://github.com/openssl/openssl/blob/af1775b60dfa141a4ad762585052cabeb9f37e9e/LICENSE.txt)).
  The selected source archive has no separate `NOTICE` file. Preserve applicable
  upstream notices and identify changes if distributing modified OpenSSL files.
  Its bundled Text::Template 1.56 is a build tool under GPL-1.0-or-later or
  Artistic-1.0, not a library linked into Aria.
- doctest: Copyright (c) 2016–2023 Viktor Kirilov. The selected header also
  identifies Catch/lest portions under the [Boost Software License 1.0](https://www.boost.org/LICENSE_1_0.txt).
  Preserve these notices if redistributing the test sources. The current build
  consumes the verified header without modifying its downloaded bytes.

## Source and binary distribution

The tracked source repository does not include the downloaded dependency source
caches or Qt SDK. A dependency URL is not a replacement for the license copies
required when distributing that dependency's source or compiled code.

SDK and application distributors must include the licenses and applicable
attributions of the components present in their build, including code linked
statically into another library. The SDK/archive installation copies Aria documents and the actual selected
Mira/JSON/OpenSSL licenses and available `NOTICE*` files under
`share/licenses/aria` (subject to `CMAKE_INSTALL_DATADIR`). Parent-supplied
dependencies without available source licenses require matching
`ARIA_JSON_LICENSE_FILE` / `ARIA_OPENSSL_LICENSE_FILE` and any applicable
`ARIA_<NAME>_NOTICE_FILES` before installation or packaging. Ordinary builds
remain usable. This collection is not a claim that every binary package,
particularly a Qt deployment, satisfies all distribution requirements.

For Qt under LGPLv3, include the required notices and both LGPLv3 and GPLv3
texts. Static linking can be supported under LGPLv3 section 4(d)(0) by providing
the corresponding library source and application material suitable for
relinking; an appropriate replaceable shared-library mechanism is the alternative
in section 4(d)(1). Distributing Qt binaries also requires an applicable route
for providing their corresponding source, including relevant modifications and
build material. Installation information may be required in the circumstances
specified by the license. This repository does not supply a complete Qt source
or relinking package. Consult the actual [LGPLv3](https://doc.qt.io/qt-6/lgpl.html),
[GPLv3 section 6](https://doc.qt.io/qt-6/gpl.html), and
[Qt third-party component list](https://doc.qt.io/qt-6/third-party-libraries.html)
for the selected SDK; Qt is not covered by Aria's MIT license.

Compiler/runtime DLLs and system SDK files, if redistributed, retain their own
terms. In particular, GCC runtime exceptions and Microsoft redistributable
conditions are not a blanket MIT license for those files.

## Historical dependencies

cpp-httplib and CPM.cmake are not used by the current build. Their original
licenses and notices continue to apply to older releases that contained them.
