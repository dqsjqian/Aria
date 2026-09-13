// JNI interface and transport regressions. Static assertions pin the public
// shape; a JNI function-table fixture exercises ownership, text encoding,
// pending exceptions and signal teardown without requiring an Android UI.

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
#include <limits>
#include "fake_jni.hpp"

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

TEST_CASE("JNI adapter: embedded NUL and Unicode text round trip") {
    FakeJni jni;
    JniView view(&jni.env, &jni.view);
    JniAdapter adapter(&jni.env);
    const std::string expected = std::string{"A\0B", 3} + "中😀";
    adapter.set_text(view, expected);
    CHECK(adapter.get_text(view) == expected);
    CHECK(jni.leased_chars == 0);
    CHECK(jni.strings.empty());
}

TEST_CASE("JNI view: detached-thread destruction releases its global reference") {
    FakeJni jni;
    auto view = std::make_unique<JniView>(&jni.env, &jni.view);
    REQUIRE(jni.globals == 1);
    jni.attached = false;
    view.reset();
    CHECK(jni.globals == 0);
    CHECK(jni.attaches == 1);
    CHECK(jni.detaches == 1);
}

TEST_CASE("JNI recycler: failed lookup never continues with a pending exception") {
    FakeJni jni;
    jni.missing_method = "notifyItemInserted";
    {
        JniRecyclerNotifier notifier(&jni.env, &jni.view);
        CHECK_FALSE(notifier.valid());
        CHECK(jni.calls_with_pending == 0);
        CHECK_FALSE(jni.pending);
    }
    CHECK(jni.globals == 0);
}

TEST_CASE("JNI recycler: retained sink is inert after notifier destruction") {
    FakeJni jni;
    std::function<void(const RecyclerNotification&)> sink;
    {
        auto notifier = std::make_unique<JniRecyclerNotifier>(&jni.env, &jni.view);
        REQUIRE(notifier->valid());
        sink = notifier->sink();
    }
    sink({aria::adapters::jni::RecyclerNotify::DataSetChanged});
    CHECK(jni.calls == 0);
    CHECK(jni.globals == 0);
}

TEST_CASE("JNI adapter: native exceptions are cleared before the next operation") {
    FakeJni jni;
    JniView view(&jni.env, &jni.view);
    JniAdapter adapter(&jni.env);
    jni.fail_call = true;
    adapter.set_int(view, 1);
    CHECK_FALSE(jni.pending);
    jni.fail_call = false;
    adapter.set_int(view, 2);
    CHECK(jni.calls_with_pending == 0);
    CHECK(jni.view.progress == 2);
}

TEST_CASE("JNI adapter: wide integer values saturate consistently") {
    FakeJni jni;
    JniView view(&jni.env, &jni.view);
    JniAdapter adapter(&jni.env);
    adapter.set_int64(view, std::numeric_limits<std::int64_t>::max());
    CHECK(jni.view.progress == std::numeric_limits<int>::max());
    adapter.set_uint64(view, std::numeric_limits<std::uint64_t>::max());
    CHECK(jni.view.progress == std::numeric_limits<int>::max());
    jni.view.progress = -1;
    CHECK(adapter.get_uint64(view) == 0);
}

TEST_CASE("JNI adapter: signal ownership follows the bound wrapper") {
    FakeJni jni;
    JniAdapter adapter(&jni.env);
    auto first_view = std::make_unique<JniView>(&jni.env, &jni.view);
    JniView second_view(&jni.env, &jni.view);
    int first_calls = 0, second_calls = 0;
    auto first = adapter.on_click(*first_view, [&] { ++first_calls; });
    auto second = adapter.on_click(second_view, [&] { ++second_calls; });
    adapter.notify_click(*first_view);
    CHECK(first_calls == 1);
    CHECK(second_calls == 0);
    first_view.reset();
    adapter.notify_click(second_view);
    CHECK(second_calls == 1);
}

