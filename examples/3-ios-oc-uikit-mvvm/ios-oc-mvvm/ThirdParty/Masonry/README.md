# Masonry (vendored)

Masonry is a lightweight Auto Layout DSL for iOS/macOS.

- Upstream: https://github.com/SnapKit/Masonry
- License: MIT (see LICENSE upstream)
- Vendored reason: 本 demo 不引入 CocoaPods，直接内嵌源码方便阅读与编译。
- 入口头：`Masonry.h`。使用时 `#import "Masonry.h"` 即可。

本目录仅拷贝 `Masonry/` 子目录下的源码，未做任何修改。
如需升级，直接用对应版本源码替换本目录内容即可（无额外工程改动，
已通过 `ios-oc-mvvm.xcodeproj` 的 HEADER_SEARCH_PATHS 配置包含本目录）。
