# ============================================================================
#  Aria Qt6 Showcase Demo
#
#  Two build modes:
#    In-tree:   part of the main Aria CMake project (add_subdirectory)
#    Standalone: built against installed aria SDK (run-msvc.ps1 sets
#                ARIA_USE_INSTALLED=ON, CMAKE_PREFIX_PATH points to the
#                framework release directory)
# ============================================================================

cmake_minimum_required(VERSION 3.20)
project(aria-qt-showcase LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(DEFINED ARIA_USE_INSTALLED)
    # ── Standalone: find installed aria ──────────────────────────────────
    find_package(aria REQUIRED)
    # Mark the variable as used to suppress CMake "not used" warning
    set(ARIA_USE_INSTALLED "${ARIA_USE_INSTALLED}" CACHE INTERNAL "" FORCE)
else()
    # ── In-tree: skip if Qt6 adapter not enabled ─────────────────────────
    if(NOT ARIA_BUILD_QT6)
        return()
    endif()
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets)

set(CMAKE_AUTOMOC ON)

# Collect source files grouped by business feature. Every .cpp under each
# feature directory is picked up. Using GLOB_RECURSE + CONFIGURE_DEPENDS
# lets CMake re-configure automatically when files are added or removed,
# which is the most convenient setup for a demo project (production
# projects should prefer an explicit file list).
file(GLOB_RECURSE EX1_APP_SRC        CONFIGURE_DEPENDS App/*.cpp)
file(GLOB_RECURSE EX1_BUSINESS_SRC   CONFIGURE_DEPENDS Business/*.cpp)

add_executable(ex_qt_showcase WIN32 MACOSX_BUNDLE
    main.cpp
    ${EX1_APP_SRC}
    ${EX1_BUSINESS_SRC}
)

# Project root include — "App/..." and "Business/Xxx/..." style includes
# both resolve from here.
target_include_directories(ex_qt_showcase PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(ex_qt_showcase PRIVATE
    # aria::qt6 PUBLIC-depends on binding/runtime/async/core/abi, so listing
    # the adapter is enough — listing the transitively pulled-in modules
    # again makes the macOS linker emit a "duplicate libraries" warning and
    # obscures the real dependency intent.
    aria::qt6
    Qt6::Widgets
    aria::warnings
)
