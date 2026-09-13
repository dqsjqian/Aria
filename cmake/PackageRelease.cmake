# cmake/PackageRelease.cmake
# ── Release packaging for aria ────────────────────────────────────────────
#
# Usage:
#   cmake --build build --target package-release
#
# This creates a self-contained release/ directory with everything an external
# consumer needs to link against aria (headers + libs + cmake config).
#
# The release directory layout:
#   release/
#   ├── include/aria/...     # public headers
#   ├── lib/                    # static libs + import libs
#   ├── bin/                    # DLLs (Windows)
#   ├── lib/cmake/aria/      # CMake package config
#   ├── LICENSE
#   ├── README.md
#   └── CHANGELOG.md

# Output directory for the packaged release tree.
#
# Defaults to <repo>/build/dist/tree/ under a dedicated build/dist/
# namespace, so it never collides with any flavor's CMake build dir. The
# archive target
# below puts .tar.gz / .zip into build/dist/archives/.
get_filename_component(ARIA_REPO_ROOT_DIR "${PROJECT_SOURCE_DIR}" ABSOLUTE)
set(ARIA_RELEASE_DIR "${ARIA_REPO_ROOT_DIR}/build/dist/tree"
    CACHE PATH "Output directory for release packaging"
)

# Packaging uses its own explicit install prefix. Ordinary `cmake --install`
# continues to respect the consumer's CMAKE_INSTALL_PREFIX.

# ── 2. Custom target: package-release ────────────────────────────────────────
add_custom_target(package-release
    DEPENDS ${ARIA_INSTALL_TARGETS}
    COMMENT "Packaging aria release to ${ARIA_RELEASE_DIR}"
)

# Always start from an empty tree. `cmake --install` updates files in place and
# never removes files that disappeared from the source package; without this,
# retired content (such as the former examples/ tree) would survive forever in
# build/dist/tree and leak into release archives.
add_custom_command(TARGET package-release PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${ARIA_RELEASE_DIR}"
    COMMENT "  - cleaning previous release tree"
)

# Install the configuration whose artifacts the target just built.
add_custom_command(TARGET package-release POST_BUILD
    COMMAND ${CMAKE_COMMAND} --install "${PROJECT_BINARY_DIR}"
            --config "$<CONFIG>" --prefix "${ARIA_RELEASE_DIR}"
    COMMENT "  - installing targets"
    VERBATIM
)

# Step 2b: copy top-level docs
foreach(doc_file LICENSE README.md README.en.md CHANGELOG.md)
    if(EXISTS "${PROJECT_SOURCE_DIR}/${doc_file}")
        add_custom_command(TARGET package-release POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PROJECT_SOURCE_DIR}/${doc_file}"
                "${ARIA_RELEASE_DIR}/${doc_file}"
            COMMENT "  - copying ${doc_file}"
        )
    endif()
endforeach()

# ── 3. Archive target (creates .zip / .tar.gz from release/) ─────────────────
set(ARIA_ARCHIVE_NAME "aria-${PROJECT_VERSION}-${ARIA_PLATFORM_NAME}")
set(ARIA_ARCHIVE_DIR "${ARIA_REPO_ROOT_DIR}/build/dist/archives")
# Note: do NOT call file(MAKE_DIRECTORY) here — that would leave an
# always-empty build/dist/archives/ even when the user never runs
# package-archive. The directory is created on demand by the
# package-archive target below (cmake -E tar / Compress-Archive both
# create their parent dir as needed).

if(WIN32)
    # CMake supplies both archive formats, without a platform shell.
    set(ARIA_ARCHIVE_EXT "zip")
    set(ARIA_ARCHIVE_PATH "${ARIA_ARCHIVE_DIR}/${ARIA_ARCHIVE_NAME}.${ARIA_ARCHIVE_EXT}")

    add_custom_target(package-archive
        COMMENT "Creating ${ARIA_ARCHIVE_NAME}.zip"
        DEPENDS package-release
    )

    add_custom_command(TARGET package-archive POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ARIA_ARCHIVE_DIR}"
        COMMAND ${CMAKE_COMMAND} -E remove -f "${ARIA_ARCHIVE_PATH}"
        COMMAND ${CMAKE_COMMAND} -E chdir "${ARIA_RELEASE_DIR}"
                ${CMAKE_COMMAND} -E tar cf "${ARIA_ARCHIVE_PATH}" --format=zip .
        COMMAND ${CMAKE_COMMAND} -E echo ""
        COMMAND ${CMAKE_COMMAND} -E echo "[OK] Archive created: ${ARIA_ARCHIVE_PATH}"
        COMMAND ${CMAKE_COMMAND} -E echo ""
    )
else()
    # Unix: use cmake tar for .tar.gz
    set(ARIA_ARCHIVE_EXT "tar.gz")
    set(ARIA_ARCHIVE_PATH "${ARIA_ARCHIVE_DIR}/${ARIA_ARCHIVE_NAME}.${ARIA_ARCHIVE_EXT}")

    add_custom_target(package-archive
        COMMENT "Creating ${ARIA_ARCHIVE_NAME}.tar.gz"
        DEPENDS package-release
    )

    add_custom_command(TARGET package-archive POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ARIA_ARCHIVE_DIR}"
        COMMAND ${CMAKE_COMMAND} -E remove -f "${ARIA_ARCHIVE_PATH}"
        COMMAND ${CMAKE_COMMAND} -E chdir "${ARIA_RELEASE_DIR}"
                ${CMAKE_COMMAND} -E tar czf "${ARIA_ARCHIVE_PATH}" .
        COMMAND ${CMAKE_COMMAND} -E echo ""
        COMMAND ${CMAKE_COMMAND} -E echo "[OK] Archive created: ${ARIA_ARCHIVE_PATH}"
        COMMAND ${CMAKE_COMMAND} -E echo ""
    )
endif()

# ── 4. Print release manifest after packaging ────────────────────────────────
add_custom_command(TARGET package-release POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "[OK] Release packaged: ${ARIA_RELEASE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "  Contents:"
    COMMAND ${CMAKE_COMMAND} -E echo "    include/    - public headers"
    COMMAND ${CMAKE_COMMAND} -E echo "    lib/        - static libraries + import libs"
    COMMAND ${CMAKE_COMMAND} -E echo "    bin/        - shared libraries, DLLs on Windows"
    COMMAND ${CMAKE_COMMAND} -E echo "    lib/cmake/  - CMake package configuration"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "  Usage in consumer CMake:"
    COMMAND ${CMAKE_COMMAND} -E echo "    find_package(aria REQUIRED PATHS /path/to/release)"
    COMMAND ${CMAKE_COMMAND} -E echo "    target_link_libraries(myapp PRIVATE aria::aria)"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    VERBATIM
)
