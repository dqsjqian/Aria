// Android widget operations resolve methods on the runtime class. Managed
// listeners feed the same JniView wrapper back through notify_*.
#include "aria/adapters/jni/JniAdapter.hpp"
#include "aria/adapters/jni/detail/jni_support.hpp"
#include "aria/abi/signal.hpp"
#include "aria/abi/slot_factory.hpp"

#include <algorithm>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace aria::adapters::jni {
namespace {
using Signal = ::aria::abi::SignalErased;

template<class Int>
int narrow_int(Int value) noexcept {
    if (std::cmp_less(value, std::numeric_limits<int>::min())) return std::numeric_limits<int>::min();
    if (std::cmp_greater(value, std::numeric_limits<int>::max())) return std::numeric_limits<int>::max();
    return static_cast<int>(value);
}

float narrow_float(double value) noexcept {
    constexpr double maximum = std::numeric_limits<float>::max();
    return static_cast<float>(std::clamp(value, -maximum, maximum));
}

jobject native_of(::aria::binding::IView& view) {
    auto* native = dynamic_cast<JniView*>(&view);
    return native ? native->native() : nullptr;
}

struct NativeCall {
    detail::Env scope;
    JNIEnv* env;
    detail::LocalRef<jobject> object;
    NativeCall(JavaVM* vm, ::aria::binding::IView& view)
        : scope(vm), env(scope.get()), object(env, retain(env, view)) {}
    explicit operator bool() const noexcept { return object.get() != nullptr; }
    static jobject retain(JNIEnv* env, ::aria::binding::IView& view) {
        if (!env || detail::check_exception(env, "enter widget operation")) return nullptr;
        auto native = native_of(view);
        if (!native) return nullptr;
        auto local = env->NewLocalRef(native);
        if (detail::check_exception(env, "NewLocalRef")) {
            if (local) env->DeleteLocalRef(local);
            return nullptr;
        }
        return local;
    }
    jmethodID method(const char* name, const char* signature) {
        return detail::method(env, object.get(), name, signature);
    }
};
} // namespace

JniView::JniView(JNIEnv* env, jobject view) : vm_(detail::vm_of(env)), view_(nullptr) {
    if (vm_ && view) {
        view_ = env->NewGlobalRef(view);
        if (detail::check_exception(env, "NewGlobalRef")) {
            detail::delete_global_ref(vm_, std::exchange(view_, nullptr));
        }
    }
}
JniView::~JniView() {
    fire_destroy_();
    detail::delete_global_ref(vm_, std::exchange(view_, nullptr));
}

struct JniAdapter::Impl : std::enable_shared_from_this<Impl> {
    struct Bucket {
        std::unordered_map<char, std::shared_ptr<Signal>> channels;
        ::aria::Subscription destroy;
    };
    JavaVM* vm = nullptr;
    std::mutex mutex;
    bool closed = false;
    std::unordered_map<const ::aria::binding::IView*, std::shared_ptr<Bucket>> views;

    static void retire(const std::shared_ptr<Bucket>& bucket) noexcept {
        bucket->destroy.release();
        for (const auto& [kind, signal] : bucket->channels) signal->clear();
    }
    void close() noexcept {
        decltype(views) retired;
        {
            std::lock_guard lock(mutex);
            closed = true;
            retired.swap(views);
        }
        for (const auto& [view, bucket] : retired) retire(bucket);
    }
    void remove(const ::aria::binding::IView* view) noexcept {
        decltype(views)::node_type removed;
        {
            std::lock_guard lock(mutex);
            removed = views.extract(view);
        }
        if (removed) retire(removed.mapped());
    }
    std::shared_ptr<Signal> signal(const ::aria::binding::IView* view, char kind) {
        std::lock_guard lock(mutex);
        auto bucket = views.find(view);
        if (bucket == views.end()) return {};
        auto channel = bucket->second->channels.find(kind);
        return channel == bucket->second->channels.end() ? nullptr : channel->second;
    }
    ::aria::Subscription connect(::aria::binding::IView& view, char kind, ::aria::abi::SlotErased slot) {
        std::shared_ptr<Bucket> bucket;
        std::shared_ptr<Signal> channel;
        bool created = false;
        {
            std::lock_guard lock(mutex);
            if (closed) return {};
            auto [it, inserted] = views.try_emplace(&view);
            if (!it->second) it->second = std::make_shared<Bucket>();
            created = inserted;
            bucket = it->second;
            auto& entry = bucket->channels[kind];
            if (!entry) entry = std::make_shared<Signal>();
            channel = entry;
        }
        if (created) {
            bucket->destroy = view.on_destroy([weak = weak_from_this(), key = &view] {
                if (auto state = weak.lock()) state->remove(key);
            });
        }
        const auto id = channel->connect(std::move(slot));
        return ::aria::Subscription{[weak = channel->weak_handle(), id] {
            Signal::disconnect_via_weak(weak, id);
        }};
    }
};

