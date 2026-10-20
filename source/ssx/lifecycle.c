/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  lifecycle.c
 * @brief Plays the part of the APK's Java activity: calls libgame.so's JNI
 *        entry points in the order com.ea.ssx.Main / MainActivity /
 *        com.ea.blast.MainActivity / MainThread would.
 */

#include "ssx/lifecycle.h"

#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>

#include "jni/jni_ssx.h"
#include "ssx/assets.h"
#include "ssx/watchdog.h"
#include "utils/dialog.h"
#include "utils/logger.h"

extern so_module so_mod;

// Name of the controller com.ea.ssx.MainActivityGenerated looks for
#define CONTROLLER_NAME "PLAYSTATION"

static void *native(const char *name) {
    void *f = (void *)so_symbol(&so_mod, name);
    if (!f)
        fatal_error("libgame.so is missing %s", name);
    return f;
}

#define NATIVE(ret, name, ...) \
    static ret (*name)(JNIEnv *, __VA_ARGS__) = NULL; \
    if (!name) name = native(#name)

static void (*NativeOnDrawFrame)(JNIEnv *, jobject) = NULL;
static void (*NativeOnControllerKeyDown)(JNIEnv *, jclass, jint) = NULL;
static void (*NativeOnControllerKeyUp)(JNIEnv *, jclass, jint) = NULL;
static void (*NativeOnControllerMotion)(JNIEnv *, jclass, jfloat, jfloat, jfloat, jfloat) = NULL;
static void (*OnBackSoftKey)(JNIEnv *, jclass, jboolean) = NULL;

static jobject main_thread_obj = (jobject)0x4D545452; // com.ea.blast.MainThread instance

void ssx_create(void) {
    // Where the Android paths given to the game live (see ssx/assets.c):
    // ANDROID_FILES_DIR, ANDROID_SDCARD and ANDROID_EXT_FILES
    static const char *dirs[] = {
        DATA_PATH "files",
        DATA_PATH "data",
        DATA_PATH "external",
        DATA_PATH "external/Android",
        DATA_PATH "external/Android/data",
        DATA_PATH "external/Android/data/com.ea.ssx",
        DATA_PATH "external/Android/data/com.ea.ssx/files",
    };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
        sceIoMkdir(dirs[i], 0777);

    // Static initializer of MainActivityGenerated loads libgame.so
    int (*JNI_OnLoad)(JavaVM *, void *) = native("JNI_OnLoad");
    // Not inside l_info: release builds compile the log calls out
    int jni_version = JNI_OnLoad(&jvm, NULL);
    l_info("JNI_OnLoad: 0x%x", jni_version);
    (void)jni_version;

    jobject activity = jobj_activity;
    jobject assets = jobj_asset_manager;

    // com.ea.ssx.MainActivity.onCreate()
    l_info("rwfilesystem.Startup");
    NATIVE(void, Java_com_ea_rwfilesystem_rwfilesystem_StartupNativeImpl, jclass, jobject, jstring, jstring);
    Java_com_ea_rwfilesystem_rwfilesystem_StartupNativeImpl(&jni, jclass_of("com/ea/rwfilesystem/rwfilesystem"),
        assets, jstr(ANDROID_FILES_DIR), jstr(ANDROID_SDCARD));

    //   com.ea.blast.MainActivity.onCreate()
    l_info("EAThread.Init");
    NATIVE(void, Java_com_ea_EAThread_EAThread_Init, jclass);
    Java_com_ea_EAThread_EAThread_Init(&jni, jclass_of("com/ea/EAThread/EAThread"));

    l_info("EAIO.Startup");
    NATIVE(void, Java_com_ea_EAIO_EAIO_StartupNativeImpl, jclass, jobject, jstring, jstring, jstring);
    Java_com_ea_EAIO_EAIO_StartupNativeImpl(&jni, jclass_of("com/ea/EAIO/EAIO"),
        assets, jstr("/data"), jstr(ANDROID_FILES_DIR), jstr(ANDROID_SDCARD));

    l_info("StorageDirectory.Startup");
    NATIVE(void, Java_com_ea_EAMIO_StorageDirectory_StartupNativeImpl, jclass);
    Java_com_ea_EAMIO_StorageDirectory_StartupNativeImpl(&jni, jclass_of("com/ea/EAMIO/StorageDirectory"));

    l_info("MainActivity.NativeOnCreate");
    NATIVE(void, Java_com_ea_blast_MainActivity_NativeOnCreate, jobject);
    Java_com_ea_blast_MainActivity_NativeOnCreate(&jni, activity);

    //   back in com.ea.ssx.MainActivity.onCreate()
    l_info("AndroidEAAudioCore.Startup");
    NATIVE(void, Java_com_ea_EAAudioCore_AndroidEAAudioCore_Init, jclass, jobject, jint, jint, jint, jint);
    // Init(AudioTrack, bufferFrames, channels, sampleRate, isOtherMusicPlaying)
    Java_com_ea_EAAudioCore_AndroidEAAudioCore_Init(&jni, jclass_of("com/ea/EAAudioCore/AndroidEAAudioCore"),
        jobj_audio_track, AUDIO_GRAIN, 2, AUDIO_RATE, -1);

    l_info("PlayerAndroid.Startup");
    NATIVE(void, Java_com_ea_VideoPlayer_PlayerAndroid_StartupNativeImpl, jclass);
    Java_com_ea_VideoPlayer_PlayerAndroid_StartupNativeImpl(&jni, jclass_of("com/ea/VideoPlayer/PlayerAndroid"));

    l_info("MainActivity.InitGameApplication");
    NATIVE(void, Java_com_ea_ssx_MainActivity_InitGameApplication, jclass);
    Java_com_ea_ssx_MainActivity_InitGameApplication(&jni, jclass_of("com/ea/ssx/MainActivity"));

    l_success("onCreate done");
}

void ssx_resume(void) {
    jobject activity = jobj_activity;
    jclass ssx_activity = jclass_of("com/ea/ssx/MainActivity");

    // com.ea.blast.MainActivity.onResume(): window has no focus yet
    l_info("MainActivity.NativeOnResume");
    NATIVE(void, Java_com_ea_blast_MainActivity_NativeOnResume, jobject, jboolean);
    Java_com_ea_blast_MainActivity_NativeOnResume(&jni, activity, JNI_FALSE);

    // com.ea.ssx.MainActivity.onResume() -> SignalPauseOrResumeEvent() without focus
    NATIVE(void, Java_com_ea_ssx_MainActivity_NativeOnPauseEvent, jclass);
    Java_com_ea_ssx_MainActivity_NativeOnPauseEvent(&jni, ssx_activity);

    // MainActivityGenerated.onResume() -> ParseInputDevices(): a pad is present
    l_info("Controller connected");
    NATIVE(void, Java_com_ea_ssx_MainActivityGenerated_NativeOnControllerConnectState, jclass, jboolean, jstring);
    Java_com_ea_ssx_MainActivityGenerated_NativeOnControllerConnectState(&jni,
        jclass_of("com/ea/ssx/MainActivityGenerated"), JNI_TRUE, jstr(CONTROLLER_NAME));

    // com.ea.blast.MainThread.surfaceCreated() / surfaceChanged()
    l_info("MainThread.NativeOnSurfaceCreated");
    NATIVE(void, Java_com_ea_blast_MainThread_NativeOnSurfaceCreated, jobject, jint);
    Java_com_ea_blast_MainThread_NativeOnSurfaceCreated(&jni, main_thread_obj, 0);

    NATIVE(void, Java_com_ea_ssx_MainActivity_NativeOnContextResetEvent, jclass, jint);
    Java_com_ea_ssx_MainActivity_NativeOnContextResetEvent(&jni, ssx_activity, 0);

    l_info("MainThread.NativeOnSurfaceChanged");
    NATIVE(void, Java_com_ea_blast_MainThread_NativeOnSurfaceChanged, jobject, jint, jint, jint);
    Java_com_ea_blast_MainThread_NativeOnSurfaceChanged(&jni, main_thread_obj, SCREEN_W, SCREEN_H, 0);

    // onWindowFocusChanged(true): com.ea.ssx first, then com.ea.blast
    l_info("Window focus gained");
    NATIVE(void, Java_com_ea_ssx_MainActivity_NativeOnResumeEvent, jclass);
    Java_com_ea_ssx_MainActivity_NativeOnResumeEvent(&jni, ssx_activity);

    NATIVE(void, Java_com_ea_blast_MainActivity_NativeOnWindowFocusChanged, jobject, jboolean);
    Java_com_ea_blast_MainActivity_NativeOnWindowFocusChanged(&jni, activity, JNI_TRUE);

    NativeOnDrawFrame = native("Java_com_ea_blast_MainThread_NativeOnDrawFrame");
    NativeOnControllerKeyDown = native("Java_com_ea_ssx_MainActivityGenerated_NativeOnControllerKeyDown");
    NativeOnControllerKeyUp = native("Java_com_ea_ssx_MainActivityGenerated_NativeOnControllerKeyUp");
    NativeOnControllerMotion = native("Java_com_ea_ssx_MainActivityGenerated_NativeOnControllerMotion");
    OnBackSoftKey = native("Java_com_ea_ssx_MainActivity_OnBackSoftKey");

    l_success("onResume done, entering frame loop");
}

void ssx_draw_frame(void) {
    static unsigned frame = 0;
    frame++;
    if (frame <= 5)
        l_info("NativeOnDrawFrame #%u...", frame);

    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    NativeOnDrawFrame(&jni, main_thread_obj);
    SceUInt64 dt_ms = (sceKernelGetProcessTimeWide() - t0) / 1000;

    if (frame <= 5)
        l_info("...NativeOnDrawFrame #%u done", frame);
    else if (dt_ms >= 1000)
        l_info("[SLOW] NativeOnDrawFrame #%u took %llu ms", frame, dt_ms);
    watchdog_frame_done();
}

// Input handlers in the game read front-end state that only exists once it
// has booted (TouchController::OnNIControllerMotion crashed on frame 1), and
// no one can press anything before the first picture anyway. The front-end
// data store comes later still: a button held at the boot logo crashed in
// TouchController::HandleNIControllerClassicModeInput reading it.
#define OFF_FEDataStoreInstance 0x17cdb7c // FEDataStore* (libgame.so v0.0.8430)

static int input_ready(void) {
    return ssx_frames_presented > 0 && *(void *volatile *)(so_mod.text_base + OFF_FEDataStoreInstance) != NULL;
}

void ssx_key(int32_t keycode, int down) {
    if (!input_ready())
        return;
    jclass cls = (jclass)0x4D414731; // MainActivityGenerated, unused by the natives
    if (down)
        NativeOnControllerKeyDown(&jni, cls, keycode);
    else
        NativeOnControllerKeyUp(&jni, cls, keycode);
}

void ssx_back(int down) {
    if (!input_ready())
        return;
    OnBackSoftKey(&jni, NULL, down ? JNI_TRUE : JNI_FALSE);
}

void ssx_sticks(float lx, float ly, float rx, float ry) {
    // Like Android, report motion only when a stick actually moves
    static float last[4];
    if (lx == last[0] && ly == last[1] && rx == last[2] && ry == last[3])
        return;
    if (!input_ready())
        return;
    last[0] = lx; last[1] = ly; last[2] = rx; last[3] = ry;
    NativeOnControllerMotion(&jni, NULL, lx, ly, rx, ry);
}
