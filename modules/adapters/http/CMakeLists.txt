# HTTP/REST/SSE adapter — exposes Aria ViewModels to a browser via
# a small REST + Server-Sent-Events protocol.
#
# Dependencies (vendored under third_party/, no network fetch, no
# system-package dependency):
#   - cpp-httplib    (Yuji Hirose, MIT)         — HTTP/1.1 server
#   - nlohmann::json (Niels Lohmann, MIT)        — JSON encode/decode
#   - OpenSSL 3.5.x  (Apache-2.0; built from source via BuildOpenSSL)
#                                                — TLS 1.2/1.3 (optional)
#
# Configure with: cmake -DARIA_BUILD_HTTP=ON ...
# To disable TLS:  cmake -DARIA_BUILD_HTTP=ON -DARIA_HTTP_ENABLE_TLS=OFF ...

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
    PRIVATE
        ${PROJECT_SOURCE_DIR}/third_party/nlohmann_json/include
)

# cpp-httplib is exposed PUBLIC (build-interface only) because the
# adapter's public header `http_adapter.hpp` returns a `httplib::Server&`
# from `native_server()` so consumers can register their own routes.
# Mark it SYSTEM so its in-source warnings don't trip consumer -Werror.
# It is NOT installed/exported — installed consumers can't reach the
# escape hatch and must rely on the high-level API only.
target_include_directories(aria_http SYSTEM PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third_party/cpp-httplib>
)

target_compile_features(aria_http PUBLIC cxx_std_20)
target_compile_definitions(aria_http PRIVATE ARIA_HTTP_BUILD)
if(NOT ARIA_BUILD_SHARED)
    target_compile_definitions(aria_http PUBLIC ARIA_HTTP_STATIC)
endif()

target_compile_definitions(aria_http PRIVATE
    CPPHTTPLIB_THREAD_POOL_COUNT=8
)

# Optional TLS via OpenSSL — either system-installed (find_package) or
# built from source by ../../../cmake/BuildOpenSSL.cmake.
if(ARIA_HTTP_ENABLE_TLS AND TARGET OpenSSL::SSL)
    target_compile_definitions(aria_http
        PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT
        PUBLIC  ARIA_HTTP_HAS_TLS)
    target_link_libraries(aria_http PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    # Only add build dependency on the from-source build target; when
    # using a system OpenSSL (find_package) there is no openssl_external.
    if(TARGET openssl_external)
        add_dependencies(aria_http openssl_external)
    endif()
    message(STATUS "aria::http: TLS enabled (OpenSSL)")
else()
    message(STATUS "aria::http: TLS disabled (HTTP-only)")
endif()

# Platform sockets
if(WIN32)
    target_link_libraries(aria_http PRIVATE ws2_32)
endif()

target_link_libraries(aria_http
    PUBLIC  aria::core aria::abi aria::binding
    PRIVATE aria::warnings Threads::Threads
)

set_target_properties(aria_http PROPERTIES
    SOVERSION ${PROJECT_VERSION_MAJOR}
    VERSION ${PROJECT_VERSION}
)

if(ARIA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
