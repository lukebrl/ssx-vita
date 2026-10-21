/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  core3.c
 * @brief The fourth CPU core for the game's background threads, when the
 *        CapUnlocker plugin makes it available.
 *
 * Games get cores 0-2; core 3 is the system's. GrapheneCt's CapUnlocker
 * (a kernel plugin) lets a game thread's affinity mask include it, but
 * threads stay on cores 0-2 unless they ask (as in LeZergan's Minecraft:
 * Story Mode port). Here the background threads (the EA job workers, file
 * streaming, texture work) widen their mask to all four cores as the game
 * names them: they still run on cores 0-2, and also on core 3 when it is
 * free. The render, simulation and audio threads stay on cores 0-2, where
 * the system's work on core 3 can't interrupt them. Without the plugin the
 * kernel refuses the mask and nothing changes. Never pin a thread to core 3
 * alone: without the plugin it would never run.
 *
 * Setting "4th core" in the settings menu (on by default), from the next
 * launch.
 */

#include "ssx/core3.h"
#include "ssx/settings.h"

#include <string.h>

#include <psp2/kernel/threadmgr.h>

#include "utils/logger.h"

#define MASK_USER_CORES 0x70000 // cores 0-2
#define MASK_ALL_CORES 0xF0000  // and core 3

// prctl names (the game cuts them at 15 characters)
static const char *const background[] = {
    "General Job Thr", "Replay Job Thre",                             // EA::Jobs workers
    "rwfilesys", "AssetStream::Lo", "AssetStream::Un", "AssetStream::Tr", // file streaming
    "PathToRWFileSys", "RecolorTex", "FontToTexture",                 // texture work
};

static volatile int state = CORE3_UNKNOWN;

int ssx_core3_state(void) {
    return ssx_core3_enabled ? state : CORE3_OFF;
}

void ssx_core3_thread_named(const char *name) {
    if (!ssx_core3_enabled || state == CORE3_UNAVAILABLE)
        return;
    int found = 0;
    for (unsigned i = 0; i < sizeof(background) / sizeof(background[0]) && !found; i++)
        found = !strcmp(name, background[i]);
    if (!found)
        return;
    int r = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), MASK_ALL_CORES);
    if (r < 0) {
        if (state != CORE3_UNAVAILABLE)
            l_info("[CORE3] the 4th core isn't available (no CapUnlocker? 0x%08X): threads stay on cores 0-2",
                   (unsigned)r);
        state = CORE3_UNAVAILABLE;
        return;
    }
    if (state != CORE3_USED)
        l_info("[CORE3] CapUnlocker: background threads on all 4 cores");
    state = CORE3_USED;
    l_info("[CORE3] \"%s\" (thread 0x%x) may run on core 3", name, (unsigned)sceKernelGetThreadId());
}
