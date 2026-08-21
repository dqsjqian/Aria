# JNI / Android Adapter

Aria supports two distinct Android integration shapes. Use the typed `JniAdapter` path for classic Android `View` objects; use a side-channel only when a UI host such as Jetpack Compose has no addressable view object for `BindingEngine` to bind.

## Typed View-backed path

`JniAdapter` implements the same typed `IViewAdapter` surface as the other first-party adapters. Its host-side contract test pins the class shape; the View-backed runtime lab in AriaTools is the behavioral gate still being completed. For Android `View`-backed screens, construct the adapter and wire properties and commands through `BindingEngine`; do not replace typed values with a string-keyed property protocol.

```cpp
// Called on the Android UI thread with real android.view.View objects.
auto adapter = std::make_shared<aria::adapters::jni::JniAdapter>(env);
aria::binding::BindingEngine engine(adapter);

aria::adapters::jni::JniView name_view(env, name_edit_text);
aria::adapters::jni::JniView submit_view(env, submit_button);
aria::adapters::jni::JniView status_view(env, status_text_view);

engine.bind_text(vm.name, name_view);                    // EditText ↔ Property<string>
engine.bind_command(vm.submit, submit_view);             // Button → Command
engine.bind_text_oneway(vm.status, status_view);         // Property/Computed → TextView
```

`JniView` owns a JNI global reference, so its C++ wrapper must follow the native screen's lifetime. The end-to-end View-backed lab belongs to [AriaTools](https://github.com/dqsjqian/AriaTools), Aria's flagship cross-platform application for Qt, iOS, and Android. Its Web experience is a work in progress.

## Compose side-channel path

Compose state does not expose addressable Android `View` instances, so the typed view adapter is not the right bridge for composables. A side-channel can instead:

1. keep reactive state in the C++ ViewModel;
2. subscribe to the relevant properties;
3. forward updates through JNI into typed Kotlin `StateFlow` holders;
4. let Compose collect those flows and recompose.

Scope this bridge to the Compose boundary. It is not a general replacement for `JniAdapter`, and application code should preserve native types rather than funneling unrelated properties through a single string map.

## Building the framework for Android

Prerequisites:

- Android NDK 29+ (Clang 20)
- CMake 3.22+ (bundled with Android SDK)

```bash
./scripts/build.sh android
```

For a runnable Android application and the View-backed typed adapter lab, follow the build instructions in [AriaTools](https://github.com/dqsjqian/AriaTools).
