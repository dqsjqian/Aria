add_executable(test_runtime
    test_main.cpp
    test_logger.cpp
    test_container.cpp
    test_event_bus.cpp
    test_dispatcher.cpp
    test_callback_boundary_integration.cpp
)
target_link_libraries(test_runtime PRIVATE
    aria::runtime
    aria::async
    doctest::doctest
    aria::warnings
)
add_test(NAME runtime_tests COMMAND test_runtime)
