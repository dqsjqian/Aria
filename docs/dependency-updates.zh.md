# 依赖更新使用指南

本文的命令均从 **Aria 仓库根目录**执行。需要 Python 3.10+；macOS/Linux 如果没有 `python` 命令，替换为 `python3`。不需要 pip 包。首次解析或主动更新需要联网；GitHub API 可使用已登录的 `gh`，未安装时使用公开 API。构建还需要 README 所列的编译器、CMake 和平台 SDK。

## 先理解三个文件

| 文件 | 用途 | 是否提交 Git |
| --- | --- | --- |
| `dependencies.json` | 来源与长期版本要求；不填 `version` 表示更新时选最新稳定版 | 是 |
| `dependencies.lock.json` | 已解析的版本、完整提交、下载地址和 SHA256（Git 源以完整提交锁定） | 是 |
| `scripts/update_dependencies.py` | 手动更新入口，无需修改脚本 | 是 |

已有锁时，普通构建复用锁；没有匹配锁时才解析最新稳定版。**“最新”指执行解析/更新时的最新稳定发布，不是每次构建自动升级，也不包含预发布或开发分支。** 已安装 Qt/编译器/系统 SDK 仍由平台工具配置，详见下文。

可用依赖名：`json`, `doctest`, `mira`, `openssl`。名称区分大小写，必须与 manifest 中的键一致。

## 常用命令

```bash
# 查看所有参数
python scripts/update_dependencies.py --help

# 按 manifest 要求更新全部依赖
python scripts/update_dependencies.py

# 只更新一个依赖，其他锁记录完全保留
python scripts/update_dependencies.py --only mira

# 更新多个依赖：每个名称使用一个 --only
python scripts/update_dependencies.py --only json --only mira

# 本次指定两个版本；其他库按 manifest（未填 version 的选择最新稳定版）
python scripts/update_dependencies.py --version json=3.12.0 --version openssl=4.0.3

# 只处理指定的两个库，并给其中一个指定版本
python scripts/update_dependencies.py --only json --only mira --version json=3.12.0
```

`--version` 可重复，但同一个名称不能重复。`--only` 与 `--version` 同用时，每个版本覆盖项都必须出现在 `--only` 中；拼错名称或遗漏选择会报错，不会默默忽略。版本写上游稳定版本号，例如 JSON 的 `3.12.0`；不接受 `main`、`nightly`、`2.0.0-rc1` 作为稳定选择。

## 两个库固定、另一个跟最新

无需改 Python 脚本。在现有 manifest 的 `json`、`openssl` 项中分别添加 `"version": "3.12.0"`、`"version": "4.0.3"`，保留其余来源字段；`mira` 项不要加 `version`。例如下面仅是 `json` 条目的写法，**不要用这个片段覆盖整个文件**：

```json
"json": {
  "provider": "github",
  "repo": "nlohmann/json",
  "artifact": "release-asset",
  "asset": "json.tar.xz",
  "tag_prefix": "v",
  "version": "3.12.0"
}
```

然后运行 `python scripts/update_dependencies.py`：两个固定库保持所需版本，未固定库选择最新稳定版。只想升级第三个库时，运行 `python scripts/update_dependencies.py --only mira`。以后想解除固定，删除该项的 `version` 字段再主动更新。

优先级与持久性：

1. 本次 `--version NAME=VERSION` 高于 manifest 的 `version`。
2. manifest 明确版本约束长期生效，后续更新也遵守。
3. 没有显式版本时，普通解析复用有效锁；主动更新才重新选择最新稳定版。
4. 命令行选中的结果会留在锁中，普通构建继续复用；下一次主动更新若仍需固定，请写入 manifest 或再次传参数。

## 更新完成后怎么用

更新脚本**更新选择与锁文件，不自动编译整个项目，也不保证新 API 兼容**。它成功退出后，再取回锁定的源码并执行正常构建/测试：

```bash
cmake -S . -B build/flavors/dependency-check -DARIA_BUILD_TESTS=ON -DARIA_BUILD_HTTP=ON -DARIA_HTTP_ENABLE_TLS=ON
cmake --build build/flavors/dependency-check --config Release --parallel 3
ctest --test-dir build/flavors/dependency-check -C Release --output-on-failure --no-tests=error
```

