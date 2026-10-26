cmake_minimum_required(VERSION 3.21)
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/prefix/lib/cmake/nlohmann_json"
    "${TEST_ROOT}/prefix/lib/cmake/OpenSSL" "${TEST_ROOT}/prefix/lib/cmake/Mira")
file(WRITE "${TEST_ROOT}/prefix/lib/cmake/nlohmann_json/nlohmann_jsonConfig.cmake"
    "add_library(nlohmann_json::nlohmann_json INTERFACE IMPORTED)\n")
file(WRITE "${TEST_ROOT}/prefix/lib/cmake/OpenSSL/OpenSSLConfig.cmake"
    "add_library(OpenSSL::SSL INTERFACE IMPORTED)\nadd_library(OpenSSL::Crypto INTERFACE IMPORTED)\n")
file(WRITE "${TEST_ROOT}/prefix/lib/cmake/Mira/MiraConfig.cmake"
    "set(Mira_VERSION 1.2.0)\nforeach(component core transport http tls)\n"
    "add_library(Mira::\${component} INTERFACE IMPORTED)\n"
    "set(Mira_\${component}_FOUND TRUE)\nendforeach()\n")
file(WRITE "${TEST_ROOT}/prefix/lib/cmake/Mira/MiraConfigVersion.cmake"
    "set(PACKAGE_VERSION 1.2.0)\nset(PACKAGE_VERSION_COMPATIBLE TRUE)\n")

function(check_case name targets prefix success diagnostic)
    set(source "${TEST_ROOT}/${name}")
    file(MAKE_DIRECTORY "${source}")
    file(WRITE "${source}/CMakeLists.txt"
        "cmake_minimum_required(VERSION 3.21)\nproject(parent LANGUAGES CXX)\n"
        "${targets}\nadd_subdirectory(\"${ARIA_SOURCE_DIR}\" aria)\n"
        "if(EXISTS \"\${CMAKE_CURRENT_BINARY_DIR}/aria/forgedeps-environment.json\")\n"
        "message(FATAL_ERROR \"Parent targets must not prepare another dependency environment\")\nendif()\n")
    set(generator_args)
    if(TEST_GENERATOR_PLATFORM)
        list(APPEND generator_args -A "${TEST_GENERATOR_PLATFORM}")
    endif()
    if(TEST_GENERATOR_TOOLSET)
        list(APPEND generator_args -T "${TEST_GENERATOR_TOOLSET}")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env "FORGEDEPS_ENVIRONMENTS=[]"
        "${CMAKE_COMMAND}" -S "${source}" -B "${source}/build"
        -G "${TEST_GENERATOR}" ${generator_args}
        "-DCMAKE_CXX_COMPILER=${TEST_CXX_COMPILER}" "-DCMAKE_PREFIX_PATH=${prefix}"
        -DCMAKE_BUILD_TYPE=Release -DARIA_BUILD_HTTP=ON -DARIA_HTTP_ENABLE_TLS=ON
        -DARIA_BUILD_TESTS=OFF -DARIA_BUILD_BENCHMARK=OFF
        -DFORGEDEPS_PYTHON_EXECUTABLE=/must-not-run/python
        -DCMAKE_DISABLE_FIND_PACKAGE_Python3=ON
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    string(REGEX REPLACE "[ \t\r\n]+" " " diagnostic_output "${output}${error}")
    if(success AND NOT result EQUAL 0)
        message(FATAL_ERROR "${name} failed: ${output}${error}")
    elseif(NOT success AND (result EQUAL 0 OR NOT "${diagnostic_output}" MATCHES "${diagnostic}"))
        message(FATAL_ERROR "${name} did not reject the expected missing dependency: ${output}${error}")
    endif()
endfunction()

set(json "add_library(nlohmann_json::nlohmann_json INTERFACE IMPORTED)")
set(ssl "add_library(OpenSSL::SSL INTERFACE IMPORTED)\nadd_library(OpenSSL::Crypto INTERFACE IMPORTED)")
set(mira "set(Mira_VERSION 1.2.0)\nforeach(component core transport http tls)\nadd_library(Mira::\${component} INTERFACE IMPORTED)\nendforeach()")
check_case(parent_targets "${json}\n${ssl}\n${mira}\nset(ARIA_PIN_MIRA_SOURCE_DIR /must-not-fetch/Mira)" "" TRUE "")
check_case(parent_prefixes "" "${TEST_ROOT}/prefix" TRUE "")
check_case(partial_openssl "${json}\nadd_library(OpenSSL::SSL INTERFACE IMPORTED)\n${mira}" "" FALSE "parent supplied only one")
check_case(partial_mira "${json}\n${ssl}\nadd_library(Mira::core INTERFACE IMPORTED)" "" FALSE "lacks the required transport")
check_case(uncovered_mira "${json}\n${ssl}" "" FALSE "Mira is outside the parent shared environment")
