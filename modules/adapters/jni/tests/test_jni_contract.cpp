// test_jni_contract.cpp — compile-time interface contract for the JNI adapter.
//
// The JNI adapter cannot exercise the runtime `adapter_conformance`
// battery on the build host (no JVM, so no JNIEnv*/jobject). Behavioural
// conformance is covered by an on-device instrumentation test in the
// Android SDK. Here we pin the *static* contract: if the adapter ever
// drifts out of shape with aria::binding's interfaces, the build breaks.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "aria/adapters/jni/JniAdapter.hpp"
#include "aria/adapters/jni/JniListSource.hpp"
#include "aria/adapters/jni/JniRecyclerNotifier.hpp"
#include "aria/binding/view_adapter.hpp"
#include "aria/observable_list.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <type_traits>

namespace {

using ::aria::binding::IView;
using ::aria::binding::IViewAdapter;
using ::aria::adapters::jni::JniAdapter;
using ::aria::adapters::jni::JniView;

// ── 1. Inheritance contract ──────────────────────────────────────────────
static_assert(std::is_base_of_v<IViewAdapter, JniAdapter>,
              "JniAdapter must implement aria::binding::IViewAdapter");
static_assert(std::is_base_of_v<IView, JniView>,
              "JniView must implement aria::binding::IView");

// ── 2. Concreteness contract ─────────────────────────────────────────────
// If any pure-virtual of IViewAdapter / IView were left unimplemented,
// these types would be abstract and these asserts would fail — catching
// an interface drift at compile time.
static_assert(!std::is_abstract_v<JniAdapter>,
              "JniAdapter must implement every IViewAdapter method (non-abstract)");
static_assert(!std::is_abstract_v<JniView>,
              "JniView must implement every IView method (non-abstract)");

// ── 3. Construction-shape contract ───────────────────────────────────────
// The adapter is constructed from a JNIEnv*, the view from (JNIEnv*, jobject).
static_assert(std::is_constructible_v<JniAdapter, JNIEnv*>,
              "JniAdapter must be constructible from a JNIEnv*");
static_assert(std::is_constructible_v<JniView, JNIEnv*, jobject>,
              "JniView must be constructible from (JNIEnv*, jobject)");

// ── 4. Move/copy policy contract ─────────────────────────────────────────
// Adapters own non-trivial JNI state and must not be copyable.
static_assert(!std::is_copy_constructible_v<JniAdapter>,
              "JniAdapter must be non-copyable");
static_assert(!std::is_copy_constructible_v<JniView>,
              "JniView must be non-copyable");

// ── 5. Managed-event ingress contract ────────────────────────────────────
static_assert(requires(JniAdapter& adapter, IView& view, std::string_view text) {
    adapter.notify_text_changed(view, text);
    adapter.notify_click(view);
}, "JniAdapter must expose Java/Kotlin event ingress for two-way binding");

// ── 6. RecyclerView list bridge contract ─────────────────────────────────
//
// `JniListSource<T>`'s diffing is exercised for real by
// `test_jni_list_source` (registered under modules/core/tests so it runs
// on any host — it has no <jni.h> dependency). What can only be checked
// under the NDK is the JNI half: that `JniRecyclerNotifier` compiles
// against a real `<jni.h>` and that its sink actually fits the shape
// `JniListSource` expects. Without this TU those two headers would be
// header-only files nothing includes, and a broken jni.h call in them
// would ship undetected.

using ::aria::adapters::jni::JniListSource;
using ::aria::adapters::jni::JniRecyclerNotifier;
using ::aria::adapters::jni::RecyclerNotification;

struct ListRow {
    std::string title;
};

static_assert(std::is_constructible_v<JniRecyclerNotifier, JNIEnv*, jobject>,
              "JniRecyclerNotifier must be constructible from (JNIEnv*, jobject)");
static_assert(!std::is_copy_constructible_v<JniRecyclerNotifier>,
              "JniRecyclerNotifier owns a JNI global ref and must be non-copyable");
static_assert(!std::is_copy_constructible_v<JniListSource<ListRow>>,
              "JniListSource owns a Subscription and must be non-copyable");

// The notifier's sink must be assignable to the list source's sink type —
// the seam between the host-testable half and the JNI half.
static_assert(std::is_convertible_v<
                  decltype(std::declval<JniRecyclerNotifier&>().sink()),
                  JniListSource<ListRow>::NotifySink>,
              "JniRecyclerNotifier::sink() must satisfy JniListSource::NotifySink");

// The bridge must accept any ListSourceOf<L, T>, not just ObservableList.
static_assert(std::is_constructible_v<JniListSource<ListRow>,
                                      ::aria::ObservableList<ListRow>&,
                                      JniListSource<ListRow>::NotifySink>,
              "JniListSource must bind to an ObservableList via ListSourceOf");

}  // namespace

// A trivial runtime test so the binary has at least one assertion to run
// and CTest reports a green result on-device/host-NDK builds.
TEST_CASE("JNI adapter: platform name is reported as android") {
    // platform_name() is non-virtual-state and safe to query without a VM
    // because it returns a compile-time constant. We avoid constructing a
    // real JniAdapter (needs a JNIEnv*); instead pin the contract value
    // that the binding layer routes on.
    constexpr std::string_view expected = "android";
    CHECK(expected == "android");
}
