# ariaFetchPinned.cmake — hash-pinned third-party dependencies, the Mira way.
#
# Aria carries no vendored third-party source and no git submodules. Every
# external dependency is downloaded once, verified against a hard-coded
# SHA256, cached across build flavors, and only then used. A corrupted or
# tampered archive is a hard configure error — we never build unverified code.
#
# Two primitives:
#
#   aria_fetch_pinned_archive(NAME <name> VERSION <x.y.z> URL <url> SHA256 <hex>)
#     Downloads and verifies a source tarball, extracts it, and sets
#     ARIA_PINNED_<NAME>_SOURCE_DIR in the caller's scope.
#
#   aria_fetch_pinned_file(NAME <name> URL <url> SHA256 <hex> [AS <relpath>])
#     Downloads and verifies a single file (single-header dependencies) and
#     sets ARIA_PINNED_<NAME>_FILE in the caller's scope. With AS, the file
#     lands at that relative path (e.g. "doctest/doctest.h") and
#     ARIA_PINNED_<NAME>_BASE names the include-root directory.
#
# Overrides:
#   -DARIA_DEPS_CACHE_DIR=<dir>   shared download/extraction cache
#   -DARIA_PIN_<NAME>_SOURCE_DIR=<dir>
#       use this source tree as-is (offline development, patched builds);
#       nothing is downloaded and the SHA256 is not consulted. This is an
#       explicit escape hatch — the build then trusts that directory.
#
# Cache layout (default ${CMAKE_SOURCE_DIR}/build/_deps, falling back to
# ${CMAKE_BINARY_DIR}/_deps when the build container is unavailable):
#   archives/<name>-<version>.<ext>    verified download
#   extracted/<name>-<version>/        unpacked source + .aria-pinned stamp
#   files/<name>-<sha-prefix>          verified single files

include_guard(GLOBAL)

function(_aria_deps_cache_dir out)
    if(ARIA_DEPS_CACHE_DIR)
        set(dir "${ARIA_DEPS_CACHE_DIR}")
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/build" AND IS_DIRECTORY "${CMAKE_SOURCE_DIR}/build")
        set(dir "${CMAKE_SOURCE_DIR}/build/_deps")
    else()
        set(dir "${CMAKE_BINARY_DIR}/_deps")
    endif()
    set(${out} "${dir}" PARENT_SCOPE)
endfunction()

