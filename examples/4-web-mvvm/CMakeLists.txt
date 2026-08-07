# Example 4 — Web MVVM via HttpAdapter
#
# A C++ executable that hosts a ViewModel and exposes it to a browser
# over Aria's HTTP/SSE adapter.
#
# Two build modes:
#   In-tree:    part of the main Aria CMake project (add_subdirectory)
#   Standalone: built against installed aria SDK (run.sh sets
#               ARIA_USE_INSTALLED=ON, CMAKE_PREFIX_PATH points to the
#               framework install tree, e.g. build/dist/tree)

cmake_minimum_required(VERSION 3.20)
project(aria-web-mvvm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(DEFINED ARIA_USE_INSTALLED)
    # ── Standalone: find installed aria ──────────────────────────────────
    find_package(aria REQUIRED)
    set(ARIA_USE_INSTALLED "${ARIA_USE_INSTALLED}" CACHE INTERNAL "" FORCE)
else()
    # ── In-tree: skip if HTTP adapter not enabled ────────────────────────
    if(NOT TARGET aria_http)
        message(STATUS "Example 4 (web-mvvm) skipped: aria::http not built "
                       "(set ARIA_BUILD_HTTP=ON to enable)")
        return()
    endif()
endif()

add_executable(example_4_web_mvvm main.cpp)
target_link_libraries(example_4_web_mvvm PRIVATE
    aria::http aria::core aria::warnings)
target_compile_features(example_4_web_mvvm PRIVATE cxx_std_20)

# index.html and aria_client.js live alongside main.cpp; the run script
# passes the source dir as static_root so the server serves them
# directly. No copy step needed.
