package com.example.aria.demo5

import android.os.Handler
import android.os.Looper
import android.util.Log
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * JniBridge — JNI side-channel bridge between C++ aria ViewModel and Kotlin
 *
 * Architecture:
 *   C++ MainViewModel (aria::Property / Command) → on_changed →
 *   JNI callback → JniBridge.onPropertyChanged →
 *   Kotlin ViewModel MutableStateFlow → Compose recomposition
 *
 * The C++ ViewModel owns ALL business logic. This bridge only:
 *   1. Creates/destroys the C++ ViewModel
 *   2. Receives property change notifications from C++
 *   3. Forwards them to the Kotlin ViewModel's StateFlows
 *   4. Exposes command methods that delegate to C++ Commands
 *
 * Naming: ARIA_JNI_* (framework/tech names, not ANDROID_*)
 */
object JniBridge {
    private const val TAG = "JniBridge"

    // ── Reference to the Kotlin ViewModel for forwarding ──────────────────
    private var viewModel: com.example.aria.demo5.viewmodel.MainViewModel? = null

    // ── Main-thread handler ───────────────────────────────────────────────
    // The C++ aria reactive Graph is single-threaded and owned by the main
    // thread (where the ViewModel is created). Background work (the DataModel
    // fetch) parks Property::set calls in a native queue and calls postToMain()
    // to drain them here — on the main thread — keeping the Graph contract.
    private val mainHandler = Handler(Looper.getMainLooper())

    init {
        System.loadLibrary("aria_jni")
    }

    /**
     * Called from C++ (on a background thread) to request that the native
     * main-thread task queue be drained on the main thread.
     */
    @JvmStatic
    fun postToMain() {
        mainHandler.post { nativeRunMainTasks() }
    }

    // ── Lifecycle ──────────────────────────────────────────────────────────

    fun attachViewModel(vm: com.example.aria.demo5.viewmodel.MainViewModel) {
        viewModel = vm
        nativeCreateViewModel()
    }

    fun detachViewModel() {
        nativeDestroyViewModel()
        viewModel = null
    }

    // ── JNI callbacks (called from C++ on property change) ────────────────

    /**
     * Called from C++ when a ViewModel property changes.
     * This is the JNI side-channel entry point.
     *
     * @param propName  The name of the changed property (e.g. "status_text", "result_text")
     * @param newValue  The new value of the property
     */
    @JvmStatic
    fun onPropertyChanged(propName: String, newValue: String?) {
        Log.d(TAG, "Property changed: $propName = $newValue")
        viewModel?.onPropertyChangedFromCpp(propName, newValue ?: "")
    }

    // ── Command delegates (forward to C++ Commands) ──────────────────────

    fun fetchViaBlock() = nativeFetchViaBlock()
    fun fetchViaNotification() = nativeFetchViaNotification()
    fun fetchViaKVO() = nativeFetchViaKVO()
    fun fetchViaDelegate() = nativeFetchViaDelegate()
    fun fetchViaTargetAction() = nativeFetchViaTargetAction()
    fun dismissToast() = nativeDismissToast()

    // ── JNI native method declarations ────────────────────────────────────

    @JvmStatic external fun nativeCreateViewModel()
    @JvmStatic external fun nativeDestroyViewModel()
    @JvmStatic external fun nativeRunMainTasks()
    @JvmStatic external fun nativeFetchViaBlock()
    @JvmStatic external fun nativeFetchViaNotification()
    @JvmStatic external fun nativeFetchViaKVO()
    @JvmStatic external fun nativeFetchViaDelegate()
    @JvmStatic external fun nativeFetchViaTargetAction()
    @JvmStatic external fun nativeDismissToast()
}
