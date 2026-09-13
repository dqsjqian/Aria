#pragma once

#include <jni.h>

#include <cstdarg>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Small JNI function table, used on both the NDK target and a host compiled
// against the NDK header. It tracks ownership and pending-exception misuse.
struct FakeJni {
    struct String : _jstring { std::u16string value; std::string bytes; std::vector<jchar> units; };
    struct View : _jobject { std::u16string text; jint progress = 0; jfloat rating = 0; jboolean checked = false; };
    struct Method { std::string name; };
    JNINativeInterface functions{};
    JNIInvokeInterface invocation{};
    JNIEnv env{&functions};
    JavaVM vm{&invocation};
    View view;
    _jclass cls;
    std::unordered_map<jobject, std::unique_ptr<String>> strings;
    std::unordered_map<std::string, std::unique_ptr<Method>> methods;
    int globals = 0, attaches = 0, detaches = 0, leased_chars = 0;
    int calls = 0, calls_with_pending = 0;
    bool attached = true, pending = false, fail_call = false;
    std::string missing_method;
    std::function<void()> on_call;
    inline static thread_local FakeJni* current = nullptr;

    static FakeJni& self() { return *current; }
    static void check() { if (self().pending) ++self().calls_with_pending; }
    String* string(std::u16string value) {
        auto owner = std::make_unique<String>();
        owner->value = std::move(value);
        auto* raw = owner.get();
        strings.emplace(raw, std::move(owner));
        return raw;
    }
    FakeJni() {
        current = this;
        invocation.GetEnv = [](JavaVM*, void** result, jint) -> jint {
            if (!self().attached) { *result = nullptr; return JNI_EDETACHED; }
            *result = &self().env; return JNI_OK;
        };
        invocation.AttachCurrentThreadAsDaemon = [](JavaVM*, JNIEnv** result, void*) -> jint {
            ++self().attaches; self().attached = true; *result = &self().env; return JNI_OK;
        };
        invocation.DetachCurrentThread = [](JavaVM*) -> jint {
            ++self().detaches; self().attached = false; return JNI_OK;
        };
        functions.GetJavaVM = [](JNIEnv*, JavaVM** result) -> jint { *result = &self().vm; return JNI_OK; };
        functions.NewGlobalRef = [](JNIEnv*, jobject object) -> jobject { check(); ++self().globals; return object; };
        functions.DeleteGlobalRef = [](JNIEnv*, jobject) { --self().globals; };
        functions.NewLocalRef = [](JNIEnv*, jobject object) -> jobject { check(); return object; };
        functions.GetObjectClass = [](JNIEnv*, jobject) -> jclass { check(); return &self().cls; };
        functions.DeleteLocalRef = [](JNIEnv*, jobject object) { self().strings.erase(object); };
        functions.GetMethodID = [](JNIEnv*, jclass, const char* name, const char*) -> jmethodID {
            check();
            if (self().missing_method == name) { self().pending = true; return nullptr; }
            auto& value = self().methods[name];
            if (!value) value = std::make_unique<Method>(Method{name});
            return reinterpret_cast<jmethodID>(value.get());
        };
        functions.ExceptionCheck = [](JNIEnv*) -> jboolean { return self().pending; };
        functions.ExceptionClear = [](JNIEnv*) { self().pending = false; };
        functions.ExceptionDescribe = [](JNIEnv*) {};
        functions.NewString = [](JNIEnv*, const jchar* text, jsize length) -> jstring {
            check();
            std::u16string value;
            for (jsize i = 0; i < length; ++i) value.push_back(static_cast<char16_t>(text[i]));
            return self().string(std::move(value));
        };
        functions.NewStringUTF = [](JNIEnv*, const char* text) -> jstring {
            check();
            // ASCII subset suffices to reproduce the old embedded-NUL loss.
            std::u16string value;
            for (; *text; ++text) value.push_back(static_cast<unsigned char>(*text));
            return self().string(std::move(value));
        };
        functions.GetStringLength = [](JNIEnv*, jstring value) -> jsize {
            check(); return static_cast<jsize>(static_cast<String*>(value)->value.size());
        };
        functions.GetStringChars = [](JNIEnv*, jstring value, jboolean*) -> const jchar* {
            check(); ++self().leased_chars;
            auto* string = static_cast<String*>(value);
            string->units.assign(string->value.begin(), string->value.end());
            // JNI may return a non-null pointer for an empty string.
            if (string->units.empty()) string->units.push_back(0);
            return string->units.data();
        };
        functions.ReleaseStringChars = [](JNIEnv*, jstring, const jchar*) { --self().leased_chars; };
        functions.GetStringUTFChars = [](JNIEnv*, jstring value, jboolean*) -> const char* {
            check(); ++self().leased_chars;
            auto* string = static_cast<String*>(value);
            string->bytes.clear();
            for (auto c : string->value) string->bytes.push_back(static_cast<char>(c));
            return string->bytes.c_str();
        };
        functions.ReleaseStringUTFChars = [](JNIEnv*, jstring, const char*) { --self().leased_chars; };
        functions.CallVoidMethodV = [](JNIEnv*, jobject object, jmethodID id, va_list args) {
            check(); ++self().calls;
            const auto name = reinterpret_cast<Method*>(id)->name;
            if (name == "setText") static_cast<View*>(object)->text = static_cast<String*>(va_arg(args, jstring))->value;
            else if (name == "setProgress") static_cast<View*>(object)->progress = va_arg(args, jint);
            else if (name == "setRating") static_cast<View*>(object)->rating = static_cast<jfloat>(va_arg(args, double));
            else if (name == "setChecked") static_cast<View*>(object)->checked = static_cast<jboolean>(va_arg(args, int));
            if (self().on_call) { auto callback = self().on_call; callback(); }
            if (self().fail_call) self().pending = true;
        };
        functions.CallObjectMethodV = [](JNIEnv*, jobject object, jmethodID id, va_list) -> jobject {
            check(); ++self().calls;
            if (self().fail_call) { self().pending = true; return nullptr; }
            if (reinterpret_cast<Method*>(id)->name == "getText") return self().string(static_cast<View*>(object)->text);
            return self().string(static_cast<String*>(object)->value);
        };
        functions.CallIntMethodV = [](JNIEnv*, jobject object, jmethodID, va_list) -> jint {
            check(); if (self().fail_call) self().pending = true; return static_cast<View*>(object)->progress;
        };
        functions.CallBooleanMethodV = [](JNIEnv*, jobject object, jmethodID, va_list) -> jboolean {
            check(); if (self().fail_call) self().pending = true; return static_cast<View*>(object)->checked;
        };
        functions.CallFloatMethodV = [](JNIEnv*, jobject object, jmethodID, va_list) -> jfloat {
            check(); if (self().fail_call) self().pending = true; return static_cast<View*>(object)->rating;
        };
    }
    ~FakeJni() { current = nullptr; }
};
