#include <jni.h>
#include <string>
#include <thread>
#include <memory>
#include <vector>
#include <mutex>
#include <functional>
#include <android/log.h>

#include "MainViewModel.hpp"
#include "DataModel.h"

// ─── JNI side-channel bridge ─────────────────────────────────────────────
// C++ ViewModel (aria::Property) → on_changed subscription →
// JNI callback → Kotlin MutableStateFlow → Compose recomposition
//
// This is the "Compose side-channel" approach:
//   - C++ ViewModel owns all business logic via aria::Property / Command
//   - JNI bridge subscribes to Property changes and pushes them to Kotlin
//   - Kotlin ViewModel is a thin shell that only holds StateFlows
//   - Compose observes StateFlows as before
//
// Naming: ARIA_JNI_* (framework/tech names, not ANDROID_*)
// ─────────────────────────────────────────────────────────────────────────

static const char* TAG = "JniBridge";

// ── Global JNI state ──────────────────────────────────────────────────────
static JavaVM* g_jvm = nullptr;
static jclass g_jniBridgeClass = nullptr;
static jmethodID g_onPropertyChangedMethod = nullptr;
static jmethodID g_postToMainMethod = nullptr;

// ── The C++ ViewModel (owned by this bridge) ─────────────────────────────
static std::shared_ptr<MainViewModel> g_viewModel;

// ── Property subscriptions (must outlive the ViewModel) ──────────────────
static std::vector<aria::Subscription> g_propertySubs;

// ── Main-thread task queue ────────────────────────────────────────────────
// Work produced on background threads (e.g. the DataModel fetch callback)
// is parked here and drained on the Android main thread via the main Looper.
// This is what marshals aria::Property::set back to the graph (= main) thread.
static std::mutex g_mainQueueMutex;
static std::vector<std::function<void()>> g_mainQueue;

// ── JNI_OnLoad ────────────────────────────────────────────────────────────
extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "JNI_OnLoad: GetEnv failed");
        return JNI_ERR;
    }

    g_jvm = vm;

    // Cache JniBridge class and method IDs
    jclass cls = env->FindClass("com/example/aria/demo5/JniBridge");
    if (!cls) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "JNI_OnLoad: JniBridge class not found");
        return JNI_ERR;
    }

    g_jniBridgeClass = reinterpret_cast<jclass>(env->NewGlobalRef(cls));
    env->DeleteLocalRef(cls);

    g_onPropertyChangedMethod = env->GetStaticMethodID(
        g_jniBridgeClass, "onPropertyChanged",
        "(Ljava/lang/String;Ljava/lang/String;)V");

    // Used by the background→main marshaling: native asks the JVM to post a
    // drain of g_mainQueue onto the main Looper.
    g_postToMainMethod = env->GetStaticMethodID(
        g_jniBridgeClass, "postToMain", "()V");
    if (!g_postToMainMethod) {
        __android_log_print(ANDROID_LOG_ERROR, TAG,
            "JNI_OnLoad: JniBridge.postToMain() not found");
        return JNI_ERR;
    }

    __android_log_print(ANDROID_LOG_INFO, TAG, "JNI_OnLoad: aria_jni loaded successfully");
    return JNI_VERSION_1_6;
}

// ── Helper: push property change to Kotlin via JNI side-channel ────────────
static void notifyPropertyChanged(const std::string& propName,
                                  const std::string& newValue) {
    JNIEnv* env = nullptr;
    bool attached = false;

    int getEnvResult = g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (getEnvResult == JNI_EDETACHED) {
        if (g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            __android_log_print(ANDROID_LOG_ERROR, TAG, "Failed to attach thread to JVM");
            return;
        }
        attached = true;
    } else if (getEnvResult != JNI_OK) {
        return;
    }

    jstring jPropName = env->NewStringUTF(propName.c_str());
    jstring jNewValue = env->NewStringUTF(newValue.c_str());

    env->CallStaticVoidMethod(g_jniBridgeClass, g_onPropertyChangedMethod,
        jPropName, jNewValue);

    env->DeleteLocalRef(jPropName);
    env->DeleteLocalRef(jNewValue);

    if (attached) {
        g_jvm->DetachCurrentThread();
    }
}

// ── Helper: marshal a unit of work onto the Android main (graph) thread ───
// Called from a background thread (the DataModel fetch worker). Parks the
// work and asks the JVM main Looper to drain it via JniBridge.postToMain().
// The work then runs in nativeRunMainTasks() ON the main thread, so the
// aria::Property::set calls inside it hit the graph thread the Graph was
// created on — satisfying reactive::Graph's single-thread contract.
static void scheduleOnMain(std::function<void()> work) {
    {
        std::lock_guard<std::mutex> lock(g_mainQueueMutex);
        g_mainQueue.push_back(std::move(work));
    }

    JNIEnv* env = nullptr;
    bool attached = false;
    int getEnvResult = g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (getEnvResult == JNI_EDETACHED) {
        if (g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            __android_log_print(ANDROID_LOG_ERROR, TAG,
                "scheduleOnMain: failed to attach thread to JVM");
            return;
        }
        attached = true;
    } else if (getEnvResult != JNI_OK) {
        return;
    }

    env->CallStaticVoidMethod(g_jniBridgeClass, g_postToMainMethod);

    if (attached) {
        g_jvm->DetachCurrentThread();
    }
}

