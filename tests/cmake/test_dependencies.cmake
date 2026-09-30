cmake_minimum_required(VERSION 3.20)

# Offline fixtures: never consult GitHub while testing selection semantics.
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/framework/cmake" "${TEST_ROOT}/framework/scripts")
foreach(helper ariaDependencies ariaFindQt ariaFetchPinned)
    configure_file("${ARIA_SOURCE_DIR}/cmake/${helper}.cmake"
        "${TEST_ROOT}/framework/cmake/${helper}.cmake" COPYONLY)
endforeach()
set(spec [[{"provider":"github","repo":"example/library","artifact":"file","path":"include.h","tag_prefix":"v"}]])
set(manifest "{\"schema\":1,\"dependencies\":{\"json\":${spec}}}")
set(entry "{\"source\":${spec},\"requested\":\"latest\",\"version\":\"1.0.0\",\"tag\":\"v1.0.0\",\"revision\":\"1111111111111111111111111111111111111111\",\"url\":\"https://invalid.example/include.h\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}")
set(lock "{\"schema\":1,\"dependencies\":{\"json\":${entry}}}")
file(WRITE "${TEST_ROOT}/framework/dependencies.json" "${manifest}")
file(WRITE "${TEST_ROOT}/framework/dependencies.lock.json" "${lock}")

function(run_case name body expected_error)
    file(MAKE_DIRECTORY "${TEST_ROOT}/${name}")
    file(WRITE "${TEST_ROOT}/${name}/case.cmake"
        "cmake_minimum_required(VERSION 3.20)\n"
        "include(\"${TEST_ROOT}/framework/cmake/ariaDependencies.cmake\")\n${body}\n")
    execute_process(COMMAND "${CMAKE_COMMAND}" -P "${TEST_ROOT}/${name}/case.cmake"
        WORKING_DIRECTORY "${TEST_ROOT}/${name}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(expected_error STREQUAL "")
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "${name} unexpectedly failed:\n${output}${error}")
        endif()
    elseif(result EQUAL 0 OR NOT "${output}${error}" MATCHES "${expected_error}")
        message(FATAL_ERROR "${name} should fail with '${expected_error}':\n${output}${error}")
    endif()
endfunction()

run_case(locked_without_python [[
set(CMAKE_DISABLE_FIND_PACKAGE_Python3 TRUE)
set(ARIA_DEPENDENCIES_OFFLINE ON)
aria_resolve_dependency(NAME JSON VERSION version URL url SHA256 hash REVISION revision TAG tag)
if(NOT version STREQUAL "1.0.0" OR NOT tag STREQUAL "v1.0.0")
    message(FATAL_ERROR "Wrong locked version")
endif()
]] "")

