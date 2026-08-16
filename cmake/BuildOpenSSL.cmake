# ──────────────────────────────────────────────
# BuildOpenSSL.cmake
# 从 third_party/openssl 源码编译 OpenSSL 静态库，
# 导出 IMPORTED 目标供 curl 等依赖使用。
# ──────────────────────────────────────────────

include(ExternalProject)

# ──────────────────────────────────────────────────────────────────────────
# Submodule-friendly opt-out
#
# When Aria is consumed as a git submodule and the parent project already
# builds (or finds) OpenSSL itself, defining the `openssl_external` ExternalProject
# target a second time aborts CMake with a duplicate-target error.
#
# To cooperate, we early-return when ALL of the following hold:
#   * a target named `openssl_external` already exists, AND
#   * the cache variables FindOpenSSL would normally provide
#     (`OPENSSL_INCLUDE_DIR` and at least one of `OPENSSL_LIBRARIES` /
#     `OPENSSL_SSL_LIBRARY` + `OPENSSL_CRYPTO_LIBRARY`) are populated.
#
# The contract: if the parent project provides the target, it MUST also
# expose the FindOpenSSL-compatible variables, otherwise downstream
# `find_package(OpenSSL)` calls (e.g. curl's) would silently get nothing.
# ──────────────────────────────────────────────────────────────────────────
if(TARGET openssl_external)
    if(NOT DEFINED OPENSSL_INCLUDE_DIR OR
       (NOT DEFINED OPENSSL_LIBRARIES AND
        (NOT DEFINED OPENSSL_SSL_LIBRARY OR NOT DEFINED OPENSSL_CRYPTO_LIBRARY)))
        message(FATAL_ERROR
            "Aria: target 'openssl_external' is already defined by the parent "
            "project, but the FindOpenSSL-compatible cache variables "
            "(OPENSSL_INCLUDE_DIR, OPENSSL_LIBRARIES or "
            "OPENSSL_SSL_LIBRARY/OPENSSL_CRYPTO_LIBRARY) are not set. "
            "The parent project must populate these so consumers like curl "
            "and aria::http can locate OpenSSL.")
    endif()
    message(STATUS
        "Aria: 'openssl_external' already provided by parent project — "
        "skipping Aria's own OpenSSL build (using parent's OpenSSL artefacts).")
    return()
endif()

set(OPENSSL_SOURCE_DIR "${PROJECT_SOURCE_DIR}/third_party/openssl")
set(OPENSSL_INSTALL_DIR "${CMAKE_BINARY_DIR}/_deps/openssl-install")
set(OPENSSL_INCLUDE_DIR "${OPENSSL_INSTALL_DIR}/include")

# 预先创建 include 目录，避免 CMake generate 阶段检查 IMPORTED target 的
# INTERFACE_INCLUDE_DIRECTORIES 时因目录不存在而报错
file(MAKE_DIRECTORY "${OPENSSL_INCLUDE_DIR}")

# 根据平台确定静态库路径
if(WIN32 AND NOT MINGW)
    set(OPENSSL_SSL_LIBRARY "${OPENSSL_INSTALL_DIR}/lib/libssl.lib")
    set(OPENSSL_CRYPTO_LIBRARY "${OPENSSL_INSTALL_DIR}/lib/libcrypto.lib")
else()
    set(OPENSSL_SSL_LIBRARY "${OPENSSL_INSTALL_DIR}/lib/libssl.a")
    set(OPENSSL_CRYPTO_LIBRARY "${OPENSSL_INSTALL_DIR}/lib/libcrypto.a")
endif()

