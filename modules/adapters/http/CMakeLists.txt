# HTTP/REST/SSE adapter — exposes Aria ViewModels to a browser via
# a small REST + Server-Sent-Events protocol.
#
# Dependencies (hash-pinned downloads, no vendored source, no submodules —
# see cmake/ariaFetchPinned.cmake):
#   - Continuo       (dqsjqian, MIT)   — coroutine-native HTTP/1.1 server,
#                                        event loop, TCP, optional TLS
#   - nlohmann::json (Niels Lohmann, MIT) — JSON encode/decode
#   - OpenSSL        (Apache-2.0; pinned release, built from source via
#                      BuildOpenSSL)     — TLS 1.2/1.3 (optional, via Continuo)
#
# Configure with: cmake -DARIA_BUILD_HTTP=ON ...
# To disable TLS:  cmake -DARIA_BUILD_HTTP=ON -DARIA_HTTP_ENABLE_TLS=OFF ...

# Continuo is fetched as a hash-pinned release archive (no vendored source,
# no submodule; see cmake/ariaFetchPinned.cmake). Only what the adapter uses
# is enabled; HTTP/2 and HTTP/3 stay off. Continuo requires C++23, and the
# adapter propagates cxx_std_23 PUBLIC for exactly that reason.
# Override for offline work: -DARIA_PIN_CONTINUO_SOURCE_DIR=/path/to/continuo
include(ariaFetchPinned)
aria_fetch_pinned_archive(
    NAME continuo
    VERSION 0.1.2
    URL "https://github.com/dqsjqian/continuo/releases/download/v0.1.2/continuo-0.1.2.tar.gz"
    SHA256 508c56092c004bda7e7e606e2c266fab8a918b38bab46c96b37604862681dafa
)
set(CONTINUO_BUILD_TESTS OFF)
set(CONTINUO_BUILD_EXAMPLES OFF)
set(CONTINUO_ENABLE_HTTP2 OFF)
set(CONTINUO_ENABLE_HTTP3 OFF)
set(CONTINUO_ENABLE_TLS ${ARIA_HTTP_ENABLE_TLS})
if(NOT ARIA_BUILD_SHARED)
    # Static aria_http: consumer link lines end at the Continuo archives,
    # so the Continuo package config must be installed alongside Aria.
    set(CONTINUO_INSTALL ON)
endif()
add_subdirectory(${ARIA_PINNED_CONTINUO_SOURCE_DIR} continuo)

if(ARIA_BUILD_SHARED)
    add_library(aria_http SHARED)
else()
    add_library(aria_http STATIC)
endif()
add_library(aria::http ALIAS aria_http)

target_sources(aria_http PRIVATE
    src/http_adapter.cpp
)

target_include_directories(aria_http
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)


target_compile_features(aria_http PUBLIC cxx_std_23)
target_compile_definitions(aria_http PRIVATE ARIA_HTTP_BUILD)
if(NOT ARIA_BUILD_SHARED)
    target_compile_definitions(aria_http PUBLIC ARIA_HTTP_STATIC)
endif()

# Optional TLS via OpenSSL — either system-installed (find_package) or
# built from source by ../../../cmake/BuildOpenSSL.cmake. Continuo's TLS
# module owns the OpenSSL linkage; Aria's public surface no longer leaks
# any SSL types.
if(ARIA_HTTP_ENABLE_TLS)
    if(NOT TARGET OpenSSL::SSL OR NOT TARGET OpenSSL::Crypto)
        message(FATAL_ERROR "HTTP TLS was requested without complete OpenSSL targets")
    endif()
    target_compile_definitions(aria_http PUBLIC ARIA_HTTP_HAS_TLS)
    target_link_libraries(aria_http PRIVATE continuo::transport continuo::http continuo::tls)
    message(STATUS "aria::http: TLS enabled (OpenSSL via Continuo)")
else()
    target_link_libraries(aria_http PRIVATE continuo::transport continuo::http)
    message(STATUS "aria::http: TLS disabled (HTTP-only)")
endif()

if(PROJECT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    if(ARIA_HTTP_ENABLE_TLS AND ARIA_BUNDLED_OPENSSL)
        # install_dev supplies relocatable upstream CMake targets, headers
        # and static libraries. Keep them private to this Aria SDK prefix.
        install(DIRECTORY "${OPENSSL_INSTALL_DIR}/"
            DESTINATION "${CMAKE_INSTALL_LIBDIR}/aria/openssl")
        install(FILES "${OPENSSL_SOURCE_DIR}/LICENSE.txt"
            DESTINATION "${CMAKE_INSTALL_LIBDIR}/aria/openssl")
    endif()
endif()

target_link_libraries(aria_http
    PUBLIC  aria::core aria::abi aria::binding
    PRIVATE aria::warnings nlohmann_json::nlohmann_json
)

set_target_properties(aria_http PROPERTIES
    SOVERSION ${PROJECT_VERSION_MAJOR}
    VERSION ${PROJECT_VERSION}
)

if(ARIA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
