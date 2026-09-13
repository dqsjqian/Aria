cmake_minimum_required(VERSION 3.20)

# Allow third-party CMake projects (older doctest etc.) that target old policies.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "")

project(aria
    VERSION 2.0.0
    DESCRIPTION "Modern MVVM framework for C++23 (C++20 minimum) — cross-platform, layered, coroutine-first"
    HOMEPAGE_URL "https://github.com/dqsjqian/Aria"
    LANGUAGES CXX
)

# ── Build-tree location guard ────────────────────────────────────────────────
#
# `build/` is a CONTAINER for build trees, not a build tree itself. The layout
# is documented at the top of scripts/build.sh:
#
#   build/ide/                IDE workspace
#   build/flavors/<name>/     command-line + CI build flavors
#   build/platforms/<name>/   cross-compilation targets
#   build/dist/               release artefacts (shared across flavors)
#
# Configuring straight into `build/` defeats two things that layout buys:
# `rm -rf build/flavors/x` can no longer isolate a single tree, and the root
# cache collides with whatever the previous `-B build` configured (Release vs
# Debug vs sanitizers), producing confusing stale-cache failures.
#
# This has been an actual accident, not a hypothetical one: the workflows
# configured eight jobs straight into `build/`, and a hand-made sibling tree
# once got its compiled binaries committed. A comment in a shell script cannot
# prevent that; a hard failure can.
#
# In-source builds trip the same check, since CMAKE_BINARY_DIR then equals
# CMAKE_SOURCE_DIR.
option(ARIA_ALLOW_BUILD_ROOT "Permit configuring directly into build/ (not recommended)" OFF)
# Guard fires only when Aria is the top-level project. When Aria is pulled in
# via add_subdirectory() (OpenRead / AriaTools / AriaAgent submodules),
# CMAKE_SOURCE_DIR is the consumer's root and the consumer owns its build
# layout — e.g. a plain `build/` there is perfectly valid and must not fail.
if(NOT ARIA_ALLOW_BUILD_ROOT AND CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    if(CMAKE_BINARY_DIR STREQUAL "${CMAKE_SOURCE_DIR}/build"
       OR CMAKE_BINARY_DIR STREQUAL CMAKE_SOURCE_DIR)
        message(FATAL_ERROR
            "\n"
            "Refusing to configure into '${CMAKE_BINARY_DIR}'.\n"
            "\n"
            "build/ is the container for build trees, not a build tree.\n"
            "Pick a subdirectory so caches stay isolated:\n"
            "\n"
            "    cmake -S . -B build/flavors/debug -DCMAKE_BUILD_TYPE=Debug\n"
            "\n"
            "Or let the build script choose for you:\n"
            "\n"
            "    scripts/build.sh            ->  build/flavors/release\n"
            "    scripts/build.sh debug      ->  build/flavors/debug\n"
            "    scripts/build.sh asan       ->  build/flavors/asan\n"
            "\n"
            "See the layout map at the top of scripts/build.sh.\n"
            "Escape hatch (not recommended): -DARIA_ALLOW_BUILD_ROOT=ON\n")
    endif()
endif()

# ── Project meta ─────────────────────────────────────────────────────────────
if(NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 20)
endif()
if(CMAKE_CXX_STANDARD LESS 20)
    message(FATAL_ERROR "Aria requires C++20 or later")
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

# Unified runtime output directory so tests can find shared libraries on Windows
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Export compile commands for clangd / clang-tidy
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# ── Options ──────────────────────────────────────────────────────────────────
option(ARIA_BUILD_TESTS      "Build tests"              ON)
option(ARIA_BUILD_BENCHMARK  "Build benchmark"          ON)
option(ARIA_BUILD_DOCS       "Build Doxygen docs"       OFF)
option(ARIA_BUILD_SHARED     "Build compiled modules as SHARED" ON)

option(ARIA_BUILD_QT6        "Build Qt6 adapter"        OFF)
option(ARIA_BUILD_APPKIT     "Build macOS AppKit adapter" OFF)
option(ARIA_BUILD_UIKIT      "Build iOS UIKit adapter"  OFF)
option(ARIA_BUILD_HTTP       "Build HTTP/REST/SSE adapter (Web)" OFF)
option(ARIA_HTTP_ENABLE_TLS  "Enable HTTPS in the HTTP adapter (builds OpenSSL from source)" ON)
option(ARIA_BUILD_JNI        "Build Android JNI adapter" OFF)
if(ARIA_BUILD_WASM)
    message(FATAL_ERROR "The WebAssembly adapter is not implemented; ARIA_BUILD_WASM is unsupported")
endif()

# ── Includes ─────────────────────────────────────────────────────────────────
include(PlatformDetect)
include(CompilerWarnings)
include(Sanitizers)

# Common warnings target
add_library(aria_warnings INTERFACE)
aria_set_warnings(aria_warnings)
aria_apply_sanitizers(aria_warnings)
add_library(aria::warnings ALIAS aria_warnings)

# Threads
find_package(Threads REQUIRED)

# ── Tests setup (must come BEFORE modules so subdirs can use doctest target) ──
if(ARIA_BUILD_TESTS)
    enable_testing()
    # Prefer bundled doctest if available (avoids network fetch)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/doctest/doctest.h")
        add_library(doctest INTERFACE)
        target_include_directories(doctest INTERFACE
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party>
        )
        add_library(doctest::doctest ALIAS doctest)
    else()
        include(CPM)
        CPMAddPackage(
            NAME doctest
            GITHUB_REPOSITORY doctest/doctest
            GIT_TAG v2.4.11
            OPTIONS "DOCTEST_WITH_TESTS OFF"
        )
    endif()
endif()

# ── Modules ──────────────────────────────────────────────────────────────────
add_subdirectory(modules/abi)
add_subdirectory(modules/core)
add_subdirectory(modules/async)
add_subdirectory(modules/runtime)
add_subdirectory(modules/binding)

# ── Optional adapters ────────────────────────────────────────────────────────
if(ARIA_BUILD_QT6)
    add_subdirectory(modules/adapters/qt6)
endif()

if(ARIA_BUILD_HTTP)
    if(ARIA_HTTP_ENABLE_TLS)
        # Reuse complete parent targets, otherwise build vendored OpenSSL.
        # The helper rejects missing source/partial parent configuration.
        include(BuildOpenSSL)
    endif()
    add_subdirectory(modules/adapters/http)
endif()

# JNI adapter — bridges Aria core to Android UI (RecyclerView/LiveData)
# via JNI. Named after the technology (JNI) to match APPKIT/UIKit/QT6/HTTP.
if(ARIA_BUILD_JNI)
    add_subdirectory(modules/adapters/jni)
endif()

# AppKit / UIKit adapters live in `modules/adapters/{appkit,uikit}/`
# so any Apple-platform CMake consumer can link them with a
# canonical aria::adapters::appkit / aria::adapters::uikit alias.
#
# These targets are opt-in via ARIA_BUILD_APPKIT / ARIA_BUILD_UIKIT,
# no longer auto-enabled on every Apple host. Rationale:
#   * macOS host should not silently build UIKit (needs iOS SDK
#     toolchain — fails or pulls wrong SDK on plain Xcode CLI).
#   * Consumers on Linux/Windows must be able to keep the option
#     OFF without their CMake config touching adapter subdirs.
#   * `if(APPLE)` is still required; non-Apple platforms cannot
#     link Cocoa/UIKit frameworks even if the option is forced ON.
if(APPLE AND ARIA_BUILD_APPKIT)
    add_subdirectory(modules/adapters/appkit)
endif()
if(APPLE AND ARIA_BUILD_UIKIT)
    add_subdirectory(modules/adapters/uikit)
endif()

# ── Acceptance tests ─────────────────────────────────────────────────────────
if(ARIA_BUILD_TESTS)
    # Cross-DSO and end-to-end contract gates that cannot be represented as
    # ordinary in-process unit tests.
    add_subdirectory(tests/acceptance)
endif()

# ── Benchmark ────────────────────────────────────────────────────────────────
if(ARIA_BUILD_BENCHMARK)
    add_subdirectory(benchmark)
endif()

# ── API reference (Doxygen) ──────────────────────────────────────────────────
# ROADMAP P1-F. Opt-in via -DARIA_BUILD_DOCS=ON. Creates a non-ALL
# `aria_docs` target so the docs are only built on explicit request:
#   cmake --build build --target aria_docs
if(ARIA_BUILD_DOCS)
    find_package(Doxygen QUIET)
    if(DOXYGEN_FOUND)
        find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
        set(ARIA_DOCS_SOURCE_REVISION "main" CACHE STRING
            "Git revision used for API reference source links")
        set(ARIA_DOCS_REPOSITORY_URL "${PROJECT_HOMEPAGE_URL}" CACHE STRING
            "GitHub repository used for API reference source links")
        set(ARIA_DOCS_OUTPUT_DIR "${CMAKE_BINARY_DIR}/docs")
        if(TARGET Doxygen::dot)
            set(ARIA_DOXYGEN_HAVE_DOT "YES")
        else()
            set(ARIA_DOXYGEN_HAVE_DOT "NO")
        endif()
        configure_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Doxyfile.in"
            "${CMAKE_BINARY_DIR}/Doxyfile"
            @ONLY)
        add_custom_target(aria_docs
            COMMAND ${CMAKE_COMMAND} -E rm -rf "${ARIA_DOCS_OUTPUT_DIR}/html"
            COMMAND ${DOXYGEN_EXECUTABLE} "${CMAKE_BINARY_DIR}/Doxyfile"
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts/prepare-docs-html.py"
                "${ARIA_DOCS_OUTPUT_DIR}/html"
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-docs-html.py"
                "${ARIA_DOCS_OUTPUT_DIR}/html"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            COMMENT "Generating Aria API reference with Doxygen → ${ARIA_DOCS_OUTPUT_DIR}/html"
            VERBATIM)
        message(STATUS "  API docs        : aria_docs target (Doxygen ${DOXYGEN_VERSION})")
    else()
        message(FATAL_ERROR
            "ARIA_BUILD_DOCS=ON but Doxygen was not found; the 'aria_docs' "
            "target was not created. Install doxygen (and optionally "
            "graphviz) to build the API reference.")
    endif()
