#pragma once

#include "aria/callback_boundary.hpp"

#include <jni.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aria::adapters::jni::detail {

inline bool check_exception(JNIEnv* env, std::string_view operation) noexcept {
    if (!env || !env->ExceptionCheck()) return false;
    env->ExceptionDescribe();
    env->ExceptionClear();
    ::aria::report_callback_failure("jni.exception", operation);
    return true;
}

inline JavaVM* vm_of(JNIEnv* env) noexcept {
    if (!env || check_exception(env, "enter JNI operation")) return nullptr;
    JavaVM* vm = nullptr;
    if (env->GetJavaVM(&vm) != JNI_OK) return nullptr;
    return vm;
}

// Never retain a JNIEnv across calls/threads. Only detach attachments created
// by this scope; a Java-owned thread keeps its original attachment.
class Env {
public:
    explicit Env(JavaVM* vm) noexcept : vm_(vm) {
        if (!vm_) return;
        const auto status = vm_->GetEnv(reinterpret_cast<void**>(&env_), JNI_VERSION_1_6);
        if (status == JNI_EDETACHED) {
            attached_ = attach_(vm_, &env_) == JNI_OK;
            if (!attached_) env_ = nullptr;
        } else if (status != JNI_OK) env_ = nullptr;
    }
    ~Env() { if (attached_) vm_->DetachCurrentThread(); }
    Env(const Env&) = delete;
    Env& operator=(const Env&) = delete;
    [[nodiscard]] JNIEnv* get() const noexcept { return env_; }
private:
    template<class VM>
    static jint attach_(VM* vm, JNIEnv** env) noexcept {
        // The NDK's C++ wrapper takes JNIEnv**, the desktop JDK takes void**.
        if constexpr (requires { vm->AttachCurrentThreadAsDaemon(env, nullptr); })
            return vm->AttachCurrentThreadAsDaemon(env, nullptr);
        else return vm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(env), nullptr);
    }
    JavaVM* vm_ = nullptr;
    JNIEnv* env_ = nullptr;
    bool attached_ = false;
};

template<class T>
class LocalRef {
public:
    LocalRef(JNIEnv* env, T value) noexcept : env_(env), value_(value) {}
    ~LocalRef() { if (value_) env_->DeleteLocalRef(value_); }
    LocalRef(const LocalRef&) = delete;
    LocalRef& operator=(const LocalRef&) = delete;
    [[nodiscard]] T get() const noexcept { return value_; }
private:
    JNIEnv* env_;
    T value_;
};

inline void delete_global_ref(JavaVM* vm, jobject object) noexcept {
    if (!object) return;
    Env scope(vm);
    if (auto* env = scope.get()) env->DeleteGlobalRef(object);
    else ::aria::report_callback_failure("jni.global_ref", "could not attach thread to release global reference");
}

inline jmethodID method(JNIEnv* env, jobject object, const char* name, const char* signature) {
    if (!env || !object || check_exception(env, name)) return nullptr;
    LocalRef<jclass> cls(env, env->GetObjectClass(object));
    if (check_exception(env, "GetObjectClass") || !cls.get()) return nullptr;
    auto id = env->GetMethodID(cls.get(), name, signature);
    if (check_exception(env, name)) return nullptr;
    return id;
}

// Public strings use standard UTF-8. JNI's *UTF routines use modified UTF-8,
// so cross the boundary through UTF-16 instead. Malformed input is replaced
// with U+FFFD, including isolated Java surrogate code units on the read path.
inline jstring to_jstring(JNIEnv* env, std::string_view text) {
    std::vector<jchar> units;
    units.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i]);
        std::uint32_t scalar = first;
        std::size_t count = 1;
        if (first >= 0xc2 && first <= 0xdf) { scalar = first & 0x1f; count = 2; }
        else if (first >= 0xe0 && first <= 0xef) { scalar = first & 0x0f; count = 3; }
        else if (first >= 0xf0 && first <= 0xf4) { scalar = first & 0x07; count = 4; }
        else if (first >= 0x80) scalar = 0xfffd;
        bool valid = count <= text.size() - i;
        for (std::size_t j = 1; valid && j < count; ++j) {
            const auto byte = static_cast<unsigned char>(text[i + j]);
            valid = (byte & 0xc0) == 0x80;
            scalar = (scalar << 6) | (byte & 0x3f);
        }
        valid = valid && scalar <= 0x10ffff && !(scalar >= 0xd800 && scalar <= 0xdfff)
            && (count != 2 || scalar >= 0x80) && (count != 3 || scalar >= 0x800)
            && (count != 4 || scalar >= 0x10000);
        if (!valid) { scalar = 0xfffd; count = 1; }
        i += count;
        if (scalar <= 0xffff) units.push_back(static_cast<jchar>(scalar));
        else {
            scalar -= 0x10000;
            units.push_back(static_cast<jchar>(0xd800 + (scalar >> 10)));
            units.push_back(static_cast<jchar>(0xdc00 + (scalar & 0x3ff)));
        }
    }
    if (units.size() > static_cast<std::size_t>(std::numeric_limits<jsize>::max()))
        throw std::length_error("JNI string exceeds jsize range");
    auto result = env->NewString(units.data(), static_cast<jsize>(units.size()));
    if (check_exception(env, "NewString")) {
        if (result) env->DeleteLocalRef(result);
        return nullptr;
    }
    return result;
}

inline std::string from_jstring(JNIEnv* env, jstring value) {
    if (!value) return {};
    const jsize size = env->GetStringLength(value);
    if (check_exception(env, "GetStringLength")) return {};
    const jchar* chars = env->GetStringChars(value, nullptr);
    struct Lease {
        JNIEnv* env; jstring value; const jchar* chars;
        ~Lease() { if (chars) env->ReleaseStringChars(value, chars); }
    } lease{env, value, chars};
    if (check_exception(env, "GetStringChars") || !chars) return {};
    std::string result;
    result.reserve(static_cast<std::size_t>(size));
    for (jsize i = 0; i < size; ++i) {
        std::uint32_t scalar = chars[i];
        if (scalar >= 0xd800 && scalar <= 0xdbff && i + 1 < size
            && chars[i + 1] >= 0xdc00 && chars[i + 1] <= 0xdfff) {
            scalar = 0x10000 + ((scalar - 0xd800) << 10) + chars[++i] - 0xdc00;
        } else if (scalar >= 0xd800 && scalar <= 0xdfff) scalar = 0xfffd;
        if (scalar < 0x80) result.push_back(static_cast<char>(scalar));
        else if (scalar < 0x800) {
            result.push_back(static_cast<char>(0xc0 | (scalar >> 6)));
            result.push_back(static_cast<char>(0x80 | (scalar & 0x3f)));
        } else if (scalar < 0x10000) {
            result.push_back(static_cast<char>(0xe0 | (scalar >> 12)));
            result.push_back(static_cast<char>(0x80 | ((scalar >> 6) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | (scalar & 0x3f)));
        } else {
            result.push_back(static_cast<char>(0xf0 | (scalar >> 18)));
            result.push_back(static_cast<char>(0x80 | ((scalar >> 12) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | ((scalar >> 6) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | (scalar & 0x3f)));
        }
    }
    return result;
}

} // namespace aria::adapters::jni::detail
