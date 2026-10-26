# Provide OpenSSL::SSL and OpenSSL::Crypto for HTTP TLS. A parent can supply
# both targets, or an openssl_external producer plus FindOpenSSL-style paths.
# Shared packages are imported by ariaSharedDependencies.cmake.
set(ARIA_BUNDLED_OPENSSL OFF)
if(TARGET OpenSSL::SSL AND TARGET OpenSSL::Crypto)
    message(STATUS "Aria TLS: using parent OpenSSL targets")
    return()
elseif(TARGET OpenSSL::SSL OR TARGET OpenSSL::Crypto)
    message(FATAL_ERROR "Aria TLS requires both OpenSSL::SSL and OpenSSL::Crypto; the parent supplied only one")
endif()

find_package(Threads REQUIRED)

function(_aria_openssl_system_libraries)
    if(WIN32)
        set_property(TARGET OpenSSL::Crypto APPEND PROPERTY
            INTERFACE_LINK_LIBRARIES ws2_32 gdi32 advapi32 crypt32 user32)
    else()
        set_property(TARGET OpenSSL::Crypto APPEND PROPERTY
            INTERFACE_LINK_LIBRARIES Threads::Threads ${CMAKE_DL_LIBS})
    endif()
endfunction()

if(TARGET openssl_external)
    # Support the aggregate variable too, but never guess an archive's role
    # from its list position (FindOpenSSL can append platform dependencies).
    foreach(_library IN LISTS OPENSSL_LIBRARIES)
        get_filename_component(_name "${_library}" NAME)
        if(_name MATCHES "^(lib)?ssl[.]")
            set(OPENSSL_SSL_LIBRARY "${_library}")
        elseif(_name MATCHES "^(lib)?crypto[.]")
            set(OPENSSL_CRYPTO_LIBRARY "${_library}")
        endif()
    endforeach()
    if(NOT OPENSSL_INCLUDE_DIR OR NOT OPENSSL_SSL_LIBRARY OR NOT OPENSSL_CRYPTO_LIBRARY)
        message(FATAL_ERROR
            "Parent openssl_external must provide OPENSSL_INCLUDE_DIR and "
            "OPENSSL_SSL_LIBRARY/OPENSSL_CRYPTO_LIBRARY (or identifiable archives in OPENSSL_LIBRARIES)")
    endif()
    foreach(_kind SSL Crypto)
        string(TOUPPER "${_kind}" _upper)
        add_library(OpenSSL::${_kind} UNKNOWN IMPORTED GLOBAL)
        set_target_properties(OpenSSL::${_kind} PROPERTIES
            IMPORTED_LOCATION "${OPENSSL_${_upper}_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}")
        add_dependencies(OpenSSL::${_kind} openssl_external)
    endforeach()
    set_property(TARGET OpenSSL::SSL PROPERTY INTERFACE_LINK_LIBRARIES OpenSSL::Crypto)
    _aria_openssl_system_libraries()
    set(OPENSSL_FOUND TRUE)
    return()
endif()

