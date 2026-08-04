#if defined(TARGET_ANDROID) || defined(__ANDROID__) || defined(ANDROID)

#include "dusk/companion.h"
#include "dusk/guide/fetch.hpp"
#include "dusk/guide/store.hpp"
#include "dusk/dualscreen.h"

#include <SDL3/SDL_system.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <string>

#include <aurora/aux_window.hpp>

// The AYN Thor-style dual-screen path: DuskActivity shows a Presentation with
// a SurfaceView on the secondary display and forwards its Surface here.

extern "C" JNIEXPORT void JNICALL Java_dev_twilitrealm_dusk_DuskActivity_nativeAuxSurfaceChanged(
    JNIEnv* env, jclass, jobject surface, jint width, jint height)
{
    if (surface != nullptr) {
        // ANativeWindow_fromSurface acquires a reference; aurora takes
        // ownership and releases it on detach/replace.
        ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
        aurora::auxwin::set_native_window(window, static_cast<uint32_t>(width),
            static_cast<uint32_t>(height));
    } else {
        aurora::auxwin::set_native_window(nullptr, 0, 0);
    }
}

extern "C" JNIEXPORT void JNICALL Java_dev_twilitrealm_dusk_DuskActivity_nativeCompanionTouchEvent(
    JNIEnv*, jclass, jint action, jfloat u, jfloat v)
{
    dusk::companion::touchEvent(action, u, v);
}

// Native -> Java, the only call in this file that goes that way. The launcher
// activity runs before this library is loaded, so the swap choice has to be
// waiting for it in SharedPreferences rather than in the config it cannot read.
namespace dusk::dualscreen {

void publishSwapPreference(bool swapped)
{
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (env == nullptr || activity == nullptr) {
        return;
    }
    // SDL hands back a LOCAL reference; leaking one per call would exhaust the
    // local frame, and this runs again on every toggle.
    jclass cls = env->GetObjectClass(activity);
    jmethodID method = cls != nullptr
        ? env->GetMethodID(cls, "publishSwapPreference", "(Z)V")
        : nullptr;
    if (method != nullptr) {
        env->CallVoidMethod(activity, method, swapped ? JNI_TRUE : JNI_FALSE);
    }
    // A missing method leaves a pending NoSuchMethodError that would abort at
    // the next JNI call — clear it and carry on unswapped rather than crash.
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
    if (cls != nullptr) {
        env->DeleteLocalRef(cls);
    }
    env->DeleteLocalRef(activity);
}

}  // namespace dusk::dualscreen

// The guide store's location is decided natively (it follows a custom data
// folder now, and only falls back to app-specific external storage when the
// data folder is at its default). DuskGuideBrowser used to hard-code the
// fallback with a comment saying it "must match guides_root()" — which stopped
// being true the moment the data folder became configurable, so pages saved
// from the browser landed somewhere the importer never looked. Asking is the
// only version of this that cannot drift.
extern "C" JNIEXPORT jstring JNICALL
Java_dev_twilitrealm_dusk_DuskGuideBrowser_nativeGuidesRoot(JNIEnv* env, jclass)
{
    std::string path;
    try {
        path = dusk::guide::guides_root().string();
    } catch (...) {
        path.clear();
    }
    return env->NewStringUTF(path.c_str());
}

// Kicked when the browser finishes saving. Without it, pages saved after the
// store stopped being empty were never picked up: begin_import() was reachable
// only while the catalogue had nothing in it, so a 24-page crawl that imported
// its first few mid-crawl left the remaining ~17 sitting in import/ until the
// converter version happened to change. Idempotent — no-ops while an import is
// already running, and the scan itself is on a worker.
extern "C" JNIEXPORT void JNICALL
Java_dev_twilitrealm_dusk_DuskGuideBrowser_nativeGuidesImport(JNIEnv*, jclass)
{
    dusk::guide::begin_import();
}

extern "C" JNIEXPORT void JNICALL Java_dev_twilitrealm_dusk_DuskActivity_nativeCompanionPinch(
    JNIEnv*, jclass, jfloat factor)
{
    dusk::companion::pinchZoom(factor);
}

extern "C" JNIEXPORT void JNICALL
Java_dev_twilitrealm_dusk_DuskActivity_nativeDualScreenAvailable(JNIEnv*, jclass,
    jboolean available)
{
    dusk::dualscreen::setDisplayAvailable(available != JNI_FALSE);
}

extern "C" JNIEXPORT void JNICALL Java_dev_twilitrealm_dusk_DuskActivity_nativeBatteryStatus(
    JNIEnv*, jclass, jint percent, jboolean charging)
{
    dusk::companion::setBatteryStatus(percent, charging != JNI_FALSE);
}

#if DUSK_COMPANION_CAPTURE
// Verification aid: dump what the second screen is presenting to a PNG.
//   adb shell am broadcast -a dev.twilitrealm.dusk.DUMP
// Java supplies the path because getExternalFilesDir() is the only location
// adb can pull from on an unrooted device.
extern "C" JNIEXPORT void JNICALL Java_dev_twilitrealm_dusk_DuskActivity_nativeCompanionScreenshot(
    JNIEnv* env, jclass, jstring path)
{
    if (path == nullptr) {
        dusk::dualscreen::requestScreenshot(nullptr);
        return;
    }
    const char* chars = env->GetStringUTFChars(path, nullptr);
    if (chars == nullptr) {  // OOM
        dusk::dualscreen::requestScreenshot(nullptr);
        return;
    }
    dusk::dualscreen::requestScreenshot(chars);
    env->ReleaseStringUTFChars(path, chars);
}


#endif  // DUSK_COMPANION_CAPTURE

#endif
