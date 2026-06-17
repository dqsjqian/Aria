package com.example.aria.demo5

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import com.example.aria.demo5.ui.Demo5Theme
import com.example.aria.demo5.ui.MainScreen

/**
 * Demo5 — Android JNI + Jetpack Compose MVVM
 *
 * Architecture mirrors iOS demo3:
 *   View (Compose) → ViewModel → C++ DataModel (via JNI)
 *
 * Key difference from iOS:
 *   iOS uses UIKit (UIViewController + RootView)
 *   Android uses Compose (no View objects in the traditional sense)
 *
 * Compose + JNI side-channel bridge:
 *   C++ ViewModel property changes → JNI callback →
 *   Kotlin MutableStateFlow → Compose recomposition
 *   (Bypasses JniAdapter's IView binding — Compose has no traditional View objects)
 *
 * Naming convention: ARIA_JNI_* (framework/tech names, not ANDROID_*)
 * Consistent with project convention (JNI, HTTP, QT6, APPKIT, UIKIT)
 */
class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // Load JNI library (aria_jni, not aria_android)
        System.loadLibrary("aria_jni")

        setContent {
            Demo5Theme {
                MainScreen()
            }
        }
    }
}
