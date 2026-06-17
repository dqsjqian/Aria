package com.example.aria.demo5.viewmodel

import android.util.Log
import androidx.lifecycle.ViewModel
import com.example.aria.demo5.JniBridge
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * MainViewModel — Thin Kotlin shell for Compose + C++ aria ViewModel
 *
 * This ViewModel does NOT contain business logic.
 * All MVVM logic lives in the C++ MainViewModel which uses the aria framework:
 *   - aria::Property<string> for observable state
 *   - aria::Command<string> for actions
 *   - aria::binding::ViewModel as base class
 *
 * This Kotlin ViewModel only:
 *   1. Holds MutableStateFlows that Compose observes
 *   2. Receives property change notifications from C++ via JniBridge
 *   3. Delegates button clicks to C++ Commands via JniBridge
 *
 * Data flow:
 *   Compose button → JniBridge.fetchViaXxx() → C++ Command.execute()
 *   → C++ Property.set() → on_changed → JNI callback →
 *   JniBridge.onPropertyChanged → this.onPropertyChangedFromCpp →
 *   MutableStateFlow → Compose recomposition
 */
class MainViewModel : ViewModel() {

    companion object {
        private const val TAG = "MainViewModel"
    }

    // ── Observable state (mirrors C++ aria::Property) ─────────────────────
    private val _statusText = MutableStateFlow("Tap a button")
    val statusText = _statusText.asStateFlow()

    private val _resultText = MutableStateFlow("")
    val resultText = _resultText.asStateFlow()

    private val _isLoading = MutableStateFlow(false)
    val isLoading = _isLoading.asStateFlow()

    private val _toastMessage = MutableStateFlow("")
    val toastMessage = _toastMessage.asStateFlow()

    init {
        // Attach this ViewModel to the JNI bridge so C++ property changes
        // flow into our StateFlows
        JniBridge.attachViewModel(this)
        Log.d(TAG, "Kotlin ViewModel attached to C++ aria ViewModel")
    }

    override fun onCleared() {
        JniBridge.detachViewModel()
        Log.d(TAG, "Kotlin ViewModel detached from C++ aria ViewModel")
        super.onCleared()
    }

    // ── Called by JniBridge when C++ Property changes ──────────────────────
    fun onPropertyChangedFromCpp(propName: String, newValue: String) {
        Log.d(TAG, "Property from C++: $propName = $newValue")
        // MutableStateFlow.value setter is thread-safe, so updates from
        // background JNI threads are safe. Compose will re-compose on
        // the main thread when it collects the next emission.
        if (propName == "status_text") {
            _statusText.value = newValue
        } else if (propName == "result_text") {
            _resultText.value = newValue
        } else if (propName == "is_loading") {
            _isLoading.value = (newValue == "true")
        } else if (propName == "toast_message") {
            _toastMessage.value = newValue
        }
    }

    // ── Command delegates (forward to C++ via JniBridge) ────────────────
    fun fetchViaBlock()        = JniBridge.fetchViaBlock()
    fun fetchViaNotification() = JniBridge.fetchViaNotification()
    fun fetchViaKVO()          = JniBridge.fetchViaKVO()
    fun fetchViaDelegate()     = JniBridge.fetchViaDelegate()
    fun fetchViaTargetAction() = JniBridge.fetchViaTargetAction()
    fun dismissToast()         = JniBridge.dismissToast()
}
