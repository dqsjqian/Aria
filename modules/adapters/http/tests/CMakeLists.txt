add_executable(test_http_adapter test_http_adapter.cpp)
target_link_libraries(test_http_adapter
    PRIVATE aria::http aria::binding aria::core aria::warnings doctest::doctest)
target_compile_features(test_http_adapter PRIVATE cxx_std_20)
add_test(NAME http_adapter COMMAND test_http_adapter)