// ── Helper: subscribe to all ViewModel properties ─────────────────────────
static void subscribeToViewModel(MainViewModel& vm) {
    g_propertySubs.push_back(vm.status_text.on_changed([](const std::string& v) {
        notifyPropertyChanged("status_text", v);
    }));
    g_propertySubs.push_back(vm.result_text.on_changed([](const std::string& v) {
        notifyPropertyChanged("result_text", v);
    }));
    g_propertySubs.push_back(vm.is_loading.on_changed([](bool v) {
        notifyPropertyChanged("is_loading", v ? "true" : "false");
    }));
    g_propertySubs.push_back(vm.toast_message.on_changed([](const std::string& v) {
        notifyPropertyChanged("toast_message", v);
    }));
}

// ── JNI: create the C++ ViewModel ────────────────────────────────────────
// Called from Kotlin when the ViewModel is first needed.
extern "C" JNIEXPORT void JNICALL
Java_com_example_aria_demo5_JniBridge_nativeCreateViewModel(JNIEnv* env, jclass) {
    if (g_viewModel) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "ViewModel already created");
        return;
    }

    // NOTE: this runs on the Android main thread (Kotlin ViewModel.init →
    // attachViewModel), so the aria reactive Graph records the main thread as
    // its owner thread here. Every Property::set must therefore happen on the
    // main thread.
    auto model = std::make_shared<DataModel>();
    g_viewModel = MainViewModel::create(model);

    // Set up the main-thread dispatcher.
    //
    // aria::Property is single-threaded by contract: the reactive Graph
    // asserts it is only ever touched on its owner (= main) thread. The
    // DataModel fetch callback fires on a *background* worker thread, so we
    // MUST marshal the Property::set calls back to the main thread — doing
    // them on the worker thread trips Graph::assert_on_graph_thread() (abort
    // in debug, a data race in release).
    //
    // scheduleOnMain() parks the work and posts a drain onto Android's main
    // Looper (via JniBridge.postToMain → nativeRunMainTasks), which is the
    // NDK-idiomatic equivalent of aria::runtime::IDispatcher backed by
    // Handler(Looper.getMainLooper()).
    g_viewModel->main_dispatcher = [](std::function<void()> work) {
        scheduleOnMain(std::move(work));
    };

    // Subscribe to all property changes
    subscribeToViewModel(*g_viewModel);

    __android_log_print(ANDROID_LOG_INFO, TAG, "C++ MainViewModel created with aria framework");
}

// ── JNI: destroy the C++ ViewModel ───────────────────────────────────────
extern "C" JNIEXPORT void JNICALL
Java_com_example_aria_demo5_JniBridge_nativeDestroyViewModel(JNIEnv* env, jclass) {
    g_propertySubs.clear();  // Release subscriptions before ViewModel
    g_viewModel.reset();
    __android_log_print(ANDROID_LOG_INFO, TAG, "C++ MainViewModel destroyed");
}

// ── JNI: drain the main-thread task queue ────────────────────────────────
// Posted onto the main Looper by JniBridge.postToMain(); therefore this runs
// ON the Android main thread, i.e. the aria Graph's owner thread. Any queued
// work that calls Property::set is now on the correct thread. Tasks hold a
// strong shared_ptr to the ViewModel (captured in doFetch), so they are safe
// to run even if the ViewModel was detached in the meantime.
extern "C" JNIEXPORT void JNICALL
Java_com_example_aria_demo5_JniBridge_nativeRunMainTasks(JNIEnv* env, jclass) {
    std::vector<std::function<void()>> tasks;
    {
        std::lock_guard<std::mutex> lock(g_mainQueueMutex);
        tasks.swap(g_mainQueue);
    }
    for (auto& task : tasks) {
        task();
    }
}

// ── JNI: execute fetch commands ──────────────────────────────────────────
// Each button calls the unified Command with a different tag.

extern "C" JNIEXPORT void JNICALL
Java_com_example_aria_demo5_JniBridge_nativeFetchViaBlock(JNIEnv* env, jclass) {
    if (g_viewModel) g_viewModel->fetch_cmd.execute("Block");
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_aria_demo5_JniBridge_nativeFetchViaNotification(JNIEnv* env, jclass) {
    if (g_viewModel) g_viewModel->fetch_cmd.execute("Notification");
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_aria_demo5_JniBridge_nativeFetchViaKVO(JNIEnv* env, jclass) {
    if (g_viewModel) g_viewModel->fetch_cmd.execute("KVO");
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_aria_demo5_JniBridge_nativeFetchViaDelegate(JNIEnv* env, jclass) {
    if (g_viewModel) g_viewModel->fetch_cmd.execute("Delegate");
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_aria_demo5_JniBridge_nativeFetchViaTargetAction(JNIEnv* env, jclass) {
    if (g_viewModel) g_viewModel->fetch_cmd.execute("TargetAction");
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_aria_demo5_JniBridge_nativeDismissToast(JNIEnv* env, jclass) {
    if (g_viewModel) g_viewModel->dismiss_toast.execute();
}
