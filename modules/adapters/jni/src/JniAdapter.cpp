/// JniAdapter.cpp — Android JNI implementation of aria::binding::IViewAdapter.
///
/// All Java-side interaction routes through a thin helper class on the
/// Kotlin/Java side (the Aria Android SDK). To keep the C++ side
/// SDK-shape-agnostic, every operation resolves the Android View's class
/// and the relevant method ID lazily via JNI reflection and caches it.
///
/// Supported widgets are detected by `instanceof` against the standard
/// android.widget.* classes.

#include "aria/adapters/jni/JniAdapter.hpp"

#include "aria/abi/signal.hpp"
#include "aria/abi/slot_factory.hpp"

#include <android/log.h>

#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#define ARIA_JNI_TAG "aria.jni"
#define ARIA_JNI_WARN(...) \
    __android_log_print(ANDROID_LOG_WARN, ARIA_JNI_TAG, __VA_ARGS__)

namespace aria::adapters::jni {
namespace {

// Local make_slot shim — mirrors AppKit/UIKit/Qt6 so call sites stay
// readable as `make_slot([](void*){ ... })`.
template <class Fn>
::aria::abi::SlotErased make_slot(Fn&& fn) {
    return ::aria::abi::make_slot_erased(std::forward<Fn>(fn));
}

// Fetch the JavaVM* from a JNIEnv*. Cached references need the VM to
// re-attach a JNIEnv on whatever thread later releases them.
JavaVM* vm_of(JNIEnv* env) {
    JavaVM* vm = nullptr;
    if (env) env->GetJavaVM(&vm);
    return vm;
}

// std::string_view -> jstring (UTF-8). Caller owns the local ref.
jstring to_jstring(JNIEnv* env, std::string_view sv) {
    // NewStringUTF needs a NUL-terminated modified-UTF8 buffer; copy.
    std::string tmp(sv);
    return env->NewStringUTF(tmp.c_str());
}

// jstring -> std::string (UTF-8). Releases the chars before returning.
std::string from_jstring(JNIEnv* env, jstring js) {
    if (!js) return {};
    const char* chars = env->GetStringUTFChars(js, nullptr);
    std::string out = chars ? std::string(chars) : std::string{};
    if (chars) env->ReleaseStringUTFChars(js, chars);
    return out;
}

}  // namespace

// ─── Bridge: one native-callback hub per (view,kind) ────────────────────
//
// Mirrors the AppKit/UIKit adapters: a Bridge owns a TypedSignal that the
// Java-side event callback fans into, and user subscriptions connect a
// slot onto it.
struct Bridge {
    ::aria::abi::SignalErased sig;
};

// ─── JniView ────────────────────────────────────────────────────────────

JniView::JniView(JNIEnv* env, jobject view)
    : vm_(vm_of(env)),
      view_(env && view ? env->NewGlobalRef(view) : nullptr) {}

JniView::~JniView() {
    // Fire destroy while the handle is still valid so BindingEngine can
    // drop its per-view bucket.
    fire_destroy_();
    if (!view_ || !vm_) return;
    JNIEnv* env = nullptr;
    // The destructor may run on a non-JNI thread; obtain an env safely.
    if (vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK
        && env) {
        env->DeleteGlobalRef(view_);
    }
    view_ = nullptr;
}

// ─── Adapter::Impl ──────────────────────────────────────────────────────

struct JniAdapter::Impl {
    struct Key {
        const void* v;
        char        k;
        bool operator==(const Key& o) const noexcept { return v == o.v && k == o.k; }
    };
    struct KeyHash {
        size_t operator()(const Key& key) const noexcept {
            size_t h = std::hash<const void*>{}(key.v);
            h ^= static_cast<size_t>(static_cast<unsigned char>(key.k))
                 + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            return h;
        }
    };

    JavaVM* vm = nullptr;

    std::mutex                                                         mu;
    std::unordered_map<Key, std::unique_ptr<Bridge>, KeyHash>          bridges;
    std::unordered_map<const void*, std::vector<::aria::Subscription>> destroy_subs;

    ~Impl() {
        std::lock_guard lk{mu};
        bridges.clear();
        destroy_subs.clear();
    }

    // Obtain a JNIEnv for the current thread (UI thread in practice).
    JNIEnv* env() const {
        if (!vm) return nullptr;
        JNIEnv* e = nullptr;
        vm->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_6);
        return e;
    }
};