# A CLI-selected version remains locked even after the one-shot selector is
# no longer supplied. An explicit matching version/tag also needs no Python.
string(JSON selected_entry SET "${entry}" requested "\"1.0.0\"")
file(WRITE "${TEST_ROOT}/selected.lock.json" "{\"schema\":1,\"dependencies\":{\"json\":${selected_entry}}}")
run_case(retain_cli_selection "
set(CMAKE_DISABLE_FIND_PACKAGE_Python3 TRUE)
aria_resolve_dependency(NAME json LOCK \"${TEST_ROOT}/selected.lock.json\" VERSION version)
set(ARIA_DEP_JSON_VERSION v1.0.0)
aria_resolve_dependency(NAME json VERSION version)
if(NOT version STREQUAL \"1.0.0\")
    message(FATAL_ERROR \"Stored or explicit selection was lost\")
endif()
" "")

# An explicit CMake version wins over the manifest's explicit version too.
string(JSON explicit_manifest SET "${manifest}" dependencies json version "\"2.0.0\"")
string(JSON explicit_spec GET "${explicit_manifest}" dependencies json)
string(JSON explicit_entry SET "${entry}" source "${explicit_spec}")
string(JSON explicit_entry SET "${explicit_entry}" requested "\"1.0.0\"")
file(WRITE "${TEST_ROOT}/explicit.json" "${explicit_manifest}")
file(WRITE "${TEST_ROOT}/explicit.lock.json" "{\"schema\":1,\"dependencies\":{\"json\":${explicit_entry}}}")
run_case(explicit_priority "
set(CMAKE_DISABLE_FIND_PACKAGE_Python3 TRUE)
set(ARIA_DEPENDENCY_MANIFEST \"${TEST_ROOT}/explicit.json\")
set(ARIA_DEPENDENCY_LOCK \"${TEST_ROOT}/explicit.lock.json\")
set(ARIA_DEP_JSON_VERSION 1.0.0)
aria_resolve_dependency(NAME json VERSION version)
if(NOT version STREQUAL \"1.0.0\")
    message(FATAL_ERROR \"Explicit version lost precedence\")
endif()
" "")

string(JSON bad_lock SET "${lock}" dependencies json sha256 "\"bad\"")
file(WRITE "${TEST_ROOT}/bad.lock.json" "${bad_lock}")
run_case(invalid_hash "aria_resolve_dependency(NAME json LOCK \"${TEST_ROOT}/bad.lock.json\" VERSION version)" "sha256 must be a SHA256")
string(JSON bad_lock SET "${lock}" dependencies json revision "\"1111111\"")
file(WRITE "${TEST_ROOT}/bad-revision.lock.json" "${bad_lock}")
run_case(invalid_revision "aria_resolve_dependency(NAME json LOCK \"${TEST_ROOT}/bad-revision.lock.json\" VERSION version)" "revision must be a full commit SHA")
string(JSON bad_lock SET "${lock}" dependencies json url "\"file:///local/file\"")
file(WRITE "${TEST_ROOT}/bad-url.lock.json" "${bad_lock}")
run_case(invalid_url "aria_resolve_dependency(NAME json LOCK \"${TEST_ROOT}/bad-url.lock.json\" VERSION version)" "url must use HTTPS without credentials")

run_case(source_override "
include(\"${TEST_ROOT}/framework/cmake/ariaFetchPinned.cmake\")
set(CMAKE_DISABLE_FIND_PACKAGE_Python3 TRUE)
set(ARIA_PIN_PATCHED_SOURCE_DIR \"${TEST_ROOT}/framework\")
aria_fetch_pinned_archive(NAME patched)
if(NOT ARIA_PINNED_PATCHED_SOURCE_DIR STREQUAL \"${TEST_ROOT}/framework\")
    message(FATAL_ERROR \"Source override was lost\")
endif()
" "")
run_case(offline_download [[
include("../framework/cmake/ariaFetchPinned.cmake")
set(ARIA_DEPENDENCIES_OFFLINE ON)
aria_fetch_pinned_file(NAME json AS include.h)
]] "offline mode requires cached file")

# Substitute a local resolver fixture, preserving the real CLI boundary.
# It refuses missing flags, records invocations, and performs no network IO.
find_package(Python3 3.10 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
file(WRITE "${TEST_ROOT}/framework/scripts/dependencies.py" [=[
import argparse
import json
from pathlib import Path
p = argparse.ArgumentParser()
p.add_argument("command", choices=["resolve"])
p.add_argument("--manifest", required=True)
p.add_argument("--lock", required=True)
p.add_argument("--base-lock")
p.add_argument("--cache-dir", required=True)
p.add_argument("--only", required=True)
p.add_argument("--version")
p.add_argument("--offline", action="store_true")
a = p.parse_args()
if a.offline:
    raise SystemExit("offline fixture: no matching lock")
manifest = json.loads(Path(a.manifest).read_text())
base = json.loads(Path(a.base_lock).read_text()) if a.base_lock else {"schema": 1, "dependencies": {}}
name = a.only
requested = a.version.split("=", 1)[1] if a.version else manifest["dependencies"][name].get("version", "latest")
version = "9.0.0" if requested == "latest" else requested
base["dependencies"][name] = dict(source=manifest["dependencies"][name], requested=requested, version=version, tag="v" + version, revision="1" * 40, url="https://invalid.example/include.h", sha256="a" * 64)
Path(a.lock).write_text(json.dumps(base))
with (Path(a.manifest).parent / "calls").open("a") as calls:
    calls.write(requested + "\n")
]=])
run_case(effective_override [[
set(ARIA_DEP_JSON_VERSION 2.0.0)
aria_resolve_dependency(NAME json VERSION version)
if(NOT version STREQUAL "2.0.0")
    message(FATAL_ERROR "Override not resolved")
endif()
set(CMAKE_DISABLE_FIND_PACKAGE_Python3 TRUE)
aria_resolve_dependency(NAME json VERSION repeated)
if(NOT repeated STREQUAL version)
    message(FATAL_ERROR "Effective lock not reused")
endif()
set(ARIA_DEP_JSON_VERSION "")
aria_resolve_dependency(NAME json VERSION restored)
if(NOT restored STREQUAL "1.0.0")
    message(FATAL_ERROR "Clearing override did not restore base lock")
endif()
]] "")
run_case(effective_override [[
set(CMAKE_DISABLE_FIND_PACKAGE_Python3 TRUE)
set(ARIA_DEP_JSON_VERSION 2.0.0)
aria_resolve_dependency(NAME json VERSION version)
if(NOT version STREQUAL "2.0.0")
    message(FATAL_ERROR "Effective lock not reused across configure processes")
endif()
]] "")
file(READ "${TEST_ROOT}/framework/calls" calls)
if(NOT calls STREQUAL "2.0.0\n")
    message(FATAL_ERROR "Repeated configure unexpectedly invoked resolver: ${calls}")
endif()
file(READ "${TEST_ROOT}/framework/dependencies.lock.json" unchanged)
if(NOT unchanged STREQUAL lock)
    message(FATAL_ERROR "CMake modified the checked-in lock")
endif()
run_case(first_resolution "
aria_resolve_dependency(NAME json LOCK \"${TEST_ROOT}/missing.lock.json\" VERSION version)
if(NOT version STREQUAL \"9.0.0\")
    message(FATAL_ERROR \"Missing lock did not select latest fixture release\")
endif()
set(CMAKE_DISABLE_FIND_PACKAGE_Python3 TRUE)
aria_resolve_dependency(NAME json LOCK \"${TEST_ROOT}/missing.lock.json\" VERSION repeated)
" "")
file(WRITE "${TEST_ROOT}/changing-base.lock.json" "${lock}")
run_case(base_changes "
set(ARIA_DEP_JSON_VERSION 2.0.0)
aria_resolve_dependency(NAME json LOCK \"${TEST_ROOT}/changing-base.lock.json\" VERSION version)
set(ARIA_DEP_JSON_VERSION \"\")
file(REMOVE \"${TEST_ROOT}/changing-base.lock.json\")
aria_resolve_dependency(NAME json LOCK \"${TEST_ROOT}/changing-base.lock.json\" VERSION version)
if(NOT version STREQUAL \"9.0.0\")
    message(FATAL_ERROR \"Deleting base lock retained the old override\")
endif()
file(READ \"${TEST_ROOT}/framework/dependencies.lock.json\" base)
string(JSON base SET \"\${base}\" dependencies json version \"\\\"3.0.0\\\"\")
string(JSON base SET \"\${base}\" dependencies json tag \"\\\"v3.0.0\\\"\")
file(WRITE \"${TEST_ROOT}/changing-base.lock.json\" \"\${base}\")
aria_resolve_dependency(NAME json LOCK \"${TEST_ROOT}/changing-base.lock.json\" VERSION version)
if(NOT version STREQUAL \"3.0.0\")
    message(FATAL_ERROR \"Effective lock shadowed updated base lock\")
endif()
file(REMOVE \"${TEST_ROOT}/changing-base.lock.json\")
aria_resolve_dependency(NAME json LOCK \"${TEST_ROOT}/changing-base.lock.json\" VERSION version)
" "")
file(READ "${TEST_ROOT}/framework/calls" calls)
if(NOT calls STREQUAL "2.0.0\nlatest\n2.0.0\nlatest\nlatest\n")
    message(FATAL_ERROR "Source lock update/deletion resurrected stale effective metadata: ${calls}")
endif()
run_case(offline_missing_lock [[
set(ARIA_DEP_JSON_VERSION 8.0.0)
set(ARIA_DEPENDENCIES_OFFLINE ON)
aria_resolve_dependency(NAME json VERSION version)
]] "offline fixture: no matching lock")
else()
    message(STATUS "Python unavailable: skipping resolver subprocess fixtures (locked CMake paths were tested)")
endif()
run_case(unresolved_without_python [[
set(CMAKE_DISABLE_FIND_PACKAGE_Python3 TRUE)
set(ARIA_DEP_JSON_VERSION 8.0.0)
aria_resolve_dependency(NAME json VERSION version)
]] "requires Python 3.10")

# Two installed SDK candidates under the same search prefix. Natural sorting
# must choose 6.10 over 6.9; an exact request must still choose the older one.
foreach(version 6.9.0 6.10.0)
    set(directory "${TEST_ROOT}/qt/Qt6-${version}/lib/cmake/Qt6")
    file(MAKE_DIRECTORY "${directory}")
    file(WRITE "${directory}/Qt6Config.cmake" "set(Qt6_VERSION \"${version}\")\n")
    file(WRITE "${directory}/Qt6ConfigVersion.cmake"
        "set(PACKAGE_VERSION \"${version}\")\n"
        "if(PACKAGE_FIND_VERSION VERSION_EQUAL PACKAGE_VERSION)\n"
        "  set(PACKAGE_VERSION_EXACT TRUE)\n"
        "endif()\n"
        "if(NOT PACKAGE_FIND_VERSION VERSION_GREATER PACKAGE_VERSION)\n"
        "  set(PACKAGE_VERSION_COMPATIBLE TRUE)\n"
        "endif()\n")
endforeach()
run_case(qt_latest "
set(CMAKE_PREFIX_PATH \"${TEST_ROOT}/qt\")
set(CMAKE_FIND_PACKAGE_SORT_ORDER NAME)
set(CMAKE_FIND_PACKAGE_SORT_DIRECTION ASC)
aria_find_qt6(REQUIRED CONFIG NO_DEFAULT_PATH PATHS \"${TEST_ROOT}/qt\")
if(NOT Qt6_VERSION STREQUAL \"6.10.0\")
    message(FATAL_ERROR \"Newest visible Qt was not selected: \${Qt6_VERSION}\")
endif()
if(NOT CMAKE_FIND_PACKAGE_SORT_ORDER STREQUAL \"NAME\" OR NOT CMAKE_FIND_PACKAGE_SORT_DIRECTION STREQUAL \"ASC\")
    message(FATAL_ERROR \"Qt helper changed parent package search settings\")
endif()
" "")
run_case(qt_exact "
set(ARIA_DEP_QT_VERSION 6.9.0)
aria_find_qt6(REQUIRED CONFIG NO_DEFAULT_PATH PATHS \"${TEST_ROOT}/qt\")
if(NOT Qt6_VERSION STREQUAL \"6.9.0\")
    message(FATAL_ERROR \"Exact Qt version was not selected\")
endif()
if(DEFINED CMAKE_FIND_PACKAGE_SORT_ORDER OR DEFINED CMAKE_FIND_PACKAGE_SORT_DIRECTION)
    message(FATAL_ERROR \"Qt helper left new search settings in the caller\")
endif()
" "")
run_case(qt_missing_exact "
set(ARIA_DEP_QT_VERSION 6.8.0)
aria_find_qt6(REQUIRED CONFIG NO_DEFAULT_PATH PATHS \"${TEST_ROOT}/qt\")
" "6.8.0")
message(STATUS "Dependency lock/override/offline/source override/Qt fixtures passed")
