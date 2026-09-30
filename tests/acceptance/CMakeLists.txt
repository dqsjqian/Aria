# Cross-binary and end-to-end contract gates that do not belong to a
# user-facing example application.

if(ARIA_BUILD_SHARED)
    add_subdirectory(cross_dylib_iproperty)
    add_subdirectory(cross_dylib_reactive)
endif()

# Host-side CMake regressions do not require a target-device executable.
if(NOT CMAKE_CROSSCOMPILING)
    add_subdirectory(../cmake cmake)
endif()
