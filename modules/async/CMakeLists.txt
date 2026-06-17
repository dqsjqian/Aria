add_library(aria_async INTERFACE)
add_library(aria::async ALIAS aria_async)

target_include_directories(aria_async INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(aria_async INTERFACE cxx_std_20)
target_link_libraries(aria_async INTERFACE aria::core)

if(ARIA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
