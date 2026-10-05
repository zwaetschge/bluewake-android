// Hypatia's HD texture pack, downloaded from the options menu.
//
// The pack is not part of the app or of this repository: the button in the
// options menu (settings_menu.cpp) asks the activity to fetch it from the
// download link its author published in the pack's Dolphin forum thread
// (android/java/.../HdPack.java), and this side unpacks its GZL folder
// (hd_pack_7z.c) into <external files>/Load/Textures/GZLE01, the folder the
// menu then sets as the texture pack. The state is shared with the menu
// under a lock: Java reports the download, the unpacking reports itself.
#include "hd_pack.h"
#include "hd_pack_7z.h"

#include <SDL3/SDL.h>
#include <ftw.h>
#include <jni.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PACK_NAME "Hypatia WWHD Mod v2.0001a (Android-Lite)"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_state = BW_HD_PACK_IDLE;
static unsigned long long g_done, g_total;
static char g_message[512];
static bool g_finished;      // a finished install the menu has not taken yet
static char g_dest[1024];

static void set_state(int state, unsigned long long done, unsigned long long total, const char* message) {
    pthread_mutex_lock(&g_lock);
    g_state = state;
    g_done = done;
    g_total = total;
    if (message) snprintf(g_message, sizeof g_message, "%s", message);
    if (state == BW_HD_PACK_DONE) g_finished = true;
    pthread_mutex_unlock(&g_lock);
    if (state == BW_HD_PACK_FAILED && message) fprintf(stderr, "[hd-pack] failed: %s\n", message);
}

const char* bw_hd_pack_dest(void) {
    pthread_mutex_lock(&g_lock);
    if (g_dest[0] == '\0') {
        const char* external = SDL_GetAndroidExternalStoragePath();
        if (external) snprintf(g_dest, sizeof g_dest, "%s/Load/Textures/GZLE01", external);
    }
    pthread_mutex_unlock(&g_lock);
    return g_dest;
}

static void marker_path(char* out, size_t n) { snprintf(out, n, "%s/.hd_pack", bw_hd_pack_dest()); }

bool bw_hd_pack_installed(void) {
    char marker[1100];
    marker_path(marker, sizeof marker);
    struct stat st;
    return stat(marker, &st) == 0;
}

int bw_hd_pack_state(unsigned long long* done, unsigned long long* total, char* message, size_t n) {
    pthread_mutex_lock(&g_lock);
    const int state = g_state;
    if (done) *done = g_done;
    if (total) *total = g_total;
    if (message && n) snprintf(message, n, "%s", g_message);
    pthread_mutex_unlock(&g_lock);
    return state;
}

bool bw_hd_pack_take_finished(void) {
    pthread_mutex_lock(&g_lock);
    const bool finished = g_finished;
    g_finished = false;
    pthread_mutex_unlock(&g_lock);
    return finished;
}

bool bw_hd_pack_start(void) {
    const int state = bw_hd_pack_state(NULL, NULL, NULL, 0);
    const char* dest = bw_hd_pack_dest();
    if (state == BW_HD_PACK_DOWNLOADING || state == BW_HD_PACK_UNPACKING || dest[0] == '\0') return false;
    JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
    jobject activity = (jobject)SDL_GetAndroidActivity();
    if (!env || !activity) return false;
    jclass cls = (*env)->GetObjectClass(env, activity);
    jmethodID start = (*env)->GetStaticMethodID(env, cls, "startHdPackDownload", "(Ljava/lang/String;)V");
    bool ok = false;
    if (start) {
        jstring jdest = (*env)->NewStringUTF(env, dest);
        set_state(BW_HD_PACK_DOWNLOADING, 0, 0, "");
        (*env)->CallStaticVoidMethod(env, cls, start, jdest);
        (*env)->DeleteLocalRef(env, jdest);
        ok = !(*env)->ExceptionCheck(env);
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (!ok) set_state(BW_HD_PACK_FAILED, 0, 0, "the download could not be started");
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
    return ok;
}

// ---------------------------------------------------------------------------
// Called by HdPack.java on its own thread
// ---------------------------------------------------------------------------
JNIEXPORT void JNICALL Java_dev_bluewake_android_HdPack_nativeProgress(JNIEnv* env, jclass cls, jint state,
                                                                       jlong done, jlong total, jstring message) {
    (void)cls;
    const char* text = message ? (*env)->GetStringUTFChars(env, message, NULL) : NULL;
    set_state(state, done > 0 ? (unsigned long long)done : 0ull, total > 0 ? (unsigned long long)total : 0ull,
              text);
    if (text) (*env)->ReleaseStringUTFChars(env, message, text);
}

static void unpack_progress(void* user, unsigned long long done, unsigned long long total) {
    (void)user;
    set_state(BW_HD_PACK_UNPACKING, done, total, NULL);
}

static int remove_entry(const char* path, const struct stat* st, int flag, struct FTW* ftw) {
    (void)st; (void)flag; (void)ftw;
    return remove(path);
}

static void remove_tree(const char* path) {
    struct stat st;
    if (stat(path, &st) == 0) nftw(path, remove_entry, 32, FTW_DEPTH | FTW_PHYS);
}

// Unpacks the archive's GZL beside the installed one, then swaps it in: an
// interrupted unpack leaves the old pack as it was.
JNIEXPORT jint JNICALL Java_dev_bluewake_android_HdPack_nativeInstall(JNIEnv* env, jclass cls, jstring jarchive,
                                                                      jstring jdest) {
    (void)cls;
    const char* archive = (*env)->GetStringUTFChars(env, jarchive, NULL);
    const char* dest = (*env)->GetStringUTFChars(env, jdest, NULL);
    char staging[1100], gzl[1100], new_gzl[1100], marker[1100], error[512];
    snprintf(staging, sizeof staging, "%s/.download", dest);
    snprintf(gzl, sizeof gzl, "%s/GZL", dest);
    snprintf(new_gzl, sizeof new_gzl, "%s/GZL", staging);
    snprintf(marker, sizeof marker, "%s/.hd_pack", dest);
    remove_tree(staging);
    set_state(BW_HD_PACK_UNPACKING, 0, 0, "");
    int files = bw_hd_pack_extract_gzl(archive, staging, unpack_progress, NULL, error, sizeof error);
    if (files >= 0) {
        remove(marker);
        remove_tree(gzl);
        if (rename(new_gzl, gzl) != 0) {
            snprintf(error, sizeof error, "cannot move the pack into %s", dest);
            files = -1;
        }
    }
    remove_tree(staging);
    if (files >= 0) {
        FILE* f = fopen(marker, "w");
        if (f) {
            fprintf(f, "%s\n%d files\n", PACK_NAME, files);
            fclose(f);
        }
        fprintf(stderr, "[hd-pack] %s: %d textures in %s\n", PACK_NAME, files, gzl);
    } else {
        set_state(BW_HD_PACK_FAILED, 0, 0, error);
    }
    (*env)->ReleaseStringUTFChars(env, jarchive, archive);
    (*env)->ReleaseStringUTFChars(env, jdest, dest);
    return files;
}
