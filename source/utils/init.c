/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021-2022 Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/init.h"

#include "utils/dialog.h"
#include "utils/glutil.h"
#include "utils/logger.h"
#include "utils/utils.h"
#include "utils/settings.h"

#include <reimpl/controls.h>

#include <string.h>

#include <psp2/appmgr.h>
#include <psp2/apputil.h>
#include <psp2/kernel/clib.h>
#include <psp2/power.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>
#include <fios/fios.h>

// Base address for the Android .so to be loaded at
#define LOAD_ADDRESS 0x98000000
// libgame.so spans ~25 MB from LOAD_ADDRESS; libgnustl_shared.so goes after it,
// below the 0x9A000000 block that patch.c reserves for kuser helpers.
#define GNUSTL_LOAD_ADDRESS 0x99C00000

extern so_module so_mod;
so_module gnustl_mod;

void soloader_init_all() {
	// Launch `app0:configurator.bin` on `-config` init param
    sceAppUtilInit(&(SceAppUtilInitParam){}, &(SceAppUtilBootParam){});
    SceAppUtilAppEventParam eventParam;
    sceClibMemset(&eventParam, 0, sizeof(SceAppUtilAppEventParam));
    sceAppUtilReceiveAppEvent(&eventParam);
    if (eventParam.type == 0x05) {
        char buffer[2048];
        sceAppUtilAppEventParseLiveArea(&eventParam, buffer);
        if (strstr(buffer, "-config"))
            sceAppMgrLoadExec("app0:/configurator.bin", NULL, NULL);
    }

    // Maximum clocks unless built with SSX_OVERCLOCK=0 (to compare with stock)
#ifndef SSX_OVERCLOCK
#define SSX_OVERCLOCK 1
#endif
#if SSX_OVERCLOCK
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
#endif

#ifdef USE_SCELIBC_IO
    if (fios_init(DATA_PATH) == 0)
        l_success("FIOS initialized.");
#endif

    if (!module_loaded("kubridge")) {
        l_fatal("kubridge is not loaded.");
        fatal_error("Error: kubridge.skprx is not installed.");
    }
    l_success("kubridge check passed.");

    if (!file_exists(SO_PATH)) {
        fatal_error("Looks like you haven't installed the data files for this "
                    "port, or they are in an incorrect location. Please make "
                    "sure that you have %s file exactly at that path.", SO_PATH);
    }

    if (!file_exists(GNUSTL_PATH)) {
        fatal_error("Looks like you haven't installed the data files for this "
                    "port, or they are in an incorrect location. Please make "
                    "sure that you have %s file exactly at that path.", GNUSTL_PATH);
    }

    // Must be loaded first: so_util links libgame.so's DT_NEEDED entries
    // against modules that are already loaded.
    int res;
    if ((res = so_file_load(&gnustl_mod, GNUSTL_PATH, GNUSTL_LOAD_ADDRESS)) < 0) {
        l_fatal("libgnustl_shared.so could not be loaded: 0x%08X", res);
        fatal_error("Error: could not load %s (0x%08X).", GNUSTL_PATH, res);
    }

    if ((res = so_file_load(&so_mod, SO_PATH, LOAD_ADDRESS)) < 0) {
        l_fatal("SO could not be loaded: 0x%08X", res);
        fatal_error("Error: could not load %s (0x%08X).", SO_PATH, res);
    }

    settings_load();
    l_success("Settings loaded.");

    so_relocate(&gnustl_mod);
    so_relocate(&so_mod);
    l_success("SO relocated.");

    resolve_imports(&gnustl_mod);
    resolve_imports(&so_mod);
    l_success("SO imports resolved.");

    so_patch();
    l_success("SO patched.");

    so_flush_caches(&gnustl_mod);
    so_flush_caches(&so_mod);
    l_success("SO caches flushed.");

    so_initialize(&gnustl_mod);
    so_initialize(&so_mod);
    l_success("SO initialized.");

    gl_preload();
    l_success("OpenGL preloaded.");

    jni_init();
    l_success("FalsoJNI initialized.");

#ifndef NDK_PORT
    controls_init();
    l_success("Controls initialized.");
#endif
}
