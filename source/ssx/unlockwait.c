/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  unlockwait.c
 * @brief Short waits in the campaign screen's post-race sequence.
 *
 * Back on the race selection screen after a race, GameFlowDataManager::
 * ProcessFEPostRaceProgression runs a queue of drop actions (GameProgression,
 * built from the game's data): mark the race completed, unlock the next one,
 * move the selection to it. Two kinds of action wait: type 1 until the menu
 * reports that its progression animation ended (FEDataStore item 200, set
 * by TrackProgressionAnimComplete), type 8 for a time stored in the action.
 * The animation shows nothing, on Android too, so the screen sat still for
 * 4-5 s before the selection moved, and 4-5 s more before it took input.
 * Each wait now lasts HOLD_MS: type 1 goes on then without the flag (its
 * wait branch is a nop, and the action is not processed until then), and
 * type 8's time is capped. With no wait at all it felt too abrupt.
 *
 * Debug builds log each action as it starts ([PROGRESS]).
 */

#include "ssx/unlockwait.h"

#include <stdint.h>

#include <kubridge.h>
#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

// libgame.so v0.0.8430
#define OFF_ProcessFEPostRaceProgression 0x363688 // GameFlowDataManager::ProcessFEPostRaceProgression(float)
#define OFF_WaitFlagBranch 0x363a64               // type 1: beq (flag 200 not set: keep waiting)
#define BEQ_WAIT 0x0a000035                       // beq 0x363b40
#define ARM_NOP 0xe320f000
// GameFlowDataManager: actions at +(i + 1835) * 16 (type +0, value +4,
// remaining time +8), the current one's index at +0x74b4, the count at +0x74b0
#define ACTION(m, i) ((uint8_t *)(m) + ((i) + 1835) * 16)
#define ACTION_INDEX(m) (*(volatile int32_t *)((uint8_t *)(m) + 0x74b4))
#define ACTION_COUNT(m) (*(volatile int32_t *)((uint8_t *)(m) + 0x74b0))
#define TYPE_WAIT_FLAG 1
#define TYPE_WAIT_TIME 8
#define HOLD_MS 1500

static void (*Process_orig)(void *, float);

static void Process_hook(void *mgr, float dt) {
    static void *last_mgr;
    static int last_index = -1;
    static uint32_t started;
    int i = ACTION_INDEX(mgr);
    if (i >= 0 && i < ACTION_COUNT(mgr)) {
        uint8_t *a = ACTION(mgr, i);
        int32_t type = *(int32_t *)a;
        uint32_t now = sceKernelGetProcessTimeLow();
        if (mgr != last_mgr || i != last_index) {
            if (SSX_DIAG)
                l_info("[PROGRESS] action %d of %d: type %d, value 0x%x, time %.2f s", i, (int)ACTION_COUNT(mgr), (int)type,
                       (unsigned)*(uint32_t *)(a + 4), *(float *)(a + 8));
            last_mgr = mgr;
            last_index = i;
            started = now;
        }
        if (type == TYPE_WAIT_FLAG && now - started < HOLD_MS * 1000)
            return; // held here, then the action goes on (no flag needed)
        if (type == TYPE_WAIT_TIME && *(float *)(a + 8) > HOLD_MS / 1000.0f)
            *(float *)(a + 8) = HOLD_MS / 1000.0f;
    }
    Process_orig(mgr, dt);
}

void ssx_unlockwait_patch(void) {
    uint32_t *branch = (uint32_t *)(so_mod.text_base + OFF_WaitFlagBranch);
    if (*branch == BEQ_WAIT) {
        uint32_t nop = ARM_NOP;
        kuKernelCpuUnrestrictedMemcpy(branch, &nop, sizeof(nop));
        kuKernelFlushCaches(branch, sizeof(nop));
    } else {
        l_error("[PROGRESS] unexpected code at game+0x%X: 0x%08X", OFF_WaitFlagBranch, (unsigned)*branch);
        return;
    }
    Process_orig = (void *)hook_arm_trampoline(&so_mod, so_mod.text_base + OFF_ProcessFEPostRaceProgression,
                                               (uintptr_t)Process_hook);
    if (!Process_orig)
        l_error("[PROGRESS] could not hook ProcessFEPostRaceProgression");
}
