/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  java.c
 * @brief FalsoJNI method and field tables for SSX.
 *
 * Names are matched first as "pkg/Class/method" (when the game got the class
 * from FindClass), then as a bare method name. See jni/jni_ssx.h.
 */

#include <falso_jni/FalsoJNI.h>
#include <falso_jni/FalsoJNI_Impl.h>
#include <falso_jni/FalsoJNI_Logger.h>

#include "jni/jni_ssx.h"

/*
 * "play" and "stop" exist on both AudioTrack (an object the game gets from
 * AndroidEAAudioCore.Init, so no class name) and PlayerAndroid.
 */

static void Dispatch_play(jmethodID id, va_list args) {
    if (jni_current_object() == (jobject)jobj_audio_track)
        AudioTrack_play(id, args);
    else
        PlayerAndroid_play(id, args);
}

static void Dispatch_stop(jmethodID id, va_list args) {
    if (jni_current_object() == (jobject)jobj_audio_track)
        AudioTrack_stop(id, args);
    else
        PlayerAndroid_stop(id, args);
}

/*
 * JNI Methods
 */

NameToMethodID nameToMethodId[] = {
    // com.ea.EAActivityArguments.EAActivityArguments
    { 1, "GetCommandLineArguments", METHOD_TYPE_OBJECT },

    // com.ea.blast.MainActivity / com.ea.ssx.MainActivity(Generated)
    { 10, "GetInstance", METHOD_TYPE_OBJECT },
    { 11, "getAssets", METHOD_TYPE_OBJECT },
    { 12, "openURL", METHOD_TYPE_VOID },
    { 13, "exitApp", METHOD_TYPE_VOID },
    { 14, "TryPauseAV", METHOD_TYPE_VOID },
    { 15, "TryResumeAV", METHOD_TYPE_VOID },
    { 16, "installUpdate", METHOD_TYPE_VOID },

    // com.ea.EAMIO.StorageDirectory
    { 20, "GetInternalStorageDirectory", METHOD_TYPE_OBJECT },
    { 21, "GetPrimaryExternalStorageDirectory", METHOD_TYPE_OBJECT },
    { 22, "GetPrimaryExternalStorageDirectoryRoot", METHOD_TYPE_OBJECT },
    { 23, "GetPrimaryExternalStorageState", METHOD_TYPE_INT },

    // Constructors of EAMCore delegates
    { 30, "com/ea/blast/SystemAndroidDelegate/<init>", METHOD_TYPE_OBJECT },
    { 31, "com/ea/blast/DisplayAndroidDelegate/<init>", METHOD_TYPE_OBJECT },
    { 32, "com/ea/blast/ViewAndroidDelegate/<init>", METHOD_TYPE_OBJECT },
    { 33, "com/ea/blast/AccelerometerAndroidDelegate/<init>", METHOD_TYPE_OBJECT },
    { 34, "com/ea/blast/BatteryAndroidDelegate/<init>", METHOD_TYPE_OBJECT },
    { 35, "com/ea/blast/DeviceOrientationHandlerAndroidDelegate/<init>", METHOD_TYPE_OBJECT },
    { 36, "com/ea/blast/PhysicalKeyboardAndroidDelegate/<init>", METHOD_TYPE_OBJECT },
    { 37, "com/ea/blast/VirtualKeyboardAndroidDelegate/<init>", METHOD_TYPE_OBJECT },
    { 38, "com/ea/blast/VibratorAndroidDelegate/<init>", METHOD_TYPE_OBJECT },
    { 39, "com/ea/blast/PowerManagerAndroid/<init>", METHOD_TYPE_OBJECT },
    { 40, "com/ea/blast/NotificationAndroid/<init>", METHOD_TYPE_OBJECT },
    { 41, "com/ea/blast/NearFieldCommunicationAndroid/<init>", METHOD_TYPE_OBJECT },
    { 42, "com/ea/blast/TouchSurfaceAndroid/<init>", METHOD_TYPE_OBJECT },
    { 43, "com/ea/VideoPlayer/PlayerAndroid/<init>", METHOD_TYPE_OBJECT },
    { 44, "com/ea/EAMIO/StorageDirectory/<init>", METHOD_TYPE_OBJECT },

    // com.ea.blast.SystemAndroidDelegate
    { 50, "GetPlatformVersion", METHOD_TYPE_OBJECT },
    { 51, "GetApiLevel", METHOD_TYPE_OBJECT },
    { 52, "GetApplicationVersion", METHOD_TYPE_OBJECT },
    { 53, "GetApplicationVersionCode", METHOD_TYPE_INT },
    { 54, "GetDeviceName", METHOD_TYPE_OBJECT },
    { 55, "GetDeviceUniqueId", METHOD_TYPE_OBJECT },
    { 56, "GetDeviceModel", METHOD_TYPE_OBJECT },
    { 57, "GetFirmware", METHOD_TYPE_OBJECT },
    { 58, "GetManufacturer", METHOD_TYPE_OBJECT },
    { 59, "GetLocale", METHOD_TYPE_OBJECT },
    { 60, "GetLanguage", METHOD_TYPE_OBJECT },
    { 61, "GetProcessorArchitecture", METHOD_TYPE_OBJECT },
    { 62, "GetHardwareFloatingPointSupport", METHOD_TYPE_OBJECT },
    { 63, "GetChipset", METHOD_TYPE_OBJECT },
    { 64, "GetAccelerometerCount", METHOD_TYPE_INT },
    { 65, "GetCompassCount", METHOD_TYPE_INT },
    { 66, "GetGyroscopeCount", METHOD_TYPE_INT },
    { 67, "GetVibratorCount", METHOD_TYPE_INT },
    { 68, "GetTouchPadCount", METHOD_TYPE_INT },
    { 69, "GetTouchScreenCount", METHOD_TYPE_INT },
    { 70, "GetTrackBallCount", METHOD_TYPE_INT },
    { 71, "GetCameraCount", METHOD_TYPE_INT },
    { 72, "GetMicrophoneCount", METHOD_TYPE_INT },
    { 73, "IntentView", METHOD_TYPE_BOOLEAN },

    // com.ea.blast.DisplayAndroidDelegate
    { 80, "GetStdOrientation", METHOD_TYPE_INT },
    { 81, "SetStdOrientation", METHOD_TYPE_VOID },
    { 82, "GetDisplayOrientationLock", METHOD_TYPE_INT },
    { 83, "GetDefaultWidth", METHOD_TYPE_INT },
    { 84, "GetDefaultHeight", METHOD_TYPE_INT },
    { 85, "GetCurrentWidth", METHOD_TYPE_INT },
    { 86, "GetCurrentHeight", METHOD_TYPE_INT },
    { 87, "GetDpiX", METHOD_TYPE_FLOAT },
    { 88, "GetDpiY", METHOD_TYPE_FLOAT },
    { 89, "GetGLView", METHOD_TYPE_OBJECT },
    { 90, "AttachView", METHOD_TYPE_VOID },
    { 91, "DetachView", METHOD_TYPE_VOID },
    { 92, "BringToFront", METHOD_TYPE_VOID },

    // com.ea.blast.ViewAndroidDelegate
    { 95, "SetViewFrame", METHOD_TYPE_VOID },

    // com.ea.blast.EglAndroidDelegate
    { 100, "GetEgl", METHOD_TYPE_OBJECT },
    { 101, "GetSurface", METHOD_TYPE_OBJECT },
    { 102, "GetEglNoContext", METHOD_TYPE_OBJECT },
    { 103, "GetEglNoDisplay", METHOD_TYPE_OBJECT },
    { 104, "GetEglNoSurface", METHOD_TYPE_OBJECT },

    // javax.microedition.khronos.egl.EGL10
    { 110, "eglGetDisplay", METHOD_TYPE_OBJECT },
    { 111, "eglInitialize", METHOD_TYPE_BOOLEAN },
    { 112, "eglTerminate", METHOD_TYPE_BOOLEAN },
    { 113, "eglQueryString", METHOD_TYPE_OBJECT },
    { 114, "eglGetConfigs", METHOD_TYPE_BOOLEAN },
    { 115, "eglChooseConfig", METHOD_TYPE_BOOLEAN },
    { 116, "eglGetConfigAttrib", METHOD_TYPE_BOOLEAN },
    { 117, "eglCreateWindowSurface", METHOD_TYPE_OBJECT },
    { 118, "eglCreatePbufferSurface", METHOD_TYPE_OBJECT },
    { 119, "eglCreatePixmapSurface", METHOD_TYPE_OBJECT },
    { 120, "eglCreateContext", METHOD_TYPE_OBJECT },
    { 121, "eglMakeCurrent", METHOD_TYPE_BOOLEAN },
    { 122, "eglSwapBuffers", METHOD_TYPE_BOOLEAN },
    { 123, "eglQuerySurface", METHOD_TYPE_BOOLEAN },
    { 124, "eglQueryContext", METHOD_TYPE_BOOLEAN },
    { 125, "eglGetError", METHOD_TYPE_INT },
    { 126, "eglDestroyContext", METHOD_TYPE_BOOLEAN },
    { 127, "eglDestroySurface", METHOD_TYPE_BOOLEAN },
    { 128, "eglWaitGL", METHOD_TYPE_BOOLEAN },
    { 129, "eglWaitNative", METHOD_TYPE_BOOLEAN },
    { 130, "eglCopyBuffers", METHOD_TYPE_BOOLEAN },

    // Other EAMCore delegates
    { 140, "ApplyKeepAwake", METHOD_TYPE_VOID },             // PowerManagerAndroid
    { 141, "SetEnabled", METHOD_TYPE_VOID },                 // Accelerometer, DeviceOrientation
    { 142, "SetUpdateFrequency", METHOD_TYPE_VOID },         // Accelerometer
    { 143, "GetBatteryLevel", METHOD_TYPE_FLOAT },           // Battery
    { 144, "IsPowerConnected", METHOD_TYPE_INT },            // Battery
    { 145, "OnLifeCycleFocusGained", METHOD_TYPE_VOID },     // DeviceOrientation
    { 146, "IsPhysicalKeyboardVisible", METHOD_TYPE_BOOLEAN },
    { 147, "IsNavigationVisible", METHOD_TYPE_BOOLEAN },
    { 148, "IsVisible", METHOD_TYPE_BOOLEAN },               // VirtualKeyboard
    { 149, "OnPhysicalKeyboardVisibilityChanged", METHOD_TYPE_VOID },
    { 150, "SetEnterKeyLabel", METHOD_TYPE_VOID },
    { 151, "SetLayout", METHOD_TYPE_VOID },
    { 152, "SetShiftEnabled", METHOD_TYPE_VOID },
    { 153, "Shutdown", METHOD_TYPE_VOID },
    { 154, "UserSetVisible", METHOD_TYPE_VOID },
    { 155, "IsTouchScreenMultiTouch", METHOD_TYPE_BOOLEAN }, // TouchSurfaceAndroid
    { 156, "Vibrate", METHOD_TYPE_VOID },                    // Vibrator
    { 157, "Cancel", METHOD_TYPE_VOID },

    // com.ea.blast.NotificationAndroid
    { 160, "RegisterApplicationForNotifications", METHOD_TYPE_VOID },
    { 161, "UnregisterApplicationForNotifications", METHOD_TYPE_VOID },
    { 162, "NotifyPendingStartupPushNotifications", METHOD_TYPE_VOID },
    { 163, "NotifyPendingBackgroundPushNotifications", METHOD_TYPE_VOID },
    { 164, "VerifyUrlLaunch", METHOD_TYPE_VOID },
    { 165, "GenerateUniqueNotificationId", METHOD_TYPE_INT },
    { 166, "ScheduleLocalNotification", METHOD_TYPE_BOOLEAN },
    { 167, "CancelLocalNotification", METHOD_TYPE_BOOLEAN },
    { 168, "CancelAllLocalNotifications", METHOD_TYPE_VOID },
    { 169, "NotifyPendingStartupLocalNotifications", METHOD_TYPE_VOID },
    { 170, "NotifyPendingBackgroundLocalNotifications", METHOD_TYPE_VOID },

    // com.ea.blast.NearFieldCommunicationAndroid
    { 175, "Init", METHOD_TYPE_BOOLEAN },
    { 176, "Exit", METHOD_TYPE_VOID },
    { 177, "IsAvailable", METHOD_TYPE_BOOLEAN },
    { 178, "RegisterUserData", METHOD_TYPE_VOID },
    { 179, "GetPendingNFC", METHOD_TYPE_OBJECT },
    { 180, "SetMimeType", METHOD_TYPE_VOID },
    { 181, "EnableAAR", METHOD_TYPE_VOID },

    // android.media.AudioTrack (+ PlayerAndroid, see Dispatch_*)
    { 190, "play", METHOD_TYPE_VOID },
    { 191, "stop", METHOD_TYPE_VOID },
    { 192, "write", METHOD_TYPE_INT },

    // android.content.res.AssetManager, java.io.InputStream, AssetFileDescriptor
    { 200, "open", METHOD_TYPE_OBJECT },
    { 201, "openFd", METHOD_TYPE_OBJECT },
    { 202, "list", METHOD_TYPE_OBJECT },
    { 203, "read", METHOD_TYPE_INT },
    { 204, "skip", METHOD_TYPE_LONG },
    { 205, "close", METHOD_TYPE_VOID },
    { 206, "getLength", METHOD_TYPE_LONG },

    // com.ea.VideoPlayer.PlayerAndroid
    { 210, "com/ea/VideoPlayer/PlayerAndroid/init", METHOD_TYPE_VOID },
    { 211, "com/ea/VideoPlayer/PlayerAndroid/play", METHOD_TYPE_VOID },
    { 212, "com/ea/VideoPlayer/PlayerAndroid/stop", METHOD_TYPE_VOID },
    { 213, "com/ea/VideoPlayer/PlayerAndroid/pause", METHOD_TYPE_INT },
    { 214, "com/ea/VideoPlayer/PlayerAndroid/isPlaying", METHOD_TYPE_BOOLEAN },
    { 215, "com/ea/VideoPlayer/PlayerAndroid/setViewFrame", METHOD_TYPE_VOID },
    { 216, "com/ea/VideoPlayer/PlayerAndroid/destroy", METHOD_TYPE_VOID },
};

