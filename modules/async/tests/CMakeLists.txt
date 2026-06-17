add_executable(test_async
    test_main.cpp
    test_task.cpp
    test_executor.cpp
    test_when_all.cpp
    test_race_slot_publish.cpp
    test_async_command.cpp
    test_async_command_result.cpp
    test_async_command_builder.cpp
    test_safe_run.cpp
    test_generator.cpp
    test_channel.cpp
    test_virtual_time.cpp
    test_timeout.cpp
    test_async_resource.cpp
    test_async_validator.cpp
    test_async_diagnostics.cpp
    test_scope.cpp
)
target_include_directories(test_async PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(test_async PRIVATE
    aria::async
    aria::core
    aria::runtime
    doctest::doctest
    aria::warnings
)
add_test(NAME async_tests COMMAND test_async)