平台 SDK 的选择、额外测试和运行探针沿用 [README](../README.md)。多配置生成器的构建和测试要使用相同 `--config` / `-C` 配置。已有其他构建目录可继续使用，但更换编译器或平台时应换目录。

检查 `git diff -- dependencies.json dependencies.lock.json`，确认版本和来源，再提交这两个文件中实际发生的修改。CI 和 Release 使用提交进仓库的锁；不会在构建过程中追随新的 upstream release。`build/` 下的源码、下载缓存、构建产物和本地覆盖锁不要提交。

## 首次解析、离线与覆盖

只补缺失的选择、保留有效锁，用底层 `resolve`：

```bash
python scripts/dependencies.py resolve --manifest dependencies.json --lock dependencies.lock.json

# 只验证/复用匹配的锁；缺失条目或新版本要求会报错
python scripts/dependencies.py resolve --manifest dependencies.json --lock dependencies.lock.json --offline
```

离线解析成功只说明有匹配元数据；离线构建还需要对应源码/归档缓存与工具链。`update --offline` 无法发现上游新版本，应使用 `resolve --offline`。



支持 Aria 源码集成的 CMake 库可用 `-DARIA_DEP_JSON_VERSION=3.12.0` 等选择本次构建版本。CMake 使用构建目录中的有效锁，不修改源码根锁；清空相应缓存参数可恢复项目默认。明确的源码目录覆盖或父工程预先提供的目标优先，由调用者负责版本和内容。Qt 用 `-DARIA_DEP_QT_VERSION=6.10.0` 选择已安装的精确版本；没有指定时从 CMake 可见的安装位置选择，仍尊重 `Qt6_DIR`、`CMAKE_PREFIX_PATH` 和工具链设置。更新脚本不会安装 Qt。

## 失败处理与回退

| 现象 | 处理 |
| --- | --- |
| 未声明的名称、重复参数、选择范围不一致 | 查看 manifest 的键以及 `--help`，修正参数 |
| 无此稳定版本、预发布版本、下载/API 失败 | 核对上游正式发布；检查网络或 `gh` 登录/限流；成功前原锁保持不变 |
| 锁内容损坏、SHA256 不符、缓存源码有修改 | 不会跳过验证；先保留本地修改，再恢复可信锁/缓存或使用新的缓存目录 |
| 元数据更新成功，但编译/测试失败 | 新版本可能不兼容；修改集成代码，或恢复已验证的版本要求和锁后重新构建 |
| 本地 Aria checkout 有修改 | 先提交或另存修改，获取器不会直接覆盖 |

回退时，从自己已知通过测试的 Git 提交恢复 `dependencies.json` 和 `dependencies.lock.json`（先保留未提交的修改），然后重复上面的获取、构建、测试流程。恢复锁不会自动恢复已编译二进制。不要手改锁中的哈希来绕过校验。

删除整个锁也能触发重新解析，但会丢失现有选择，并可能扩大更新范围。**正常升级请用更新脚本**；先确认没有仍然生效的显式版本参数或源目录覆盖。

## 高级参数

| 参数 | 用途 |
| --- | --- |
| `--only NAME` | 只更新某项，可重复 |
| `--version NAME=VERSION` | 本次版本覆盖，可重复 |
| `--manifest PATH` | 使用另一份版本要求文件 |
| `--lock PATH` | 将结果保存到另一份锁；不会自动让 CMake 使用它 |
| `--base-lock PATH` | 以另一份锁为基础，合并现有输出记录 |
| `--cache-dir PATH` | 解析时下载的校验缓存目录 |
| `--offline` | 禁止联网解析；主动更新通常无法在此模式下完成 |

使用自定义锁进行 Aria 源码构建时，相应传入 `-DARIA_DEPENDENCY_MANIFEST=... -DARIA_DEPENDENCY_LOCK=...`；自定义 fetch 或依赖 builder 也要传入一致的 `--manifest` / `--lock`。不要以为另写了一份锁就会自动替换所有构建入口。

本体的 CMake API、离线缓存与 SDK 集成细节见 [Dependency versions and reproducible builds](dependencies.md)。
