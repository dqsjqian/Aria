cmake_minimum_required(VERSION 3.20)

# Public Aria wrapper -> packaged CMake bridge -> Python verified cache.
# Cache-layout and hostile-archive details belong to the ForgeDeps suite.
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
set(module "${ARIA_SOURCE_DIR}/cmake/ariaFetchPinned.cmake")
if(DEFINED ENV{FORGEDEPS_PYTHON_EXECUTABLE} AND NOT "$ENV{FORGEDEPS_PYTHON_EXECUTABLE}" STREQUAL "")
    set(Python3_EXECUTABLE "$ENV{FORGEDEPS_PYTHON_EXECUTABLE}")
endif()
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND "${Python3_EXECUTABLE}" -c
    "import pathlib,sys; print(pathlib.Path(sys.argv[1]).resolve().as_uri())" "${TEST_ROOT}"
    OUTPUT_VARIABLE fixture_uri OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
function(run_fetch name body expected_error)
    file(WRITE "${TEST_ROOT}/${name}.cmake"
        "cmake_minimum_required(VERSION 3.20)\ninclude(\"${module}\")\n${body}\n")
    execute_process(COMMAND "${CMAKE_COMMAND}" -P "${TEST_ROOT}/${name}.cmake"
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
set(file_fetch "set(FORGEDEPS_CACHE_DIR \"${TEST_ROOT}/file-cache\")\n"
    "aria_fetch_pinned_file(NAME fixture URL \"${fixture_uri}/fixture.hpp\" SHA256 ${header_hash} AS include/fixture.hpp)\n"
    "file(WRITE \"${TEST_ROOT}/file-path\" \"\${ARIA_PINNED_FIXTURE_FILE}\")\n"
    "if(NOT ARIA_PINNED_FIXTURE_FILE STREQUAL \"\${ARIA_PINNED_FIXTURE_BASE}/include/fixture.hpp\")\n"
    "message(FATAL_ERROR \"File/base/AS contract changed\")\nendif()")
string(JOIN "" file_fetch ${file_fetch})
run_fetch(file_fetch "${file_fetch}" "")
run_fetch(file_warm_offline "set(ARIA_DEPENDENCIES_OFFLINE ON)\n${file_fetch}" "")
file(READ "${TEST_ROOT}/file-path" cached_file)
file(WRITE "${cached_file}" "corrupted\n")
run_fetch(file_corruption "${file_fetch}" "SHA256|integrity|modified|mismatch")
run_fetch(file_path_escape
    "set(FORGEDEPS_CACHE_DIR \"${TEST_ROOT}/escape-cache\")\naria_fetch_pinned_file(NAME fixture URL \"${fixture_uri}/fixture.hpp\" SHA256 ${header_hash} AS ../outside.hpp)"
    "[Pp]ath|[Ee]scape|relative")
run_fetch(file_offline_missing
    "set(ARIA_DEPENDENCIES_OFFLINE ON)\nset(FORGEDEPS_CACHE_DIR \"${TEST_ROOT}/empty-cache\")\naria_fetch_pinned_file(NAME fixture URL \"${fixture_uri}/fixture.hpp\" SHA256 ${header_hash})"
    "[Oo]ffline")

# Same URL basename, distinct contents: neither source can alias the other.
foreach(name alpha beta)
    file(MAKE_DIRECTORY "${TEST_ROOT}/${name}/payload")
    file(WRITE "${TEST_ROOT}/${name}/payload/value.txt" "${name}")
    # Python's tar writer does not add macOS AppleDouble metadata siblings.
    execute_process(COMMAND "${Python3_EXECUTABLE}" -c
        "import pathlib,sys,tarfile; root=pathlib.Path(sys.argv[1]); archive=tarfile.open(root/'v1.tar','w'); archive.add(root/'payload',arcname='payload'); archive.close()"
        "${TEST_ROOT}/${name}" RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Cannot create test archive")
    endif()
    file(SHA256 "${TEST_ROOT}/${name}/v1.tar" ${name}_hash)
    set(${name}_fetch "set(FORGEDEPS_CACHE_DIR \"${TEST_ROOT}/archive-cache\")\naria_fetch_pinned_archive(NAME ${name} VERSION 1 URL \"${fixture_uri}/${name}/v1.tar\" SHA256 ${${name}_hash})\nfile(WRITE \"${TEST_ROOT}/${name}-path\" \"\${ARIA_PINNED_${name}_SOURCE_DIR}\")")
    # Public output names are uppercase even when dependency lookup is lowercase.
    string(TOUPPER "${name}" upper)
    string(REPLACE "ARIA_PINNED_${name}" "ARIA_PINNED_${upper}" ${name}_fetch "${${name}_fetch}")
    run_fetch(${name}_archive "${${name}_fetch}" "")
    file(READ "${TEST_ROOT}/${name}-path" source)
    file(READ "${source}/value.txt" value)
    if(NOT value STREQUAL name)
        message(FATAL_ERROR "Archive basename collision: expected ${name}, got ${value}")
    endif()
endforeach()
run_fetch(archive_warm_offline "set(ARIA_DEPENDENCIES_OFFLINE ON)\n${alpha_fetch}" "")
file(READ "${TEST_ROOT}/beta-path" source)
file(WRITE "${source}/value.txt" "modified source")
run_fetch(extracted_source_corruption "${beta_fetch}" "integrity|modified|mismatch")

# Concurrent requests publish one verified result through the shared package.
set(parallel_script "${TEST_ROOT}/parallel.cmake")
file(WRITE "${parallel_script}"
    "cmake_minimum_required(VERSION 3.20)\ninclude(\"${module}\")\n"
    "set(FORGEDEPS_CACHE_DIR \"${TEST_ROOT}/parallel-cache\")\n"
    "aria_fetch_pinned_archive(NAME alpha VERSION 1 URL \"${fixture_uri}/alpha/v1.tar\" SHA256 ${alpha_hash})\n"
    "aria_fetch_pinned_file(NAME fixture URL \"${fixture_uri}/fixture.hpp\" SHA256 ${header_hash} AS include/fixture.hpp)\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -P "${parallel_script}"
    COMMAND "${CMAKE_COMMAND}" -P "${parallel_script}"
    COMMAND "${CMAKE_COMMAND}" -P "${parallel_script}"
    RESULTS_VARIABLE results OUTPUT_VARIABLE output ERROR_VARIABLE error)
foreach(result IN LISTS results)
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "Concurrent cache access failed: ${results}\n${output}${error}")
    endif()
endforeach()
message(STATUS "ForgeDeps pinned-cache bridge regressions passed")
