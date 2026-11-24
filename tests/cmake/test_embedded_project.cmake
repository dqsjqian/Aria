cmake_minimum_required(VERSION 3.20)

# A host may have unrelated or stale helpers with the same filenames.
# Aria must load its own implementation, including in optional modules.
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/parent/cmake")
foreach(module PlatformDetect CompilerWarnings Sanitizers ariaFetchPinned ariaDependencies ariaFindQt ariaSharedDependencies ariaParentOpenSSL PackageRelease)
    file(WRITE "${TEST_ROOT}/parent/cmake/${module}.cmake"
        "message(FATAL_ERROR \"Host ${module} shadowed Aria's own helper\")\n")
endforeach()
file(WRITE "${TEST_ROOT}/parent/CMakeLists.txt"
    "cmake_minimum_required(VERSION 3.20)\n"
    "project(aria_host LANGUAGES CXX)\n"
    "list(PREPEND CMAKE_MODULE_PATH \"${TEST_ROOT}/parent/cmake\")\n"
    "add_subdirectory(\"${ARIA_SOURCE_DIR}\" aria)\n")
set(generator_args "")
if(TEST_GENERATOR_PLATFORM)
    list(APPEND generator_args -A "${TEST_GENERATOR_PLATFORM}")
endif()
if(TEST_GENERATOR_TOOLSET)
    list(APPEND generator_args -T "${TEST_GENERATOR_TOOLSET}")
endif()
# Reproduce the parent's explicit build identity, so the embedded project
# shares (instead of replacing) the parent's dependency artifacts.
set(identity_args "")
if(TEST_BUILD_TYPE)
    list(APPEND identity_args "-DCMAKE_BUILD_TYPE=${TEST_BUILD_TYPE}")
endif()
if(TEST_OSX_ARCHITECTURES)
    string(REPLACE "|" "\\;" architectures "${TEST_OSX_ARCHITECTURES}")
    list(APPEND identity_args "-DCMAKE_OSX_ARCHITECTURES=${architectures}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}"
    -S "${TEST_ROOT}/parent" -B "${TEST_ROOT}/build"
    -G "${TEST_GENERATOR}" ${generator_args} ${identity_args}
    "-DCMAKE_CXX_COMPILER=${TEST_CXX_COMPILER}"
    "-DFORGEDEPS_CACHE_DIR=${TEST_DEPS_CACHE}"
    "-DCMAKE_PREFIX_PATH=${TEST_PREFIX_PATH}"
    "-DARIA_PIN_MIRA_SOURCE_DIR=${TEST_MIRA_SOURCE}"
    "-DARIA_BUILD_HTTP=${TEST_HTTP}" "-DARIA_HTTP_ENABLE_TLS=${TEST_TLS}"
    -DARIA_BUILD_TESTS=OFF -DARIA_BUILD_BENCHMARK=OFF -DARIA_BUILD_DOCS=OFF
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Embedded project failed (${result}):\n${output}${error}")
endif()
