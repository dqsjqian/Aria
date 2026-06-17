cmake_minimum_required(VERSION 3.22.1)

project(aria_jni)

# ── C++ standard ──────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Aria framework integration ───────────────────────────────────────────
# The Aria library is pre-built for Android (NDK) and installed at:
#   ${ARIA_PREFIX}/lib/libaria_*.a
#   ${ARIA_PREFIX}/include/aria/*.hpp
#
# Set ARIA_PREFIX via -DARIA_PREFIX=<path> on the cmake command line,
# or via the Gradle externalNativeBuild block.
# ────────────────────────────────────────────────────────────────────────

if(NOT ARIA_PREFIX)
    # Default: assume Aria is built in the standard build/ layout
    # relative to the project root (3 levels up from this file)
    set(ARIA_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../build/platforms/android")
    message(STATUS "ARIA_PREFIX not set, using default: ${ARIA_PREFIX}")
endif()

# Headers are in the dist tree (shared across all platforms)
if(NOT ARIA_INCLUDE_PREFIX)
    set(ARIA_INCLUDE_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../build/dist/tree")
    message(STATUS "ARIA_INCLUDE_PREFIX not set, using default: ${ARIA_INCLUDE_PREFIX}")
endif()

# ── JNI side-channel bridge ─────────────────────────────────────────────
# C++ aria ViewModel → Property on_changed → JNI callback →
# Kotlin MutableStateFlow → Compose recomposition
# ────────────────────────────────────────────────────────────────────────

add_library(aria_jni SHARED
    jni_bridge.cpp
    MainViewModel.cpp
    DataModel.cpp
)

# ── Include directories ───────────────────────────────────────────────────
target_include_directories(aria_jni PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${ARIA_INCLUDE_PREFIX}/include
)

# ── Link with Aria static libraries ──────────────────────────────────────
# Order matters: adapters::jni → binding → runtime → async → core → abi
target_link_libraries(aria_jni
    ${ARIA_PREFIX}/lib/libaria_adapters_jni.a
    ${ARIA_PREFIX}/lib/libaria_binding.a
    ${ARIA_PREFIX}/lib/libaria_runtime.a
    ${ARIA_PREFIX}/lib/libaria_abi.a
    android
    log
)

# ── Compiler flags ───────────────────────────────────────────────────────
target_compile_options(aria_jni PRIVATE
    -Wall -Wextra -Wpedantic
    -fvisibility=hidden
)

# ── Definitions ──────────────────────────────────────────────────────────
target_compile_definitions(aria_jni PRIVATE
    ARIA_BINDING_STATIC
    ARIA_JNI_STATIC
)
