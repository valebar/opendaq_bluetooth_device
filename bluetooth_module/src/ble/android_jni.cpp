#include "android_jni.h"

#include <bluetooth_module/ble/ble_central.h>

#include <pthread.h>

#include <atomic>
#include <mutex>

namespace opendaq_bt::ble::jni
{

namespace
{

std::atomic<JavaVM*> javaVm{nullptr};

// Readers take local references before reinitialization can delete the global one.
std::mutex appContextMutex;
jobject appContext = nullptr;

// Only threads attached here receive this key and are detached on exit.
pthread_key_t detachKey;
std::atomic<bool> detachKeyReady{false};

void detachOnThreadExit(void* /*value*/)
{
    if (auto* const vm = javaVm.load(std::memory_order_acquire))
        vm->DetachCurrentThread();
}

void ensureDetachKey()
{
    if (!detachKeyReady.load(std::memory_order_acquire))
    {
        if (pthread_key_create(&detachKey, detachOnThreadExit) == 0)
            detachKeyReady.store(true, std::memory_order_release);
    }
}

}

ScopedEnv::ScopedEnv()
{
    auto* const vm = javaVm.load(std::memory_order_acquire);
    if (vm == nullptr)
        return;

    JNIEnv* attached = nullptr;
    const auto status = vm->GetEnv(reinterpret_cast<void**>(&attached), JNI_VERSION_1_6);

    if (status == JNI_OK)
    {
        // Borrow environments owned by the JVM.
        env = attached;
        return;
    }

    if (status != JNI_EDETACHED)
        return;

    if (vm->AttachCurrentThread(&attached, nullptr) != JNI_OK)
        return;

    if (detachKeyReady.load(std::memory_order_acquire))
        pthread_setspecific(detachKey, reinterpret_cast<void*>(1));

    env = attached;
}

bool isInitialized()
{
    if (javaVm.load(std::memory_order_acquire) == nullptr)
        return false;

    std::scoped_lock lock(appContextMutex);
    return appContext != nullptr;
}

jobject applicationContext(JNIEnv* env)
{
    if (env == nullptr)
        return nullptr;

    std::scoped_lock lock(appContextMutex);
    return appContext == nullptr ? nullptr : env->NewLocalRef(appContext);
}

void onLibraryLoaded(JavaVM* vm)
{
    if (vm == nullptr)
        return;

    javaVm.store(vm, std::memory_order_release);
    ensureDetachKey();
}

void setApplicationContext(JNIEnv* env, jobject context)
{
    if (env == nullptr)
        return;

    // ModuleManager may dlopen the module without invoking JNI_OnLoad.
    if (javaVm.load(std::memory_order_acquire) == nullptr)
    {
        JavaVM* vm = nullptr;
        if (env->GetJavaVM(&vm) == JNI_OK)
            javaVm.store(vm, std::memory_order_release);
    }

    ensureDetachKey();

    const auto replacement = context == nullptr ? nullptr : env->NewGlobalRef(context);

    std::scoped_lock lock(appContextMutex);
    if (appContext != nullptr)
        env->DeleteGlobalRef(appContext);
    appContext = replacement;
}

int sdkInt()
{
    ScopedEnv env;
    if (!env)
        return 0;

    const jclass versionClass = env->FindClass("android/os/Build$VERSION");
    if (versionClass == nullptr)
    {
        takeException(env.get());
        return 0;
    }

    const jfieldID field = env->GetStaticFieldID(versionClass, "SDK_INT", "I");
    if (field == nullptr)
    {
        takeException(env.get());
        env->DeleteLocalRef(versionClass);
        return 0;
    }

    const auto value = static_cast<int>(env->GetStaticIntField(versionClass, field));
    env->DeleteLocalRef(versionClass);
    return value;
}

std::string takeException(JNIEnv* env)
{
    if (env == nullptr || env->ExceptionCheck() == JNI_FALSE)
        return {};

    const jthrowable thrown = env->ExceptionOccurred();
    env->ExceptionClear();

    if (thrown == nullptr)
        return {};

    std::string message;

    // Failure-only reflection avoids another cached method.
    if (const jclass throwableClass = env->FindClass("java/lang/Throwable"))
    {
        if (const jmethodID toString =
                env->GetMethodID(throwableClass, "toString", "()Ljava/lang/String;"))
        {
            const auto text = static_cast<jstring>(env->CallObjectMethod(thrown, toString));

            // Drop an exception from Throwable.toString().
            if (env->ExceptionCheck() == JNI_TRUE)
                env->ExceptionClear();
            else if (text != nullptr)
            {
                message = toStdString(env, text);
                env->DeleteLocalRef(text);
            }
        }

        env->DeleteLocalRef(throwableClass);
    }

    env->DeleteLocalRef(thrown);
    return message;
}

void throwIfException(JNIEnv* env, const std::string& context)
{
    const auto message = takeException(env);
    if (message.empty())
        return;

    throw BleError(context + ": " + message);
}

std::string toStdString(JNIEnv* env, jstring value)
{
    if (env == nullptr || value == nullptr)
        return {};

    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (chars == nullptr)
        return {};

    std::string result(chars, static_cast<size_t>(env->GetStringUTFLength(value)));
    env->ReleaseStringUTFChars(value, chars);
    return result;
}

jstring toJavaString(JNIEnv* env, const std::string& value)
{
    return env == nullptr ? nullptr : env->NewStringUTF(value.c_str());
}

std::vector<uint8_t> toBytes(JNIEnv* env, jbyteArray value)
{
    if (env == nullptr || value == nullptr)
        return {};

    const auto length = env->GetArrayLength(value);
    if (length <= 0)
        return {};

    std::vector<uint8_t> result(static_cast<size_t>(length));
    env->GetByteArrayRegion(value, 0, length, reinterpret_cast<jbyte*>(result.data()));
    return result;
}

jbyteArray toJavaBytes(JNIEnv* env, const std::vector<uint8_t>& value)
{
    if (env == nullptr)
        return nullptr;

    const jbyteArray result = env->NewByteArray(static_cast<jsize>(value.size()));
    if (result == nullptr)
        return nullptr;

    if (!value.empty())
    {
        env->SetByteArrayRegion(result, 0, static_cast<jsize>(value.size()),
                                reinterpret_cast<const jbyte*>(value.data()));
    }

    return result;
}

}

// Optional because ModuleManager may load the library through dlopen.
extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* /*reserved*/)
{
    opendaq_bt::ble::jni::onLibraryLoaded(vm);
    return JNI_VERSION_1_6;
}
