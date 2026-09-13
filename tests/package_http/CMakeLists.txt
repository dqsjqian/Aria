cmake_minimum_required(VERSION 3.20)
project(aria_http_package_consumer LANGUAGES CXX)

# Configure this independent consumer against an installed Aria SDK:
#   cmake -S tests/package_http -B <build> -DCMAKE_PREFIX_PATH=<prefix>
# Cross builds can pass aria_DIR directly with the normal target toolchain.
option(ARIA_PACKAGE_EXPECT_TLS "Require the installed HTTP TLS dependency contract" ON)
find_package(aria CONFIG QUIET OPTIONAL_COMPONENTS missing_test_component)
if(NOT aria_FOUND OR aria_missing_test_component_FOUND)
    message(FATAL_ERROR "An unavailable optional component must leave the package usable")
endif()
find_package(aria CONFIG QUIET COMPONENTS missing_test_component)
if(aria_FOUND)
    message(FATAL_ERROR "An unavailable required component must reject the package")
endif()
find_package(aria CONFIG REQUIRED COMPONENTS core async runtime binding http)
if(NOT TARGET aria::http)
    message(FATAL_ERROR "The installed Aria SDK has no HTTP adapter")
endif()
if(ARIA_PACKAGE_EXPECT_TLS AND NOT ARIA_HTTP_HAS_TLS)
    message(FATAL_ERROR "The installed HTTP adapter must expose its TLS capability")
endif()

add_executable(aria_http_package_consumer main.cpp)
target_link_libraries(aria_http_package_consumer PRIVATE aria::http)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Exercise each installed DSO's runtime dependency lookup even when the
    # consumer only needs aria::http directly. GNU toolchains commonly enable
    # this by default; Clang must cover the same package-loading contract.
    target_link_options(aria_http_package_consumer PRIVATE "LINKER:--as-needed")
endif()
enable_testing()
add_test(NAME http_package_consumer COMMAND aria_http_package_consumer)
