# ariaFetchPinned.cmake — hash-pinned third-party dependencies, the Mira way.
#
# Aria carries no vendored third-party source and no git submodules. Every
# external dependency is downloaded once, verified against a locked
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
#   archives/<name>-<version>-<sha>   verified download
#   extracted/<name>-<version>/        unpacked source + .aria-pinned stamp
#   files/<name>-<sha-prefix>          verified single files

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/ariaDependencies.cmake")

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

# Include file contents, names, empty directories and symlink destinations.
# The extraction stamp is cache metadata, not part of the pinned source tree.
function(_aria_source_digest source output)
    file(GLOB_RECURSE _paths LIST_DIRECTORIES TRUE RELATIVE "${source}" "${source}/*")
    list(SORT _paths)
    set(_records "")
    foreach(_path IN LISTS _paths)
        if(_path STREQUAL ".aria-pinned")
            continue()
        endif()
        string(SHA256 _name_hash "${_path}")
        if(IS_SYMLINK "${source}/${_path}")
            file(READ_SYMLINK "${source}/${_path}" _link)
            string(SHA256 _content_hash "${_link}")
            string(APPEND _records "L${_name_hash}${_content_hash}\n")
        elseif(IS_DIRECTORY "${source}/${_path}")
            string(APPEND _records "D${_name_hash}\n")
        else()
            file(SHA256 "${source}/${_path}" _content_hash)
            string(APPEND _records "F${_name_hash}${_content_hash}\n")
        endif()
    endforeach()
    string(SHA256 _digest "${_records}")
    set(${output} "${_digest}" PARENT_SCOPE)
endfunction()

function(_aria_archive_root unpack output)
    file(GLOB _entries "${unpack}/*")
    list(LENGTH _entries _count)
    if(_count EQUAL 1 AND IS_DIRECTORY "${_entries}")
        set(${output} "${_entries}" PARENT_SCOPE)
    else()
        set(${output} "${unpack}" PARENT_SCOPE)
    endif()
endfunction()

