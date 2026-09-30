cmake_minimum_required(VERSION 3.20)
project(aria_core_package_consumer LANGUAGES CXX)

# Deliberately model an older parent application. Linking Aria must raise its
# language mode through the installed target, without a top-level Aria build.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Optional adapters in the SDK must not impose their dependencies on core.
set(CMAKE_DISABLE_FIND_PACKAGE_Qt6 TRUE)
set(CMAKE_DISABLE_FIND_PACKAGE_Mira TRUE)
set(CMAKE_DISABLE_FIND_PACKAGE_OpenSSL TRUE)
find_package(aria 3.0 CONFIG REQUIRED COMPONENTS core)
find_package(aria 3.0 CONFIG REQUIRED COMPONENTS core OPTIONAL_COMPONENTS qt6)
if(aria_qt6_FOUND)
    message(FATAL_ERROR "An unavailable optional Qt dependency must leave only core usable")
endif()
# A shared HTTP build without TLS has no external runtime package dependency.
if(ARIA_HTTP_NEEDS_MIRA OR ARIA_HTTP_HAS_TLS)
    find_package(aria 3.0 CONFIG REQUIRED COMPONENTS core OPTIONAL_COMPONENTS http)
    if(aria_http_FOUND)
        message(FATAL_ERROR "An unavailable optional HTTP dependency must not be reported found")
    endif()
endif()
add_executable(aria_core_package_consumer main.cpp)
target_link_libraries(aria_core_package_consumer PRIVATE aria::core)
enable_testing()
add_test(NAME core_package_consumer COMMAND aria_core_package_consumer)
