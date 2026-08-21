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
# Consumers link this target directly. The public namespace and aliases
# remain stable for source compatibility.

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