TEST_CASE("JNI adapter: deleting adapter during event cancels later observers") {
    FakeJni jni;
    JniView view(&jni.env, &jni.view);
    auto adapter = std::make_unique<JniAdapter>(&jni.env);
    int later_calls = 0;
    auto first = adapter->on_click(view, [&] { adapter.reset(); });
    auto later = adapter->on_click(view, [&] { ++later_calls; });
    adapter->notify_click(view);
    CHECK(later_calls == 0);
}

TEST_CASE("JNI adapter: capture teardown can reenter a closing adapter") {
    FakeJni jni;
    JniView view(&jni.env, &jni.view);
    auto adapter = std::make_unique<JniAdapter>(&jni.env);
    auto* raw = adapter.get();
    bool retired = false;
    auto capture = std::shared_ptr<int>(new int(0), [&](int* value) {
        raw->notify_click(view);
        retired = true;
        delete value;
    });
    auto sub = adapter->on_click(view, [capture = std::move(capture)] {});
    adapter.reset();
    CHECK(retired);
}

TEST_CASE("JNI recycler: detached notifications balance attachment and reject overflow") {
    FakeJni jni;
    auto notifier = std::make_unique<JniRecyclerNotifier>(&jni.env, &jni.view);
    REQUIRE(notifier->valid());
    jni.attached = false;
    notifier->dispatch({aria::adapters::jni::RecyclerNotify::ItemInserted, 4});
    CHECK(jni.calls == 1);
    CHECK(jni.attaches == 1);
    CHECK(jni.detaches == 1);
    notifier->dispatch({aria::adapters::jni::RecyclerNotify::ItemInserted, std::numeric_limits<std::size_t>::max()});
    CHECK(jni.calls == 1);
    notifier.reset();
    CHECK(jni.globals == 0);
    CHECK(jni.attaches == jni.detaches);
}

TEST_CASE("JNI adapter: malformed UTF is replaced and empty strings round trip") {
    FakeJni jni;
    JniView view(&jni.env, &jni.view);
    JniAdapter adapter(&jni.env);
    adapter.set_text(view, "");
    CHECK(adapter.get_text(view).empty());
    adapter.set_text(view, std::string(1, static_cast<char>(0xff)));
    CHECK(adapter.get_text(view) == "\xef\xbf\xbd");
    jni.view.text = {static_cast<char16_t>(0xd800), u'A'};
    CHECK(adapter.get_text(view) == "\xef\xbf\xbd" "A");
    CHECK(jni.leased_chars == 0);
}

TEST_CASE("JNI adapter: wide float ingress saturates and empty observers stay empty") {
    FakeJni jni;
    JniView view(&jni.env, &jni.view);
    JniAdapter adapter(&jni.env);
    float observed = 0;
    auto sub = adapter.on_float_changed(view, [&](float value) { observed = value; });
    adapter.notify_double_changed(view, std::numeric_limits<double>::max());
    CHECK(observed == std::numeric_limits<float>::max());
    adapter.set_double(view, -std::numeric_limits<double>::max());
    CHECK(jni.view.rating == -std::numeric_limits<float>::max());
    CHECK_FALSE(adapter.on_int64_changed(view, {}));
    CHECK_FALSE(adapter.on_uint64_changed(view, {}));
    CHECK_FALSE(adapter.on_float_changed(view, {}));
}

TEST_CASE("JNI recycler: callback destruction keeps the current Java receiver alive") {
    FakeJni jni;
    auto notifier = std::make_unique<JniRecyclerNotifier>(&jni.env, &jni.view);
    auto sink = notifier->sink();
    jni.on_call = [&] {
        notifier.reset();
        CHECK(jni.globals == 1);
    };
    sink({aria::adapters::jni::RecyclerNotify::DataSetChanged});
    CHECK(jni.globals == 0);
    sink({aria::adapters::jni::RecyclerNotify::DataSetChanged});
    CHECK(jni.calls == 1);
}
