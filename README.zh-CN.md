# aria

> **现代 C++20 MVVM 框架 —— 跨平台、分层架构、协程优先。**
>
> 一套共享核心，覆盖 Windows / macOS / Linux / iOS / Android / Web。

[![Status](https://img.shields.io/badge/status-v1.0.0-blue.svg)](#)
[![C++](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](#)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

[English](README.md) | [简体中文](README.zh-CN.md) | [HTML 版本](README.zh-CN.html)

---

## 为什么做这个项目

现有的 C++ MVVM 方案要么捆绑一个庞大的 UI 框架（Qt 动辄 100+ MB），要么把你锁死在单一平台上，要么藏在宏后面让你摸不着头脑。**aria** 走相反的路：

- 小巧的 **仅头文件核心**，底层是一套统一的 **响应式依赖图引擎**（push 染色 + pull 求值）。`Property<T>` / `Computed<T>` / `Effect` / `Command<>` / `ObservableList<T>` / `Validator<T>` 共享同一个引擎 —— `Computed` 自动跟踪依赖（不再需要显式依赖列表），`reactive::batch` / `reactive::untracked` 在必要时让你精确控制通知范围。
- **类型擦除 ABI 层** —— 框架中非模板部分可以作为真正的共享库发布。`aria-abi` / `aria-runtime` / `aria-binding`（非模板导出）在主版本号内 ABI 稳定；模板层（`aria-core` / `aria-async`）仅源码兼容。
- **C++20 协程层** —— `Task<T>`、执行器、`co_await schedule_on(pool)`，让异步代码写起来像同步代码一样自然。
- 适配器抽象 (`IViewAdapter`) —— Qt6、AppKit、UIKit、JNI/Compose、Emscripten/WebAssembly，任何 UI 工具包都能用同一套业务逻辑驱动视图。

## 架构（10 个模块）

```
┌────────────────────────────────────────────────────────────────────────┐
│                         应用层 (Application)                            │
└────────────────────────────────┬───────────────────────────────────────┘
              ┌──────────────────┼──────────────────┐
              ▼                  ▼                  ▼
   ┌──────────────┐    ┌──────────────┐    ┌──────────────┐
   │ Qt6 适配器    │    │ JNI 适配器    │    │ HTTP 适配器   │     (可选模块；
   │ (Win/Mac/Lin)│    │  (Android)   │    │ REST/SSE Web │      按需启用)
   │ AppKit/UIKit │    │              │    │ WASM 计划中   │
   └──────┬───────┘    └──────┬───────┘    └──────┬───────┘
          └───────────────────┴───────────────────┘
                              ▼
              ┌─────────────────────────────────┐
              │    aria-binding  (SHARED)       │
              │  BindingEngine + IViewAdapter   │
              └────────────────┬────────────────┘
                               │
        ┌──────────────────────┴───────────────────────┐
        ▼                                              ▼
┌───────────────────┐                       ┌────────────────────┐
│ aria-runtime      │                       │  aria-async        │
│ (SHARED .dylib)   │                       │   (仅头文件)        │
│ EventBus          │                       │ Task<T>            │
│ Container         │                       │ Scheduler          │
│ Dispatcher        │                       │ Executor           │
│ Logger            │                       │ schedule_on        │
└───────┬───────────┘                       └─────────┬──────────┘
        └──────────────────┬─────────────────────────┘
                           ▼
              ┌─────────────────────────────┐
              │ aria-core  (仅头文件)        │
              │ Property / Computed / Cmd   │
              │ ObservableList / Validator  │
              │ Subscription                │
              └──────────────┬──────────────┘
                             ▼
              ┌─────────────────────────────┐
              │  aria-abi  (STATIC .a)      │
              │ 类型擦除 Signal/Slot         │
              │ ABI 稳定，无模板              │
              └─────────────────────────────┘
```

| 模块 | 类型 | 依赖 | 说明 |
|------|------|------|------|
| `aria-abi` | `STATIC` | 无 | 类型擦除的信号/槽，无模板，**ABI 稳定**。 |
| `aria-core` | 仅头文件 | abi | 全部模板：`Property`、`Computed`、`Command`、`ObservableList`、`Validator`。仅源码兼容（非 ABI 稳定）。 |
| `aria-async` | 仅头文件 | core | C++20 `Task<T>`、执行器。仅源码兼容。 |
| `aria-runtime` | `SHARED` | core, abi | EventBus / Container / Dispatcher / Logger —— 单例统一放在 **一个** 动态库中。**ABI 稳定**（非模板导出）。 |
| `aria-binding` | `SHARED` | core, runtime | `BindingEngine`、`IViewAdapter`。**ABI 稳定**（非模板导出）。 |
| 适配器 | `SHARED`/`STATIC` | binding | Qt6 / AppKit / UIKit / JNI / HTTP（按需启用）；WASM 计划中。 |

## 环境要求

- **CMake** >= 3.20
- **完整支持 C++20 的编译器**：
  - GCC >= 12（Windows 下可走 MSYS2 UCRT64 工具链）
  - Clang >= 15（macOS/iOS 上 AppleClang 15+ 即可）
  - **MSVC v143 / Visual Studio 2022**（Windows，详见下文）
- *(可选)* **Qt6** >= 6.4（用于 Qt6 适配器和 GUI 示例）

> **Windows 同时支持 MSYS2 UCRT64（GCC）和 MSVC / Visual Studio 2022 两条工具链。** 团队栈里有哪个就用哪个 —— 同一棵源码树都能编出完整框架 + 测试 + 适配器，不需要分支或 fork。见下文 [Windows 工具链](#windows-工具链)。

## 快速开始

```bash
git clone https://github.com/dqsjqian/aria.git
cd aria
cmake -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

> 首次配置会通过内置的 `CPM.cmake` 拉取 [doctest](https://github.com/doctest/doctest)。之后全部离线可用。

### 一键构建脚本

```bash
# macOS / Linux
scripts/build.sh             # Release
scripts/build.sh tests       # Release + 跑测试
scripts/build.sh asan        # Debug + AddressSanitizer + UBSan
scripts/build.sh tsan        # Debug + ThreadSanitizer
scripts/build.sh clean

# Windows —— MSYS2 UCRT64（GCC + Ninja）
scripts\build.ps1            # Release
scripts\build.ps1 tests
scripts\build.ps1 asan

# Windows —— MSVC / Visual Studio 2022
scripts\build-msvc.ps1       # Release（使用 build/flavors/msvc/ 目录）
scripts\build-msvc.ps1 tests
scripts\build-msvc.ps1 debug
scripts\build-msvc.ps1 asan  # /fsanitize=address（MSVC 不带 UBSan）
```

### Windows 工具链

aria 在 `scripts/` 下提供**两个并行的构建脚本**，分别对应 Windows 上两条主流工具链。它们写到不同的 build 目录、互相独立，不需要互相感知。

| 工具链 | 脚本 | 构建目录 | 备注 |
|---|---|---|---|
| **MSYS2 UCRT64**（GCC 14+ / Clang 18+） | `scripts\build.ps1` | `build/` | 体积小（≈300 MB），大多数 CI 镜像已预装。脚本会从 `C:\msys64\ucrt64\bin` 等常见路径自动定位。 |
| **MSVC v143**（VS 2022） | `scripts\build-msvc.ps1` | `build/flavors/msvc/` | 通过 `vswhere` 自动定位 VS 安装；进入 CMake 之前会先清掉 MSYS2 留下的 `INCLUDE` / `LIB` / `CPATH` 等环境变量；使用 `Visual Studio 17 2022` 生成器。 |

两条工具链可以来回切换、不需要 `clean`，build 目录互不影响。CI 每晚都会跑两条以确保不退化。

#### MSVC 一次性配置

```powershell
# 1. 安装 Visual Studio 2022 Build Tools（或完整 IDE），勾选
#    "Desktop development with C++" + "C++ CMake tools"。
# 2. （可选）安装 Qt 6 的 msvc2022_64 组件，如果需要 Qt6 适配器 / Qt 示例。
# 3. 任意 PowerShell 窗口里：
scripts\build-msvc.ps1 tests
```

#### MSYS2 一次性配置

```powershell
# 1. 从 https://www.msys2.org 安装 MSYS2
# 2. 打开 "MSYS2 UCRT64" 终端：
pacman -Syu
pacman -S --needed mingw-w64-ucrt-x86_64-toolchain `
                   mingw-w64-ucrt-x86_64-cmake `
                   mingw-w64-ucrt-x86_64-ninja git
# 3. （可选）把 C:\msys64\ucrt64\bin 加入 PATH
# 4. 从任意终端执行：
scripts\build.ps1 tests
```

为什么两条都支持：aria 是协程重的 C++20 代码，libstdc++、libc++ 和 MSVC STL 都能干净处理。早期只支持 MSYS2 把 .NET / Visual Studio 生态的用户挡在门外，意义不大。现在 MSVC v143 与 macOS / Ubuntu / MSYS2 走的是**同一条 release 闸门**。

### 在自己的项目中使用

**方式 A —— 先安装，再用 `find_package`**（生产环境推荐）：

```bash
# 在 aria 目录下：
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build -j && sudo cmake --install build
```

```cmake
# 在你项目的 CMakeLists.txt 中：
find_package(aria 1.0 REQUIRED)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE aria::aria)
# 也可以按需选择模块：aria::core / ::async / ::runtime / ::binding
```

**方式 B —— 直接嵌入（不安装）**：

```cmake
add_subdirectory(third_party/aria EXCLUDE_FROM_ALL)
target_link_libraries(my_app PRIVATE aria::core aria::async)
```

即拷即用的模板在 [`templates/quickstart/`](templates/quickstart/)。

### 示例项目

aria 提供覆盖**每一个受支持 UI 工具包**的可运行示例，外加几个无界面、专门压核心的控制台示例。

**UI 展示示例 —— 每个工具包一个：**

| #  | 项目                     | 工具包                 | 构建方式 | 演示内容 |
|----|--------------------------|------------------------|----------|----------|
| 1  | **qt-showcase**          | Qt6 (Widgets)          | CMake（`ARIA_BUILD_QT6=ON`） | **总展厅 Demo**：一个应用、九个 Tab，覆盖框架的每一个公开能力 —— 响应式 Property/Computed/Effect、Command、ObservableList + QAbstractListModel、Validator、`Task<T>` + 执行器、取消、重试、`when_all`、EventBus、DI Container、Dispatcher、导航、双向绑定。 |
| 2  | **macos-appkit-mvvm**    | macOS AppKit (ObjC++)  | Xcode | 独立的 Xcode 项目，演示在 Objective-C++ 里使用 `aria`：自定义 `IViewAdapter` 绑定原生 `NSTextField`/`NSButton`，同一个 ViewModel 直接驱动 AppKit 控件。 |
| 3  | **ios-oc-uikit-mvvm**    | iOS UIKit (ObjC++)     | Xcode | iOS 端独立 Xcode 项目：自定义 `IViewAdapter` 绑定原生 `UILabel`/`UITextField`/`UIButton`（布局走 Masonry），同一个 ViewModel 在 iPhone/iPad 上原生跑起来。 |
| 4  | **web-mvvm**             | Web（HTTP/REST/SSE）   | CMake（`ARIA_BUILD_HTTP=ON`） | 用 `HttpAdapter` 把一个 C++ ViewModel 暴露给浏览器 —— 状态经 SSE 推送、命令经 REST 触发、双向绑定，配套一个 vanilla-JS 客户端（`aria_client.js`）。可选 HTTPS。 |
| 5  | **android-jni-mvvm**     | Android（JNI + Compose/View） | Gradle（NDK r26+） | Android Studio / Gradle 工程，通过 `aria-jni` 适配器从 Kotlin 驱动同一个 C++ ViewModel。 |

**无界面 / 控制台示例**（`ARIA_BUILD_EXAMPLES=ON` 时构建，无需 GUI）：

| 项目 | 演示内容 |
|------|----------|
| **inspector-demo** | CLI 响应式图 flush 追踪器 —— 打印实时依赖图的 push/pull 轨迹（诊断 / `TraceSink`）。 |
| **plugin-property-demo** | 跨 dylib 的 ABI 冒烟：宿主 exe + 插件共享库，仅通过稳定的非模板 `aria::IProperty` 接口跨 DSO 边界操作一个 `Property<T>`。以 `cross_dylib_abi_smoke` 测试运行。 |
| **todomvc** | 无界面 TodoMVC：`ObservableList` + 两个实时 `FilteredList` 视图（active/completed）+ `Selection`，全部增量联动。以 `todomvc_smoke` 测试运行。 |

构建并运行示例 1（Qt）：

```bash
cmake -S . -B build -DARIA_BUILD_QT6=ON
cmake --build build -j
./build/examples/1-qt-showcase/ex_qt_showcase
```

示例 4（web）需要 HTTP 适配器，运行说明见
[`examples/4-web-mvvm/README.md`](examples/4-web-mvvm/)：

```bash
cmake -S . -B build -DARIA_BUILD_HTTP=ON
cmake --build build --target example_4_web_mvvm
```

无界面示例默认随 `ARIA_BUILD_EXAMPLES=ON` 构建，可用 `ctest`
（`cross_dylib_abi_smoke`、`todomvc_smoke`）或直接从 `build/bin/` 运行。

示例 2、3 **不参与** CMake 构建 —— 直接打开 Xcode 工程运行；示例 5 是
Android Studio / Gradle 工程（需 NDK r26+）：

- [`examples/2-macos-appkit-mvvm/mac-oc-mvvm.xcodeproj`](examples/2-macos-appkit-mvvm/) —— macOS AppKit
- [`examples/3-ios-oc-uikit-mvvm/ios-oc-mvvm.xcodeproj`](examples/3-ios-oc-uikit-mvvm/) —— iOS UIKit
- [`examples/5-android-jni-mvvm/`](examples/5-android-jni-mvvm/) —— Android（Gradle）

### 构建选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `ARIA_BUILD_TESTS` | ON | 构建单元测试并注册到 ctest。 |
| `ARIA_BUILD_EXAMPLES` | ON | 构建所有示例（控制台 + 启用的 Qt6 示例）。 |
| `ARIA_BUILD_BENCHMARK` | ON | 构建微基准测试。 |
| `ARIA_BUILD_SHARED` | ON | runtime/binding 编译为动态库。 |
| `ARIA_BUILD_QT6` | OFF | 构建 Qt6 适配器和 GUI 示例（需要 `Qt6Widgets`）。 |
| `ARIA_BUILD_APPKIT` | OFF | **（已 production-grade）** macOS AppKit 适配器作为一等 CMake 模块——以 `STATIC` + `.mm` 方式编译，导出 `aria::adapters::appkit`，完整通过 `adapter_conformance` 测试套件；需要 `APPLE` 平台。`examples/2-macos-appkit-mvvm/` 示例已通过 `BindingEngine::bind_text_oneway` / `bind_command` 真接入该适配器。 |
| `ARIA_BUILD_UIKIT` | OFF | **（已 production-grade）** iOS UIKit 适配器作为一等 CMake 模块——以 `STATIC` + `.mm` 方式编译，导出 `aria::adapters::uikit`，在 iPhone 17 Pro Max 模拟器跑通 25/25 in-app 一致性用例；需要 `APPLE` 平台。`examples/3-ios-oc-uikit-mvvm/` 示例已通过 `BindingEngine::bind_text_oneway` / `bind_command` 真接入该适配器。 |
| `ARIA_BUILD_JNI` | OFF | Android JNI 适配器，作为一等 CMake 模块——构建为 `STATIC`，提供 `aria::adapters::jni`，通过 JNI 反射调用实现与 Qt/AppKit/UIKit 相同的 `IViewAdapter` 契约（文本 / 布尔 / 整数 / 浮点 / 可见性 / 点击）。需要 Android NDK 工具链（**NDK r26+**——重度依赖 C++20 concepts 的核心无法在 NDK r25 的 libc++ 下编译）。 |
| `ARIA_BUILD_WASM` | OFF | *(计划中)* WebAssembly 适配器。 |
| `ARIA_ENABLE_ASAN` | OFF | AddressSanitizer。 |
| `ARIA_ENABLE_UBSAN` | OFF | UndefinedBehaviorSanitizer。 |
| `ARIA_ENABLE_TSAN` | OFF | ThreadSanitizer。 |

## Hello, world

```cpp
#include "aria/aria.hpp"
using namespace aria;

Property<int> count{0};

// 不再需要显式依赖列表 —— Computed 首次求值时，
// 内部读到的每一个 Property::get() 都会被自动追踪为依赖。
Computed<std::string> label([&]{
    return "count = " + std::to_string(count.get());
});

Command<> increment([&]{ count = count.get() + 1; });

auto sub = label.bind([](const std::string& s) { std::cout << s << '\n'; });

increment();   // → "count = 1"
increment();   // → "count = 2"
```

## 异步编程（C++20 协程）

```cpp
#include "aria/async/task.hpp"
#include "aria/async/executor.hpp"
using namespace aria::async;

Task<std::string> fetch_user(int id) {
    co_await schedule_on(network_pool);     // 跳到工作线程
    auto raw = http::get("/users/" + std::to_string(id));
    co_await schedule_on(main_dispatcher);   // 切回 UI 线程
    co_return parse(raw);
}
```

## 跨平台映射

| 平台   | UI 宿主        | 适配器                            |
|--------|----------------|------------------------------------|
| Windows    | Qt6 / WinUI    | `aria-qt6` ✅ 可用（MSYS2 UCRT64 + MSVC 2022） |
| macOS      | AppKit / Qt6   | `aria-qt6` ✅ 可用；AppKit ✅ 可用（示例 2） |
| Linux      | Qt6 / GTK      | `aria-qt6` ✅ 可用             |
| iOS        | UIKit / SwiftUI bridge | UIKit ✅ 可用（示例 3）；`aria-uikit` 模块化计划中 |
| Android    | Compose / View | `aria-jni` ✅ 就绪（NDK r26+）   |
| **Web（服务端驱动）** | **浏览器 HTML/JS** | **`aria-http` ✅ 可用（REST + SSE；示例 4）** |
| Web（浏览器内 C++） | DOM via WASM     | `aria-wasm` 计划中             |

HTTP 适配器内置一个小型服务器（`HttpAdapter`），通过 JSON REST + Server-Sent-Events 协议把任意 ViewModel 暴露给浏览器，并附带一个 vanilla-JS SDK（`aria_client.js`）。它适合给桌面应用挂一个网页 UI、给无界面服务做前端、做本地调试看板等场景。WASM 适配器把 C++ 业务编进浏览器沙箱，解决的是另一类受限问题，仍在路线图上。详见 [RFC 0001](docs/rfc/0001-http-adapter.md)。

**当前版本**已交付平台无关的核心、runtime、async 和 binding 层，全部通过单元测试。Qt6、AppKit、UIKit、JNI、HTTP 都已作为 CMake 一等可选适配器交付（受各自平台要求约束）。WASM 仍在路线图上；`IViewAdapter` 接口已稳定。

## 测试状态

```
$ ctest --test-dir build --output-on-failure
Test project /…/aria/build
    Start 1: abi_tests           ✅ Passed
    Start 2: core_tests          ✅ Passed
    Start 3: fuzz_tests          ✅ Passed
    Start 4: async_tests         ✅ Passed
    Start 5: runtime_tests       ✅ Passed
    Start 6: binding_tests       ✅ Passed
    Start 7: qt6_tests           ✅ Passed   (开启 ARIA_BUILD_QT6 时)
    Start 8: appkit_conformance  ✅ Passed   (Apple 平台)
    Start 9: appkit_table_source ✅ Passed   (Apple 平台)

100% tests passed, 0 tests failed（按选项最多 9 个 suites）
```

75+ 个测试用例分布在以上 suite 中，涵盖 `docs/reference/lifecycle.md`、`docs/reference/error-model.md` 中所有生命周期 / 重入 / 异常安全契约的回归测试。

## 性能基准（Apple M 系列, -O3 -DNDEBUG）

| 操作 | 纳秒/次 |
|-----------|-------|
| `Property<int>::get()`                          | 10.4 |
| `Property<int>::set()` 无观察者                  | 28.5 |
| `Property<int>::set()` 1 个观察者                | 29.3 |
| `Property<int>::set()` 10 个观察者               | 45.9 |
| 订阅 + 自动取消订阅周期                           | 54.9 |
| Computed 链 x5（set + 重新计算 + get）            | 289.1 |
| `EventBus::publish`（1 个订阅者）                 | 13.4 |
| `Container::resolve<Singleton>`                  | 7.6  |
| 10 次 set 包在 `reactive::batch` 中（只通知一次）    | 156.1 |
| 批量更新加速比（对比逐次更新）                         | **1.91×** |

## 框架本体契约

Aria 承诺的所有非平庸行为都钉在一份带编号的契约文档里，每条契约都有一个稳定 ID（如 `L-13` / `E-22` / `LD-7` / `D-4` / `S-31`）——测试断言失败或 PR 评审可以直接指向权威描述。

| 文档 | 前缀 | 范围 |
|---|---|---|
| [`docs/reference/api-style.md`](docs/reference/api-style.md)            | `S-N`  | 命名、命名空间、错误与异步入口的风格约束 |
| [`docs/reference/lifecycle.md`](docs/reference/lifecycle.md)            | `L-N`  | 线程、订阅、响应式 flush、view 销毁、异步 cancel/dtor 不变式 |
| [`docs/reference/error-model.md`](docs/reference/error-model.md)        | `E-N`  | `aria::Error` / `ErrorKind` taxonomy 与各子系统错误面契约 |
| [`docs/reference/list-diff-contract.md`](docs/reference/list-diff-contract.md) | `LD-N` | `Insert / Remove / Replace / Move / Reset / ItemChanged` 语义 |
| [`docs/reference/diagnostics.md`](docs/reference/diagnostics.md)        | `D-N`  | `aria::TraceEvent` + `aria::TraceSink` 诊断协议 |
| [`docs/reference/performance.md`](docs/reference/performance.md)        | `PERF-N` | 每个公开 API 的复杂度上界与实测基线 |

P0 硬地基所以五件套（详见 CHANGELOG）推平了上表所有契约；`modules/core/fuzz/` 下七个框架级 fuzzer 为 lifecycle 不变式提供压力验证（默认 50k 迭代 / fuzzer；nightly 设 `ARIA_FUZZ_ITERS=1000000` 拉高）。

## 路线图

Aria 不对外发版，主版本号永远停留在 `1.0.0`，也不维护版本演进史。
**待办（TODO）与已延后清单**的唯一信息源在
[`docs/ROADMAP.md`](docs/ROADMAP.md)；当前能力的快照见
[`CHANGELOG.md`](CHANGELOG.md)。

## 贡献指南

欢迎贡献！涉及架构改动的改动请先开 Issue 讨论。

- 代码风格由 `.clang-format` 和 `.clang-tidy` 统一管控。
- 所有变更必须通过 `ctest --output-on-failure`。
- 新功能需要在对应 `modules/*/tests/` 套件中补充测试。

## 致谢

- [doctest](https://github.com/doctest/doctest) —— 轻量级测试框架
- [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake) —— CMake 依赖管理

## 许可证

MIT © 2026 aria contributors

---

## 📖 其他格式

本文档还提供其他格式，以获得更好的阅读体验：

- **[HTML 版本](README.zh-CN.html)** - 增强的视觉体验，响应式设计
- **[English](README.md)** - 英文版本
- **[English HTML](README.html)** - 英文HTML版本

### 快速访问脚本

使用提供的脚本快速在浏览器中打开HTML版本：

```bash
# 打开中文HTML版本
./scripts/open-readme.sh zh

# 打开英文HTML版本
./scripts/open-readme.sh          # 或：./scripts/open-readme.sh en

# 同时打开两个版本
./scripts/open-readme.sh all
```

HTML版本相比Markdown版本提供更好的视觉布局、响应式设计和代码高亮效果。
