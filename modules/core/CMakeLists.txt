add_library(aria_core INTERFACE)
add_library(aria::core ALIAS aria_core)

target_include_directories(aria_core INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(aria_core INTERFACE cxx_std_20)
target_link_libraries(aria_core INTERFACE aria::abi)

if(ARIA_BUILD_TESTS)
    add_subdirectory(tests)
    add_subdirectory(fuzz)
endif()
