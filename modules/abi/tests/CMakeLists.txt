add_executable(test_abi
    test_signal.cpp
)
target_link_libraries(test_abi PRIVATE aria::abi doctest::doctest aria::warnings)
add_test(NAME abi_tests COMMAND test_abi)