endif()

# ── Install / export (only when built standalone, not as a subdirectory) ─────
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    set(ARIA_INSTALL_TARGETS
        aria_warnings
        aria_abi
        aria_core
        aria_async
        aria_runtime
        aria_binding
    )
    if(ARIA_BUILD_QT6)
        list(APPEND ARIA_INSTALL_TARGETS aria_qt6)
    endif()
    if(ARIA_BUILD_HTTP)
        list(APPEND ARIA_INSTALL_TARGETS aria_http)
    endif()
    # JNI adapter: only present when building with an Android NDK toolchain
    # (target created in modules/adapters/jni/CMakeLists.txt, gated on ANDROID).
    if(ANDROID AND ARIA_BUILD_JNI)
        list(APPEND ARIA_INSTALL_TARGETS aria_adapters_jni)
    endif()
    # Apple-platform adapters: only present (and only valid to install)
    # when both APPLE and the corresponding option are ON. Targets are
    # created in modules/adapters/{appkit,uikit}/CMakeLists.txt.
    if(APPLE AND ARIA_BUILD_APPKIT)
        list(APPEND ARIA_INSTALL_TARGETS aria_adapters_appkit)
    endif()
    if(APPLE AND ARIA_BUILD_UIKIT)
        list(APPEND ARIA_INSTALL_TARGETS aria_adapters_uikit)
    endif()

    # ELF RUNPATH is not inherited by indirect dependencies. Each installed
    # shared library must resolve its sibling Aria libraries independently,
    # including after the SDK prefix moves or --as-needed prunes a consumer's
    # direct dependencies. Preserve any explicitly configured install paths.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        foreach(aria_target IN LISTS ARIA_INSTALL_TARGETS)
            get_target_property(aria_target_type ${aria_target} TYPE)
            if(aria_target_type STREQUAL "SHARED_LIBRARY")
                set_property(TARGET ${aria_target} APPEND PROPERTY
                    INSTALL_RPATH "$ORIGIN")
            endif()
        endforeach()
        unset(aria_target)
        unset(aria_target_type)
    endif()

    install(TARGETS ${ARIA_INSTALL_TARGETS}
        EXPORT ariaTargets
        ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    # Strip the "aria_" prefix on export so installed targets become
    # aria::core / aria::abi / aria::async / ... matching the in-tree aliases.
    set_target_properties(aria_warnings PROPERTIES EXPORT_NAME warnings)
    set_target_properties(aria_abi      PROPERTIES EXPORT_NAME abi)
    set_target_properties(aria_core     PROPERTIES EXPORT_NAME core)
    set_target_properties(aria_async    PROPERTIES EXPORT_NAME async)
    set_target_properties(aria_runtime  PROPERTIES EXPORT_NAME runtime)
    set_target_properties(aria_binding  PROPERTIES EXPORT_NAME binding)
    if(ARIA_BUILD_QT6)
        set_target_properties(aria_qt6 PROPERTIES EXPORT_NAME qt6)
    endif()
    if(ARIA_BUILD_HTTP)
        set_target_properties(aria_http PROPERTIES EXPORT_NAME http)
    endif()
    # JNI adapter exports under aria::adapters::jni to match the in-tree alias.
    if(ANDROID AND ARIA_BUILD_JNI)
        set_target_properties(aria_adapters_jni
            PROPERTIES EXPORT_NAME adapters::jni)
    endif()
    # Apple adapters export under aria::adapters::{appkit,uikit} to
    # match the in-tree alias namespace.
    if(APPLE AND ARIA_BUILD_APPKIT)
        set_target_properties(aria_adapters_appkit
            PROPERTIES EXPORT_NAME adapters::appkit)
    endif()
    if(APPLE AND ARIA_BUILD_UIKIT)
        set_target_properties(aria_adapters_uikit
            PROPERTIES EXPORT_NAME adapters::uikit)
    endif()

    # Headers — every module ships its own include/ tree.
    set(ARIA_HEADER_MODULES abi core async runtime binding)
    if(ARIA_BUILD_QT6)
        list(APPEND ARIA_HEADER_MODULES adapters/qt6)
    endif()
    if(ARIA_BUILD_HTTP)
        list(APPEND ARIA_HEADER_MODULES adapters/http)
    endif()
    if(ANDROID AND ARIA_BUILD_JNI)
        list(APPEND ARIA_HEADER_MODULES adapters/jni)
    endif()
    if(APPLE AND ARIA_BUILD_APPKIT)
        list(APPEND ARIA_HEADER_MODULES adapters/appkit)
    endif()
    if(APPLE AND ARIA_BUILD_UIKIT)
        list(APPEND ARIA_HEADER_MODULES adapters/uikit)
    endif()
    foreach(mod ${ARIA_HEADER_MODULES})
        install(DIRECTORY modules/${mod}/include/
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
            FILES_MATCHING
                PATTERN "*.hpp"
                PATTERN "*.inl"
        )
    endforeach()

    install(EXPORT ariaTargets
        FILE ariaTargets.cmake
        NAMESPACE aria::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/aria
    )

    configure_package_config_file(
        cmake/ariaConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/ariaConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/aria
    )
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/ariaConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/ariaConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/ariaConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/aria
    )
    include(PackageRelease)
