add_executable(test_uikit_conformance
    test_uikit_conformance.mm
)

target_link_libraries(test_uikit_conformance PRIVATE
    aria::adapters::uikit
    aria::core
    aria::binding
    aria::async
    doctest::doctest
)

target_compile_options(test_uikit_conformance PRIVATE -fobjc-arc)

add_test(NAME uikit_conformance COMMAND test_uikit_conformance)
