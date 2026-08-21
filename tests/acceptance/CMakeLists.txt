# Cross-binary and end-to-end contract gates that do not belong to a
# user-facing example application.

if(ARIA_BUILD_SHARED)
    add_subdirectory(cross_dylib_iproperty)
endif()