MethodsBoolean methodsBoolean[] = {
    { 73, Bool_false },
    { 111, EGL10_eglInitialize },
    { 112, EGL10_eglTerminate },
    { 114, EGL10_eglGetConfigs },
    { 115, EGL10_eglChooseConfig },
    { 116, EGL10_eglGetConfigAttrib },
    { 121, EGL10_eglMakeCurrent },
    { 122, EGL10_eglSwapBuffers },
    { 123, EGL10_eglQuerySurface },
    { 124, EGL10_eglQueryContext },
    { 126, EGL10_true },
    { 127, EGL10_true },
    { 128, EGL10_true },
    { 129, EGL10_true },
    { 130, EGL10_true },
    { 146, Bool_false },
    { 147, Bool_false },
    { 148, Bool_false },
    { 155, Bool_true },
    { 166, Bool_false },
    { 167, Bool_true },
    { 175, Bool_false },
    { 177, Bool_false },
    { 214, PlayerAndroid_isPlaying },
};

MethodsByte methodsByte[] = {};
MethodsChar methodsChar[] = {};
MethodsDouble methodsDouble[] = {};

MethodsFloat methodsFloat[] = {
    { 87, Display_GetDpi },
    { 88, Display_GetDpi },
    { 143, Battery_GetBatteryLevel },
};

