<div align="center">

# ⚡ Aria

**为工业级跨平台软件而生的现代 C++ MVVM 框架** · C++23 · 响应式 · 协程优先

一套 C++ 核心，六大平台。以优雅架构承载复杂业务，以工程契约支撑长期演进。

Windows / macOS / Linux / iOS / Android / Web

[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![CI](https://github.com/dqsjqian/Aria/actions/workflows/ci.yml/badge.svg)](https://github.com/dqsjqian/Aria/actions/workflows/ci.yml)
[![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20macOS%20%7C%20Linux%20%7C%20iOS%20%7C%20Android%20%7C%20Web-lightgrey.svg)](#)

[English](README.en.md) | [简体中文](README.md)

</div>

---

## 📚 从入门到精通：AriaTutorial

想系统地学 Aria？配套教程仓库 **[AriaTutorial](https://github.com/dqsjqian/AriaTutorial)** 提供 20 章中英双语渐进式教程，**每一章都配一个可以编译、可以运行的最小 demo**。

- 正文里每个代码块都与 `demos/` 下的源文件**逐字一致**，不是手抄的示意代码；
- 每段输出都是 demo 在 Windows / MSVC 下的**真实 stdout**，并由脚本自动校验；
- 每章配一张结论图（数据流 / 依赖图 / 状态机 / 时间线），图里的数字同样取自 demo 的真实运行结果；
- 教学路径覆盖：响应式核心 → 集合与表单 → 绑定与适配器 → 异步、诊断与测试 → 一个完整应用。

## 🌟 旗舰示例：AriaTools

想先看 Aria 如何落到真实应用？请从 [AriaTools](https://github.com/dqsjqian/AriaTools) 开始。它是 Aria 唯一的旗舰跨平台示例，同一份 C++ ViewModel 驱动 Qt、iOS、Android 与 Web 四端。本仓库只保留框架、验收测试和文档中的最小代码片段。

## 🚀 30 秒看懂 Aria

Aria 把一个界面切成两半：**ViewModel** 是纯 C++，不认识任何 UI 库；**View** 是各平台原生控件。
中间由 `BindingEngine` 连接，它只认 `IViewAdapter` 这个接口，所以换平台只换适配器。

**上半 —— ViewModel（纯 C++，可单元测试，所有平台共用这一份）**

```cpp
// AA 制账单：总额 ÷ 人数 = 每人多少
struct BillViewModel {
    aria::Property<double> bill{100.0};   // 可读写的状态
    aria::Property<int>    people{2};

    // Computed 是只读派生值。它的依赖不用手写：首次求值时读到了
    // bill 和 people，就自动记下这两个依赖。
    aria::Computed<double> per_person{[this] {
        return bill.get() / people.get();
    }};
};
```

这段代码里没有一行 UI，也没有 `#include` 任何界面库 —— 它在命令行下就能测。

**下半 —— View 侧接线（每个平台十几行，界面本身仍用各平台原生方式写）**

先说清最容易误会的一点：**界面不用 C++ 写。** 按钮、布局、动画照旧用 Qt Designer、
Storyboard、Compose、HTML 写。下面这十几行只是"把已经存在的控件交给 engine"的接线代码，
三步永远一样 —— ① 造平台适配器 ② 用它造 `BindingEngine` ③ 把控件和 Property 绑上。

<details open>
<summary><b>Qt6</b>（Windows / macOS / Linux · 纯 C++）</summary>

```cpp
auto adapter = std::make_shared<aria::adapters::qt6::QtAdapter>();
aria::binding::BindingEngine engine{adapter};

BillViewModel vm;
// label_view 包住你在 Qt Designer 里拖出来的那个 QLabel
aria::adapters::qt6::QtView label_view{real_label};
engine.bind_text_projected(vm.per_person, label_view,
    [](double v) { return std::format("¥{:.2f}", v); });
```
</details>

<details>
<summary><b>iOS / UIKit</b>（接线文件是 Objective-C++ <code>.mm</code>，界面仍是 Storyboard / SwiftUI）</summary>

```objc++
#import "aria/adapters/uikit/UIKitAdapter.hpp"

auto adapter = std::make_shared<aria::adapters::uikit::UIKitAdapter>();
aria::binding::BindingEngine engine(adapter, ui_dispatcher,
    aria::binding::BindingEngine::DispatchPolicy::SmartMarshal);

// 把 Storyboard 里的 UILabel* 包一层，C++ 侧就能绑它
auto label = std::make_shared<aria::adapters::uikit::UIKitView>(self.totalLabel);
engine.bind_text_projected(vm.per_person, *label,
    [](double v) { return std::format("¥{:.2f}", v); });
```

`UIKitView` 用 ARC 强引用持有 `UIView*`，析构时会在原生 view 还活着的时候通知
`BindingEngine` 清理订阅，所以不会回调到已释放的控件上。
</details>

<details>
<summary><b>macOS / AppKit</b>（同上，<code>.mm</code> + NSView）</summary>

```objc++
#import "aria/adapters/appkit/AppKitAdapter.hpp"

auto adapter = std::make_shared<aria::adapters::appkit::AppKitAdapter>();
aria::binding::BindingEngine engine(adapter, ui_dispatcher,
    aria::binding::BindingEngine::DispatchPolicy::SmartMarshal);

auto label = std::make_shared<aria::adapters::appkit::AppKitView>(self.totalField);
engine.bind_text_projected(vm.per_person, *label, /* ... */);
```
</details>

<details>
<summary><b>Android</b>（界面写 Kotlin / Compose，C++ 只接线）</summary>

```cpp
// 在 Android UI 线程上调用，传进来的是真实的 android.view.View 对象
auto adapter = std::make_shared<aria::adapters::jni::JniAdapter>(env);
aria::binding::BindingEngine engine(adapter);

aria::adapters::jni::JniView total_view(env, total_text_view);
engine.bind_text_projected(vm.per_person, total_view,
    [](double v) { return std::format("¥{:.2f}", v); });   // → TextView
```

Kotlin 侧的监听器把原生事件转发回来（`adapter->notify_text_changed(...)` /
`notify_click(...)`），**监听器归属仍在 Android 侧**，C++ 这边保持强类型。
Compose 没有可寻址的 view 对象，用文档里的 side-channel 形态。
</details>

<details>
<summary><b>Web</b>（浏览器里没有 C++ —— 前端是 HTML/JS，C++ 跑在服务端）</summary>

```cpp
aria::adapters::http::HttpAdapterConfig config;
config.port = 9090;
auto http = std::make_shared<aria::adapters::http::HttpAdapter>(config);

// "控件"在这里是字符串 ID，对应浏览器里的 DOM 元素
auto& total = http->register_view("total", "text");

aria::binding::BindingEngine engine{http};
engine.bind_text_projected(vm.per_person, total,
    [](double v) { return std::format("¥{:.2f}", v); });
http->start();  // Property 变化经 SSE 推给浏览器，用户操作经 REST 回来
```
</details>

**这才是重点**：五份接线代码长得几乎一样，而上面那个 `BillViewModel` **一个字都没改过**。
你换平台换的是这十几行，不是业务逻辑。

**接线之后 —— 只改数据，界面自己跟着变**

上面每个平台绑完，剩下的事就跟平台无关了。下面这段在五个平台上行为完全一致，
**没有一行手写的刷新代码**：

```cpp
BillViewModel vm;                       // per_person = 100/2 = ¥50.00
// ... 按上面任意一个平台绑定到 label ...

vm.people = 4;                          // label → ¥25.00
vm.bill   = 200.0;                      // label → ¥50.00
```

改 `bill` 或 `people` 任意一个，`per_person` 都会重算并推给 label —— 因为它的依赖是
`Computed` 首次求值时自动记下的，你没写过任何"people 变了要更新 label"这类代码。

连续改多个值时有一个细节值得知道：

```cpp
// 逐个改 → 每次都推一次，label 会闪过中间值
vm.bill = 300.0;    // label → ¥150.00  ← 中间态
vm.people = 4;      // label → ¥75.00

// 包进 batch → 只在结束时推一次，不出现中间态
aria::reactive::batch([&] {
    vm.bill   = 1200.0;
    vm.people = 8;
});                 // label → ¥150.00（一次）
```

还有一条省心的默认行为：**如果最终结果和当前值相同，一次通知都不会发。**
比如上面之后再 `batch` 里设 `bill=600, people=4`（仍是 150），label 不会被打扰。

整个架构一张图看全——上半是纯 C++ 的 ViewModel，中间是 `BindingEngine`（只认 `IViewAdapter` 接口），下半是五个原生适配器：

![Aria 架构总览](docs/marketing/images/aria-arch.png)

继续阅读：[绑定指南](docs/guide/binding.md) · [各平台适配器指南](docs/guide/adapters/) · [Cookbook](docs/cookbook/README.md) · [AriaTools](https://github.com/dqsjqian/AriaTools)（Qt / iOS / Android / Web 四端完整应用）。

## 🎯 设计理念与工程实力

Aria 将**响应式状态、异步协程与跨端绑定统一为独立于 UI 工具包的现代 C++ 架构**，让复杂业务只实现一次，让各端界面充分发挥原生能力。

优雅来自清晰的分工：ViewModel 是普通 C++ 类，无需继承框架基类、编写宏或运行代码生成器；
UI 通过 `IViewAdapter` 接入。Qt6 / AppKit / UIKit / JNI / HTTP 五个适配器共享同一套绑定协议，切换 UI 工具包无需改写 ViewModel。

**以世界级工业软件的工程标准打造 C++ 框架**：把生命周期、线程、错误处理与集合事件写成可追踪的[工程契约](docs/index.md#reference)，用自动化测试、模糊测试和可复现的[性能基准](docs/reference/performance.md)检验实现。架构之美，落实到每一次状态更新、异步取消与跨端交付。

| 架构优势 | 工程设计 |
|---|---|
| **现代 C++ 基础** | C++23 基线（GCC 14+ / Clang 19+ / AppleClang 21+ / MSVC v143），使用完整的协程与 concepts 能力；集成项目需采用 C++23 或更高标准。 |
| **原生 UI 自由** | 控件、布局和动画交给所选 UI 工具包，Aria 统一状态与界面之间的单向/双向数据流；共享业务核心，各端保留原生体验。 |
| **分层兼容策略** | `aria-abi` / `aria-runtime` / `aria-binding` 在主版本号内保持 ABI 稳定，要求编译器、标准库与构建选项一致；`Property<T>` 等模板及其宿主类型更新后需重新编译。 |
| **开放适配协议** | Qt6 / AppKit / UIKit / JNI / HTTP 开箱可用；其他 UI 工具包通过实现 `IViewAdapter` 接入（见[适配器指南](docs/guide/adapters/)）。 |
| **应用实践与开发资料** | AriaTools、AriaAgent 与 AriaRead 展示跨端工作台、Agent GUI 和阅读引擎中的应用实践；[指南](docs/index.md)、[Cookbook](docs/cookbook/README.md) 与工程契约覆盖从入门到扩展的开发路径。 |

**为共享 C++ 业务核心、原生多端 UI 和长期维护而设计。** Aria 负责业务与状态层，控件渲染与界面复用由所选 UI 工具包负责；两层通过明确的适配协议协作。

## ✨ 核心特性

- 📦 **模板化响应式核心** —— `Property<T>` / `Computed<T>` / `Effect` / `Command<>` / `ObservableList<T>` / `Validator<T>` 共享同一个响应式依赖图引擎。`Computed` 自动跟踪依赖，`reactive::batch` / `reactive::untracked` 精确控制通知范围。
- 🔌 **共享基础库与 ABI 层** —— `aria::core` 自动链接 `aria::abi`，统一跨动态库的响应式图、诊断和信号存储。ABI 要求一致的编译器、标准库、构建选项和主版本；模板及其宿主类型在更新后需要重新编译。
- ⚡ **C++ 协程** —— `Task<T>`、执行器、`co_await schedule_on(pool)`，异步代码写起来像同步代码。
- 🖥 **适配器抽象** (`IViewAdapter`) —— Qt6 / AppKit / UIKit / JNI / HTTP，任何 UI 工具包都能用同一套业务逻辑驱动。

## 🏗 架构（10 个模块）

```
┌────────────────────────────────────────────────────────────────────────┐
│                         应用层 (Application)                            │
└────────────────────────────────┬───────────────────────────────────────┘
              ┌──────────────────┼──────────────────┐
              ▼                  ▼                  ▼
   ┌──────────────┐    ┌──────────────┐    ┌──────────────┐
   │ Qt6 适配器    │    │ JNI 适配器    │    │ HTTP 适配器   │     (可选模块；
   │ (Win/Mac/Lin)│    │  (Android)   │    │ REST/SSE Web │      按需启用)
   │ AppKit/UIKit │    │              │    │ WASM 按需评估   │
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
              └────────────┬──────────────┘
                             ▼
              ┌─────────────────────────────┐
              │  aria-abi  (SHARED)      │
              │ 类型擦除 Signal/Slot         │
              │ ABI 稳定，无模板              │
              └─────────────────────────────┘
```

| 模块 | 类型 | 依赖 | 说明 |
|------|------|------|------|
| `aria-abi` | 默认 `SHARED` | Threads | 编译型基础库：信号/槽、共享响应式图、诊断存储、调度器基类与版本信息；支持静态构建。 |
| `aria-core` | 仅头文件 | abi | 全部模板：`Property`、`Computed`、`Command`、`ObservableList`、`Validator`。仅源码兼容。 |
| `aria-async` | 仅头文件 | core | 协程 `Task<T>`、执行器。仅源码兼容。 |
| `aria-runtime` | `SHARED` | core, abi | EventBus / Container / Dispatcher / Logger —— 单例统一放在**一个**动态库中。**ABI 稳定**。 |
| `aria-binding` | `SHARED` | core, runtime | `BindingEngine`、`IViewAdapter`。**ABI 稳定**。 |
| 适配器 | `SHARED`/`STATIC` | binding | Qt6 / AppKit / UIKit / JNI / HTTP（按需启用）；WASM 按需评估。 |

## 📋 环境要求

- **CMake** >= 3.20
- **完整支持 C++23 的编译器**：
  - GCC >= 13（Windows 下可走 MSYS2 UCRT64 工具链）
  - Clang >= 18（macOS 上 AppleClang 21+ 即可）
  - **MSVC v143 / Visual Studio 2022**（Windows，详见下文）
- *(可选)* **Qt6** >= 6.4（用于 Qt6 适配器）

> **Windows 同时支持 MSYS2 UCRT64（GCC）和 MSVC / Visual Studio 2022 两条工具链。** 团队栈里有哪个就用哪个 —— 同一棵源码树都能编出完整框架 + 测试 + 适配器，不需要分支或 fork。

## 🚀 快速开始

```bash
git clone https://github.com/dqsjqian/Aria.git
cd Aria
cmake -B build/flavors/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/flavors/release -j
ctest --test-dir build/flavors/release --output-on-failure
```

> `build/` 是构建树的**容器**，不要直接配置进它。统一布局见 [`scripts/build.sh`](scripts/build.sh) 顶部。

### 🔧 一键构建脚本

```bash
# macOS / Linux
scripts/build.sh             # Release
scripts/build.sh tests       # Release + 跑测试
scripts/build.sh asan        # Debug + AddressSanitizer + UBSan
scripts/build.sh tsan        # Debug + ThreadSanitizer

# Windows —— MSYS2 UCRT64（GCC + Ninja）
scripts\build.ps1            # Release
scripts\build.ps1 tests
scripts\build.ps1 asan
scripts\build.ps1 tsan       # Debug + ThreadSanitizer（MSVC 不支持，见下）

# Windows —— MSVC / Visual Studio 2022
scripts\build-msvc.ps1       # Release（使用 build/flavors/msvc/ 目录）
scripts\build-msvc.ps1 tests
scripts\build-msvc.ps1 debug
```

### 🛠 Windows 工具链

| 工具链 | 脚本 | 构建目录 | 备注 |
|---|---|---|---|
| **MSYS2 UCRT64**（GCC 14+ / Clang 19+） | `scripts\build.ps1` | `build/` | 体积小（≈300 MB），大多数 CI 镜像已预装。 |
| **MSVC v143**（VS 2022） | `scripts\build-msvc.ps1` | `build/flavors/msvc/` | 通过 `vswhere` 自动定位 VS 安装；使用 `Visual Studio 17 2022` 生成器。 |

<details>
<summary>📖 MSVC 一次性配置</summary>

```powershell
# 1. 安装 Visual Studio 2022 Build Tools（或完整 IDE），勾选
#    "Desktop development with C++" + "C++ CMake tools"。
# 2. （可选）安装 Qt 6 的 msvc2022_64 组件。
# 3. 任意 PowerShell 窗口里：
scripts\build-msvc.ps1 tests
```
</details>

<details>
<summary>📖 MSYS2 一次性配置</summary>

```powershell
# 1. 从 https://www.msys2.org 安装 MSYS2
# 2. 打开 "MSYS2 UCRT64" 终端：
pacman -Syu
pacman -S --needed mingw-w64-ucrt-x86_64-toolchain \
                   mingw-w64-ucrt-x86_64-cmake \
                   mingw-w64-ucrt-x86_64-ninja git
# 3. （可选）把 C:\msys64\ucrt64\bin 加入 PATH
# 4. 从任意终端执行：
scripts\build.ps1 tests
```
</details>

### 📦 在自己的项目中使用

**方式 A —— 先安装，再用 `find_package`**（生产环境推荐）：

```bash
cmake -S . -B build/flavors/release -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build/flavors/release -j && sudo cmake --install build/flavors/release
```

```cmake
find_package(aria 3.0 CONFIG REQUIRED)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE aria::aria)
# 也可以按需选择模块：aria::core / ::async / ::runtime / ::binding
```

Linux 安装包中的 Aria 动态库从同目录解析其他 Aria 库。请将这些库一并保留；
移动 SDK 后，以新安装路径重新配置使用它的项目。

**方式 B —— 直接嵌入（不安装）**：

```cmake
add_subdirectory(third_party/aria EXCLUDE_FROM_ALL)
target_link_libraries(my_app PRIVATE aria::core aria::async)
```

## 💻 旗舰示例

[AriaTools](https://github.com/dqsjqian/AriaTools) 是唯一的旗舰跨平台示例，同一份 C++ ViewModel 驱动 Qt、iOS、Android 与 Web 四端，四端均由 CI 把关。它也是 Android 两种集成形态（Compose side-channel 与 typed JniAdapter）的参考实现。Aria 仓库本身不再承载应用示例，框架行为由 `tests/acceptance/` 和各模块测试固定，文档只保留聚焦单一概念的最小片段。

## ⚙️ 构建选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `ARIA_BUILD_TESTS` | ON | 构建单元测试并注册到 ctest。 |
| `ARIA_BUILD_BENCHMARK` | ON | 构建微基准测试。 |
| `ARIA_BUILD_SHARED` | ON | runtime/binding 编译为动态库。 |
| `ARIA_BUILD_QT6` | OFF | 构建 Qt6 适配器。 |
| `ARIA_BUILD_APPKIT` | OFF | macOS AppKit 适配器（需 `APPLE`）。 |
| `ARIA_BUILD_UIKIT` | OFF | iOS UIKit 适配器（需 `APPLE`）。 |
| `ARIA_BUILD_JNI` | OFF | Android JNI 适配器（需 NDK r26+）。 |
| `ARIA_BUILD_HTTP` | OFF | 构建 HTTP/REST/SSE 适配器。 |
| `ARIA_ENABLE_ASAN` | OFF | AddressSanitizer。 |
| `ARIA_ENABLE_UBSAN` | OFF | UndefinedBehaviorSanitizer。 |
| `ARIA_ENABLE_TSAN` | OFF | ThreadSanitizer。 |

## 👋 Hello, world

上一节需要 UI 适配器。如果只想在控制台里看清响应式本身，不需要任何 UI：

```cpp
#include "aria/aria.hpp"
using namespace aria;

Property<int> count{0};

// Computed 不需要显式依赖列表 —— 首次求值时读到了 count，
// 就自动记下这个依赖。
Computed<std::string> label([&]{
    return "count = " + std::to_string(count.get());
});

Command<> increment([&]{ count = count.get() + 1; });

// bind 建立订阅：先立刻用当前值调一次，之后 label 每次变化都再调一次。
// 返回的 Subscription 是这条订阅的"遥控器"，它决定订阅活多久 ——
// 所以必须接住。写成 `label.bind(...);` 丢掉返回值，临时对象立即析构，
// 订阅在这一行就断了，后面什么都不会打印（bind 标了 [[nodiscard]]，
// 编译器会警告你）。
auto sub = label.bind([](const std::string& s) { std::cout << s << '\n'; });
// ↑ 这一行就已经打印了 "count = 0"（initial sync）

increment();   // → "count = 1"
increment();   // → "count = 2"

// sub 析构时自动退订，不需要手写反注册；也可以提前主动断开：
sub.release();
increment();   // 不再打印任何东西
```

`sub` 的作用就是**用作用域表达订阅的生命周期**：变量活着订阅就活着，变量没了订阅自动断开。
在真实的 UI 里，这个 `Subscription` 通常存成 View 的成员，View 销毁时订阅随之解除，
不会回调到一个已经析构的控件上。

## ⚡ 异步编程（C++ 协程）

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

## 🌍 跨平台映射

| 平台 | UI 宿主 | 适配器 | 状态 |
|------|---------|--------|------|
| Windows | Qt6 | `aria-qt6` | ✅ MSYS2 UCRT64 + MSVC 2022 |
| macOS | AppKit / Qt6 | `aria-qt6` / `aria-appkit` | ✅ 可用 |
| Linux | Qt6 | `aria-qt6` | ✅ 可用 |
| iOS | UIKit | `aria-uikit` | ✅ 可用 |
| Android | Compose / View | `aria-jni` | ✅ 就绪（NDK r26+） |
| **Web（服务端驱动）** | **浏览器 HTML/JS** | **`aria-http`** | **✅ REST + SSE** |
| Web（浏览器内 C++） | DOM via WASM | `aria-wasm` | 按实际需求评估，未实现 |

## 🖼 跨端实战成果

先看全景——一个框架长出的三个真实应用：

![Aria 生态：框架 + 三个真实应用](docs/marketing/images/aria-eco.png)

下面这些是 Aria 框架在真实应用里跑出来的样子 —— 同一份 C++ ViewModel，跨多个平台的原生壳。**[AriaTools](https://github.com/dqsjqian/AriaTools)**（17 个模块的跨端工作台，Qt / iOS / Android / Web 四端）、**[AriaAgent](https://github.com/dqsjqian/AriaAgent)**（Provider 无关的 LLM Agent GUI）、[AriaRead](https://github.com/dqsjqian/AriaRead)（跨平台书源引擎，HTTP/SSE Web 壳）均已在生产形态上使用 Aria 1.x。所有截图均来自稳定版本，一次构建、跨端共用同一份 C++ 业务核心。

### AriaTools —— 跨端工作台（17 模块）

Aria 旗舰跨平台示例：一份 ViewModel 跑四端。左侧导航的购物车 / 主题切换 / Framework Lab / Echo 等模块，全部由 `ObservableList`、`Computed`、`reactive::batch` 驱动。

| 平台 | 截图 | 适配器 |
|---|---|---|
| macOS（Qt6） | ![AriaTools-Mac](docs/marketing/images/AriaTools-Mac.png) | `aria-qt6` |
| iOS / UIKit | ![AriaTools-iOS](docs/marketing/images/AriaTools-iOS.png) | `aria-uikit` |
| Android（Compose side-channel） | ![AriaTools-Android](docs/marketing/images/AriaTools-Android.png) | `aria-jni` |
| Web（HTTP/REST/SSE） | ![AriaTools-Web](docs/marketing/images/AriaTools-Web.png) | `aria-http` |

### AriaAgent —— LLM Agent GUI

Aria + Qt6 实现的 Provider 无关 Agent GUI：真流式 SSE、工具调用链可视化、权限审批、Markdown 渲染。

| 视图 | 截图 |
|---|---|
| 主界面（对话） | ![AriaAgent-Main](docs/marketing/images/AriaAgent-Mac-main.png) |
| 设置（General / Model / Plugins / Agent Presets） | ![AriaAgent-Setting](docs/marketing/images/AriaAgent-Mac-setting.png) |

### AriaRead —— 跨平台书源引擎

Aria HTTP 适配器驱动的书源管理 Web 端：左侧书源列表 + 右侧书本卡片网格，全文搜索、订阅、调试一站式。同一份 C++ 核心同时驱动 REST/SSE 薄客户端与 SSR 两种 Web 形态。

| 视图 | 截图 |
|---|---|
| 书源管理（Web / REST+SSE） | ![AriaRead-Web](docs/marketing/images/AriaRead-Web.png) |
| 书源管理（Web / SSR） | ![AriaRead-SSR](docs/marketing/images/AriaRead-SSR.png) |

> 以上截图来自 macOS 示例应用。其他平台复用同一份 ViewModel；原生控件外观由平台、Qt 样式和宿主应用决定。框架的 Windows / Linux 构建与测试状态以 CI 为准。

## 🧪 测试状态

```bash
ctest --test-dir build/flavors/release --no-tests=error --output-on-failure
```

测试涵盖响应式状态、集合事件、异步取消、绑定生命周期、ABI 和适配器契约；
实际启用的测试取决于构建选项与平台。查看 [CI 结果](https://github.com/dqsjqian/Aria/actions/workflows/ci.yml)
以及 [生命周期契约](docs/reference/lifecycle.md)、[错误模型](docs/reference/error-model.md)。

## 📊 性能基准

2026-09-13，Apple M3 Pro / Apple Clang 21 / C++20 Release（`-O3 -DNDEBUG`）。
下面是五次配对运行中，各次平均耗时的中位数；原版为 `eeb613f`，新版测量快照为 2.0 改造提交 `c33850d`。

| 操作 | 原版 | 新版 |
|---|---:|---:|
| Property set，无观察者 | 20.5 ns | 13.2 ns |
| Property set，1 个观察者 | 98.9 ns | 35.0 ns |
| Computed 链 ×5 | 570.8 ns | 229.0 ns |
| 10 次 set 包在一个 batch 中 | 250.6 ns | 102.5 ns |
| FilteredList 尾部追加，初始 10k 行 | 11.18 μs | 0.23 μs |
| SortedList 随机键追加，初始 10k 行 | 8.38 μs | 17.28 μs |

性能有升有降：拥有事件数据和维护批量修改后的排序正确性也有成本。
完整场景、复杂度、剩余回退和复现方法见 [性能说明](docs/reference/performance.md)。

## 📋 框架本体契约

所有非平庸行为都钉在带编号的契约文档里，每条契约有稳定 ID（如 `L-13` / `E-22` / `LD-7`）。

| 文档 | 前缀 | 范围 |
|---|---|---|
| [`api-style.md`](docs/reference/api-style.md) | `S-N` | 命名、命名空间、错误与异步风格约束 |
| [`lifecycle.md`](docs/reference/lifecycle.md) | `L-N` | 线程、订阅、flush、view 销毁、cancel/dtor 不变式 |
| [`error-model.md`](docs/reference/error-model.md) | `E-N` | `aria::Error` / `ErrorKind` taxonomy |
| [`list-diff-contract.md`](docs/reference/list-diff-contract.md) | `LD-N` | `Insert / Remove / Replace / Move / Reset` 语义 |
| [`diagnostics.md`](docs/reference/diagnostics.md) | `D-N` | `TraceEvent` + `TraceSink` 诊断协议 |
| [`performance.md`](docs/reference/performance.md) | `PERF-N` | 复杂度上界与实测基线 |

公开 [API 参考](https://dqsjqian.github.io/Aria/) 由主分支自动构建并发布。
从 2.x 升级请阅读 [3.0 迁移指南](docs/migration-3.0.md)；1.2.x → 2.0 见[迁移指南](docs/migration-2.0.md)。

## 🗺 路线图

Aria 已开源（MIT License），源码托管在 [GitHub](https://github.com/dqsjqian/Aria)。待办与已延后清单的唯一信息源在 [`docs/ROADMAP.md`](docs/ROADMAP.md)；当前能力快照见 [`CHANGELOG.md`](CHANGELOG.md)。

## 🤝 贡献指南

欢迎贡献！涉及架构改动的请先开 Issue 讨论。

- 代码风格由 `.clang-format` 和 `.clang-tidy` 统一管控
- 所有变更必须通过 `ctest --output-on-failure`
- 新功能需要在对应 `modules/*/tests/` 套件中补充测试

## 🙏 致谢

- [doctest](https://github.com/doctest/doctest) —— 轻量级测试框架
- [nlohmann_json](https://github.com/nlohmann/json) —— JSON for Modern C++
- [Continuo](https://github.com/dqsjqian/continuo) —— 协程原生的 C++23 网络库（HTTP 适配器的传输层）
- [OpenSSL](https://www.openssl.org/) —— TLS 1.2/1.3（哈希固定的 release 下载）
- 第三方依赖统一经 [ariaFetchPinned.cmake](cmake/ariaFetchPinned.cmake) 按 SHA256 固定下载，无 vendored 源码、无 submodule

## 📄 License

[MIT](LICENSE) © 2026 aria contributors

---

<div align="center">

**📖 其他语言**

[English](README.en.md)

</div>
