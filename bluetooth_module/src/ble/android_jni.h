#pragma once

// Shared JNI plumbing for the Android backend.

#include <jni.h>

#include <string>
#include <vector>

namespace opendaq_bt::ble::jni
{

/*! Attaches the current thread when needed; attached threads detach at thread exit. */
class ScopedEnv
{
public:
    ScopedEnv();

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

    JNIEnv* get() const { return env; }
    JNIEnv* operator->() const { return env; }
    explicit operator bool() const { return env != nullptr; }

private:
    JNIEnv* env = nullptr;
};

/*! Whether OpenDaqBluetooth.initialize(Context) supplied a VM and Context. */
bool isInitialized();

/*! Returns an owned local Context reference, safe across concurrent reinitialization. */
jobject applicationContext(JNIEnv* env);

/*! Stores the application Context and the JavaVM. Called from the Java shim's initialize(). */
void setApplicationContext(JNIEnv* env, jobject context);

/*! Records the JavaVM. Called from JNI_OnLoad; see the note there for why it is not load-bearing. */
void onLibraryLoaded(JavaVM* vm);

/*! android.os.Build.VERSION.SDK_INT of the running device, or 0 if it cannot be read. */
int sdkInt();

/*! Clears a Java exception and returns its toString(), or an empty string. */
std::string takeException(JNIEnv* env);

/*! Clears a pending Java exception and throws it as a BleError prefixed with @p context. */
void throwIfException(JNIEnv* env, const std::string& context);

std::string toStdString(JNIEnv* env, jstring value);

/*! Returns a local reference the caller owns. */
jstring toJavaString(JNIEnv* env, const std::string& value);

std::vector<uint8_t> toBytes(JNIEnv* env, jbyteArray value);

/*! Returns a local reference the caller owns. */
jbyteArray toJavaBytes(JNIEnv* env, const std::vector<uint8_t>& value);

}
