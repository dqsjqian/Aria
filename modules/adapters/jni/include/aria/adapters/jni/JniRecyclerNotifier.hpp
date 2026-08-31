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
// This notifier does NOT marshal — it calls straight through, because
// Aria owns no looper abstraction (see the JniListSource header). If the
// list can mutate off-main, wrap `sink()` in a lambda that posts to a
// `Handler`, and note that the notification value is copyable precisely
// so it can be captured into such a post.
//
// The notifier attaches a JNIEnv for the calling thread when needed, so
// a sink invoked from a non-JNI thread does not crash — it will still be
// the caller's bug if that thread is not the main looper, but it will
// surface as an Android RecyclerView complaint rather than as JNI UB.

#include "aria/adapters/jni/JniListSource.hpp"

#include <jni.h>

#include <functional>
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
    /// @param env      JNI environment of the calling (usually main) thread.
    /// @param adapter  The managed RecyclerView.Adapter instance.
    JniRecyclerNotifier(JNIEnv* env, jobject adapter) {
        if (!env || !adapter) return;
        env->GetJavaVM(&vm_);
        adapter_ = env->NewGlobalRef(adapter);
        if (!adapter_) return;

        jclass cls = env->GetObjectClass(adapter_);
        if (!cls) return;
        // RecyclerView.Adapter's notify* methods are public final on
        // androidx.recyclerview.widget.RecyclerView$Adapter, so they
        // resolve against the concrete subclass too.
        m_inserted_ = env->GetMethodID(cls, "notifyItemInserted", "(I)V");
        m_removed_  = env->GetMethodID(cls, "notifyItemRemoved", "(I)V");
        m_changed_  = env->GetMethodID(cls, "notifyItemChanged", "(I)V");
        m_moved_    = env->GetMethodID(cls, "notifyItemMoved", "(II)V");
        m_dataset_  = env->GetMethodID(cls, "notifyDataSetChanged", "()V");
        env->DeleteLocalRef(cls);
        // A failed lookup leaves an exception pending, which would abort
        // the next JNI call made by unrelated code.
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    ~JniRecyclerNotifier() {
        if (!adapter_ || !vm_) return;
        JNIEnv* env = nullptr;
        if (vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK
            && env) {
            env->DeleteGlobalRef(adapter_);
        }
        adapter_ = nullptr;
    }

    JniRecyclerNotifier(const JniRecyclerNotifier&)            = delete;
    JniRecyclerNotifier& operator=(const JniRecyclerNotifier&) = delete;

    /// True iff the managed adapter and every notify method resolved.
    /// A false result means notifications will be dropped — check it
    /// during bring-up rather than wondering why rows never refresh.
    [[nodiscard]] bool valid() const noexcept {
        return adapter_ != nullptr && m_inserted_ && m_removed_
            && m_changed_ && m_moved_ && m_dataset_;
    }

    /// Sink to hand to `JniListSource<T>`. Captures `this`, so the
    /// notifier must outlive the list source.
    ///
    /// The sink signature does not depend on the element type, so one
    /// accessor serves every `JniListSource<T>`.
    [[nodiscard]] std::function<void(const RecyclerNotification&)> sink() {
        return [this](const RecyclerNotification& n) { dispatch(n); };
    }

    /// Forward one notification to the managed adapter.
    void dispatch(const RecyclerNotification& n) {
        if (!valid()) return;
        JNIEnv* env = env_for_current_thread();
        if (!env) return;

        const auto pos  = static_cast<jint>(n.position);
        const auto from = static_cast<jint>(n.from_position);
        switch (n.kind) {
        case RecyclerNotify::ItemInserted:
            env->CallVoidMethod(adapter_, m_inserted_, pos);
            break;
        case RecyclerNotify::ItemRemoved:
            env->CallVoidMethod(adapter_, m_removed_, pos);
            break;
        case RecyclerNotify::ItemChanged:
            env->CallVoidMethod(adapter_, m_changed_, pos);
            break;
        case RecyclerNotify::ItemMoved:
            env->CallVoidMethod(adapter_, m_moved_, from, pos);
            break;
        case RecyclerNotify::DataSetChanged:
            env->CallVoidMethod(adapter_, m_dataset_);
            break;
        }
        // A managed exception (e.g. RecyclerView complaining about an
        // inconsistency) must not leak into the next unrelated JNI call.
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
    }

private:
    /// JNIEnv for the calling thread. Returns nullptr when the thread is
    /// not attached and cannot be attached, rather than risking UB by
    /// reusing an env from another thread.
    JNIEnv* env_for_current_thread() {
        if (!vm_) return nullptr;
        JNIEnv* env = nullptr;
        const jint rc =
            vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if (rc == JNI_OK) return env;
        if (rc == JNI_EDETACHED) {
            // Attach as a daemon so a stray producer thread cannot keep
            // the VM alive past shutdown.
            if (vm_->AttachCurrentThreadAsDaemon(&env, nullptr) == JNI_OK) {
                return env;
            }
        }
        return nullptr;
    }

    JavaVM*   vm_          = nullptr;
    jobject   adapter_     = nullptr;   // JNI global reference
    jmethodID m_inserted_  = nullptr;
    jmethodID m_removed_   = nullptr;
    jmethodID m_changed_   = nullptr;
    jmethodID m_moved_     = nullptr;
    jmethodID m_dataset_   = nullptr;
};

}  // namespace aria::adapters::jni
