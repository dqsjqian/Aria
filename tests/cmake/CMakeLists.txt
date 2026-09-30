add_test(NAME pinned_dependency_cache
    COMMAND ${CMAKE_COMMAND}
        "-DARIA_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/pinned-cache"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/test_fetch_pinned.cmake")
set_tests_properties(pinned_dependency_cache PROPERTIES TIMEOUT 60)

add_test(NAME dependency_selection
    COMMAND ${CMAKE_COMMAND}
        "-DARIA_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/dependency-selection"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/test_dependencies.cmake")
set_tests_properties(dependency_selection PROPERTIES TIMEOUT 60)

add_test(NAME license_packaging
    COMMAND ${CMAKE_COMMAND}
        "-DARIA_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/license-packaging"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/test_licenses.cmake")
set_tests_properties(license_packaging PROPERTIES TIMEOUT 60)

find_package(Python3 3.10 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME dependency_resolver
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/python/test_dependencies.py")
    set_tests_properties(dependency_resolver PROPERTIES TIMEOUT 60)
endif()

_aria_deps_cache_dir(_aria_test_cache)
add_test(NAME embedded_project_configure
    COMMAND ${CMAKE_COMMAND}
        "-DARIA_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/embedded-project"
        "-DTEST_GENERATOR=${CMAKE_GENERATOR}"
        "-DTEST_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
        "-DTEST_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
        "-DTEST_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DTEST_DEPS_CACHE=${_aria_test_cache}"
        "-DTEST_HTTP=${ARIA_BUILD_HTTP}"
        "-DTEST_TLS=${ARIA_HTTP_ENABLE_TLS}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/test_embedded_project.cmake")
set_tests_properties(embedded_project_configure PROPERTIES TIMEOUT 120)
unset(_aria_test_cache)

if(PROJECT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    get_target_property(_aria_sanitizer_link_options aria_warnings INTERFACE_LINK_OPTIONS)
    if(NOT _aria_sanitizer_link_options)
        set(_aria_sanitizer_link_options "")
    endif()
    add_test(NAME installed_sdk
        COMMAND ${CMAKE_COMMAND}
            "-DARIA_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
            "-DARIA_BINARY_DIR=${PROJECT_BINARY_DIR}"
            "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/installed-sdk"
            "-DTEST_CONFIG=$<CONFIG>"
            "-DTEST_GENERATOR=${CMAKE_GENERATOR}"
            "-DTEST_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
            "-DTEST_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
            "-DTEST_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DTEST_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}"
            "-DTEST_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}"
            "-DTEST_MSVC_RUNTIME_LIBRARY=${CMAKE_MSVC_RUNTIME_LIBRARY}"
            "-DTEST_SANITIZER_LINK_OPTIONS=${_aria_sanitizer_link_options}"
            "-DTEST_HTTP=${ARIA_BUILD_HTTP}"
            "-DTEST_TLS=${ARIA_HTTP_ENABLE_TLS}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/test_package.cmake")
    set_tests_properties(installed_sdk PROPERTIES TIMEOUT 120)
    unset(_aria_sanitizer_link_options)
endif()
