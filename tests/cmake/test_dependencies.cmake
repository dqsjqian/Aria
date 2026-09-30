cmake_minimum_required(VERSION 3.20)

# Offline fixtures: never consult GitHub while testing selection semantics.
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/framework/cmake" "${TEST_ROOT}/framework/scripts")
foreach(helper ariaDependencies ariaFindQt ariaFetchPinned)
    configure_file("${ARIA_SOURCE_DIR}/cmake/${helper}.cmake"
        "${TEST_ROOT}/framework/cmake/${helper}.cmake" COPYONLY)
endforeach()
include("${TEST_ROOT}/framework/cmake/ariaDependencies.cmake")
_aria_dependency_request_hash([[{"provider":"github","repo":"example/library","artifact":"git","tag_prefix":"v"}]] hash)
if(NOT hash STREQUAL "1044731845c62e4a50b0251f6039d78e6ce1233cdcac02a2b086809fb6526f54")
    message(FATAL_ERROR "Request fingerprint differs from the Python format")
endif()
set(spec [[{"provider":"github","repo":"example/library","artifact":"file","path":"include.h","tag_prefix":"v"}]])
_aria_dependency_request_hash("${spec}" request_hash)
set(resolved "{\"request_hash\":\"${request_hash}\",\"requested\":\"latest\",\"version\":\"1.0.0\",\"tag\":\"v1.0.0\",\"revision\":\"1111111111111111111111111111111111111111\",\"url\":\"https://invalid.example/include.h\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}")
string(JSON entry SET "${spec}" resolved "${resolved}")
set(document "{\"schema\":2,\"dependencies\":{\"json\":${entry}}}")
set(unresolved "{\"schema\":2,\"dependencies\":{\"json\":${spec}}}")
file(WRITE "${TEST_ROOT}/framework/dependencies.json" "${document}")

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
    message(FATAL_ERROR "Wrong resolved version")
endif()
]] "")

