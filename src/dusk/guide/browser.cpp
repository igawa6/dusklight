#include "dusk/guide/browser.hpp"

#if defined(__ANDROID__)
#include <SDL3/SDL_system.h>
#include <android/log.h>
#include <jni.h>
#endif

namespace dusk::guide {

#if defined(__ANDROID__)

// Opens the in-app browser (DuskGuideBrowser.java) at `url`.
//
// A WebView rather than an HTTP GET because the sites worth reading sit behind
// bot protection: zeldadungeon.net answers a plain client with 403, robots.txt
// included. A WebView is a real browser, so it is served normally without
// pretending to be anything it is not.
#define GLOG(...) __android_log_print(ANDROID_LOG_INFO, "dusk-guide", __VA_ARGS__)

bool open_browser(const char* url) {
    GLOG("open_browser: entry");
    JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
    jobject activity = (jobject)SDL_GetAndroidActivity();
    if (env == nullptr || activity == nullptr) {
        GLOG("open_browser: no JNIEnv (%p) or activity (%p)", (void*)env, (void*)activity);
        return false;
    }
    // Resolved through the activity's own class loader: FindClass on a
    // non-main thread would miss app classes (same reason http/android.cpp
    // does this).
    // Local refs are freed on EVERY path below, not just the happy one. The
    // frame holds 512 by default and this is called once per button press, so
    // leaking four each time aborts the VM after ~128 opens.
    struct LocalRefs {
        JNIEnv* env;
        jobject r[4]{};
        int n = 0;
        jobject keep(jobject o) { if (o != nullptr && n < 4) { r[n++] = o; } return o; }
        ~LocalRefs() { for (int i = 0; i < n; i++) { env->DeleteLocalRef(r[i]); } }
    } refs{env};

    auto activityClass = (jclass)refs.keep(env->GetObjectClass(activity));
    if (activityClass == nullptr) {
        env->ExceptionClear();
        GLOG("open_browser: no activity class");
        return false;
    }
    jmethodID getClassLoader =
        env->GetMethodID(activityClass, "getClassLoader", "()Ljava/lang/ClassLoader;");
    if (getClassLoader == nullptr) {
        env->ExceptionClear();
        GLOG("open_browser: no getClassLoader");
        return false;
    }
    jobject loader = refs.keep(env->CallObjectMethod(activity, getClassLoader));
    auto loaderClass = (jclass)refs.keep(env->FindClass("java/lang/ClassLoader"));
    jmethodID loadClass =
        env->GetMethodID(loaderClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    if (loader == nullptr || loaderClass == nullptr || loadClass == nullptr) {
        env->ExceptionClear();
        GLOG("open_browser: class loader unavailable");
        return false;
    }
    jstring name = env->NewStringUTF("dev.twilitrealm.dusk.DuskGuideBrowser");
    auto browserClass = (jclass)refs.keep(env->CallObjectMethod(loader, loadClass, name));
    env->DeleteLocalRef(name);
    if (env->ExceptionCheck()) {
        // Must clear BEFORE any further JNI call; leaving a pending exception
        // makes everything after it undefined.
        env->ExceptionDescribe();
        env->ExceptionClear();
        GLOG("open_browser: loadClass threw");
        return false;
    }
    if (browserClass == nullptr) {
        GLOG("open_browser: class not found");
        return false;
    }
    jmethodID openMethod = env->GetStaticMethodID(
        browserClass, "open", "(Landroid/app/Activity;Ljava/lang/String;)V");
    if (openMethod == nullptr) {
        env->ExceptionClear();
        GLOG("open_browser: open() method not found");
        return false;
    }
    jstring jurl = env->NewStringUTF(url != nullptr ? url : "");
    env->CallStaticVoidMethod(browserClass, openMethod, activity, jurl);
    env->DeleteLocalRef(jurl);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        GLOG("open_browser: open() threw");
        return false;
    }
    GLOG("open_browser: dispatched OK");
    return true;
}

bool browser_available() {
    return true;
}

#else

// Desktop has no in-app browser: the user already has a real one, and the
// import folder is trivially reachable there.
bool open_browser(const char*) {
    return false;
}

bool browser_available() {
    return false;
}

#endif

}  // namespace dusk::guide
