add_executable(test_app test_main.cpp test_app.cpp)
target_link_libraries(test_app PRIVATE aria::app doctest::doctest aria::warnings)
add_test(NAME app_tests COMMAND test_app)
