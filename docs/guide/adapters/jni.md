# JNI / Android Adapter

Integrating Aria with Android applications via JNI + Jetpack Compose.

## Architecture Overview

```
┌─────────────────────────────────────────────────┐
│  Compose UI                                     │
│  ┌──────────┐  collects  ┌──────────────────┐   │
│  │ MainScreen│ ◄──────── │ Kotlin ViewModel │   │
│  └──────────┘            │  (thin shell)    │   │
│                          └────────┬─────────┘   │
│                                   │ StateFlow    │
│                          ┌────────▼─────────┐   │
│                          │   JniBridge.kt   │   │
│                          └────────┬─────────┘   │
└───────────────────────────────────┼─────────────┘
                                    │ JNI callbacks
┌───────────────────────────────────┼─────────────┐
│  C++ (Aria Framework)             │             │
│                          ┌────────▼─────────┐   │
│                          │  jni_bridge.cpp   │   │
│                          │  (side-channel)   │   │
│                          └────────┬─────────┘   │
│                          ┌────────▼─────────┐   │
│                          │  MainViewModel    │   │
│                          │  (aria::binding)  │   │
│                          └────────┬─────────┘   │
│                          ┌────────▼─────────┐   │
│                          │  DataModel        │   │
│                          │  (async logic)    │   │
│                          └──────────────────┘   │
└─────────────────────────────────────────────────┘
```

## Side-Channel Bridge Pattern

The JNI adapter uses a **side-channel** pattern to push property changes from C++ to Kotlin:

1. C++ `MainViewModel` owns `Property<string>` and `Property<bool>` instances
2. Each property registers an `on_changed` callback via `subscribe()`
3. The callback invokes a JNI function that updates a Kotlin `MutableStateFlow`
4. Compose observes the `StateFlow` and recomposes automatically

This avoids polling and keeps the C++ ViewModel as the single source of truth.

## Building

Prerequisites:
- Android NDK 29+ (Clang 20)
- CMake 3.22+ (bundled with Android SDK)
- Aria pre-built for `arm64-v8a` at `build/platforms/android/`

```bash
# From project root — build Aria for Android
./scripts/build.sh android

# Build and install the demo
cd examples/5-android-jni-mvvm
./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.example.aria.demo5/.MainActivity
```

## Key Files

| File | Purpose |
|------|---------|
| `cpp/MainViewModel.cpp/hpp` | Aria C++ ViewModel with reactive Properties |
| `cpp/DataModel.cpp/h` | Business logic (async greeting, counter) |
| `cpp/jni_bridge.cpp` | Side-channel: C++ → JNI → Kotlin StateFlow |
| `JniBridge.kt` | Kotlin-side JNI interface + StateFlow holders |
| `MainViewModel.kt` | Thin Kotlin ViewModel delegating to C++ |
| `MainScreen.kt` | Compose UI collecting from StateFlows |

See also: [Demo 5 — Android JNI MVVM](../../../examples/5-android-jni-mvvm/)
