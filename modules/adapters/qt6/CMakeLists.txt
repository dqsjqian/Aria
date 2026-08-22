# Qt6 adapter — only built when ARIA_BUILD_QT6=ON.
#
# Linux/MSYS2:   pacman -S mingw-w64-ucrt-x86_64-qt6-base    (Win)
#                apt install qt6-base-dev libqt6widgets6-dev (Ubuntu)
# macOS:         brew install qt
#
# Then configure with: cmake -DARIA_BUILD_QT6=ON ...

find_package(Qt6 REQUIRED COMPONENTS Core Widgets)

# Reject cross-toolchain Qt kits. An MSVC build that consumes a MinGW/MSYS2
# Qt (e.g. via a stale Qt6_DIR cache entry or PATH-derived search prefixes)
# pulls mingw-w64 CRT headers (corecrt.h / math.h full of __asm__) into the
# MSVC compilation unit and fails with hundreds of unrelated-looking
# vcruntime.h / uintptr_t errors inside qt_adapter.cpp. The reverse mix
# (MSVC Qt + MinGW compiler) fails at link time instead. Fail fast with a
# message that names the actual problem.
if(WIN32)
    string(TOLOWER "${Qt6_DIR}" _aria_qt6_dir_lower)
    if(MSVC AND _aria_qt6_dir_lower MATCHES "msys|mingw|ucrt64")
        message(FATAL_ERROR
            "Qt6 resolved to a MinGW/MSYS2 kit, incompatible with the MSVC "
            "toolchain:\n  Qt6_DIR = ${Qt6_DIR}\n"
            "Reconfigure with the MSVC kit, e.g.:\n"
            "  cmake -DQt6_DIR=D:/worksoft/Qt/6.11.1/msvc2022_64/lib/cmake/Qt6 ...")
    elseif(NOT MSVC AND _aria_qt6_dir_lower MATCHES "msvc[0-9]")
        message(FATAL_ERROR
            "Qt6 resolved to an MSVC kit, incompatible with the "
            "${CMAKE_CXX_COMPILER_ID} toolchain:\n  Qt6_DIR = ${Qt6_DIR}\n"
            "Reconfigure with the MinGW/MSYS2 kit, e.g.:\n"
            "  cmake -DQt6_DIR=D:/worksoft/msys64/ucrt64/lib/cmake/Qt6 ...")
    endif()
    unset(_aria_qt6_dir_lower)
endif()

set(CMAKE_AUTOMOC ON)

if(ARIA_BUILD_SHARED)
    add_library(aria_qt6 SHARED)
else()
    add_library(aria_qt6 STATIC)
endif()
add_library(aria::qt6 ALIAS aria_qt6)

target_sources(aria_qt6 PRIVATE
    src/qt_adapter.cpp
)

target_include_directories(aria_qt6 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(aria_qt6 PUBLIC cxx_std_20)

# Disable Qt's "emit"/"signals"/"slots" macros — they collide with our
# SignalErased::emit() method and any user method named `signals`/`slots`.
# Use Q_EMIT / Q_SIGNALS / Q_SLOTS instead inside Qt code that we author.
target_compile_definitions(aria_qt6 PUBLIC QT_NO_KEYWORDS)
target_compile_definitions(aria_qt6 PRIVATE ARIA_QT6_BUILD)
if(NOT ARIA_BUILD_SHARED)
    target_compile_definitions(aria_qt6 PUBLIC ARIA_QT6_STATIC)
endif()

target_link_libraries(aria_qt6
    PUBLIC  aria::core aria::abi aria::binding
            Qt6::Core Qt6::Widgets
    PRIVATE aria::warnings
)

set_target_properties(aria_qt6 PROPERTIES
    SOVERSION ${PROJECT_VERSION_MAJOR}
    VERSION ${PROJECT_VERSION}
)

if(ARIA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
