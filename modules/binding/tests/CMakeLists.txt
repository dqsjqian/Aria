add_executable(test_binding
    test_main.cpp
    test_binding_engine.cpp
    test_binding_readonly_source.cpp
    test_binding_dispatcher.cpp
    test_view_model.cpp
    test_view_model_scope.cpp
    test_navigation.cpp
    test_form.cpp
    test_fake_conformance.cpp
    test_view_adapter_base.cpp
    test_binding_diagnostics.cpp
    test_numeric_saturate.cpp
)
target_link_libraries(test_binding PRIVATE
    aria::binding
    aria::runtime
    doctest::doctest
    aria::warnings
)
add_test(NAME binding_tests COMMAND test_binding)