JniAdapter::JniAdapter(JNIEnv* env) : p_(std::make_shared<Impl>()) { p_->vm = detail::vm_of(env); }
JniAdapter::~JniAdapter() { p_->close(); }

void JniAdapter::set_text(::aria::binding::IView& view, std::string_view text) {
    auto state = p_;
    NativeCall call(state->vm, view);
    if (!call) return;
    auto method = call.method("setText", "(Ljava/lang/CharSequence;)V");
    if (!method) return;
    detail::LocalRef<jstring> value(call.env, detail::to_jstring(call.env, text));
    if (!value.get()) return;
    call.env->CallVoidMethod(call.object.get(), method, value.get());
    detail::check_exception(call.env, "setText");
}
std::string JniAdapter::get_text(::aria::binding::IView& view) {
    auto state = p_;
    NativeCall call(state->vm, view);
    if (!call) return {};
    auto method = call.method("getText", "()Ljava/lang/CharSequence;");
    if (!method) return {};
    detail::LocalRef<jobject> sequence(call.env, call.env->CallObjectMethod(call.object.get(), method));
    if (detail::check_exception(call.env, "getText") || !sequence.get()) return {};
    auto to_string = detail::method(call.env, sequence.get(), "toString", "()Ljava/lang/String;");
    if (!to_string) return {};
    detail::LocalRef<jstring> value(call.env, static_cast<jstring>(call.env->CallObjectMethod(sequence.get(), to_string)));
    if (detail::check_exception(call.env, "toString")) return {};
    return detail::from_jstring(call.env, value.get());
}

void JniAdapter::set_bool(::aria::binding::IView& view, bool value) {
    auto state = p_;
    NativeCall call(state->vm, view);
    if (!call) return;
    auto method = call.method("setChecked", "(Z)V");
    if (!method) return;
    call.env->CallVoidMethod(call.object.get(), method, static_cast<jboolean>(value));
    detail::check_exception(call.env, "setChecked");
}
bool JniAdapter::get_bool(::aria::binding::IView& view) {
    auto state = p_;
    NativeCall call(state->vm, view);
    if (!call) return false;
    auto method = call.method("isChecked", "()Z");
    if (!method) return false;
    const auto result = call.env->CallBooleanMethod(call.object.get(), method);
    if (detail::check_exception(call.env, "isChecked")) return false;
    return result == JNI_TRUE;
}

void JniAdapter::set_int(::aria::binding::IView& view, int value) {
    auto state = p_;
    NativeCall call(state->vm, view);
    if (!call) return;
    auto method = call.method("setProgress", "(I)V");
    if (!method) return;
    call.env->CallVoidMethod(call.object.get(), method, static_cast<jint>(value));
    detail::check_exception(call.env, "setProgress");
}
int JniAdapter::get_int(::aria::binding::IView& view) {
    auto state = p_;
    NativeCall call(state->vm, view);
    if (!call) return 0;
    auto method = call.method("getProgress", "()I");
    if (!method) return 0;
    const auto result = call.env->CallIntMethod(call.object.get(), method);
    if (detail::check_exception(call.env, "getProgress")) return 0;
    return static_cast<int>(result);
}

void JniAdapter::set_double(::aria::binding::IView& view, double value) {
    auto state = p_;
    NativeCall call(state->vm, view);
    if (!call) return;
    auto method = call.method("setRating", "(F)V");
    if (!method) return;
    call.env->CallVoidMethod(call.object.get(), method, narrow_float(value));
    detail::check_exception(call.env, "setRating");
}
double JniAdapter::get_double(::aria::binding::IView& view) {
    auto state = p_;
    NativeCall call(state->vm, view);
    if (!call) return 0.0;
    auto method = call.method("getRating", "()F");
    if (!method) return 0.0;
    const auto result = call.env->CallFloatMethod(call.object.get(), method);
    if (detail::check_exception(call.env, "getRating")) return 0.0;
    return static_cast<double>(result);
}

void JniAdapter::set_visible(::aria::binding::IView& view, bool value) {
    auto state = p_;
    NativeCall call(state->vm, view);
    if (!call) return;
    auto method = call.method("setVisibility", "(I)V");
    if (!method) return;
    call.env->CallVoidMethod(call.object.get(), method, static_cast<jint>(value ? 0 : 8));
    detail::check_exception(call.env, "setVisibility");
}

void JniAdapter::set_enabled(::aria::binding::IView& view, bool value) {
    auto state = p_;
    NativeCall call(state->vm, view);
    if (!call) return;
    auto method = call.method("setEnabled", "(Z)V");
    if (!method) return;
    call.env->CallVoidMethod(call.object.get(), method, static_cast<jboolean>(value));
    detail::check_exception(call.env, "setEnabled");
}

