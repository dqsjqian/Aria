# Example 4 — Web MVVM via HttpAdapter
#
# A C++ executable that hosts a ViewModel and exposes it to a browser
# over Aria's HTTP/SSE adapter.

if(NOT TARGET aria_http)
    message(STATUS "Example 4 (web-mvvm) skipped: aria::http not built "
                   "(set ARIA_BUILD_HTTP=ON to enable)")
    return()
endif()

add_executable(example_4_web_mvvm main.cpp)
target_link_libraries(example_4_web_mvvm PRIVATE
    aria::http aria::core aria::warnings)
target_compile_features(example_4_web_mvvm PRIVATE cxx_std_20)

# index.html and aria_client.js live alongside main.cpp; the run script
# passes the source dir as static_root so the server serves them
# directly. No copy step needed.
