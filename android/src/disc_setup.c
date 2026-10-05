// The first-run setup's native end (android/java/.../SetupActivity.java): the
// player's disc, once the app copied it into its own folder, is prepared by
// the iOS app's importer (apple/ios/src/disc_import.c, with CommonCrypto's
// SHA-1 from src/compat): it checks the disc id, verifies main.dol against
// USA revision 0 and writes game/main.dol and game/rels, the same bytes the
// PC builder's extraction gives.
#include "disc_import.h"

#include <SDL3/SDL.h>
#include <jni.h>
#include <stdio.h>

typedef struct {
    JNIEnv* env;
    jobject activity;
    jmethodID on_progress;
} Progress;

static void report(void* context, double fraction, const char* stage) {
    (void)stage;
    Progress* p = (Progress*)context;
    (*p->env)->CallVoidMethod(p->env, p->activity, p->on_progress, (jdouble)fraction);
}

// null when game/main.dol and game/rels were written, else a sentence for the player.
JNIEXPORT jstring JNICALL Java_dev_bluewake_android_SetupActivity_nativePrepareDisc(JNIEnv* env, jobject activity,
                                                                                   jstring jiso, jstring jout) {
    const char* iso = (*env)->GetStringUTFChars(env, jiso, NULL);
    const char* out = (*env)->GetStringUTFChars(env, jout, NULL);
    jclass cls = (*env)->GetObjectClass(env, activity);
    Progress progress = {env, activity, (*env)->GetMethodID(env, cls, "onPrepareProgress", "(D)V")};
    char error[512] = {0};
    int result = bluewake_disc_check(iso, error, sizeof error);
    if (result == 0)
        result = bluewake_disc_prepare(iso, out, progress.on_progress ? report : NULL, &progress, error,
                                       sizeof error);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    fprintf(stderr, "[setup] prepare %s: %s\n", iso, result == 0 ? "ok" : error);
    (*env)->ReleaseStringUTFChars(env, jiso, iso);
    (*env)->ReleaseStringUTFChars(env, jout, out);
    (*env)->DeleteLocalRef(env, cls);
    return result == 0 ? NULL : (*env)->NewStringUTF(env, error);
}

// The options menu's "Choose your European disc": the setup screen, for that disc only.
void bw_choose_european_disc(void) {
    JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
    jobject activity = (jobject)SDL_GetAndroidActivity();
    if (!env || !activity) return;
    jclass cls = (*env)->GetObjectClass(env, activity);
    jmethodID open = (*env)->GetStaticMethodID(env, cls, "chooseEuropeanDisc", "()V");
    if (open) (*env)->CallStaticVoidMethod(env, cls, open);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
}
