# HTTP/REST/SSE adapter — exposes Aria ViewModels to a browser via
# a small REST + Server-Sent-Events protocol.
#
# Dependencies (hash-pinned downloads, no vendored source, no submodules —
# see cmake/ariaFetchPinned.cmake):
#   - Mira       (dqsjqian, MIT)   — coroutine-native HTTP/1.1 server,
#                                        event loop, TCP, optional TLS
#   - nlohmann::json (Niels Lohmann, MIT) — JSON encode/decode
#   - OpenSSL        (Apache-2.0; shared ForgeRecipes package) — TLS 1.2/1.3
#
# Configure with: cmake -DARIA_BUILD_HTTP=ON ...
# To disable TLS:  cmake -DARIA_BUILD_HTTP=ON -DARIA_HTTP_ENABLE_TLS=OFF ...

# Mira is fetched as a hash-pinned release archive (no vendored source,
# no submodule; see cmake/ariaFetchPinned.cmake). Only what the adapter uses
# is enabled; HTTP/2 and HTTP/3 stay off. Mira requires C++23, and the
# adapter propagates cxx_std_23 PUBLIC for exactly that reason.
# Override for offline work: -DARIA_PIN_MIRA_SOURCE_DIR=/path/to/Mira
set(_aria_mira_components transport http)
if(ARIA_HTTP_ENABLE_TLS)
    list(APPEND _aria_mira_components tls)