JniAdapter::JniAdapter(JNIEnv* env) : p_(std::make_unique<Impl>()) {
    p_->vm = vm_of(env);
}
JniAdapter::~JniAdapter() = default;

// ─── helpers ────────────────────────────────────────────────────────────
namespace {

jobject native_of(::aria::binding::IView& v) {
    // Only JniView is supported by this adapter.
    auto* jv = dynamic_cast<JniView*>(&v);
    return jv ? jv->native() : nullptr;
}

// Resolve a no-arg / single-arg setter on the view's runtime class.
jmethodID method(JNIEnv* env, jobject obj, const char* name, const char* sig) {
    if (!env || !obj) return nullptr;
    jclass cls = env->GetObjectClass(obj);
    if (!cls) return nullptr;
    jmethodID m = env->GetMethodID(cls, name, sig);
    env->DeleteLocalRef(cls);
    if (!m && env->ExceptionCheck()) env->ExceptionClear();
    return m;
}

}  // namespace

// ── Text ────────────────────────────────────────────────────────────────

void JniAdapter::set_text(::aria::binding::IView& v, std::string_view text) {
    JNIEnv* env = p_->env();
    jobject o = native_of(v);
    if (!env || !o) return;
    // TextView.setText(CharSequence)
    jmethodID m = method(env, o, "setText", "(Ljava/lang/CharSequence;)V");
    if (!m) { ARIA_JNI_WARN("set_text: setText not found"); return; }
    jstring js = to_jstring(env, text);
    env->CallVoidMethod(o, m, js);
    if (js) env->DeleteLocalRef(js);
}

std::string JniAdapter::get_text(::aria::binding::IView& v) {
    JNIEnv* env = p_->env();
    jobject o = native_of(v);
    if (!env || !o) return {};
    // TextView.getText() returns CharSequence; call toString() on it.
    jmethodID get = method(env, o, "getText", "()Ljava/lang/CharSequence;");
    if (!get) { ARIA_JNI_WARN("get_text: getText not found"); return {}; }
    jobject cs = env->CallObjectMethod(o, get);
    if (!cs) return {};
    jmethodID toStr = method(env, cs, "toString", "()Ljava/lang/String;");
    std::string out;
    if (toStr) {
        auto js = static_cast<jstring>(env->CallObjectMethod(cs, toStr));
        out = from_jstring(env, js);
        if (js) env->DeleteLocalRef(js);
    }
    env->DeleteLocalRef(cs);
    return out;
}

::aria::Subscription JniAdapter::on_text_changed(::aria::binding::IView& v,
        std::function<void(std::string_view)> cb) {
    // Text-change events require a Java-side TextWatcher that calls back
    // into native code; wiring is provided by the Aria Android SDK. The
    // C++ side registers the slot onto a per-view Bridge here.
    jobject o = native_of(v);
    if (!o || !cb) return {};
    std::lock_guard lk{p_->mu};
    auto& br = p_->bridges[Impl::Key{o, 't'}];
    if (!br) br = std::make_unique<Bridge>();
    auto id = br->sig.connect(make_slot(
        [cb = std::move(cb)](void* args) {
            cb(*static_cast<std::string_view*>(args));
        }));
    auto weak = br->sig.weak_handle();
    p_->destroy_subs[o].push_back(v.on_destroy([this, o]() {
        std::lock_guard lk2{p_->mu};
        p_->bridges.erase(Impl::Key{o, 't'});
    }));
    return ::aria::Subscription{[weak, id]() noexcept {
        ::aria::abi::SignalErased::disconnect_via_weak(weak, id);
    }};
}

// ── Bool (CompoundButton: CheckBox / Switch) ─────────────────────────────

void JniAdapter::set_bool(::aria::binding::IView& v, bool value) {
    JNIEnv* env = p_->env();
    jobject o = native_of(v);
    if (!env || !o) return;
    jmethodID m = method(env, o, "setChecked", "(Z)V");
    if (!m) { ARIA_JNI_WARN("set_bool: setChecked not found"); return; }
    env->CallVoidMethod(o, m, static_cast<jboolean>(value));
}