# 根据平台选择 OpenSSL Configure 目标
if(APPLE)
    if(CMAKE_OSX_ARCHITECTURES STREQUAL "arm64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
        set(_OPENSSL_TARGET "darwin64-arm64-cc")
    else()
        set(_OPENSSL_TARGET "darwin64-x86_64-cc")
    endif()
elseif(WIN32)
    if(MINGW)
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(_OPENSSL_TARGET "mingw64")
        else()
            set(_OPENSSL_TARGET "mingw")
        endif()
    else()
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(_OPENSSL_TARGET "VC-WIN64A")
        else()
            set(_OPENSSL_TARGET "VC-WIN32")
        endif()
    endif()
elseif(ANDROID)
    if(ANDROID_ABI STREQUAL "arm64-v8a")
        set(_OPENSSL_TARGET "android-arm64")
    elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
        set(_OPENSSL_TARGET "android-arm")
    elseif(ANDROID_ABI STREQUAL "x86_64")
        set(_OPENSSL_TARGET "android-x86_64")
    elseif(ANDROID_ABI STREQUAL "x86")
        set(_OPENSSL_TARGET "android-x86")
    else()
        set(_OPENSSL_TARGET "android-arm64")
    endif()
else()
    # Linux / 其他 Unix
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
        set(_OPENSSL_TARGET "linux-aarch64")
    else()
        set(_OPENSSL_TARGET "linux-x86_64")
    endif()
endif()

# 获取 CPU 核心数用于并行编译
include(ProcessorCount)
ProcessorCount(NPROC)
if(NPROC EQUAL 0)
    set(NPROC 4)
endif()

# 查找 perl（OpenSSL Configure 需要）
# 优先从 MSYS2 的 usr/bin 查找（与 MinGW 编译器配套）
# MSYS2 布局: <msys2_root>/ucrt64/bin/  (编译器)
#             <msys2_root>/usr/bin/     (perl, bash, make 等 coreutils)
# 所以要从编译器目录上溯两级再进 usr/bin
#
# 注意：项目可能只启用 CXX（CMAKE_C_COMPILER 为空），所以编译器路径
# 要优先用 CMAKE_C_COMPILER，为空时回退到 CMAKE_CXX_COMPILER。
if(CMAKE_C_COMPILER)
    set(_ARIA_CC "${CMAKE_C_COMPILER}")
else()
    set(_ARIA_CC "${CMAKE_CXX_COMPILER}")
endif()
get_filename_component(_COMPILER_DIR "${_ARIA_CC}" DIRECTORY)
get_filename_component(_UCRT64_DIR "${_COMPILER_DIR}" DIRECTORY)
get_filename_component(_MSYS2_ROOT "${_UCRT64_DIR}" DIRECTORY)
set(_MSYS2_PERL_CANDIDATES
    "${_MSYS2_ROOT}/usr/bin/perl.exe"
    "${_UCRT64_DIR}/usr/bin/perl.exe"
    "${_COMPILER_DIR}/../usr/bin/perl.exe"
    "C:/msys64/usr/bin/perl.exe"
    "D:/msys64/usr/bin/perl.exe"
    "D:/worksoft/msys64/usr/bin/perl.exe"
)
set(_OPENSSL_PERL_COMMAND "")
foreach(_perl_path IN LISTS _MSYS2_PERL_CANDIDATES)
    get_filename_component(_perl_abs "${_perl_path}" ABSOLUTE)
    if(EXISTS "${_perl_abs}")
        set(_OPENSSL_PERL_COMMAND "${_perl_abs}")
        break()
    endif()
endforeach()
if(NOT _OPENSSL_PERL_COMMAND)
    find_program(_OPENSSL_PERL_COMMAND perl REQUIRED)
endif()
# perl 所在目录（供 PATH 注入和 build/install 命令使用）
get_filename_component(_PERL_DIR "${_OPENSSL_PERL_COMMAND}" DIRECTORY)

# 构建 Configure 参数
set(_OPENSSL_CONFIGURE_ARGS
    "${_OPENSSL_TARGET}"
    "no-shared"           # 只编译静态库
    "no-tests"            # 不编译测试
    "no-apps"             # 不编译 openssl 命令行工具
    "no-docs"             # 不生成文档
    "no-comp"             # 不需要压缩
    "no-dtls"             # 不需要 DTLS
    "no-engine"           # 不需要 ENGINE（OpenSSL 3.x 已弃用）
    "no-legacy"           # 不需要旧算法
    "--prefix=${OPENSSL_INSTALL_DIR}"
    "--libdir=lib"        # 统一输出到 lib/ 而非 lib64/
)

# MSYS2 MinGW: disable ASM. OpenSSL's generated Makefile uses
# `CC="gcc" $(PERL) ...` for ASM recipe lines, which requires a Unix
# shell (sh.exe). MSYS2's sh.exe can't find gcc when invoked from
# cmake/PowerShell because the MSYS2 runtime strips its own paths from
# the inherited Windows PATH. Using cmd.exe instead fails on the
# `CC="gcc"` prefix. Disabling ASM eliminates all shell-dependent
# recipes — the remaining `gcc -c ...` and `perl ...` commands work
# fine with cmd.exe. The crypto performance impact is negligible for
# a demo SDK. MSVC (nmake) and non-Windows platforms keep ASM.
if(WIN32 AND MINGW)
    list(APPEND _OPENSSL_CONFIGURE_ARGS "no-asm")
endif()

# 新版 Clang（Apple Clang 17+ / Clang 16+）对 C99 implicit-int 等更严格，
# OpenSSL 3.5.x 的宏展开会触发这些错误，需要通过 CFLAGS 抑制
set(_OPENSSL_EXTRA_CFLAGS "-Wno-implicit-int -Wno-incompatible-pointer-types -Wno-int-conversion -Wno-deprecated-non-prototype")

# 如果是 Android，需要设置 NDK 工具链
if(ANDROID)
    list(APPEND _OPENSSL_CONFIGURE_ARGS
        "-D__ANDROID_API__=${ANDROID_NATIVE_API_LEVEL}"
    )
    set(_OPENSSL_ENV "ANDROID_NDK_ROOT=${ANDROID_NDK}" "CFLAGS=${_OPENSSL_EXTRA_CFLAGS}")
else()
    set(_OPENSSL_ENV "CFLAGS=${_OPENSSL_EXTRA_CFLAGS}")
endif()

# 根据工具链选择构建命令，优先从编译器所在目录查找构建工具
if(WIN32 AND MINGW)
    # MinGW 下用 mingw32-make + MSYS2 sh.exe。
    # MSYS2 UCRT64 默认只装 mingw-w64-ucrt-x86_64-make（提供 mingw32-make.exe），
    # 不装 msys/make（提供 make.exe），所以必须探测 mingw32-make。
    # OpenSSL Makefile 用 Unix shell 语法，必须通过 SHELL 环境变量让 make
    # 用 sh.exe 而非 cmd.exe 来执行 recipe。
    set(_MSYS2_SH_CANDIDATES
        "${_MSYS2_ROOT}/usr/bin/sh.exe"
        "${_UCRT64_DIR}/usr/bin/sh.exe"
        "${_COMPILER_DIR}/../usr/bin/sh.exe"
        "C:/msys64/usr/bin/sh.exe"
        "D:/msys64/usr/bin/sh.exe"
        "D:/worksoft/msys64/usr/bin/sh.exe"
    )
    set(_OPENSSL_SH_COMMAND "")
    foreach(_sh_path IN LISTS _MSYS2_SH_CANDIDATES)
        get_filename_component(_sh_abs "${_sh_path}" ABSOLUTE)
        if(EXISTS "${_sh_abs}")
            set(_OPENSSL_SH_COMMAND "${_sh_abs}")
            break()
        endif()
    endforeach()
    if(NOT _OPENSSL_SH_COMMAND)
        find_program(_OPENSSL_SH_COMMAND sh REQUIRED)
    endif()

    # 优先用 make.exe（如果装了 msys/make 包），否则用 mingw32-make.exe。
    # 必须带硬编码兜底路径 —— 原生 Windows cmake 的 find_program 不认 bash
    # 导出的 POSIX 风格 PATH（/d/worksoft/...），候选列表里的 ${_MSYS2_ROOT}
    # 在 CMAKE_C_COMPILER 为空时也会变空，只能靠硬编码兜底。
    set(_OPENSSL_MAKE_CANDIDATES
        "${_MSYS2_ROOT}/usr/bin/make.exe"
        "${_UCRT64_DIR}/usr/bin/make.exe"
        "${_COMPILER_DIR}/mingw32-make.exe"
        "${_UCRT64_DIR}/bin/mingw32-make.exe"
        "C:/msys64/usr/bin/make.exe"
        "D:/msys64/usr/bin/make.exe"
        "D:/worksoft/msys64/usr/bin/make.exe"
        "C:/msys64/ucrt64/bin/mingw32-make.exe"
        "D:/msys64/ucrt64/bin/mingw32-make.exe"
        "D:/worksoft/msys64/ucrt64/bin/mingw32-make.exe"
    )
    set(_OPENSSL_MAKE_COMMAND "")
    foreach(_make_path IN LISTS _OPENSSL_MAKE_CANDIDATES)
        get_filename_component(_make_abs "${_make_path}" ABSOLUTE)
        if(EXISTS "${_make_abs}")
            set(_OPENSSL_MAKE_COMMAND "${_make_abs}")
            break()
        endif()
    endforeach()
    if(NOT _OPENSSL_MAKE_COMMAND)
        find_program(_OPENSSL_MAKE_COMMAND NAMES make mingw32-make REQUIRED)
    endif()

    # Windows 默认禁止创建符号链接（需要开发者模式或管理员权限）。
    # OpenSSL 的 link-utils 规则在 out-of-source 构建时用 `ln -sf` 把
    # 源码目录里的 util/opensslwrap.sh、apps/openssl.cnf "链接"到 build 目录，
    # 没有开发者模式时 ln 失败 → build_sw 中断。
    # 解法：写一个 ln 包装脚本，把 `ln -sf SRC DST` 转成 `cp -rf SRC DST`。
    # OpenSSL 在 mingw 上用 ln 仅为复制配置/脚本文件，复制完全等价，不破坏
    # 任何真实符号链接语义（这里本来就没有）。作用域仅限 OpenSSL 构建。
    set(_OPENSSL_LN_WRAPPER_DIR "${CMAKE_BINARY_DIR}/_deps/openssl-ln-wrapper")
    file(MAKE_DIRECTORY "${_OPENSSL_LN_WRAPPER_DIR}")
    set(_OPENSSL_LN_WRAPPER "${_OPENSSL_LN_WRAPPER_DIR}/ln")
    file(WRITE "${_OPENSSL_LN_WRAPPER}"
"#!/bin/sh
# ln wrapper for OpenSSL build on Windows without Developer Mode.
# Windows forbids creating symlinks unless Developer Mode is enabled, but
# OpenSSL's link-utils rules use 'ln -sf SRC DST' to \"link\" source files
# (util/opensslwrap.sh, apps/openssl.cnf) into the out-of-source build dir.
# We emulate ln -s as a copy.  The subtlety: ln -s resolves SRC relative to
# DST's directory (symlink semantics), NOT relative to CWD.  So we must
# rebase a relative SRC onto DST's directory before copying, otherwise the
# path resolves one level too shallow and cp fails with 'No such file'.
# OpenSSL only uses ln to copy config/script files; a real copy is fully
# equivalent here — there is no live symlink semantics to preserve.

src=''
dst=''
for a in \"\$@\"; do
  case \"\$a\" in
    -*) ;;                      # drop -s / -f / -n / -sf ... flag combos
    *)
      if [ -z \"\$src\" ]; then src=\"\$a\"; else dst=\"\$a\"; fi
      ;;
  esac
done
if [ -z \"\$dst\" ]; then
  echo \"ln-wrapper: missing destination\" >&2; exit 1
fi
# Directory the symlink would live in (DST is a dir for OpenSSL's rules;
# fall back to dirname for the DST-is-a-file-path form).
if [ -d \"\$dst\" ]; then
  dst_dir=\"\$dst\"
else
  dst_dir=\`dirname \"\$dst\"\`
fi
# Rebase relative SRC onto dst_dir to replicate symlink resolution.
case \"\$src\" in
  /*) resolved=\"\$src\" ;;
  *)  resolved=\"\$dst_dir/\$src\" ;;
esac
exec cp -rf \"\$resolved\" \"\$dst\"
")
    file(CHMOD "${_OPENSSL_LN_WRAPPER}"
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                    GROUP_READ GROUP_EXECUTE
                    WORLD_READ WORLD_EXECUTE)

    # mingw32-make 默认用 sh.exe（如果在 PATH 里）执行 recipe。但 MSYS2 的
    # sh.exe 从 cmake/PowerShell 调用时 PATH 转换有问题 —— 它看不到 ucrt64/bin，
    # 导致 gcc 找不到 cc1.exe 的依赖 DLL（libgmp/libmpfr/libmpc/libisl 等）。
    #
    # 解决方案：用 no-asm 消除所有 `CC="gcc" perl ...` Unix-shell 语法 recipe
    # （只剩 `gcc -c ...` 和 `perl ...` 直接调用，cmd.exe 能处理），然后在
    # BUILD/INSTALL 阶段不设 SHELL、不把 usr/bin 加进 PATH —— 这样 mingw32-make
    # 回退到 cmd.exe，gcc 继承正确的 Windows PATH，cc1 的 DLL 能找到。
    #
    # CONFIGURE 阶段仍需 SHELL=sh.exe + usr/bin（ln-wrapper 是 shell 脚本）。
    set(_OPENSSL_BUILD_COMMAND
        ${CMAKE_COMMAND} -E env
        --modify PATH=path_list_prepend:${_COMPILER_DIR}
        --modify PATH=path_list_prepend:${_OPENSSL_LN_WRAPPER_DIR}
        "${_OPENSSL_MAKE_COMMAND}" -j${NPROC}
    )
    set(_OPENSSL_INSTALL_COMMAND
        ${CMAKE_COMMAND} -E env
        --modify PATH=path_list_prepend:${_COMPILER_DIR}
        --modify PATH=path_list_prepend:${_OPENSSL_LN_WRAPPER_DIR}
        "${_OPENSSL_MAKE_COMMAND}" install_sw
    )
    set(_OPENSSL_PATH_ENV "")
elseif(WIN32 AND NOT MINGW)
    find_program(_OPENSSL_MAKE_COMMAND nmake REQUIRED)
    set(_OPENSSL_BUILD_COMMAND ${_OPENSSL_MAKE_COMMAND})
    set(_OPENSSL_INSTALL_COMMAND ${_OPENSSL_MAKE_COMMAND} install_sw)
    set(_OPENSSL_PATH_ENV "")
else()
    find_program(_OPENSSL_MAKE_COMMAND make REQUIRED)
    set(_OPENSSL_BUILD_COMMAND ${_OPENSSL_MAKE_COMMAND} -j${NPROC})
    set(_OPENSSL_INSTALL_COMMAND ${_OPENSSL_MAKE_COMMAND} install_sw)
    set(_OPENSSL_PATH_ENV "")
endif()

# 组装环境变量（MinGW 需要把编译器和 perl 目录加入 PATH）
set(_OPENSSL_CONFIGURE_COMMAND
    ${CMAKE_COMMAND} -E env
)
if(WIN32 AND MINGW)
    # 设置 PERL 环境变量，让 Configure 生成的 Makefile 使用正确的 perl 路径
    list(APPEND _OPENSSL_CONFIGURE_COMMAND
        --modify PATH=path_list_prepend:${_COMPILER_DIR}
        --modify PATH=path_list_prepend:${_PERL_DIR}
        "PERL=${_OPENSSL_PERL_COMMAND}"
    )
endif()

# 使用 ExternalProject 编译 OpenSSL
ExternalProject_Add(openssl_external
    SOURCE_DIR        "${OPENSSL_SOURCE_DIR}"
    BUILD_IN_SOURCE   0
    # OpenSSL 的 Configure 脚本需要在源码目录运行，但我们用 CONFIGURE_COMMAND 指定
    CONFIGURE_COMMAND ${_OPENSSL_CONFIGURE_COMMAND} ${_OPENSSL_ENV}
        "${_OPENSSL_PERL_COMMAND}" "${OPENSSL_SOURCE_DIR}/Configure" ${_OPENSSL_CONFIGURE_ARGS}
    BUILD_COMMAND     ${_OPENSSL_BUILD_COMMAND}
    INSTALL_COMMAND   ${_OPENSSL_INSTALL_COMMAND}
    BUILD_BYPRODUCTS  "${OPENSSL_SSL_LIBRARY}" "${OPENSSL_CRYPTO_LIBRARY}"
    LOG_CONFIGURE     TRUE
    LOG_BUILD         TRUE
    LOG_INSTALL       TRUE
)

# 创建 IMPORTED 目标
# OpenSSL::Crypto
add_library(OpenSSL::Crypto STATIC IMPORTED GLOBAL)
set_target_properties(OpenSSL::Crypto PROPERTIES
    IMPORTED_LOCATION "${OPENSSL_CRYPTO_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
)
add_dependencies(OpenSSL::Crypto openssl_external)

# OpenSSL::SSL
add_library(OpenSSL::SSL STATIC IMPORTED GLOBAL)
set_target_properties(OpenSSL::SSL PROPERTIES
    IMPORTED_LOCATION "${OPENSSL_SSL_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES OpenSSL::Crypto
)
add_dependencies(OpenSSL::SSL openssl_external)

# 设置 FindOpenSSL 兼容变量，供 curl 的 find_package(OpenSSL) 使用
set(OPENSSL_FOUND TRUE CACHE BOOL "" FORCE)
set(OPENSSL_INCLUDE_DIR "${OPENSSL_INCLUDE_DIR}" CACHE PATH "" FORCE)
set(OPENSSL_SSL_LIBRARY "${OPENSSL_SSL_LIBRARY}" CACHE FILEPATH "" FORCE)
set(OPENSSL_CRYPTO_LIBRARY "${OPENSSL_CRYPTO_LIBRARY}" CACHE FILEPATH "" FORCE)
set(OPENSSL_LIBRARIES "${OPENSSL_SSL_LIBRARY};${OPENSSL_CRYPTO_LIBRARY}" CACHE STRING "" FORCE)
set(OPENSSL_VERSION "3.5.7" CACHE STRING "" FORCE)
set(OPENSSL_ROOT_DIR "${OPENSSL_INSTALL_DIR}" CACHE PATH "" FORCE)

# 平台特定的链接依赖
if(APPLE)
    # macOS 需要链接 Security 和 CoreFoundation 框架
    set_property(TARGET OpenSSL::Crypto APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES "-framework Security" "-framework CoreFoundation")
elseif(WIN32)
    # Windows 需要链接系统库
    set_property(TARGET OpenSSL::Crypto APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES ws2_32 crypt32)
elseif(UNIX AND NOT ANDROID)
    # Linux 需要 pthread 和 dl
    set_property(TARGET OpenSSL::Crypto APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES pthread dl)
endif()

message(STATUS "OpenSSL will be built from source: ${OPENSSL_SOURCE_DIR}")
message(STATUS "OpenSSL install prefix: ${OPENSSL_INSTALL_DIR}")
message(STATUS "OpenSSL target platform: ${_OPENSSL_TARGET}")
