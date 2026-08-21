cmake_minimum_required(VERSION 3.20)
project(my_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Pick ONE of the two integration modes below ─────────────────────────────

# Mode A — system-wide install (recommended for production):
#   $ cd aria && cmake -S . -B build && cmake --build build && sudo cmake --install build
#   $ cd templates/quickstart && cmake -S . -B build && cmake --build build
#
# find_package(aria 2.0 REQUIRED)

# Mode B — vendored as a sub-folder (good for trying it out without installing):
add_subdirectory(../.. aria
    EXCLUDE_FROM_ALL  # don't build aria's own tests and optional targets
)

add_executable(my_app main.cpp)

# Either link individual modules ...
target_link_libraries(my_app PRIVATE
    aria::core
    aria::async
    aria::runtime
)

# ... or link the convenience aggregate (only available via find_package):
# target_link_libraries(my_app PRIVATE aria::aria)
