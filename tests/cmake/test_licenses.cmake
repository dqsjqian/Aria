cmake_minimum_required(VERSION 3.20)

function(checked)
    execute_process(COMMAND ${ARGV} RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "License fixture failed: ${ARGV}\n${out}${err}")
    endif()
endfunction()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/project")
file(WRITE "${TEST_ROOT}/project/LICENSE" "fixture own license\n")
file(WRITE "${TEST_ROOT}/project/THIRD_PARTY_NOTICES.md" "fixture notices\n")
foreach(component json mira openssl)
    file(MAKE_DIRECTORY "${TEST_ROOT}/sources/${component}")
endforeach()
file(WRITE "${TEST_ROOT}/sources/json/LICENSE.MIT" "selected JSON license\n")
file(WRITE "${TEST_ROOT}/sources/json/NOTICE" "selected JSON notice\n")
file(WRITE "${TEST_ROOT}/sources/json/include/nlohmann/example.hpp"
    "// SPDX-FileCopyrightText: fixture Björn\n// SPDX-License-Identifier: MIT\n")
file(WRITE "${TEST_ROOT}/sources/mira/LICENSE" "selected Mira license\n")
file(WRITE "${TEST_ROOT}/sources/mira/THIRD_PARTY_NOTICES.md" "selected Mira dependency notices\n")
file(WRITE "${TEST_ROOT}/sources/openssl/LICENSE.txt" "selected OpenSSL license\n")
file(WRITE "${TEST_ROOT}/project/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.20)
project(license_fixture VERSION 1.0 LANGUAGES NONE)
set(CMAKE_INSTALL_DATADIR share)
set(ARIA_BUNDLED_OPENSSL ON)
set(_aria_json_license_source "${JSON_SOURCE}")
set(_aria_mira_license_source "${SOURCE_ROOT}/mira")
set(OPENSSL_SOURCE_DIR "${SOURCE_ROOT}/openssl")
# A stale/inherited downloader variable must not identify a parent target.
set(ARIA_PINNED_JSON_SOURCE_DIR "${SOURCE_ROOT}/json")
include("${ARIA_SOURCE_DIR}/cmake/ariaLicenses.cmake")
set(ARIA_PLATFORM_NAME fixture)
include("${ARIA_SOURCE_DIR}/cmake/PackageRelease.cmake")
]=])
set(common -S "${TEST_ROOT}/project" "-DARIA_SOURCE_DIR=${ARIA_SOURCE_DIR}"
    "-DSOURCE_ROOT=${TEST_ROOT}/sources")
checked("${CMAKE_COMMAND}" ${common} -B "${TEST_ROOT}/full"
    -DARIA_BUILD_HTTP=ON -DARIA_HTTP_ENABLE_TLS=ON
    "-DJSON_SOURCE=${TEST_ROOT}/sources/json" "-DARIA_RELEASE_DIR=${TEST_ROOT}/release")
checked("${CMAKE_COMMAND}" --build "${TEST_ROOT}/full" --target package-release)
include("${TEST_ROOT}/full/aria-license-files.cmake")
list(LENGTH aria_license_sources count)
math(EXPR last "${count} - 1")
foreach(index RANGE ${last})
    list(GET aria_license_sources ${index} source)
    list(GET aria_license_destinations ${index} destination)
    file(SHA256 "${source}" expected)
    file(SHA256 "${TEST_ROOT}/release/${destination}" actual)
    if(NOT expected STREQUAL actual)
        message(FATAL_ERROR "Wrong selected license bytes: ${destination}")
    endif()
endforeach()
if(NOT EXISTS "${TEST_ROOT}/release/share/licenses/aria/json/NOTICE"
   OR NOT EXISTS "${TEST_ROOT}/release/share/licenses/aria/mira/THIRD_PARTY_NOTICES.md"
   OR NOT EXISTS "${TEST_ROOT}/release/THIRD_PARTY_NOTICES.md")
    message(FATAL_ERROR "Package omitted notices")
endif()
file(READ "${TEST_ROOT}/release/share/licenses/aria/json/json-ATTRIBUTIONS.txt" attributions)
if(NOT attributions MATCHES "fixture Björn")
    message(FATAL_ERROR "JSON attribution lost UTF-8 copyright text")
endif()

checked("${CMAKE_COMMAND}" ${common} -B "${TEST_ROOT}/core" -DARIA_BUILD_HTTP=OFF)
checked("${CMAKE_COMMAND}" --install "${TEST_ROOT}/core" --prefix "${TEST_ROOT}/core-prefix")
if(EXISTS "${TEST_ROOT}/core-prefix/share/licenses/aria/json")
    message(FATAL_ERROR "Core-only install included an unused dependency")
endif()

# Parent target: ordinary builds remain usable; distribution must fail before
# package-release removes a previously prepared tree. No default substitution.
checked("${CMAKE_COMMAND}" ${common} -B "${TEST_ROOT}/parent"
    -DARIA_BUILD_HTTP=ON -DARIA_HTTP_ENABLE_TLS=OFF
    "-DARIA_RELEASE_DIR=${TEST_ROOT}/previous-release")
checked("${CMAKE_COMMAND}" --build "${TEST_ROOT}/parent")
file(WRITE "${TEST_ROOT}/previous-release/keep.txt" "keep\n")
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${TEST_ROOT}/parent" --target package-release
    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(result EQUAL 0 OR NOT "${out}${err}" MATCHES "ARIA_JSON_LICENSE_FILE"
   OR NOT EXISTS "${TEST_ROOT}/previous-release/keep.txt")
    message(FATAL_ERROR "Missing parent license was not safely diagnosed: ${out}${err}")
endif()
file(WRITE "${TEST_ROOT}/parent-license.txt" "actual parent library license\n")
checked("${CMAKE_COMMAND}" ${common} -B "${TEST_ROOT}/parent"
    "-DARIA_JSON_LICENSE_FILE=${TEST_ROOT}/parent-license.txt")
checked("${CMAKE_COMMAND}" --build "${TEST_ROOT}/parent" --target package-release)
file(READ "${TEST_ROOT}/previous-release/share/licenses/aria/json/parent-license.txt" actual)
if(NOT actual STREQUAL "actual parent library license\n")
    message(FATAL_ERROR "Parent override was not used")
endif()
