# aria::adapters::jni — IViewAdapter implementation for native Android
# via JNI (bridge to RecyclerView / LiveData / Compose).
#
# Named after the bridging technology (JNI) to match the project's
# convention of naming adapters after their framework/tech rather than
# the platform: APPKIT (macOS), UIKIT (iOS), QT6, HTTP (Web). JNI is the
# *only* way to bridge C++ to Android UI, so "Android adapter" == "JNI
# adapter" — one option, one module.
#
# This module is a STATIC library that implements IViewAdapter using JNI
# calls into the Java/Kotlin layer.
#
# Build is gated by `ARIA_BUILD_JNI=ON` (root CMakeLists). The defensive
# `if(NOT ANDROID) return()` here is a belt-and-braces guard for direct
# `add_subdirectory()` use; the canonical entry is via the option.
#
# Platform requirements:
#   - Android NDK r25+ (Clang compiler, C++20 support)
#   - CMake 3.20+
#   - Android API level 21+ (Android 5.0 Lollipop)
#
# Usage:
#   cmake -DARIA_BUILD_JNI=ON \
#         -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
#         -DANDROID_ABI=arm64-v8a \
#         -DANDROID_PLATFORM=android-21 \
#         -B build/platforms/android

if(NOT ANDROID)
    return()
endif()

add_library(aria_adapters_jni STATIC
    src/JniAdapter.cpp
)
add_library(aria::adapters::jni ALIAS aria_adapters_jni)

target_include_directories(aria_adapters_jni PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(aria_adapters_jni PUBLIC cxx_std_20)

target_link_libraries(aria_adapters_jni
    PUBLIC  aria::core aria::binding
    PRIVATE aria::abi
)

# Android log library (liblog) for __android_log_print diagnostics.
target_link_libraries(aria_adapters_jni PUBLIC log)

# Symbol-visibility build macro.
target_compile_definitions(aria_adapters_jni PRIVATE ARIA_JNI_BUILD)
if(NOT ARIA_BUILD_SHARED)
    target_compile_definitions(aria_adapters_jni PUBLIC ARIA_JNI_STATIC)
endif()

if(ARIA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