endif()
if(NOT TARGET Mira::core AND CMAKE_PREFIX_PATH AND NOT ARIA_PIN_MIRA_SOURCE_DIR)
    unset(Mira_DIR CACHE)
    find_package(Mira 1.0 CONFIG QUIET COMPONENTS ${_aria_mira_components}
        PATHS ${CMAKE_PREFIX_PATH} NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
endif()
if(TARGET Mira::core)
    foreach(_component IN LISTS _aria_mira_components)
        if(NOT TARGET Mira::${_component})
            message(FATAL_ERROR "Parent Mira package lacks the required ${_component} component")
        endif()
    endforeach()
    set(_aria_mira_version "${Mira_VERSION}")
    if(NOT _aria_mira_version)
        get_target_property(_aria_mira_target Mira::core ALIASED_TARGET)
        if(NOT _aria_mira_target)
            set(_aria_mira_target Mira::core)
        endif()
        get_target_property(_aria_mira_imported "${_aria_mira_target}" IMPORTED)
        if(_aria_mira_imported)
            message(FATAL_ERROR "Parent imported Mira targets must provide their actual Mira_VERSION")
        endif()
        get_target_property(_aria_mira_source "${_aria_mira_target}" SOURCE_DIR)
        get_directory_property(_aria_mira_version DIRECTORY "${_aria_mira_source}"
            DEFINITION PROJECT_VERSION)
        # A parent source build supplies the actual package version and license.
        if(EXISTS "${_aria_mira_source}/../../LICENSE")
            set(_aria_mira_license_source "${_aria_mira_source}/../.." PARENT_SCOPE)
        endif()
    endif()
    foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
        if(Mira_DIR)
            cmake_path(IS_PREFIX _prefix "${Mira_DIR}" NORMALIZE _aria_match)
            if(_aria_match)
                foreach(_license_dir share/licenses/mira share/licenses/Mira)
                    if(EXISTS "${_prefix}/${_license_dir}/LICENSE")
                        set(_aria_mira_license_source "${_prefix}/${_license_dir}" PARENT_SCOPE)
                    endif()
                endforeach()
            endif()
        endif()
    endforeach()
else()
    foreach(_component IN LISTS _aria_mira_components)
        if(TARGET Mira::${_component})
            message(FATAL_ERROR "Parent supplied partial Mira targets without Mira::core")
        endif()
    endforeach()
    if(DEFINED ENV{FORGEDEPS_ENVIRONMENTS} OR DEFINED ENV{FORGEDEPS_ENVIRONMENT})
        # A normal Aria build deliberately keeps Mira as source. A shared SDK
        # recipe must declare Mira in its requires graph before entering a lease.
        if(NOT ARIA_PIN_MIRA_SOURCE_DIR AND NOT ARIA_HTTP_PREPARED_MIRA_SOURCE)
            message(FATAL_ERROR "Mira is outside the parent shared environment; declare it in the recipe dependency graph")
        endif()
    endif()
    include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/ariaFetchPinned.cmake")
    aria_fetch_pinned_archive(NAME Mira)
    set(_aria_mira_license_source "${ARIA_PINNED_MIRA_SOURCE_DIR}" PARENT_SCOPE)
    set(MIRA_BUILD_TESTS OFF)
    set(MIRA_BUILD_EXAMPLES OFF)
    set(MIRA_ENABLE_HTTP2 OFF)
    set(MIRA_ENABLE_HTTP3 OFF)
    set(MIRA_ENABLE_WEBSOCKET OFF)
    set(MIRA_ENABLE_TLS ${ARIA_HTTP_ENABLE_TLS})
    if(NOT ARIA_BUILD_SHARED)
        set(MIRA_INSTALL ON)
    endif()
    add_subdirectory(${ARIA_PINNED_MIRA_SOURCE_DIR} Mira)
    get_directory_property(_aria_mira_version DIRECTORY "${ARIA_PINNED_MIRA_SOURCE_DIR}"
        DEFINITION PROJECT_VERSION)
    set(ARIA_HTTP_PREPARED_MIRA_SOURCE "${ARIA_PINNED_MIRA_SOURCE_DIR}" CACHE INTERNAL "Prepared Mira source integration")
    set(_aria_mira_test_source "${ARIA_PINNED_MIRA_SOURCE_DIR}" PARENT_SCOPE)
endif()
if(NOT _aria_mira_version OR _aria_mira_version VERSION_LESS 1.0)
    message(FATAL_ERROR "Aria HTTP requires Mira 1.0 or later with its actual package version")
endif()
# Mira promises source compatibility, not a stable binary ABI. A static SDK must
# resolve the same Mira version used to compile its HTTP implementation.
set(ARIA_HTTP_MIRA_VERSION "${_aria_mira_version}" PARENT_SCOPE)


# Sanitizers must reach Mira as well: MSVC refuses to link annotated
# and unannotated TUs together (LNK2038 on the container-annotation
# metadata), and on GCC/Clang the Mira archives would otherwise
# silently opt out of the sanitizer guarantees the rest of the build has.
# aria_apply_sanitizers() is INTERFACE-shaped — for these third-party
# targets the flags must land PRIVATE so their own TUs are instrumented.
set(_mira_san_flags "")
if(ARIA_ENABLE_ASAN)
    if(MSVC)
        list(APPEND _mira_san_flags /fsanitize=address /Zi)
    else()
        list(APPEND _mira_san_flags -fsanitize=address -fno-omit-frame-pointer)
    endif()
endif()
if(ARIA_ENABLE_UBSAN AND NOT MSVC)
    list(APPEND _mira_san_flags -fsanitize=undefined -fno-sanitize-recover=undefined)
endif()
if(ARIA_ENABLE_TSAN AND NOT MSVC)
    list(APPEND _mira_san_flags -fsanitize=thread)
endif()
if(ARIA_ENABLE_MSAN AND NOT MSVC)
    list(APPEND _mira_san_flags -fsanitize=memory -fsanitize-memory-track-origins -fno-omit-frame-pointer)
endif()
if(_mira_san_flags)
    foreach(_mira_target mira_core mira_transport mira_http mira_tls)
        if(TARGET ${_mira_target})
            get_target_property(_aria_mira_imported ${_mira_target} IMPORTED)
            if(_aria_mira_imported)
                continue()
            endif()
            target_compile_options(${_mira_target} PRIVATE ${_mira_san_flags})
            target_link_options(${_mira_target} PRIVATE ${_mira_san_flags})
        endif()
    endforeach()
endif()

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

# Optional TLS via the parent or shared OpenSSL package. Mira's TLS
# module owns the OpenSSL linkage; Aria's public surface no longer leaks
# any SSL types.
if(ARIA_HTTP_ENABLE_TLS)
    if(NOT TARGET OpenSSL::SSL OR NOT TARGET OpenSSL::Crypto)
        message(FATAL_ERROR "HTTP TLS was requested without complete OpenSSL targets")
    endif()
    target_compile_definitions(aria_http PUBLIC ARIA_HTTP_HAS_TLS)
    target_link_libraries(aria_http PRIVATE Mira::transport Mira::http Mira::tls)
    message(STATUS "aria::http: TLS enabled (OpenSSL via Mira)")
else()
    target_link_libraries(aria_http PRIVATE Mira::transport Mira::http)
    message(STATUS "aria::http: TLS disabled (HTTP-only)")
endif()

if(PROJECT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    if(ARIA_HTTP_ENABLE_TLS AND ARIA_BUNDLED_OPENSSL)
        # install_dev supplies relocatable upstream CMake targets, headers
        # and static libraries. Keep them private to this Aria SDK prefix.
        install(DIRECTORY "${OPENSSL_INSTALL_DIR}/"
            DESTINATION "${CMAKE_INSTALL_LIBDIR}/aria/openssl"
            PATTERN "manifest.json" EXCLUDE PATTERN "build.log" EXCLUDE)
        install(FILES "${OPENSSL_SOURCE_DIR}/LICENSE.txt"
            DESTINATION "${CMAKE_INSTALL_LIBDIR}/aria/openssl")
    endif()
endif()

target_link_libraries(aria_http
    PUBLIC  aria::core aria::abi aria::binding
    # JSON is used only in compiled implementation files. Static exports must
    # not reference this private, uninstalled header-only build target.
    PRIVATE aria::warnings $<BUILD_INTERFACE:nlohmann_json::nlohmann_json>
)

set_target_properties(aria_http PROPERTIES
    SOVERSION ${PROJECT_VERSION_MAJOR}
    VERSION ${PROJECT_VERSION}
)

if(ARIA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