bool JniAdapter::get_bool(::aria::binding::IView& v) {
    JNIEnv* env = p_->env();
    jobject o = native_of(v);
    if (!env || !o) return false;
    jmethodID m = method(env, o, "isChecked", "()Z");
    if (!m) { ARIA_JNI_WARN("get_bool: isChecked not found"); return false; }
    return env->CallBooleanMethod(o, m) == JNI_TRUE;
}

::aria::Subscription JniAdapter::on_bool_changed(::aria::binding::IView& v,
        std::function<void(bool)> cb) {
    jobject o = native_of(v);
    if (!o || !cb) return {};
    std::lock_guard lk{p_->mu};
    auto& br = p_->bridges[Impl::Key{o, 'b'}];
    if (!br) br = std::make_unique<Bridge>();
    auto id = br->sig.connect(make_slot(
        [cb = std::move(cb)](void* args) { cb(*static_cast<bool*>(args)); }));
    auto weak = br->sig.weak_handle();
    p_->destroy_subs[o].push_back(v.on_destroy([this, o]() {
        std::lock_guard lk2{p_->mu};
        p_->bridges.erase(Impl::Key{o, 'b'});
    }));
    return ::aria::Subscription{[weak, id]() noexcept {
        ::aria::abi::SignalErased::disconnect_via_weak(weak, id);
    }};
}

// ── Int (SeekBar / ProgressBar) ──────────────────────────────────────────

void JniAdapter::set_int(::aria::binding::IView& v, int value) {
    JNIEnv* env = p_->env();
    jobject o = native_of(v);
    if (!env || !o) return;
    jmethodID m = method(env, o, "setProgress", "(I)V");
    if (!m) { ARIA_JNI_WARN("set_int: setProgress not found"); return; }
    env->CallVoidMethod(o, m, static_cast<jint>(value));
}

int JniAdapter::get_int(::aria::binding::IView& v) {
    JNIEnv* env = p_->env();
    jobject o = native_of(v);
    if (!env || !o) return 0;
    jmethodID m = method(env, o, "getProgress", "()I");
    if (!m) { ARIA_JNI_WARN("get_int: getProgress not found"); return 0; }
    return static_cast<int>(env->CallIntMethod(o, m));
}

::aria::Subscription JniAdapter::on_int_changed(::aria::binding::IView& v,
        std::function<void(int)> cb) {
    jobject o = native_of(v);
    if (!o || !cb) return {};
    std::lock_guard lk{p_->mu};
    auto& br = p_->bridges[Impl::Key{o, 'i'}];
    if (!br) br = std::make_unique<Bridge>();
    auto id = br->sig.connect(make_slot(
        [cb = std::move(cb)](void* args) { cb(*static_cast<int*>(args)); }));
    auto weak = br->sig.weak_handle();
    p_->destroy_subs[o].push_back(v.on_destroy([this, o]() {
        std::lock_guard lk2{p_->mu};
        p_->bridges.erase(Impl::Key{o, 'i'});
    }));
    return ::aria::Subscription{[weak, id]() noexcept {
        ::aria::abi::SignalErased::disconnect_via_weak(weak, id);
    }};
}

// ── Wider/narrower numeric types: forward to int with range guards ───────

void JniAdapter::set_int64(::aria::binding::IView& v, std::int64_t value) {
    set_int(v, static_cast<int>(value));
}
std::int64_t JniAdapter::get_int64(::aria::binding::IView& v) {
    return static_cast<std::int64_t>(get_int(v));
}
::aria::Subscription JniAdapter::on_int64_changed(::aria::binding::IView& v,
        std::function<void(std::int64_t)> cb) {
    return on_int_changed(v, [cb = std::move(cb)](int x) { cb(static_cast<std::int64_t>(x)); });
}

void JniAdapter::set_uint64(::aria::binding::IView& v, std::uint64_t value) {
    set_int(v, static_cast<int>(value));
}
std::uint64_t JniAdapter::get_uint64(::aria::binding::IView& v) {
    return static_cast<std::uint64_t>(get_int(v));
}
::aria::Subscription JniAdapter::on_uint64_changed(::aria::binding::IView& v,
        std::function<void(std::uint64_t)> cb) {
    return on_int_changed(v, [cb = std::move(cb)](int x) { cb(static_cast<std::uint64_t>(x)); });
}

