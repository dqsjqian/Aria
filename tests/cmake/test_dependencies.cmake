cmake_minimum_required(VERSION 3.20)

# Exercise Aria's compatibility bridge against the installed package offline.
# Detailed selection/provider semantics live in ForgeDeps and the Python suite.
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
file(READ "${ARIA_SOURCE_DIR}/dependencies.json" document)
string(JSON expected_version GET "${document}" dependencies json resolved version)
string(JSON expected_tag GET "${document}" dependencies json resolved tag)
file(WRITE "${TEST_ROOT}/dependencies.json" "${document}")
file(READ "${TEST_ROOT}/dependencies.json" original_content)
string(REPLACE "\r\n" "\n" original_content "${original_content}")

function(run_case name body expected_error)
    file(MAKE_DIRECTORY "${TEST_ROOT}/${name}")
    file(WRITE "${TEST_ROOT}/${name}/case.cmake"
        "cmake_minimum_required(VERSION 3.20)\n"
        "include(\"${ARIA_SOURCE_DIR}/cmake/ariaDependencies.cmake\")\n"
        "set(ARIA_DEPENDENCIES_FILE \"${TEST_ROOT}/dependencies.json\")\n"
        "set(ARIA_DEPENDENCIES_OFFLINE ON)\n${body}\n")
    execute_process(COMMAND "${CMAKE_COMMAND}" -P "${TEST_ROOT}/${name}/case.cmake"
        WORKING_DIRECTORY "${TEST_ROOT}/${name}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(expected_error STREQUAL "")
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "${name} unexpectedly failed:\n${output}${error}")
        endif()
    elseif(result EQUAL 0 OR NOT "${output}${error}" MATCHES "${expected_error}")
        message(FATAL_ERROR "${name} should fail with '${expected_error}':\n${output}${error}")
    endif()
endfunction()

