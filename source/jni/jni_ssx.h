/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  jni_ssx.h
 * @brief Java-side classes of the SSX APK, reimplemented for FalsoJNI.
 *
 * Every function here stands in for one Java method that libgame.so calls
 * through JNI. The Java originals are in the APK's classes.dex (decompile with
 * jadx); the class each function belongs to is noted next to it.
 */

#ifndef SSX_JNI_H
#define SSX_JNI_H

#include <stdarg.h>
#include <stdint.h>

#include <falso_jni/FalsoJNI.h>
#include <falso_jni/FalsoJNI_Impl.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCREEN_W 960
#define SCREEN_H 544

// Opaque Java objects handed to the game. Only their addresses matter.
extern char jobj_activity[];
extern char jobj_asset_manager[];
extern char jobj_gl_view[];
extern char jobj_delegate[];
extern char jobj_audio_track[];
extern char jobj_surface_holder[];
extern char jobj_egl[];

// Helpers
jobject jstr(const char *s);
jint *jint_array(jobject arr);

// Class names, as returned by FindClass (FalsoJNI uses them as jclass)
jclass jclass_of(const char *name);

/* jni_system.c */
// Shared handlers for methods whose result does not matter on the Vita
void Void_noop(jmethodID id, va_list args);
jboolean Bool_false(jmethodID id, va_list args);
jboolean Bool_true(jmethodID id, va_list args);
jint Int_zero(jmethodID id, va_list args);
jint Int_one(jmethodID id, va_list args);
jobject Obj_null(jmethodID id, va_list args);

jobject GetCommandLineArguments(jmethodID id, va_list args);
jobject MainActivity_GetInstance(jmethodID id, va_list args);
jobject Activity_getAssets(jmethodID id, va_list args);
jobject Delegate_init(jmethodID id, va_list args);

jobject StorageDirectory_GetInternalStorageDirectory(jmethodID id, va_list args);
jobject StorageDirectory_GetPrimaryExternalStorageDirectory(jmethodID id, va_list args);
jobject StorageDirectory_GetPrimaryExternalStorageDirectoryRoot(jmethodID id, va_list args);
jint StorageDirectory_GetPrimaryExternalStorageState(jmethodID id, va_list args);

jobject System_GetPlatformVersion(jmethodID id, va_list args);
jobject System_GetApiLevel(jmethodID id, va_list args);
jobject System_GetApplicationVersion(jmethodID id, va_list args);
jint System_GetApplicationVersionCode(jmethodID id, va_list args);
jobject System_GetDeviceName(jmethodID id, va_list args);
jobject System_GetDeviceUniqueId(jmethodID id, va_list args);
jobject System_GetDeviceModel(jmethodID id, va_list args);
jobject System_GetFirmware(jmethodID id, va_list args);
jobject System_GetManufacturer(jmethodID id, va_list args);
jobject System_GetLocale(jmethodID id, va_list args);
jobject System_GetLanguage(jmethodID id, va_list args);
jobject System_GetProcessorArchitecture(jmethodID id, va_list args);
jobject System_GetHardwareFloatingPointSupport(jmethodID id, va_list args);
jobject System_GetChipset(jmethodID id, va_list args);
jint System_GetTouchScreenCount(jmethodID id, va_list args);

jint Display_GetStdOrientation(jmethodID id, va_list args);
jint Display_GetDisplayOrientationLock(jmethodID id, va_list args);
jint Display_GetWidth(jmethodID id, va_list args);
jint Display_GetHeight(jmethodID id, va_list args);
jfloat Display_GetDpi(jmethodID id, va_list args);
jobject Display_GetGLView(jmethodID id, va_list args);

jfloat Battery_GetBatteryLevel(jmethodID id, va_list args);
jint Battery_IsPowerConnected(jmethodID id, va_list args);

void Activity_exitApp(jmethodID id, va_list args);

/* jni_egl.c */
// Number of eglSwapBuffers calls so far; 0 while the game is still booting
extern volatile unsigned ssx_frames_presented;
jobject EglDelegate_GetEgl(jmethodID id, va_list args);
jobject EglDelegate_GetSurface(jmethodID id, va_list args);
jobject EglDelegate_GetEglNoContext(jmethodID id, va_list args);
jobject EglDelegate_GetEglNoDisplay(jmethodID id, va_list args);
jobject EglDelegate_GetEglNoSurface(jmethodID id, va_list args);

jobject EGL10_eglGetDisplay(jmethodID id, va_list args);
jboolean EGL10_eglInitialize(jmethodID id, va_list args);
jboolean EGL10_eglTerminate(jmethodID id, va_list args);
jobject EGL10_eglQueryString(jmethodID id, va_list args);
jboolean EGL10_eglGetConfigs(jmethodID id, va_list args);
jboolean EGL10_eglChooseConfig(jmethodID id, va_list args);
jboolean EGL10_eglGetConfigAttrib(jmethodID id, va_list args);
jobject EGL10_eglCreateWindowSurface(jmethodID id, va_list args);
jobject EGL10_eglCreatePbufferSurface(jmethodID id, va_list args);
jobject EGL10_eglCreatePixmapSurface(jmethodID id, va_list args);
jobject EGL10_eglCreateContext(jmethodID id, va_list args);
jboolean EGL10_eglMakeCurrent(jmethodID id, va_list args);
jboolean EGL10_eglSwapBuffers(jmethodID id, va_list args);
jboolean EGL10_eglQuerySurface(jmethodID id, va_list args);
jboolean EGL10_eglQueryContext(jmethodID id, va_list args);
jint EGL10_eglGetError(jmethodID id, va_list args);
jboolean EGL10_true(jmethodID id, va_list args);

/* jni_audio.c */
#define AUDIO_RATE  48000 // what AudioTrack.getNativeOutputSampleRate() returns
#define AUDIO_GRAIN 1024  // frames per sceAudioOutOutput call

void AudioTrack_play(jmethodID id, va_list args);
void AudioTrack_stop(jmethodID id, va_list args);
jint AudioTrack_write(jmethodID id, va_list args);
void audio_start(void);
void audio_report(void);

/* jni_io.c */
jobject AssetManager_open(jmethodID id, va_list args);
jobject AssetManager_openFd(jmethodID id, va_list args);
jobject AssetManager_list(jmethodID id, va_list args);
jint InputStream_read(jmethodID id, va_list args);
jlong InputStream_skip(jmethodID id, va_list args);
void InputStream_close(jmethodID id, va_list args);
jlong AssetFileDescriptor_getLength(jmethodID id, va_list args);

/* jni_video.c */
jobject PlayerAndroid_new(jmethodID id, va_list args);
void PlayerAndroid_init(jmethodID id, va_list args);
void PlayerAndroid_play(jmethodID id, va_list args);
void PlayerAndroid_stop(jmethodID id, va_list args);
jint PlayerAndroid_pause(jmethodID id, va_list args);
jboolean PlayerAndroid_isPlaying(jmethodID id, va_list args);
void PlayerAndroid_setViewFrame(jmethodID id, va_list args);
void PlayerAndroid_destroy(jmethodID id, va_list args);
void video_poll(void);
// Draws the playing movie over the frame about to be presented (GL thread)
void movie_draw(void);

// FalsoJNI field ID of PlayerAndroid.mInstanceID
#define FIELD_PLAYER_INSTANCE_ID 101

#ifdef __cplusplus
};
#endif

#endif // SSX_JNI_H
