# JNI adapter tests.
#
# Unlike the Qt6/AppKit/UIKit adapters, the JNI adapter cannot run the
# shared `adapter_conformance` battery on the build host: constructing a
# JniView requires a live JNIEnv* / jobject, which only exist inside a
# running Dalvik/ART VM. Full behavioural conformance therefore lives in
# an on-device instrumentation test (Android SDK side), not here.
#
# What we CAN verify on the host (under the NDK toolchain) is the
# compile-time interface contract: that JniAdapter implements the full
# IViewAdapter surface (i.e. it is non-abstract) and that JniView is an
# IView. These are static_asserts that fail the build if the adapter
# drifts out of conformance with the binding interface.
#
# Guarded by `if(NOT ANDROID)` so a host build that flips ARIA_BUILD_TESTS
# on without an NDK toolchain simply skips this subdirectory.

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
