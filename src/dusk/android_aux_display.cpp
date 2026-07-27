#if defined(TARGET_ANDROID) || defined(__ANDROID__) || defined(ANDROID)

#include "dusk/companion.h"
#include "dusk/dualscreen.h"

#include <android/native_window_jni.h>
#include <jni.h>

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
