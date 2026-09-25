add_executable(test_http_adapter test_http_adapter.cpp test_http_wire.cpp)
target_link_libraries(test_http_adapter
    PRIVATE aria::http aria::binding aria::core aria::warnings doctest::doctest)
target_compile_features(test_http_adapter PRIVATE cxx_std_20)
add_test(NAME http_adapter COMMAND test_http_adapter)

target_link_libraries(test_http_adapter PRIVATE nlohmann_json::nlohmann_json)
set_tests_properties(http_adapter PROPERTIES TIMEOUT 45)

add_executable(http_guide_example http_guide_example.cpp)
target_link_libraries(http_guide_example PRIVATE aria::http aria::runtime aria::warnings)
add_test(NAME http_guide_example COMMAND http_guide_example)
set_tests_properties(http_guide_example PROPERTIES TIMEOUT 10)

find_program(ARIA_NODE_EXECUTABLE NAMES node nodejs)
if(ARIA_NODE_EXECUTABLE)
    add_test(NAME http_web_sdk COMMAND ${ARIA_NODE_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/../web-sdk/test_aria_client.mjs)
endif()