run_case(locked_offline "
aria_resolve_dependency(NAME JSON VERSION version URL url SHA256 hash REVISION revision TAG tag)
if(NOT version STREQUAL \"${expected_version}\" OR NOT tag STREQUAL \"${expected_tag}\")
    message(FATAL_ERROR \"Wrong resolved version\")
endif()
aria_resolve_dependency(NAME json VERSION repeated)
if(NOT repeated STREQUAL version)
    message(FATAL_ERROR \"Warm effective selection changed\")
endif()
" "")
run_case(exact_selection "
set(ARIA_DEP_JSON_VERSION \"${expected_version}\")
aria_resolve_dependency(NAME json VERSION version)
if(NOT version STREQUAL \"${expected_version}\")
    message(FATAL_ERROR \"CMake version override was lost\")
endif()
" "")
run_case(file_override "
set(ARIA_DEPENDENCIES_FILE \"${TEST_ROOT}/missing.json\")
aria_resolve_dependency(NAME json FILE \"${TEST_ROOT}/dependencies.json\" VERSION version)
" "")
run_case(offline_missing_selection [[
set(ARIA_DEP_JSON_VERSION 99.0.0)
aria_resolve_dependency(NAME json VERSION version)
]] "matching resolved")

string(JSON bad SET "${document}" dependencies json resolved sha256 "\"bad\"")
file(WRITE "${TEST_ROOT}/bad.json" "${bad}")
run_case(invalid_hash "aria_resolve_dependency(NAME json FILE \"${TEST_ROOT}/bad.json\" VERSION version)" "SHA256")
string(JSON stale SET "${document}" dependencies json asset "\"changed.tar.xz\"")
file(WRITE "${TEST_ROOT}/stale.json" "${stale}")
run_case(changed_spec_offline "aria_resolve_dependency(NAME json FILE \"${TEST_ROOT}/stale.json\" VERSION version)" "matching resolved")
run_case(source_override "
include(\"${ARIA_SOURCE_DIR}/cmake/ariaFetchPinned.cmake\")
set(FORGEDEPS_PYTHON_EXECUTABLE \"\")
set(CMAKE_DISABLE_FIND_PACKAGE_Python3 TRUE)
set(ARIA_PIN_PATCHED_SOURCE_DIR \"${TEST_ROOT}\")
aria_fetch_pinned_archive(NAME patched)
if(NOT ARIA_PINNED_PATCHED_SOURCE_DIR STREQUAL \"${TEST_ROOT}\")
    message(FATAL_ERROR \"Source override was lost\")
endif()
" "")
run_case(missing_python [[
set(FORGEDEPS_PYTHON_EXECUTABLE "")
set(ForgeDeps_PYTHON_EXECUTABLE "")
set(CMAKE_DISABLE_FIND_PACKAGE_Python3 TRUE)
aria_resolve_dependency(NAME json VERSION version)
]] "require Python 3.14")
# Compare content, not the platform's line endings: the claim under test is
# that configuration must not rewrite the source selections at all.
file(READ "${TEST_ROOT}/dependencies.json" after_content)
string(REPLACE "\r\n" "\n" after_content "${after_content}")
if(NOT after_content STREQUAL original_content)
    message(FATAL_ERROR "Configure modified source dependency selections:\nbefore:\n${original_content}\nafter:\n${after_content}")
endif()

# Two installed SDK candidates under the same search prefix. Natural sorting
# must choose 6.10 over 6.9; an exact request must still choose the older one.
foreach(version 6.9.0 6.10.0)
    set(directory "${TEST_ROOT}/qt/Qt6-${version}/lib/cmake/Qt6")
    file(MAKE_DIRECTORY "${directory}")
    file(WRITE "${directory}/Qt6Config.cmake" "set(Qt6_VERSION \"${version}\")\n")
    file(WRITE "${directory}/Qt6ConfigVersion.cmake"
        "set(PACKAGE_VERSION \"${version}\")\n"
        "if(PACKAGE_FIND_VERSION VERSION_EQUAL PACKAGE_VERSION)\n"
        "  set(PACKAGE_VERSION_EXACT TRUE)\n"
        "endif()\n"
        "if(NOT PACKAGE_FIND_VERSION VERSION_GREATER PACKAGE_VERSION)\n"
        "  set(PACKAGE_VERSION_COMPATIBLE TRUE)\n"
        "endif()\n")
endforeach()
run_case(qt_latest "
set(CMAKE_PREFIX_PATH \"${TEST_ROOT}/qt\")
set(CMAKE_FIND_PACKAGE_SORT_ORDER NAME)
set(CMAKE_FIND_PACKAGE_SORT_DIRECTION ASC)
aria_find_qt6(REQUIRED CONFIG NO_DEFAULT_PATH PATHS \"${TEST_ROOT}/qt\")
if(NOT Qt6_VERSION STREQUAL \"6.10.0\")
    message(FATAL_ERROR \"Newest visible Qt was not selected: \${Qt6_VERSION}\")
endif()
if(NOT CMAKE_FIND_PACKAGE_SORT_ORDER STREQUAL \"NAME\" OR NOT CMAKE_FIND_PACKAGE_SORT_DIRECTION STREQUAL \"ASC\")
    message(FATAL_ERROR \"Qt helper changed parent package search settings\")
endif()
" "")
run_case(qt_exact "
set(ARIA_DEP_QT_VERSION 6.9.0)
aria_find_qt6(REQUIRED CONFIG NO_DEFAULT_PATH PATHS \"${TEST_ROOT}/qt\")
if(NOT Qt6_VERSION STREQUAL \"6.9.0\")
    message(FATAL_ERROR \"Exact Qt version was not selected\")
endif()
if(DEFINED CMAKE_FIND_PACKAGE_SORT_ORDER OR DEFINED CMAKE_FIND_PACKAGE_SORT_DIRECTION)
    message(FATAL_ERROR \"Qt helper left new search settings in the caller\")
endif()
" "")
run_case(qt_missing_exact "
set(ARIA_DEP_QT_VERSION 6.8.0)
aria_find_qt6(REQUIRED CONFIG NO_DEFAULT_PATH PATHS \"${TEST_ROOT}/qt\")
" "6.8.0")
set(qt_scope_source "${TEST_ROOT}/qt_imported_targets")
file(MAKE_DIRECTORY "${qt_scope_source}")
file(WRITE "${qt_scope_source}/CMakeLists.txt"
    "cmake_minimum_required(VERSION 3.20)\n"
    "project(QtImportedTargets NONE)\n"
    "include(\"${ARIA_SOURCE_DIR}/cmake/ariaFindQt.cmake\")\n"
    "add_library(Qt6::Core INTERFACE IMPORTED)\n"
    "add_library(Qt6::Widgets INTERFACE IMPORTED)\n"
    "aria_find_qt6(QUIET COMPONENTS Core Widgets)\n"
    "if(NOT Qt6_FOUND OR NOT Qt6_Core_FOUND OR NOT Qt6_Widgets_FOUND)\n"
    "  message(FATAL_ERROR \"Existing Qt6 component targets did not produce a successful package result\")\n"
    "endif()\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${qt_scope_source}" -B "${qt_scope_source}/build"
    RESULT_VARIABLE qt_scope_result OUTPUT_VARIABLE qt_scope_output ERROR_VARIABLE qt_scope_error)
if(NOT qt_scope_result EQUAL 0)
    message(FATAL_ERROR "qt_imported_targets_in_parent_scope failed:\n${qt_scope_output}${qt_scope_error}")
endif()
message(STATUS "ForgeDeps bridge selection/override/offline/source override/Qt fixtures passed")