MethodsInt methodsInt[] = {
    { 23, StorageDirectory_GetPrimaryExternalStorageState },
    { 53, System_GetApplicationVersionCode },
    { 64, Int_one },  // accelerometers: the game assumes one exists
    { 65, Int_zero },
    { 66, Int_zero },
    { 67, Int_zero },
    { 68, Int_zero },
    { 69, System_GetTouchScreenCount },
    { 70, Int_zero },
    { 71, Int_zero },
    { 72, Int_zero },
    { 80, Display_GetStdOrientation },
    { 82, Display_GetDisplayOrientationLock },
    { 83, Display_GetWidth },
    { 84, Display_GetHeight },
    { 85, Display_GetWidth },
    { 86, Display_GetHeight },
    { 125, EGL10_eglGetError },
    { 144, Battery_IsPowerConnected },
    { 165, Int_one },
    { 192, AudioTrack_write },
    { 203, InputStream_read },
    { 213, PlayerAndroid_pause },
};

MethodsLong methodsLong[] = {
    { 204, InputStream_skip },
    { 206, AssetFileDescriptor_getLength },
};

MethodsObject methodsObject[] = {
    { 1, GetCommandLineArguments },
    { 10, MainActivity_GetInstance },
    { 11, Activity_getAssets },
    { 20, StorageDirectory_GetInternalStorageDirectory },
    { 21, StorageDirectory_GetPrimaryExternalStorageDirectory },
    { 22, StorageDirectory_GetPrimaryExternalStorageDirectoryRoot },
    { 30, Delegate_init },
    { 31, Delegate_init },
    { 32, Delegate_init },
    { 33, Delegate_init },
    { 34, Delegate_init },
    { 35, Delegate_init },
    { 36, Delegate_init },
    { 37, Delegate_init },
    { 38, Delegate_init },
    { 39, Delegate_init },
    { 40, Delegate_init },
    { 41, Delegate_init },
    { 42, Delegate_init },
    { 43, PlayerAndroid_new },
    { 44, Delegate_init },
    { 50, System_GetPlatformVersion },
    { 51, System_GetApiLevel },
    { 52, System_GetApplicationVersion },
    { 54, System_GetDeviceName },
    { 55, System_GetDeviceUniqueId },
    { 56, System_GetDeviceModel },
    { 57, System_GetFirmware },
    { 58, System_GetManufacturer },
    { 59, System_GetLocale },
    { 60, System_GetLanguage },
    { 61, System_GetProcessorArchitecture },
    { 62, System_GetHardwareFloatingPointSupport },
    { 63, System_GetChipset },
    { 89, Display_GetGLView },
    { 100, EglDelegate_GetEgl },
    { 101, EglDelegate_GetSurface },
    { 102, EglDelegate_GetEglNoContext },
    { 103, EglDelegate_GetEglNoDisplay },
    { 104, EglDelegate_GetEglNoSurface },
    { 110, EGL10_eglGetDisplay },
    { 113, EGL10_eglQueryString },
    { 117, EGL10_eglCreateWindowSurface },
    { 118, EGL10_eglCreatePbufferSurface },
    { 119, EGL10_eglCreatePixmapSurface },
    { 120, EGL10_eglCreateContext },
    { 179, Obj_null },
    { 200, AssetManager_open },
    { 201, AssetManager_openFd },
    { 202, AssetManager_list },
};

