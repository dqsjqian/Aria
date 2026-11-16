add_executable(test_i18n test_main.cpp test_i18n.cpp)
target_link_libraries(test_i18n PRIVATE aria::i18n doctest::doctest aria::warnings)
add_test(NAME i18n_tests COMMAND test_i18n)