# A CLI-selected version remains resolved when no declaration pins a version.
string(JSON selected SET "${document}" dependencies json resolved requested "\"1.0.0\"")
file(WRITE "${TEST_ROOT}/selected.json" "${selected}")
run_case(retain_cli_selection "
set(CMAKE_DISABLE_FIND_PACKAGE_Python3 TRUE)
aria_resolve_dependency(NAME json FILE \"${TEST_ROOT}/selected.json\" VERSION version)
set(ARIA_DEP_JSON_VERSION v1.0.0)
aria_resolve_dependency(NAME json VERSION version)
if(NOT version STREQUAL \"1.0.0\")
    message(FATAL_ERROR \"Stored or explicit selection was lost\")
endif()
" "")

# A declaration of "latest" is the default policy, so it retains a previously
# CLI-selected version just like an omitted version does.
string(JSON latest SET "${selected}" dependencies json version "\"latest\"")
string(JSON latest_spec GET "${latest}" dependencies json)
_aria_dependency_request_hash("${latest_spec}" latest_hash)
string(JSON latest SET "${latest}" dependencies json resolved request_hash "\"${latest_hash}\"")
file(WRITE "${TEST_ROOT}/latest.json" "${latest}")
run_case(declared_latest "
set(CMAKE_DISABLE_FIND_PACKAGE_Python3 TRUE)
aria_resolve_dependency(NAME json FILE \"${TEST_ROOT}/latest.json\" VERSION version)
if(NOT version STREQUAL \"1.0.0\")
    message(FATAL_ERROR \"Latest declaration discarded the stored CLI selection\")
endif()
" "")

# An explicit CMake version wins over the declaration's explicit version too.
string(JSON explicit SET "${document}" dependencies json version "\"2.0.0\"")
string(JSON explicit_spec GET "${explicit}" dependencies json)
_aria_dependency_request_hash("${explicit_spec}" explicit_hash)
string(JSON explicit SET "${explicit}" dependencies json resolved request_hash "\"${explicit_hash}\"")
string(JSON explicit SET "${explicit}" dependencies json resolved requested "\"1.0.0\"")
file(WRITE "${TEST_ROOT}/explicit.json" "${explicit}")
run_case(explicit_priority "
set(CMAKE_DISABLE_FIND_PACKAGE_Python3 TRUE)
set(ARIA_DEPENDENCIES_FILE \"${TEST_ROOT}/explicit.json\")
set(ARIA_DEP_JSON_VERSION 1.0.0)
aria_resolve_dependency(NAME json VERSION version)
if(NOT version STREQUAL \"1.0.0\")
    message(FATAL_ERROR \"Explicit version lost precedence\")
endif()
" "")

string(JSON bad SET "${document}" dependencies json resolved sha256 "\"bad\"")
file(WRITE "${TEST_ROOT}/bad.json" "${bad}")
run_case(invalid_hash "aria_resolve_dependency(NAME json FILE \"${TEST_ROOT}/bad.json\" VERSION version)" "sha256 must be a SHA256")
string(JSON bad SET "${document}" dependencies json resolved revision "\"1111111\"")
file(WRITE "${TEST_ROOT}/bad-revision.json" "${bad}")
run_case(invalid_revision "aria_resolve_dependency(NAME json FILE \"${TEST_ROOT}/bad-revision.json\" VERSION version)" "revision must be a full commit SHA")
string(JSON bad SET "${document}" dependencies json resolved url "\"file:///local/file\"")
file(WRITE "${TEST_ROOT}/bad-url.json" "${bad}")
run_case(invalid_url "aria_resolve_dependency(NAME json FILE \"${TEST_ROOT}/bad-url.json\" VERSION version)" "url must use HTTPS without credentials")
string(JSON stale SET "${document}" dependencies json path "\"changed.h\"")
file(WRITE "${TEST_ROOT}/stale.json" "${stale}")
run_case(changed_spec_without_python "
set(CMAKE_DISABLE_FIND_PACKAGE_Python3 TRUE)
aria_resolve_dependency(NAME json FILE \"${TEST_ROOT}/stale.json\" VERSION version)
" "requires Python 3.10")
string(JSON bad SET "${document}" dependencies json extra "17")
file(WRITE "${TEST_ROOT}/bad-spec.json" "${bad}")
run_case(invalid_spec "aria_resolve_dependency(NAME json FILE \"${TEST_ROOT}/bad-spec.json\" VERSION version)" "specification keys")

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
import hashlib
import json
from pathlib import Path
p = argparse.ArgumentParser()
p.add_argument("command", choices=["resolve"])
p.add_argument("--file", required=True)
p.add_argument("--output", required=True)
p.add_argument("--cache-dir", required=True)
p.add_argument("--only", required=True)
p.add_argument("--version")
p.add_argument("--offline", action="store_true")
a = p.parse_args()
if a.offline:
    raise SystemExit("offline fixture: no matching resolution")
source = Path(a.file).read_bytes()
base = json.loads(source)
name = a.only
spec = {key: value for key, value in base["dependencies"][name].items() if key != "resolved"}
preimage = "aria-dependency-request-v1\n" + "".join(f"{len(k.encode())}:{k}{len(v.encode())}:{v}" for k, v in sorted(spec.items()))
requested = a.version.split("=", 1)[1] if a.version else spec.get("version", "latest")
version = "9.0.0" if requested == "latest" else requested
base["dependencies"][name]["resolved"] = dict(request_hash=hashlib.sha256(preimage.encode()).hexdigest(), requested=requested, version=version, tag="v" + version, revision="1" * 40, url="https://invalid.example/include.h", sha256="a" * 64)
base["_base_sha256"] = hashlib.sha256(source).hexdigest()
Path(a.output).write_text(json.dumps(base))
with (Path(a.file).parent / "calls").open("a") as calls:
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
    message(FATAL_ERROR "Effective resolution not reused")
endif()
set(ARIA_DEP_JSON_VERSION "")
aria_resolve_dependency(NAME json VERSION restored)
if(NOT restored STREQUAL "1.0.0")
    message(FATAL_ERROR "Clearing override did not restore source resolution")
endif()
]] "")
run_case(effective_override [[
set(CMAKE_DISABLE_FIND_PACKAGE_Python3 TRUE)
set(ARIA_DEP_JSON_VERSION 2.0.0)
aria_resolve_dependency(NAME json VERSION version)
if(NOT version STREQUAL "2.0.0")
    message(FATAL_ERROR "Effective resolution not reused across configure processes")
endif()
]] "")
file(READ "${TEST_ROOT}/framework/calls" calls)
if(NOT calls STREQUAL "2.0.0\n")
    message(FATAL_ERROR "Repeated configure unexpectedly invoked resolver: ${calls}")
endif()
file(READ "${TEST_ROOT}/framework/dependencies.json" unchanged)
if(NOT unchanged STREQUAL document)
    message(FATAL_ERROR "CMake modified the checked-in dependency file")
endif()
file(WRITE "${TEST_ROOT}/unresolved.json" "${unresolved}")
run_case(first_resolution "
aria_resolve_dependency(NAME json FILE \"${TEST_ROOT}/unresolved.json\" VERSION version)
if(NOT version STREQUAL \"9.0.0\")
    message(FATAL_ERROR \"Missing resolved field did not select latest fixture release\")
endif()
set(CMAKE_DISABLE_FIND_PACKAGE_Python3 TRUE)
aria_resolve_dependency(NAME json FILE \"${TEST_ROOT}/unresolved.json\" VERSION repeated)
" "")
file(WRITE "${TEST_ROOT}/changing-base.json" "${document}")
run_case(base_changes "
set(ARIA_DEPENDENCIES_FILE \"${TEST_ROOT}/changing-base.json\")
set(ARIA_DEP_JSON_VERSION 2.0.0)
aria_resolve_dependency(NAME json VERSION version)
set(ARIA_DEP_JSON_VERSION \"\")
file(READ \"${TEST_ROOT}/changing-base.json\" base)
string(JSON unresolved REMOVE \"\${base}\" dependencies json resolved)
file(WRITE \"${TEST_ROOT}/changing-base.json\" \"\${unresolved}\")
aria_resolve_dependency(NAME json VERSION version)
if(NOT version STREQUAL \"9.0.0\")
    message(FATAL_ERROR \"Deleting resolved metadata retained the old override\")
endif()
string(JSON base SET \"\${base}\" dependencies json resolved version \"\\\"3.0.0\\\"\")
string(JSON base SET \"\${base}\" dependencies json resolved tag \"\\\"v3.0.0\\\"\")
file(WRITE \"${TEST_ROOT}/changing-base.json\" \"\${base}\")
aria_resolve_dependency(NAME json VERSION version)
if(NOT version STREQUAL \"3.0.0\")
    message(FATAL_ERROR \"Effective resolution shadowed updated source metadata\")
endif()
file(WRITE \"${TEST_ROOT}/changing-base.json\" \"\${unresolved}\")
aria_resolve_dependency(NAME json VERSION version)
" "")
file(READ "${TEST_ROOT}/framework/calls" calls)
if(NOT calls STREQUAL "2.0.0\n")
    message(FATAL_ERROR "A FILE override accidentally used framework defaults: ${calls}")
endif()
file(READ "${TEST_ROOT}/calls" calls)
if(NOT calls STREQUAL "latest\n2.0.0\nlatest\nlatest\n")
    message(FATAL_ERROR "Source update/deletion resurrected stale effective metadata: ${calls}")
endif()
# UTF-8 and delimiter-like characters must produce the same hash in Python
# and CMake. CRLF source bytes must also preserve the effective fingerprint.
string(JSON unicode SET "${unresolved}" dependencies json path "\"头;文件:1\\n.h\"")
string(REPLACE "\n" "\r\n" unicode "${unicode}")
file(WRITE "${TEST_ROOT}/unicode.json" "${unicode}")
run_case(unicode_request "
aria_resolve_dependency(NAME json FILE \"${TEST_ROOT}/unicode.json\" VERSION version)
set(CMAKE_DISABLE_FIND_PACKAGE_Python3 TRUE)
aria_resolve_dependency(NAME json FILE \"${TEST_ROOT}/unicode.json\" VERSION repeated)
" "")
run_case(explicit_latest "
set(ARIA_DEP_JSON_VERSION latest)
aria_resolve_dependency(NAME json FILE \"${TEST_ROOT}/latest.json\" VERSION version)
if(NOT version STREQUAL \"9.0.0\")
    message(FATAL_ERROR \"Explicit CMake latest selector did not take precedence\")
endif()
" "")
run_case(offline_missing_resolution [[
set(ARIA_DEP_JSON_VERSION 8.0.0)
set(ARIA_DEPENDENCIES_OFFLINE ON)
aria_resolve_dependency(NAME json VERSION version)
]] "offline fixture: no matching resolution")
else()
    message(STATUS "Python unavailable: skipping resolver subprocess fixtures (resolved CMake paths were tested)")
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
message(STATUS "Single-file dependency/override/offline/source override/Qt fixtures passed")
