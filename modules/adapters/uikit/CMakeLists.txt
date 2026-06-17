# aria::adapters::uikit — IViewAdapter implementation for native iOS
# (UIKit). Production-grade implementation, structurally mirrors
# the AppKit module.
#
# This module also ships a STATIC target so:
#   * In-tree CMake builds (Apple host with iOS toolchain) can link
#     `aria::adapters::uikit` directly, matching aria::adapters::appkit.
#   * `find_package(aria)` consumers obtain a real installed target,
#     not a header-only stub that silently drops the .mm symbols.
#
# Build is opt-in via root option `ARIA_BUILD_UIKIT=ON`. The Xcode
# example continues to work — the .mm is harmless to compile twice
# (UIKit symbols only matter for one final link unit) and the
# `using` aliases in the header keep its global-namespace API intact.
#
# Platform gate:
#   Two layers guard the UIKit build:
#     1. Root CMakeLists.txt guards `if(APPLE AND ARIA_BUILD_UIKIT)`
#        so non-Apple hosts never enter this subdirectory.
#     2. This file additionally requires a real iOS toolchain — either
#        CMAKE_SYSTEM_NAME=iOS (Xcode/ios.toolchain.cmake) or an
#        OSX_SYSROOT pointing at iphoneos / iphonesimulator. A macOS
#        host that force-enables ARIA_BUILD_UIKIT=ON without an iOS
#        toolchain would otherwise try to compile UIKitAdapter.mm
#        against the macOS SDK and fail with opaque ObjC linker
#        errors; we fail fast with a clear message instead.

if(NOT APPLE)
    return()
endif()

# Detect a real iOS toolchain. Accept either the modern
# `CMAKE_SYSTEM_NAME=iOS` (Xcode Generator / ios.toolchain.cmake) or a
# legacy `CMAKE_OSX_SYSROOT` pointing at an iPhone SDK.
set(_aria_uikit_is_ios FALSE)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(_aria_uikit_is_ios TRUE)
elseif(CMAKE_OSX_SYSROOT MATCHES "iphoneos|iphonesimulator|iPhoneOS|iPhoneSimulator")
    set(_aria_uikit_is_ios TRUE)
endif()

if(NOT _aria_uikit_is_ios)
    message(FATAL_ERROR
        "ARIA_BUILD_UIKIT=ON requires an iOS toolchain, but CMake is configured for "
        "CMAKE_SYSTEM_NAME='${CMAKE_SYSTEM_NAME}' with CMAKE_OSX_SYSROOT='${CMAKE_OSX_SYSROOT}'. "
        "To build the UIKit adapter, configure with an iOS toolchain "
        "(e.g. `-G Xcode -DCMAKE_SYSTEM_NAME=iOS`, or pass an ios.toolchain.cmake file). "
        "The Xcode example in examples/3-ios-oc-uikit-mvvm/ compiles UIKitAdapter.mm "
        "directly and does not require this option."
    )
endif()
unset(_aria_uikit_is_ios)

add_library(aria_adapters_uikit STATIC
    src/UIKitAdapter.mm
)
add_library(aria::adapters::uikit ALIAS aria_adapters_uikit)

target_include_directories(aria_adapters_uikit PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(aria_adapters_uikit PUBLIC cxx_std_20)
target_link_libraries(aria_adapters_uikit
    PUBLIC  aria::core aria::binding
    PRIVATE aria::abi
)
# UIKit + Foundation (UIView / UIButton / UILabel / NSObject etc.)
target_link_libraries(aria_adapters_uikit PUBLIC
    "-framework UIKit"
    "-framework Foundation"
)

# ARC for the Objective-C++ source.
target_compile_options(aria_adapters_uikit PRIVATE -fobjc-arc)
