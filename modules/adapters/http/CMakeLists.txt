# HTTP/REST/SSE adapter — exposes Aria ViewModels to a browser via
# a small REST + Server-Sent-Events protocol.
#
# Dependencies (vendored under third_party/, no network fetch, no
# system-package dependency):
#   - cpp-httplib    (Yuji Hirose, MIT)         — HTTP/1.1 server
#   - nlohmann::json (Niels Lohmann, MIT)        — JSON encode/decode
#   - OpenSSL       (Apache-2.0; built from source via BuildOpenSSL)
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
)
target_include_directories(aria_http SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/third_party/nlohmann_json/include
)

# cpp-httplib is exposed PUBLIC because the
# adapter's public header `http_adapter.hpp` returns a `httplib::Server&`
# from `native_server()` so consumers can register their own routes.
# Mark it SYSTEM so its in-source warnings don't trip consumer -Werror.
# Installed consumers receive the same header and TLS definition, avoiding
# different definitions of httplib classes across the library boundary.
target_include_directories(aria_http SYSTEM PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third_party/cpp-httplib>
)

target_compile_features(aria_http PUBLIC cxx_std_20)
target_compile_definitions(aria_http PRIVATE ARIA_HTTP_BUILD)
if(NOT ARIA_BUILD_SHARED)
    target_compile_definitions(aria_http PUBLIC ARIA_HTTP_STATIC)
endif()

# Optional TLS via OpenSSL — either system-installed (find_package) or
# built from source by ../../../cmake/BuildOpenSSL.cmake.
if(ARIA_HTTP_ENABLE_TLS)
    if(NOT TARGET OpenSSL::SSL OR NOT TARGET OpenSSL::Crypto)
        message(FATAL_ERROR "HTTP TLS was requested without complete OpenSSL targets")
    endif()
    target_compile_definitions(aria_http
        PUBLIC CPPHTTPLIB_OPENSSL_SUPPORT ARIA_HTTP_HAS_TLS)
    target_link_libraries(aria_http PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    if(APPLE)
        # The public httplib TLS client loads the Apple system trust store.
        target_link_libraries(aria_http PUBLIC "-framework Security" "-framework CoreFoundation")
    endif()
    message(STATUS "aria::http: TLS enabled (OpenSSL)")
else()
    message(STATUS "aria::http: TLS disabled (HTTP-only)")
endif()

if(PROJECT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    include(GNUInstallDirs)
    install(FILES "${PROJECT_SOURCE_DIR}/third_party/cpp-httplib/httplib.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
    if(ARIA_HTTP_ENABLE_TLS AND ARIA_BUNDLED_OPENSSL)
        # install_dev supplies relocatable upstream CMake targets, headers
        # and static libraries. Keep them private to this Aria SDK prefix.
        install(DIRECTORY "${OPENSSL_INSTALL_DIR}/"
            DESTINATION "${CMAKE_INSTALL_LIBDIR}/aria/openssl")
        install(FILES "${OPENSSL_SOURCE_DIR}/LICENSE.txt"
            DESTINATION "${CMAKE_INSTALL_LIBDIR}/aria/openssl")
    endif()
endif()

# Platform sockets
if(WIN32)
    target_link_libraries(aria_http PUBLIC ws2_32)
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
