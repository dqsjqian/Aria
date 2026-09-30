cmake_minimum_required(VERSION 3.20)

# A host may have unrelated or stale helpers with the same filenames.
# Aria must load its own implementation, including in optional modules.
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/parent/cmake")
foreach(module PlatformDetect CompilerWarnings Sanitizers ariaFetchPinned ariaDependencies ariaFindQt BuildOpenSSL PackageRelease)
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
execute_process(COMMAND "${CMAKE_COMMAND}"
    -S "${TEST_ROOT}/parent" -B "${TEST_ROOT}/build"
    -G "${TEST_GENERATOR}" ${generator_args}
    "-DCMAKE_CXX_COMPILER=${TEST_CXX_COMPILER}"
    "-DARIA_DEPS_CACHE_DIR=${TEST_DEPS_CACHE}"
    "-DARIA_BUILD_HTTP=${TEST_HTTP}" "-DARIA_HTTP_ENABLE_TLS=${TEST_TLS}"
    -DARIA_BUILD_TESTS=OFF -DARIA_BUILD_BENCHMARK=OFF -DARIA_BUILD_DOCS=OFF
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Embedded project failed (${result}):\n${output}${error}")
endif()