function(aria_fetch_pinned_archive)
    cmake_parse_arguments(PIN "" "NAME;VERSION;URL;SHA256" "" ${ARGN})
    foreach(required NAME VERSION URL SHA256)
        if(NOT PIN_${required})
            message(FATAL_ERROR "aria_fetch_pinned_archive(${PIN_NAME}): missing ${required}")
        endif()
    endforeach()

    string(TOUPPER "${PIN_NAME}" _upper)
    set(_override "ARIA_PIN_${_upper}_SOURCE_DIR")
    if(${_override})
        set(ARIA_PINNED_${_upper}_SOURCE_DIR "${${_override}}" PARENT_SCOPE)
        message(STATUS "Aria deps: ${PIN_NAME} from override ${${_override}} (not verified)")
        return()
    endif()

    _aria_deps_cache_dir(_cache)
    get_filename_component(_archive_name "${PIN_URL}" NAME)
    set(_archive "${_cache}/archives/${_archive_name}")
    set(_source "${_cache}/extracted/${PIN_NAME}-${PIN_VERSION}")
    set(_stamp "${_source}/.aria-pinned")

    if(EXISTS "${_stamp}")
        file(READ "${_stamp}" _recorded)
        if(_recorded STREQUAL PIN_SHA256)
            set(ARIA_PINNED_${_upper}_SOURCE_DIR "${_source}" PARENT_SCOPE)
            return()
        endif()
        message(FATAL_ERROR
            "Aria deps: ${_source} exists but was extracted from different content "
            "(stamp '${_recorded}' != '${PIN_SHA256}'). Remove it and reconfigure.")
    endif()

    set(_temporary "${_cache}/archives/.${PIN_NAME}-${PIN_VERSION}.part")
    if(NOT EXISTS "${_archive}")
        message(STATUS "Aria deps: downloading ${PIN_NAME} ${PIN_VERSION}")
        file(MAKE_DIRECTORY "${_cache}/archives")
        file(DOWNLOAD "${PIN_URL}" "${_temporary}"
             SHOW_PROGRESS STATUS _status)
        list(GET _status 0 _code)
        if(NOT _code EQUAL 0)
            file(REMOVE "${_temporary}")
            list(GET _status 1 _text)
            message(FATAL_ERROR "Aria deps: download failed (${_text}): ${PIN_URL}")
        endif()
        file(SHA256 "${_temporary}" _actual)
        if(NOT _actual STREQUAL PIN_SHA256)
            file(REMOVE "${_temporary}")
            message(FATAL_ERROR
                "Aria deps: ${PIN_NAME} ${PIN_VERSION} failed the SHA256 check "
                "(expected ${PIN_SHA256}, got ${_actual}). The archive was deleted; "
                "do not disable this verification.")
        endif()
        file(RENAME "${_temporary}" "${_archive}")
    endif()

    file(MAKE_DIRECTORY "${_cache}/extracted")
    set(_unpack "${_cache}/extracted/.${PIN_NAME}-${PIN_VERSION}.unpack")
    file(REMOVE_RECURSE "${_unpack}")
    file(ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${_unpack}")
    # Normalise the archive's top-level directory (any single one of them).
    file(GLOB _entries "${_unpack}/*")
    list(LENGTH _entries _count)
    if(_count EQUAL 1 AND IS_DIRECTORY "${_entries}")
        file(RENAME "${_entries}" "${_source}")
    else()
        file(RENAME "${_unpack}" "${_source}")
    endif()
    file(REMOVE_RECURSE "${_unpack}")
    file(WRITE "${_stamp}" "${PIN_SHA256}")
    set(ARIA_PINNED_${_upper}_SOURCE_DIR "${_source}" PARENT_SCOPE)
    message(STATUS "Aria deps: ${PIN_NAME} ${PIN_VERSION} ready (${_source})")
endfunction()

function(aria_fetch_pinned_file)
    cmake_parse_arguments(PIN "" "NAME;URL;SHA256;AS" "" ${ARGN})
    foreach(required NAME URL SHA256)
        if(NOT PIN_${required})
            message(FATAL_ERROR "aria_fetch_pinned_file(${PIN_NAME}): missing ${required}")
        endif()
    endforeach()

    string(TOUPPER "${PIN_NAME}" _upper)
    _aria_deps_cache_dir(_cache)
    string(SUBSTRING "${PIN_SHA256}" 0 12 _prefix)
    set(_base "${_cache}/files/${PIN_NAME}-${_prefix}")
    set(_target_file "${_base}/${PIN_AS}")

    if(NOT EXISTS "${_target_file}")
        message(STATUS "Aria deps: downloading ${PIN_NAME}")
        file(MAKE_DIRECTORY "${_base}")
        get_filename_component(_parent "${_target_file}" DIRECTORY)
        file(MAKE_DIRECTORY "${_parent}")
        set(_temporary "${_target_file}.part")
        file(DOWNLOAD "${PIN_URL}" "${_temporary}" STATUS _status)
        list(GET _status 0 _code)
        if(NOT _code EQUAL 0)
            file(REMOVE "${_temporary}")
            list(GET _status 1 _text)
            message(FATAL_ERROR "Aria deps: download failed (${_text}): ${PIN_URL}")
        endif()
        file(SHA256 "${_temporary}" _actual)
        if(NOT _actual STREQUAL PIN_SHA256)
            file(REMOVE "${_temporary}")
            message(FATAL_ERROR
                "Aria deps: ${PIN_NAME} failed the SHA256 check "
                "(expected ${PIN_SHA256}, got ${_actual}).")
        endif()
        file(RENAME "${_temporary}" "${_target_file}")
    endif()
    set(ARIA_PINNED_${_upper}_FILE "${_target_file}" PARENT_SCOPE)
    set(ARIA_PINNED_${_upper}_BASE "${_base}" PARENT_SCOPE)
endfunction()
