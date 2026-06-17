# Qt6 adapter — only built when ARIA_BUILD_QT6=ON.
#
# Linux/MSYS2:   pacman -S mingw-w64-ucrt-x86_64-qt6-base    (Win)
#                apt install qt6-base-dev libqt6widgets6-dev (Ubuntu)
# macOS:         brew install qt
#
# Then configure with: cmake -DARIA_BUILD_QT6=ON ...

find_package(Qt6 REQUIRED COMPONENTS Core Widgets)

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
