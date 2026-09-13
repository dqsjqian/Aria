#pragma once

// JniRecyclerNotifier.hpp — the JNI half of the RecyclerView list bridge.
//
// `JniListSource<T>` (JniListSource.hpp) owns the snapshot and the
// diffing and deliberately has no `<jni.h>` dependency so its logic is
// host-testable. This header supplies the piece that genuinely needs a
// JVM: a `NotifySink` that calls `notifyItemInserted` /
// `notifyItemRemoved` / `notifyItemChanged` / `notifyItemMoved` /
// `notifyDataSetChanged` on a managed `RecyclerView.Adapter`.
//
// Usage (C++ side, typically from a JNI entry point):
//
//   // `adapter` is the Kotlin RecyclerView.Adapter instance.
//   auto notifier = std::make_shared<JniRecyclerNotifier>(env, adapter);
//   auto rows = std::make_unique<JniListSource<Movie>>(
//       vm.movies, notifier->sink());
//
// Then forward the managed adapter's overrides to the C++ side:
//
//   getItemCount()                  -> rows->item_count()
//   onBindViewHolder(holder, pos)   -> rows->at(pos)
//
// Threading
// ---------
// RecyclerView notifications MUST be raised on the Android main thread.
// This notifier calls straight through. For off-main list mutations,
// give JniListSource an owner-thread dispatcher so snapshot updates and
// notifications both run on the main looper (see JniListSource.hpp).
//
// The notifier attaches a JNIEnv for the calling thread when needed, so
// a sink invoked from a non-JNI thread does not crash — it will still be
// the caller's bug if that thread is not the main looper, but it will
// surface as an Android RecyclerView complaint rather than as JNI UB.

#include "aria/adapters/jni/JniListSource.hpp"
#include "aria/adapters/jni/detail/jni_support.hpp"

#include <jni.h>

#include <atomic>
#include <functional>
#include <limits>
#include <memory>

namespace aria::adapters::jni {

/// Holds a global reference to a managed `RecyclerView.Adapter` and
/// exposes a `JniListSource<T>::NotifySink` that drives its
/// `notifyItem*` methods.
///
/// Method IDs are resolved once in the constructor: they are stable for
/// the adapter's class, and resolving them per notification would put
/// reflection on the scroll path.
class JniRecyclerNotifier {
public:
    JniRecyclerNotifier(JNIEnv* env, jobject adapter) : state_(std::make_shared<State>()) {
        if (!env || !adapter) return;
        auto& state = *state_;
        state.vm = detail::vm_of(env);
        if (!state.vm) return;
        state.adapter = env->NewGlobalRef(adapter);
        if (detail::check_exception(env, "NewGlobalRef") || !state.adapter) return;
        state.inserted = detail::method(env, state.adapter, "notifyItemInserted", "(I)V");
        if (!state.inserted) return;
        state.removed = detail::method(env, state.adapter, "notifyItemRemoved", "(I)V");
        if (!state.removed) return;
        state.changed = detail::method(env, state.adapter, "notifyItemChanged", "(I)V");
        if (!state.changed) return;
        state.moved = detail::method(env, state.adapter, "notifyItemMoved", "(II)V");
        if (!state.moved) return;
        state.dataset = detail::method(env, state.adapter, "notifyDataSetChanged", "()V");
        state.active.store(state.dataset != nullptr, std::memory_order_release);
    }

    ~JniRecyclerNotifier() { state_->active.store(false, std::memory_order_release); }
    JniRecyclerNotifier(const JniRecyclerNotifier&) = delete;
    JniRecyclerNotifier& operator=(const JniRecyclerNotifier&) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return state_->active.load(std::memory_order_acquire);
    }

    /// A retained sink becomes inert at notifier destruction. An already
    /// executing notification retains its JNI state until the call returns.
    [[nodiscard]] std::function<void(const RecyclerNotification&)> sink() {
        return [weak = std::weak_ptr{state_}](const RecyclerNotification& notification) {
            if (auto state = weak.lock()) dispatch_(state, notification);
        };
    }

    void dispatch(const RecyclerNotification& notification) { dispatch_(state_, notification); }

private:
    struct State {
        JavaVM* vm = nullptr;
        jobject adapter = nullptr;
        jmethodID inserted = nullptr, removed = nullptr, changed = nullptr;
        jmethodID moved = nullptr, dataset = nullptr;
        std::atomic<bool> active{false};
        ~State() { detail::delete_global_ref(vm, adapter); }
    };

    static void dispatch_(std::shared_ptr<State> state, const RecyclerNotification& n) {
        if (!state->active.load(std::memory_order_acquire)) return;
        constexpr auto maximum = static_cast<std::size_t>(std::numeric_limits<jint>::max());
        if (n.kind != RecyclerNotify::DataSetChanged
            && (n.position > maximum || (n.kind == RecyclerNotify::ItemMoved && n.from_position > maximum))) {
            ::aria::report_callback_failure("jni.recycler", "notification position exceeds jint range");
            return;
        }
        detail::Env scope(state->vm);
        auto* env = scope.get();
        if (!env || detail::check_exception(env, "RecyclerView notification")) return;
        if (!state->active.load(std::memory_order_acquire)) return;
        switch (n.kind) {
        case RecyclerNotify::ItemInserted:
            env->CallVoidMethod(state->adapter, state->inserted, static_cast<jint>(n.position)); break;
        case RecyclerNotify::ItemRemoved:
            env->CallVoidMethod(state->adapter, state->removed, static_cast<jint>(n.position)); break;
        case RecyclerNotify::ItemChanged:
            env->CallVoidMethod(state->adapter, state->changed, static_cast<jint>(n.position)); break;
        case RecyclerNotify::ItemMoved:
            env->CallVoidMethod(state->adapter, state->moved, static_cast<jint>(n.from_position), static_cast<jint>(n.position)); break;
        case RecyclerNotify::DataSetChanged:
            env->CallVoidMethod(state->adapter, state->dataset); break;
        }
        detail::check_exception(env, "RecyclerView notification");
    }

    std::shared_ptr<State> state_;
};

}  // namespace aria::adapters::jni
