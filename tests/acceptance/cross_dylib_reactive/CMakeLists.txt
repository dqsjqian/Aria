# Host and plugin use the same C++ toolchain and standard-library ABI.
# Neither links aria::runtime: the core target supplies its complete foundation.
add_library(aria_test_reactive_plugin SHARED plugin.cpp)
target_compile_definitions(aria_test_reactive_plugin PRIVATE ARIA_TEST_BRIDGE_BUILD)
target_link_libraries(aria_test_reactive_plugin PRIVATE aria::core aria::warnings)

add_executable(test_cross_dylib_reactive host.cpp)
target_link_libraries(test_cross_dylib_reactive PRIVATE
    aria::core aria::warnings aria_test_reactive_plugin)
add_test(NAME cross_dylib_reactive COMMAND test_cross_dylib_reactive)