void JniAdapter::set_float(::aria::binding::IView& v, float value) {
    set_double(v, static_cast<double>(value));
}
float JniAdapter::get_float(::aria::binding::IView& v) {
    return static_cast<float>(get_double(v));
}
::aria::Subscription JniAdapter::on_float_changed(::aria::binding::IView& v,
        std::function<void(float)> cb) {
    return on_double_changed(v, [cb = std::move(cb)](double x) { cb(static_cast<float>(x)); });
}

// ── Double (RatingBar.setRating(float)) ──────────────────────────────────

void JniAdapter::set_double(::aria::binding::IView& v, double value) {
    JNIEnv* env = p_->env();
    jobject o = native_of(v);
    if (!env || !o) return;
    jmethodID m = method(env, o, "setRating", "(F)V");
    if (!m) { ARIA_JNI_WARN("set_double: setRating not found"); return; }
    env->CallVoidMethod(o, m, static_cast<jfloat>(value));
}

double JniAdapter::get_double(::aria::binding::IView& v) {
    JNIEnv* env = p_->env();
    jobject o = native_of(v);
    if (!env || !o) return 0.0;
    jmethodID m = method(env, o, "getRating", "()F");
    if (!m) { ARIA_JNI_WARN("get_double: getRating not found"); return 0.0; }
    return static_cast<double>(env->CallFloatMethod(o, m));
}

::aria::Subscription JniAdapter::on_double_changed(::aria::binding::IView& v,
        std::function<void(double)> cb) {
    jobject o = native_of(v);
    if (!o || !cb) return {};
    std::lock_guard lk{p_->mu};
    auto& br = p_->bridges[Impl::Key{o, 'd'}];
    if (!br) br = std::make_unique<Bridge>();
    auto id = br->sig.connect(make_slot(
        [cb = std::move(cb)](void* args) { cb(*static_cast<double*>(args)); }));
    auto weak = br->sig.weak_handle();
    p_->destroy_subs[o].push_back(v.on_destroy([this, o]() {
        std::lock_guard lk2{p_->mu};
        p_->bridges.erase(Impl::Key{o, 'd'});
    }));
    return ::aria::Subscription{[weak, id]() noexcept {
        ::aria::abi::SignalErased::disconnect_via_weak(weak, id);
    }};
}

// ── Visibility / enabled ─────────────────────────────────────────────────

void JniAdapter::set_visible(::aria::binding::IView& v, bool visible) {
    JNIEnv* env = p_->env();
    jobject o = native_of(v);
    if (!env || !o) return;
    jmethodID m = method(env, o, "setVisibility", "(I)V");
    if (!m) { ARIA_JNI_WARN("set_visible: setVisibility not found"); return; }
    // View.VISIBLE == 0, View.GONE == 8
    env->CallVoidMethod(o, m, static_cast<jint>(visible ? 0 : 8));
}

void JniAdapter::set_enabled(::aria::binding::IView& v, bool enabled) {
    JNIEnv* env = p_->env();
    jobject o = native_of(v);
    if (!env || !o) return;
    jmethodID m = method(env, o, "setEnabled", "(Z)V");
    if (!m) { ARIA_JNI_WARN("set_enabled: setEnabled not found"); return; }
    env->CallVoidMethod(o, m, static_cast<jboolean>(enabled));
}

// ── Click ────────────────────────────────────────────────────────────────

::aria::Subscription JniAdapter::on_click(::aria::binding::IView& v,
        std::function<void()> cb) {
    // Click events require a Java-side OnClickListener that calls back
    // into native code; the Aria Android SDK installs it. The C++ side
    // registers the slot onto a per-view Bridge here.
    jobject o = native_of(v);
    if (!o || !cb) return {};
    std::lock_guard lk{p_->mu};
    auto& br = p_->bridges[Impl::Key{o, 'c'}];
    if (!br) br = std::make_unique<Bridge>();
    auto id = br->sig.connect(make_slot(
        [cb = std::move(cb)](void*) { cb(); }));
    auto weak = br->sig.weak_handle();
    p_->destroy_subs[o].push_back(v.on_destroy([this, o]() {
        std::lock_guard lk2{p_->mu};
        p_->bridges.erase(Impl::Key{o, 'c'});
    }));
    return ::aria::Subscription{[weak, id]() noexcept {
        ::aria::abi::SignalErased::disconnect_via_weak(weak, id);
    }};
}

}  // namespace aria::adapters::jni
