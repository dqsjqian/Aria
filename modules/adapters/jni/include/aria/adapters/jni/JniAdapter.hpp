#pragma once

/// JniAdapter — Android JNI implementation of aria::binding::IViewAdapter.
///
/// JNI (Java Native Interface) is the *only* bridge between the Aria C++
/// core and the Android Java/Kotlin UI layer, so this single adapter *is*
/// the Android adapter. It is named after the bridging technology (JNI)
/// to match the project's convention — APPKIT (macOS), UIKIT (iOS), QT6,
/// HTTP (Web) — all named after framework/tech rather than platform.
///
/// Provides two-way binding for:
///   * `TextView` / `EditText`              — text two-way binding
///   * `CheckBox` / `Switch` / `CompoundButton` — bool two-way binding
///   * `SeekBar` / `ProgressBar`            — int two-way binding
///   * `Button` / `ImageButton`             — click events
///   * `View`                               — visibility / enabled state
///
/// The adapter uses JNI to talk to the Java/Kotlin layer. The companion
/// Kotlin/Java classes ship in the Aria Android SDK library.
///
/// Lifetime
/// --------
///   * `JniView` holds a JNI *global* reference (`NewGlobalRef`) to its
///     `jobject`, so the Java-side view survives as long as the C++
///     wrapper does. `~JniView()` releases it via `DeleteGlobalRef`.
///   * `~JniView()` calls `fire_destroy_()` while the native handle is
///     still valid — this lets `BindingEngine` drop the per-view
///     subscription bucket before storage is reclaimed.
///   * Every subscription returned from on_*_changed / on_click detaches
///     its signal slot when released.
///
/// Threading
/// ---------
///   Android UI widgets live on the main (Looper) thread; the adapter
///   assumes its methods run there. Cross-thread Property writes must be
///   marshalled through a Dispatcher before reaching the adapter.
///
/// This header requires `<jni.h>` — it is only compilable with the
/// Android NDK toolchain.

#include "aria/abi/export.hpp"
#include "aria/binding/view_adapter.hpp"
#include "aria/subscription.hpp"

#include <jni.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace aria::adapters::jni {

// ─── View wrapper ───────────────────────────────────────────────────────

/// Wraps an Android View (`jobject`) as an IView.
/// The jobject is stored as a JNI global reference to prevent GC.
class ARIA_JNI_API JniView : public ::aria::binding::IView {
public:
    /// Construct from a JNI jobject (Android View). Creates a global
    /// reference to prevent garbage collection.
    JniView(JNIEnv* env, jobject view);

    ~JniView() override;

    JniView(const JniView&) = delete;
    JniView& operator=(const JniView&) = delete;

    /// Reported as "android" — this is the runtime platform name, used
    /// by BindingEngine for adapter routing (the *adapter* is "jni", the
    /// *platform* is "android").
    [[nodiscard]] std::string_view kind() const noexcept override { return "android"; }

    /// Access the underlying JNI global-ref jobject.
    [[nodiscard]] jobject native() const noexcept { return view_; }

private:
    JavaVM* vm_ = nullptr;   // cached to obtain a JNIEnv at destruction time
    jobject view_;           // JNI global reference
};

// ─── Adapter ────────────────────────────────────────────────────────────

/// Android JNI implementation of IViewAdapter.
///
/// Bridges Aria's property system to Android View widgets via JNI,
/// caching jclass/jmethodID lookups for the supported widget operations.
class ARIA_JNI_API JniAdapter : public ::aria::binding::IViewAdapter {
public:
    /// @param env  JNI environment from the current (UI) thread.
    explicit JniAdapter(JNIEnv* env);

    ~JniAdapter() override;

    JniAdapter(const JniAdapter&) = delete;
    JniAdapter& operator=(const JniAdapter&) = delete;

    [[nodiscard]] std::string_view platform_name() const noexcept override {
        return "android";
    }

    // ── Text ────────────────────────────────────────────────────────────
    void                 set_text(::aria::binding::IView& v, std::string_view text) override;
    [[nodiscard]] std::string get_text(::aria::binding::IView& v) override;
    ::aria::Subscription on_text_changed(::aria::binding::IView& v,
        std::function<void(std::string_view)> cb) override;

    // ── Bool ────────────────────────────────────────────────────────────
    void                 set_bool(::aria::binding::IView& v, bool value) override;
    [[nodiscard]] bool   get_bool(::aria::binding::IView& v) override;
    ::aria::Subscription on_bool_changed(::aria::binding::IView& v,
        std::function<void(bool)> cb) override;

    // ── Int ─────────────────────────────────────────────────────────────
    void                 set_int(::aria::binding::IView& v, int value) override;
    [[nodiscard]] int    get_int(::aria::binding::IView& v) override;
    ::aria::Subscription on_int_changed(::aria::binding::IView& v,
        std::function<void(int)> cb) override;

    void                 set_int64(::aria::binding::IView& v, std::int64_t value) override;
    [[nodiscard]] std::int64_t get_int64(::aria::binding::IView& v) override;
    ::aria::Subscription on_int64_changed(::aria::binding::IView& v,
        std::function<void(std::int64_t)> cb) override;

    void                 set_uint64(::aria::binding::IView& v, std::uint64_t value) override;
    [[nodiscard]] std::uint64_t get_uint64(::aria::binding::IView& v) override;
    ::aria::Subscription on_uint64_changed(::aria::binding::IView& v,
        std::function<void(std::uint64_t)> cb) override;

    void                 set_float(::aria::binding::IView& v, float value) override;
    [[nodiscard]] float  get_float(::aria::binding::IView& v) override;
    ::aria::Subscription on_float_changed(::aria::binding::IView& v,
        std::function<void(float)> cb) override;

    // ── Double ──────────────────────────────────────────────────────────
    void                 set_double(::aria::binding::IView& v, double value) override;
    [[nodiscard]] double get_double(::aria::binding::IView& v) override;
    ::aria::Subscription on_double_changed(::aria::binding::IView& v,
        std::function<void(double)> cb) override;

    // ── Visibility / enabled ────────────────────────────────────────────
    void                 set_visible(::aria::binding::IView& v, bool visible) override;
    void                 set_enabled(::aria::binding::IView& v, bool enabled) override;

    // ── Click ───────────────────────────────────────────────────────────
    ::aria::Subscription on_click(::aria::binding::IView& v,
        std::function<void()> cb) override;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace aria::adapters::jni