endif()

# ── Summary ──────────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "aria v${PROJECT_VERSION} configuration:")
message(STATUS "  Platform        : ${ARIA_PLATFORM_NAME}")
message(STATUS "  Build type      : ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard    : ${CMAKE_CXX_STANDARD}")
message(STATUS "  Compiler        : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Tests           : ${ARIA_BUILD_TESTS}")
message(STATUS "  Benchmark       : ${ARIA_BUILD_BENCHMARK}")
message(STATUS "  Shared libs     : ${ARIA_BUILD_SHARED}")
message(STATUS "  Qt6 adapter     : ${ARIA_BUILD_QT6}")
message(STATUS "  HTTP adapter    : ${ARIA_BUILD_HTTP}")
if(ARIA_BUILD_HTTP)
    message(STATUS "  HTTP TLS        : ${ARIA_HTTP_ENABLE_TLS}")
endif()
message(STATUS "  JNI adapter     : ${ARIA_BUILD_JNI}")
if(ARIA_BUILD_JNI)
    message(STATUS "  Android ABI     : ${ANDROID_ABI}")
    message(STATUS "  Android API     : ${ANDROID_PLATFORM}")
endif()
message(STATUS "  macOS AppKit    : ${ARIA_BUILD_APPKIT}")
message(STATUS "  iOS UIKit       : ${ARIA_BUILD_UIKIT}")
message(STATUS "  ASan/UBSan/TSan : ${ARIA_ENABLE_ASAN}/${ARIA_ENABLE_UBSAN}/${ARIA_ENABLE_TSAN}")
message(STATUS "")
