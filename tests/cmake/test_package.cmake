cmake_minimum_required(VERSION 3.20)

function(checked)
    execute_process(COMMAND ${ARGV}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "SDK command failed (${result}): ${ARGV}\n${output}${error}")
    endif()
endfunction()

file(REMOVE_RECURSE "${TEST_ROOT}")
checked("${CMAKE_COMMAND}" --install "${ARIA_BINARY_DIR}"
    --config "${TEST_CONFIG}" --prefix "${TEST_ROOT}/original")
file(RENAME "${TEST_ROOT}/original" "${TEST_ROOT}/relocated")
set(prefix "${TEST_ROOT}/relocated")
set(toolchain_args "-DCMAKE_CXX_COMPILER=${TEST_CXX_COMPILER}")
if(TEST_GENERATOR_PLATFORM)
    list(APPEND toolchain_args -A "${TEST_GENERATOR_PLATFORM}")
endif()
if(TEST_GENERATOR_TOOLSET)
    list(APPEND toolchain_args -T "${TEST_GENERATOR_TOOLSET}")
endif()
foreach(setting OSX_ARCHITECTURES OSX_DEPLOYMENT_TARGET MSVC_RUNTIME_LIBRARY)
    if(TEST_${setting})
        list(APPEND toolchain_args "-DCMAKE_${setting}=${TEST_${setting}}")
    endif()
endforeach()
set(consumers core)
if(TEST_HTTP)
    list(APPEND consumers http)
endif()
foreach(consumer IN LISTS consumers)
    set(dependency_args "")
    if(consumer STREQUAL "http" AND TEST_TLS)
        # A pre-existing config cache must not redirect a bundled TLS SDK to
        # a different ABI. ariaConfig must shadow this hint in its own scope.
        file(WRITE "${TEST_ROOT}/stale-openssl/OpenSSLConfig.cmake"
            "message(FATAL_ERROR \"Stale OpenSSL_DIR overrode the bundled SDK\")\n")
        list(APPEND dependency_args "-DOpenSSL_DIR=${TEST_ROOT}/stale-openssl")
    endif()
    checked("${CMAKE_COMMAND}" -S "${ARIA_SOURCE_DIR}/tests/package_${consumer}"
        -B "${TEST_ROOT}/${consumer}" -G "${TEST_GENERATOR}"
        "-DCMAKE_BUILD_TYPE=${TEST_CONFIG}" ${toolchain_args}
        "-DCMAKE_PREFIX_PATH=${prefix}" "-DARIA_PACKAGE_EXPECT_TLS=${TEST_TLS}" ${dependency_args})
    checked("${CMAKE_COMMAND}" --build "${TEST_ROOT}/${consumer}" --config "${TEST_CONFIG}")
    if(WIN32)
        set(runtime_path "${prefix}/bin;$ENV{PATH}")
    else()
        set(runtime_path "${prefix}/bin:$ENV{PATH}")
    endif()
    # The Windows loader needs the SDK DLL directory. Unix consumers resolve
    # the relocated libraries through the installed target's runtime paths.
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env "PATH=${runtime_path}"
        "${CMAKE_CTEST_COMMAND}" --test-dir "${TEST_ROOT}/${consumer}"
        -C "${TEST_CONFIG}" --output-on-failure --no-tests=error --timeout 30
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${consumer} SDK runtime failed (${result})\n${output}${error}")
    endif()
endforeach()
