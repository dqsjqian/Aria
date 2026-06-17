# aria::adapters::appkit — IViewAdapter implementation for native macOS
# (Cocoa / AppKit). Production-grade implementation.
#
# The .mm provides a full IViewAdapter implementation
# (NSTextField / NSButton / NSStepper / NSSlider, with proper signal
# bridge + subscription detach + view destroy hooks), so we build
# it as a STATIC library and let conformance tests link against it
# directly.
#
# Build is gated by `ARIA_BUILD_APPKIT=ON` (root CMakeLists). The
# subdirectory is no longer auto-added on every Apple host; the
# defensive `if(NOT APPLE) return()` here is kept as a belt-and-braces
# guard for direct `add_subdirectory()` use, but the canonical entry
# is via the option.
#
# Existing Xcode example projects (examples/2-macos-appkit-mvvm) that
# compile the .mm via their own build phases continue to work — the
# headers expose the same `AppKitAdapter` / `AppKitView` types in the
# global namespace via `using` aliases, and the .mm file is harmless
# to compile twice (the AppKit symbols only matter for one final link
# unit).

if(NOT APPLE)
    return()
endif()

add_library(aria_adapters_appkit STATIC
    src/AppKitAdapter.mm
)
add_library(aria::adapters::appkit ALIAS aria_adapters_appkit)

target_include_directories(aria_adapters_appkit PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(aria_adapters_appkit PUBLIC cxx_std_20)
target_link_libraries(aria_adapters_appkit
    PUBLIC  aria::core aria::binding
    PRIVATE aria::abi
)
# Cocoa / AppKit
target_link_libraries(aria_adapters_appkit PUBLIC "-framework Cocoa")

# ARC for the Objective-C++ source.
target_compile_options(aria_adapters_appkit PRIVATE -fobjc-arc)

if(ARIA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