MethodsShort methodsShort[] = {};

MethodsVoid methodsVoid[] = {
    { 12, Void_noop },
    { 13, Activity_exitApp },
    { 14, Void_noop },
    { 15, Void_noop },
    { 16, Void_noop },
    { 81, Void_noop },
    { 90, Void_noop },
    { 91, Void_noop },
    { 92, Void_noop },
    { 95, Void_noop },
    { 140, Void_noop },
    { 141, Void_noop },
    { 142, Void_noop },
    { 145, Void_noop },
    { 149, Void_noop },
    { 150, Void_noop },
    { 151, Void_noop },
    { 152, Void_noop },
    { 153, Void_noop },
    { 154, Void_noop },
    { 156, Void_noop },
    { 157, Void_noop },
    { 160, Void_noop },
    { 161, Void_noop },
    { 162, Void_noop },
    { 163, Void_noop },
    { 164, Void_noop },
    { 168, Void_noop },
    { 169, Void_noop },
    { 170, Void_noop },
    { 176, Void_noop },
    { 178, Void_noop },
    { 180, Void_noop },
    { 181, Void_noop },
    { 190, Dispatch_play },
    { 191, Dispatch_stop },
    { 205, InputStream_close },
    { 210, PlayerAndroid_init },
    { 211, PlayerAndroid_play },
    { 212, PlayerAndroid_stop },
    { 215, PlayerAndroid_setViewFrame },
    { 216, PlayerAndroid_destroy },
};

/*
 * JNI Fields
 */

NameToFieldID nameToFieldId[] = {
    { 100, "mActivity", FIELD_TYPE_OBJECT },  // com.ea.ssx.MainActivity (static)
    { FIELD_PLAYER_INSTANCE_ID, "mInstanceID", FIELD_TYPE_INT }, // PlayerAndroid
};

FieldsBoolean fieldsBoolean[] = {};
FieldsByte fieldsByte[] = {};
FieldsChar fieldsChar[] = {};
FieldsDouble fieldsDouble[] = {};
FieldsFloat fieldsFloat[] = {};
FieldsInt fieldsInt[] = {
    { FIELD_PLAYER_INSTANCE_ID, 0 },
};
FieldsObject fieldsObject[] = {
    { 100, jobj_activity },
};
FieldsLong fieldsLong[] = {};
FieldsShort fieldsShort[] = {};

__FALSOJNI_IMPL_CONTAINER_SIZES