function(aria_fetch_pinned_archive)
    cmake_parse_arguments(PIN "" "NAME;VERSION;URL;SHA256" "" ${ARGN})
    if(NOT PIN_NAME)
        message(FATAL_ERROR "aria_fetch_pinned_archive: missing NAME")
    endif()

    string(TOUPPER "${PIN_NAME}" _upper)
    set(_override "ARIA_PIN_${_upper}_SOURCE_DIR")
    if(${_override})
        if(NOT IS_DIRECTORY "${${_override}}")
            message(FATAL_ERROR "Aria deps: ${_override} is not a source directory: ${${_override}}")
        endif()
        set(ARIA_PINNED_${_upper}_SOURCE_DIR "${${_override}}" PARENT_SCOPE)
        message(STATUS "Aria deps: ${PIN_NAME} from override ${${_override}} (not verified)")
        return()
    endif()
    if(NOT PIN_VERSION AND NOT PIN_URL AND NOT PIN_SHA256)
        aria_resolve_dependency(NAME "${PIN_NAME}"
            VERSION PIN_VERSION URL PIN_URL SHA256 PIN_SHA256)
    endif()
    foreach(required VERSION URL SHA256)
        if(NOT PIN_${required})
            message(FATAL_ERROR "aria_fetch_pinned_archive(${PIN_NAME}): missing ${required}")
        endif()
    endforeach()

    _aria_deps_cache_dir(_cache)
    file(MAKE_DIRECTORY "${_cache}")
    # Configure processes for different flavors share this cache. Hold the
    # lock through verification, download and publication of the source tree.
    file(LOCK "${_cache}/.aria-cache.lock" GUARD FUNCTION TIMEOUT 180)
    get_filename_component(_archive_name "${PIN_URL}" NAME)
    set(_archive "${_cache}/archives/${PIN_NAME}-${PIN_VERSION}-${PIN_SHA256}")
    set(_source "${_cache}/extracted/${PIN_NAME}-${PIN_VERSION}")
    set(_stamp "${_source}/.aria-pinned")
    set(_manifest "${_cache}/manifests/${PIN_NAME}-${PIN_VERSION}-${PIN_SHA256}")
    set(_unpack "${_cache}/extracted/.${PIN_NAME}-${PIN_VERSION}.unpack")

    # Reuse older cache layouts only after checking their bytes. A URL basename
    # alone is not an identity: unrelated releases often use 'v1.0.0.tar.gz'.
    if(NOT EXISTS "${_archive}" AND EXISTS "${_cache}/archives/${_archive_name}")
        file(SHA256 "${_cache}/archives/${_archive_name}" _legacy_hash)
        if(_legacy_hash STREQUAL PIN_SHA256)
            configure_file("${_cache}/archives/${_archive_name}" "${_archive}" COPYONLY)
        endif()
    endif()

    if(EXISTS "${_stamp}")
        file(READ "${_stamp}" _recorded)
        if(NOT _recorded STREQUAL PIN_SHA256)
            message(FATAL_ERROR
                "Aria deps: ${_source} exists but was extracted from different content "
                "(stamp '${_recorded}' != '${PIN_SHA256}'). Remove it and reconfigure.")
        endif()
    endif()

    set(_temporary "${_cache}/archives/.${PIN_NAME}-${PIN_VERSION}.part")
    if(NOT EXISTS "${_archive}")
        if(ARIA_DEPENDENCIES_OFFLINE)
            message(FATAL_ERROR "Aria deps: offline mode requires cached archive '${_archive}'")
        endif()
        message(STATUS "Aria deps: downloading ${PIN_NAME} ${PIN_VERSION}")
        file(MAKE_DIRECTORY "${_cache}/archives")
        file(DOWNLOAD "${PIN_URL}" "${_temporary}"
             SHOW_PROGRESS STATUS _status TLS_VERIFY ON
             TIMEOUT 120 INACTIVITY_TIMEOUT 30)
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

    # A cache hit is not proof of integrity. Verify existing archives too,
    # including on the fast path where a source tree has already been stamped.
    file(SHA256 "${_archive}" _actual)
    if(NOT _actual STREQUAL PIN_SHA256)
        message(FATAL_ERROR
            "Aria deps: cached ${PIN_NAME} ${PIN_VERSION} failed the SHA256 check "
            "(expected ${PIN_SHA256}, got ${_actual}). Remove '${_archive}' and reconfigure.")
    endif()
    if(EXISTS "${_stamp}")
        if(EXISTS "${_manifest}")
            file(READ "${_manifest}" _expected_source)
        else()
            # Upgrade older stamp-only caches using the verified archive as
            # the authority, never by blessing the current extracted bytes.
            file(REMOVE_RECURSE "${_unpack}")
            file(ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${_unpack}")
            _aria_archive_root("${_unpack}" _canonical)
            _aria_source_digest("${_canonical}" _expected_source)
            file(REMOVE_RECURSE "${_unpack}")
        endif()
        _aria_source_digest("${_source}" _actual_source)
        if(NOT _actual_source STREQUAL _expected_source)
            message(FATAL_ERROR
                "Aria deps: extracted ${PIN_NAME} source failed the integrity check. "
                "Remove '${_source}' and reconfigure, or use ${_override} for an intentional patched tree.")
        endif()
        file(WRITE "${_manifest}" "${_expected_source}")
        set(ARIA_PINNED_${_upper}_SOURCE_DIR "${_source}" PARENT_SCOPE)
        return()
    endif()

    file(MAKE_DIRECTORY "${_cache}/extracted")
    file(REMOVE_RECURSE "${_unpack}")
    file(ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${_unpack}")
    # A previous process may have exited between publishing the tree and
    # writing its stamp. Only the unstamped, cache-owned tree is replaced.
    file(REMOVE_RECURSE "${_source}")
    # Normalise the archive's top-level directory (any single one of them).
    _aria_archive_root("${_unpack}" _canonical)
    file(RENAME "${_canonical}" "${_source}")
    file(REMOVE_RECURSE "${_unpack}")
    _aria_source_digest("${_source}" _source_hash)
    file(WRITE "${_manifest}" "${_source_hash}")
    file(WRITE "${_stamp}" "${PIN_SHA256}")
    set(ARIA_PINNED_${_upper}_SOURCE_DIR "${_source}" PARENT_SCOPE)
    message(STATUS "Aria deps: ${PIN_NAME} ${PIN_VERSION} ready (${_source})")
endfunction()

function(aria_fetch_pinned_file)
    cmake_parse_arguments(PIN "" "NAME;URL;SHA256;AS" "" ${ARGN})
    if(PIN_NAME AND NOT PIN_URL AND NOT PIN_SHA256)
        aria_resolve_dependency(NAME "${PIN_NAME}" URL PIN_URL SHA256 PIN_SHA256)
    endif()
    foreach(required NAME URL SHA256)
        if(NOT PIN_${required})
            message(FATAL_ERROR "aria_fetch_pinned_file(${PIN_NAME}): missing ${required}")
        endif()
    endforeach()

    string(TOUPPER "${PIN_NAME}" _upper)
    _aria_deps_cache_dir(_cache)
    file(MAKE_DIRECTORY "${_cache}")
    file(LOCK "${_cache}/.aria-cache.lock" GUARD FUNCTION TIMEOUT 180)
    if(NOT PIN_AS)
        get_filename_component(PIN_AS "${PIN_URL}" NAME)
    endif()
    if(IS_ABSOLUTE "${PIN_AS}" OR PIN_AS MATCHES "(^|[/\\\\])\\.\\.([/\\\\]|$)")
        message(FATAL_ERROR "aria_fetch_pinned_file(${PIN_NAME}): AS must stay inside the cache")
    endif()
    string(SUBSTRING "${PIN_SHA256}" 0 12 _prefix)
    set(_base "${_cache}/files/${PIN_NAME}-${_prefix}")
    set(_target_file "${_base}/${PIN_AS}")

    if(NOT EXISTS "${_target_file}")
        if(ARIA_DEPENDENCIES_OFFLINE)
            message(FATAL_ERROR "Aria deps: offline mode requires cached file '${_target_file}'")
        endif()
        message(STATUS "Aria deps: downloading ${PIN_NAME}")
        file(MAKE_DIRECTORY "${_base}")
        get_filename_component(_parent "${_target_file}" DIRECTORY)
        file(MAKE_DIRECTORY "${_parent}")
        set(_temporary "${_target_file}.part")
        file(DOWNLOAD "${PIN_URL}" "${_temporary}" STATUS _status TLS_VERIFY ON
             TIMEOUT 120 INACTIVITY_TIMEOUT 30)
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
    file(SHA256 "${_target_file}" _actual)
    if(NOT _actual STREQUAL PIN_SHA256)
        message(FATAL_ERROR
            "Aria deps: cached ${PIN_NAME} failed the SHA256 check "
            "(expected ${PIN_SHA256}, got ${_actual}). Remove '${_target_file}' and reconfigure.")
    endif()
    set(ARIA_PINNED_${_upper}_FILE "${_target_file}" PARENT_SCOPE)
    set(ARIA_PINNED_${_upper}_BASE "${_base}" PARENT_SCOPE)
endfunction()
