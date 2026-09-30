cmake_minimum_required(VERSION 3.20)

# Offline integration tests: exercise the public fetch functions in separate
# configure processes, including persisted cache hits and hard-error paths.
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
set(module "${ARIA_SOURCE_DIR}/cmake/ariaFetchPinned.cmake")

function(run_fetch name body expected_error)
    set(script "${TEST_ROOT}/${name}.cmake")
    file(WRITE "${script}"
        "cmake_minimum_required(VERSION 3.20)\ninclude(\"${module}\")\n${body}\n")
    execute_process(COMMAND "${CMAKE_COMMAND}" -P "${script}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(expected_error)
        if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "${expected_error}")
            message(FATAL_ERROR "${name}: expected ${expected_error}, got ${result}\n${output}${error}")
        endif()
    elseif(NOT result EQUAL 0)
        message(FATAL_ERROR "${name}: failed (${result})\n${output}${error}")
    endif()
endfunction()

file(WRITE "${TEST_ROOT}/fixture.hpp" "verified header\n")
file(SHA256 "${TEST_ROOT}/fixture.hpp" header_hash)
set(file_fetch "set(ARIA_DEPS_CACHE_DIR \"${TEST_ROOT}/file-cache\")\n"
    "aria_fetch_pinned_file(NAME fixture URL \"file://${TEST_ROOT}/fixture.hpp\" SHA256 ${header_hash})")
string(JOIN "" file_fetch ${file_fetch})
run_fetch(file_default_name "${file_fetch}" "")
run_fetch(file_warm_cache "${file_fetch}" "")
string(SUBSTRING "${header_hash}" 0 12 header_prefix)
file(WRITE "${TEST_ROOT}/file-cache/files/fixture-${header_prefix}/fixture.hpp" "corrupted\n")
run_fetch(file_corruption "${file_fetch}" "failed the SHA256 check")
run_fetch(file_download_corruption
    "set(ARIA_DEPS_CACHE_DIR \"${TEST_ROOT}/bad-file-cache\")\naria_fetch_pinned_file(NAME fixture URL \"file://${TEST_ROOT}/fixture.hpp\" SHA256 deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef AS fixture.hpp)"
    "failed the SHA256 check")
run_fetch(file_path_escape
    "set(ARIA_DEPS_CACHE_DIR \"${TEST_ROOT}/escape-cache\")\naria_fetch_pinned_file(NAME fixture URL \"file://${TEST_ROOT}/fixture.hpp\" SHA256 ${header_hash} AS ../outside.hpp)"
    "AS must stay inside the cache")

# Unrelated URLs with the same basename must not alias one cached archive.
foreach(name alpha beta)
    file(MAKE_DIRECTORY "${TEST_ROOT}/${name}/payload")
    file(WRITE "${TEST_ROOT}/${name}/payload/value.txt" "${name}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "../v1.tar" payload
        WORKING_DIRECTORY "${TEST_ROOT}/${name}" RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Cannot create test archive")
    endif()
    file(RENAME "${TEST_ROOT}/v1.tar" "${TEST_ROOT}/${name}/v1.tar")
    file(SHA256 "${TEST_ROOT}/${name}/v1.tar" ${name}_hash)
    set(${name}_fetch "set(ARIA_DEPS_CACHE_DIR \"${TEST_ROOT}/archive-cache\")\naria_fetch_pinned_archive(NAME ${name} VERSION 1 URL \"file://${TEST_ROOT}/${name}/v1.tar\" SHA256 ${${name}_hash})")
    run_fetch(${name}_archive "${${name}_fetch}" "")
    file(READ "${TEST_ROOT}/archive-cache/extracted/${name}-1/value.txt" value)
    if(NOT value STREQUAL name)
        message(FATAL_ERROR "Archive basename collision: expected ${name}, got ${value}")
    endif()
endforeach()
run_fetch(archive_warm_cache "${alpha_fetch}" "")
file(WRITE "${TEST_ROOT}/archive-cache/extracted/beta-1/value.txt" "modified source")
run_fetch(extracted_source_corruption "${beta_fetch}" "source failed the integrity check")
# Removing the manifest must not turn an edited, old-layout tree into trusted
# content: the expected digest must come from a fresh verified extraction.
file(REMOVE "${TEST_ROOT}/archive-cache/manifests/beta-1-${beta_hash}")
run_fetch(legacy_source_corruption "${beta_fetch}" "source failed the integrity check")
file(WRITE "${TEST_ROOT}/archive-cache/extracted/beta-1/value.txt" "beta")
run_fetch(legacy_source_migration "${beta_fetch}" "")

# Interrupted publication leaves an unstamped source tree. Re-extract cleanly.
file(REMOVE "${TEST_ROOT}/archive-cache/extracted/alpha-1/.aria-pinned")
file(WRITE "${TEST_ROOT}/archive-cache/extracted/alpha-1/stale.txt" "partial tree")
run_fetch(archive_interrupted_publish "${alpha_fetch}" "")
if(EXISTS "${TEST_ROOT}/archive-cache/extracted/alpha-1/stale.txt")
    message(FATAL_ERROR "An incomplete source tree survived recovery")
endif()

# Even a stamped source tree must not bypass verification of cached archives.
file(WRITE "${TEST_ROOT}/archive-cache/archives/alpha-1-${alpha_hash}" "corrupted\n")
run_fetch(archive_corruption "${alpha_fetch}" "failed the SHA256 check")
file(REMOVE_RECURSE "${TEST_ROOT}/archive-cache/extracted/alpha-1")
run_fetch(archive_corruption_without_source "${alpha_fetch}" "failed the SHA256 check")

# CMake executes pipeline commands concurrently. Each independent configure
# process requests the same initially empty cache, exercising shared .part and
# unpack publication without introducing a Python or shell test dependency.
set(parallel_script "${TEST_ROOT}/parallel.cmake")
file(WRITE "${parallel_script}"
    "cmake_minimum_required(VERSION 3.20)\ninclude(\"${module}\")\n"
    "set(ARIA_DEPS_CACHE_DIR \"${TEST_ROOT}/parallel-cache\")\n"
    "aria_fetch_pinned_archive(NAME alpha VERSION 1 URL \"file://${TEST_ROOT}/alpha/v1.tar\" SHA256 ${alpha_hash})\n"
    "aria_fetch_pinned_file(NAME fixture URL \"file://${TEST_ROOT}/fixture.hpp\" SHA256 ${header_hash} AS include/fixture.hpp)\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -P "${parallel_script}"
    COMMAND "${CMAKE_COMMAND}" -P "${parallel_script}"
    COMMAND "${CMAKE_COMMAND}" -P "${parallel_script}"
    RESULTS_VARIABLE parallel_results OUTPUT_VARIABLE output ERROR_VARIABLE error)
foreach(result IN LISTS parallel_results)
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "Concurrent cache access failed: ${parallel_results}\n${output}${error}")
    endif()
endforeach()

message(STATUS "Pinned cache regressions passed")
