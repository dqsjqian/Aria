# Install only material from the sources actually selected by this build.
# Missing material is a distribution error, not a configure/build error.
set(ARIA_LICENSE_INSTALL_DIR "${CMAKE_INSTALL_DATADIR}/licenses/aria")
set(ARIA_LICENSE_CHECK_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/aria-check-licenses.cmake")
set(ARIA_LICENSE_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/aria-license-files.cmake")
file(WRITE "${ARIA_LICENSE_CHECK_SCRIPT}" "cmake_minimum_required(VERSION 3.20)\n")
file(WRITE "${ARIA_LICENSE_MANIFEST}" "set(aria_license_sources)\nset(aria_license_destinations)\n")
install(SCRIPT "${ARIA_LICENSE_CHECK_SCRIPT}")

function(_aria_install_license source destination diagnostic)
    file(APPEND "${ARIA_LICENSE_CHECK_SCRIPT}"
        "if(NOT EXISTS [==[${source}]==] OR IS_DIRECTORY [==[${source}]==])\n"
        "  message(FATAL_ERROR [==[Missing distribution license: ${diagnostic}]==])\nendif()\n")
    if(NOT source STREQUAL "")
        install(FILES "${source}" DESTINATION "${destination}")
        get_filename_component(name "${source}" NAME)
        file(APPEND "${ARIA_LICENSE_MANIFEST}"
            "list(APPEND aria_license_sources [==[${source}]==])\n"
            "list(APPEND aria_license_destinations [==[${destination}/${name}]==])\n")
    endif()
endfunction()

function(_aria_install_dependency_license name source_dir license_name)
    string(TOUPPER "${name}" upper)
    set(ARIA_${upper}_LICENSE_FILE "" CACHE FILEPATH
        "Matching license for an externally supplied ${name} dependency")
    set(ARIA_${upper}_NOTICE_FILES "" CACHE STRING
        "Additional matching notices for the selected ${name} dependency")
    set(license "${ARIA_${upper}_LICENSE_FILE}")
    if(NOT license AND source_dir)
        set(license "${source_dir}/${license_name}")
    endif()
    _aria_install_license("${license}" "${ARIA_LICENSE_INSTALL_DIR}/${name}"
        "${name}; provide its actual source license or ARIA_${upper}_LICENSE_FILE before installing/packaging")
    set(notices ${ARIA_${upper}_NOTICE_FILES})
    if(source_dir)
        file(GLOB source_notices CONFIGURE_DEPENDS "${source_dir}/NOTICE*")
        foreach(notice IN LISTS source_notices)
            if(NOT IS_DIRECTORY "${notice}")
                list(APPEND notices "${notice}")
            endif()
        endforeach()
    endif()
    list(REMOVE_DUPLICATES notices)
    foreach(notice IN LISTS notices)
        _aria_install_license("${notice}" "${ARIA_LICENSE_INSTALL_DIR}/${name}"
            "${name} notice ${notice}")
    endforeach()
endfunction()

_aria_install_license("${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    "${ARIA_LICENSE_INSTALL_DIR}" "Aria LICENSE")
_aria_install_license("${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
    "${ARIA_LICENSE_INSTALL_DIR}" "Aria THIRD_PARTY_NOTICES.md")
if(ARIA_BUILD_HTTP)
    _aria_install_dependency_license(json "${_aria_json_license_source}" LICENSE.MIT)
    if(_aria_json_license_source)
        # JSON embeds several independently credited MIT implementations.
        file(GLOB_RECURSE json_headers CONFIGURE_DEPENDS
            "${_aria_json_license_source}/include/nlohmann/*.hpp")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${json_headers})
        set(json_attributions "")
        foreach(header IN LISTS json_headers)
            file(STRINGS "${header}" notices ENCODING UTF-8
                REGEX "SPDX-(FileCopyrightText|License-Identifier):")
            list(APPEND json_attributions ${notices})
        endforeach()
        list(REMOVE_DUPLICATES json_attributions)
        list(SORT json_attributions)
        string(JOIN "\n" json_attributions ${json_attributions})
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/json-ATTRIBUTIONS.txt" "${json_attributions}\n")
        _aria_install_license("${CMAKE_CURRENT_BINARY_DIR}/json-ATTRIBUTIONS.txt"
            "${ARIA_LICENSE_INSTALL_DIR}/json" "selected JSON header attributions")
    endif()
    _aria_install_dependency_license(mira "${_aria_mira_license_source}" LICENSE)
    if(ARIA_HTTP_ENABLE_TLS)
        set(_aria_openssl_license_source "")
        if(ARIA_BUNDLED_OPENSSL)
            set(_aria_openssl_license_source "${OPENSSL_SOURCE_DIR}")
        endif()
        _aria_install_dependency_license(openssl "${_aria_openssl_license_source}" LICENSE.txt)
    endif()
endif()