::aria::Subscription JniAdapter::on_text_changed(::aria::binding::IView& view, std::function<void(std::string_view)> cb) {
    auto state = p_;
    if (!native_of(view) || !cb) return {};
    return state->connect(view, 't', ::aria::abi::make_slot_erased(
        [cb = std::move(cb)](void* args) { cb(*static_cast<std::string_view*>(args)); }));
}
void JniAdapter::notify_text_changed(::aria::binding::IView& view, std::string_view value) {
    auto state = p_;
    if (auto signal = state->signal(&view, 't')) signal->emit(&value);
}

::aria::Subscription JniAdapter::on_bool_changed(::aria::binding::IView& view, std::function<void(bool)> cb) {
    auto state = p_;
    if (!native_of(view) || !cb) return {};
    return state->connect(view, 'b', ::aria::abi::make_slot_erased(
        [cb = std::move(cb)](void* args) { cb(*static_cast<bool*>(args)); }));
}
void JniAdapter::notify_bool_changed(::aria::binding::IView& view, bool value) {
    auto state = p_;
    if (auto signal = state->signal(&view, 'b')) signal->emit(&value);
}

::aria::Subscription JniAdapter::on_int_changed(::aria::binding::IView& view, std::function<void(int)> cb) {
    auto state = p_;
    if (!native_of(view) || !cb) return {};
    return state->connect(view, 'i', ::aria::abi::make_slot_erased(
        [cb = std::move(cb)](void* args) { cb(*static_cast<int*>(args)); }));
}
void JniAdapter::notify_int_changed(::aria::binding::IView& view, int value) {
    auto state = p_;
    if (auto signal = state->signal(&view, 'i')) signal->emit(&value);
}

::aria::Subscription JniAdapter::on_double_changed(::aria::binding::IView& view, std::function<void(double)> cb) {
    auto state = p_;
    if (!native_of(view) || !cb) return {};
    return state->connect(view, 'd', ::aria::abi::make_slot_erased(
        [cb = std::move(cb)](void* args) { cb(*static_cast<double*>(args)); }));
}
void JniAdapter::notify_double_changed(::aria::binding::IView& view, double value) {
    auto state = p_;
    if (auto signal = state->signal(&view, 'd')) signal->emit(&value);
}

::aria::Subscription JniAdapter::on_click(::aria::binding::IView& view, std::function<void()> cb) {
    auto state = p_;
    if (!native_of(view) || !cb) return {};
    return state->connect(view, 'c', ::aria::abi::make_slot_erased(
        [cb = std::move(cb)](void*) { cb(); }));
}
void JniAdapter::notify_click(::aria::binding::IView& view) {
    auto state = p_;
    if (auto signal = state->signal(&view, 'c')) signal->emit(nullptr);
}

void JniAdapter::set_int64(::aria::binding::IView& view, std::int64_t value) { set_int(view, narrow_int(value)); }
std::int64_t JniAdapter::get_int64(::aria::binding::IView& view) { return static_cast<std::int64_t>(get_int(view)); }
::aria::Subscription JniAdapter::on_int64_changed(::aria::binding::IView& view, std::function<void(std::int64_t)> cb) {
    if (!cb) return {};
    return on_int_changed(view, [cb = std::move(cb)](int value) { cb(static_cast<std::int64_t>(value)); });
}
void JniAdapter::notify_int64_changed(::aria::binding::IView& view, std::int64_t value) { notify_int_changed(view, narrow_int(value)); }

void JniAdapter::set_uint64(::aria::binding::IView& view, std::uint64_t value) { set_int(view, narrow_int(value)); }
std::uint64_t JniAdapter::get_uint64(::aria::binding::IView& view) { return static_cast<std::uint64_t>(std::max(0, get_int(view))); }
::aria::Subscription JniAdapter::on_uint64_changed(::aria::binding::IView& view, std::function<void(std::uint64_t)> cb) {
    if (!cb) return {};
    return on_int_changed(view, [cb = std::move(cb)](int value) { cb(static_cast<std::uint64_t>(std::max(0, value))); });
}
void JniAdapter::notify_uint64_changed(::aria::binding::IView& view, std::uint64_t value) { notify_int_changed(view, narrow_int(value)); }

void JniAdapter::set_float(::aria::binding::IView& view, float value) { set_double(view, value); }
float JniAdapter::get_float(::aria::binding::IView& view) { return static_cast<float>(get_double(view)); }
::aria::Subscription JniAdapter::on_float_changed(::aria::binding::IView& view, std::function<void(float)> cb) {
    if (!cb) return {};
    return on_double_changed(view, [cb = std::move(cb)](double value) { cb(narrow_float(value)); });
}
void JniAdapter::notify_float_changed(::aria::binding::IView& view, float value) { notify_double_changed(view, value); }

} // namespace aria::adapters::jni
