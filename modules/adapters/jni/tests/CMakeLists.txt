# JNI adapter tests.
#
# Two tiers, split by what each can actually verify:
#
#  1. `test_jni_contract` — compile-time interface contract, NDK-only.
#     Constructing a JniView needs a live JNIEnv*/jobject, which exist
#     only inside a running ART VM, so the shared `adapter_conformance`
#     battery cannot run on the host. What we pin instead is that
#     JniAdapter implements the full IViewAdapter surface (i.e. is
#     non-abstract) and JniView is an IView — static_asserts that break
#     the build if the adapter drifts. Behavioural conformance lives in
#     an on-device instrumentation test on the Android SDK side.
#
#  2. `test_jni_list_source` — behavioural, and deliberately NOT
#     NDK-only. `JniListSource<T>` carries the snapshot and the diffing
#     with no <jni.h> dependency, so its row arithmetic is executable on
#     any host. That target is registered by
#     `modules/core/tests/CMakeLists.txt` (which is not gated on
#     ANDROID) so the bridge is covered by the ordinary `ctest` run on
#     every platform; see the comment there. Only the JNI half
#     (`JniRecyclerNotifier`, which calls notifyItem* through a real VM)
#     is device-bound.
#
# Guarded by `if(NOT ANDROID)` so a host build that flips ARIA_BUILD_TESTS
# on without an NDK toolchain simply skips the NDK-only tier.

if(NOT ANDROID)
    return()
endif()

add_executable(test_jni_contract
    test_jni_contract.cpp
)

target_link_libraries(test_jni_contract PRIVATE
    aria::adapters::jni
    aria::core
    aria::binding
    doctest::doctest
)

add_test(NAME jni_contract COMMAND test_jni_contract)
