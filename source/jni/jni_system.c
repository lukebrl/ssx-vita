/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  jni_system.c
 * @brief Activity, storage and EAMCore ("blast") device delegates.
 */

#include "jni/jni_ssx.h"

#include <string.h>

#include "ssx/assets.h"

#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

char jobj_activity[16];
char jobj_asset_manager[16];
char jobj_gl_view[16];
char jobj_delegate[16];

jobject jstr(const char *s) {
    return jni->NewStringUTF(&jni, s);
}

jint *jint_array(jobject arr) {
    JavaDynArray *jda = (JavaDynArray *)arr;
    return jda ? (jint *)jda->array : NULL;
}

jclass jclass_of(const char *name) {
    return jni->FindClass(&jni, name);
}

static jint call_native_int(const char *symbol) {
    jint (*f)(JNIEnv *, jclass) = (void *)so_symbol(&so_mod, symbol);
    if (!f) {
        l_error("Missing native %s", symbol);
        return 0;
    }
    return f(&jni, NULL);
}

/*
 * Shared handlers
 */

void Void_noop(jmethodID id, va_list args) {}
jboolean Bool_false(jmethodID id, va_list args) { return JNI_FALSE; }
jboolean Bool_true(jmethodID id, va_list args) { return JNI_TRUE; }
jint Int_zero(jmethodID id, va_list args) { return 0; }
jint Int_one(jmethodID id, va_list args) { return 1; }
jobject Obj_null(jmethodID id, va_list args) { return NULL; }

/*
 * com.ea.EAActivityArguments.EAActivityArguments
 */

jobject GetCommandLineArguments(jmethodID id, va_list args) {
    return (jobject)jda_alloc(0, FIELD_TYPE_OBJECT);
}

/*
 * com.ea.blast.MainActivity / android.app.Activity
 */

jobject MainActivity_GetInstance(jmethodID id, va_list args) {
    return jobj_activity;
}

jobject Activity_getAssets(jmethodID id, va_list args) {
    return jobj_asset_manager;
}

// com.ea.ssx.MainActivity.exitApp(boolean killApp)
void Activity_exitApp(jmethodID id, va_list args) {
    l_info("exitApp() requested by the game");
    sceKernelExitProcess(0);
}

// Constructor of every com.ea.blast.*Delegate / *Android helper class. None
// of them carry state we need, so they share one dummy instance.
jobject Delegate_init(jmethodID id, va_list args) {
    return jobj_delegate;
}

/*
 * com.ea.EAMIO.StorageDirectory
 */

jobject StorageDirectory_GetInternalStorageDirectory(jmethodID id, va_list args) {
    return jstr(ANDROID_FILES_DIR);
}

jobject StorageDirectory_GetPrimaryExternalStorageDirectory(jmethodID id, va_list args) {
    return jstr(ANDROID_EXT_FILES);
}

jobject StorageDirectory_GetPrimaryExternalStorageDirectoryRoot(jmethodID id, va_list args) {
    return jstr(ANDROID_SDCARD);
}

jint StorageDirectory_GetPrimaryExternalStorageState(jmethodID id, va_list args) {
    return 2; // mounted
}

/*
 * com.ea.blast.SystemAndroidDelegate
 */

jobject System_GetPlatformVersion(jmethodID id, va_list args) { return jstr("4.1.2"); }
jobject System_GetApiLevel(jmethodID id, va_list args) { return jstr("16"); }
jobject System_GetApplicationVersion(jmethodID id, va_list args) { return jstr("0.0.8430"); }
jint System_GetApplicationVersionCode(jmethodID id, va_list args) { return 8430; }
jobject System_GetDeviceName(jmethodID id, va_list args) { return jstr("PS Vita"); }
jobject System_GetDeviceUniqueId(jmethodID id, va_list args) { return jstr("0123456789abcdef"); }
jobject System_GetDeviceModel(jmethodID id, va_list args) { return jstr("PS Vita"); }
jobject System_GetFirmware(jmethodID id, va_list args) { return jstr("3.60"); }
jobject System_GetManufacturer(jmethodID id, va_list args) { return jstr("Sony"); }
jobject System_GetLocale(jmethodID id, va_list args) { return jstr("en_US"); }
jobject System_GetLanguage(jmethodID id, va_list args) { return jstr("en"); }
jobject System_GetProcessorArchitecture(jmethodID id, va_list args) { return jstr("armeabi-v7a"); }
jobject System_GetHardwareFloatingPointSupport(jmethodID id, va_list args) { return jstr("true"); }
jobject System_GetChipset(jmethodID id, va_list args) { return jstr("PS Vita"); }
jint System_GetTouchScreenCount(jmethodID id, va_list args) { return 1; }

/*
 * com.ea.blast.DisplayAndroidDelegate
 */

jint Display_GetStdOrientation(jmethodID id, va_list args) {
    return call_native_int("Java_com_ea_blast_DisplayAndroidDelegate_NativeGetOrientationNormal");
}

jint Display_GetDisplayOrientationLock(jmethodID id, va_list args) {
    return call_native_int("Java_com_ea_blast_DisplayAndroidDelegate_NativeGetDisplayOrientationLocked");
}

jint Display_GetWidth(jmethodID id, va_list args) { return SCREEN_W; }
jint Display_GetHeight(jmethodID id, va_list args) { return SCREEN_H; }
jfloat Display_GetDpi(jmethodID id, va_list args) { return 220.0f; }

jobject Display_GetGLView(jmethodID id, va_list args) {
    return jobj_gl_view;
}

/*
 * com.ea.blast.BatteryAndroidDelegate
 */

jfloat Battery_GetBatteryLevel(jmethodID id, va_list args) {
    return (jfloat)scePowerGetBatteryLifePercent() / 100.0f;
}

jint Battery_IsPowerConnected(jmethodID id, va_list args) {
    return scePowerIsPowerOnline() ? 1 : 0;
}
