# Minimal CPM.cmake for aria
# Provides CPMAddPackage(NAME ... GITHUB_REPOSITORY ... VERSION ...) for tiny dependencies.
# Falls back to FetchContent — keeping zero external requirement.

include_guard(GLOBAL)

include(FetchContent)

# CPMAddPackage(NAME pkg GITHUB_REPOSITORY user/repo GIT_TAG v1.0)
function(CPMAddPackage)
    set(options "")
    set(oneValueArgs NAME GITHUB_REPOSITORY GIT_TAG VERSION GIT_REPOSITORY DOWNLOAD_ONLY)
    set(multiValueArgs OPTIONS)
    cmake_parse_arguments(CPM "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT CPM_NAME)
        message(FATAL_ERROR "CPMAddPackage: NAME is required")
    endif()

    set(_repo "")
    if(CPM_GIT_REPOSITORY)
        set(_repo "${CPM_GIT_REPOSITORY}")
    elseif(CPM_GITHUB_REPOSITORY)
        set(_repo "https://github.com/${CPM_GITHUB_REPOSITORY}.git")
    else()
        message(FATAL_ERROR "CPMAddPackage(${CPM_NAME}): GIT_REPOSITORY or GITHUB_REPOSITORY required")
    endif()

    set(_tag "${CPM_GIT_TAG}")
    if(NOT _tag AND CPM_VERSION)
        set(_tag "v${CPM_VERSION}")
    endif()

    foreach(opt IN LISTS CPM_OPTIONS)
        string(REPLACE " " ";" _opt_list "${opt}")
        list(GET _opt_list 0 _opt_key)
        list(LENGTH _opt_list _len)
        if(_len GREATER 1)
            list(GET _opt_list 1 _opt_val)
        else()
            set(_opt_val ON)
        endif()
        set(${_opt_key} ${_opt_val} CACHE INTERNAL "Set by CPM")
    endforeach()

    FetchContent_Declare(
        ${CPM_NAME}
        GIT_REPOSITORY ${_repo}
        GIT_TAG ${_tag}
        GIT_SHALLOW TRUE
        UPDATE_DISCONNECTED TRUE
    )
    FetchContent_MakeAvailable(${CPM_NAME})

    string(TOLOWER ${CPM_NAME} _lower)
    set(${CPM_NAME}_SOURCE_DIR ${${_lower}_SOURCE_DIR} PARENT_SCOPE)
    set(${CPM_NAME}_BINARY_DIR ${${_lower}_BINARY_DIR} PARENT_SCOPE)
endfunction()
