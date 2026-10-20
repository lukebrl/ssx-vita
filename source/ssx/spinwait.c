/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  spinwait.c
 * @brief Makes the game's busy-wait loops sleep between checks.
 *
 * A few EA loops wait for another thread by re-reading a value until it
 * changes, without ever yielding, e.g. in EA::Ant::SceneOpNode::RunFunc:
 *
 *     loop: ldr r2, [r1, #68]; ldr r3, [r4, #68]; cmp r2, r3; bne loop
 *
 * Android time-slices threads, so the thread they wait for gets to run
 * anyway. The Vita doesn't time-slice threads of equal priority: with
 * spinners on all three cores, the thread they wait for never runs again,
 * and the game froze a few minutes into a race (at a different time on each
 * run).
 *
 * The backward branch of each loop goes through a veneer that sleeps for a
 * moment and then jumps back to the start of the loop, which re-reads the
 * value; the loop's exit path is untouched.
 */

#include "ssx/spinwait.h"

#include <stdint.h>

#include <psp2/kernel/threadmgr.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

// Backward branches of the busy-wait loops in libgame.so v0.0.8430
static const struct {
    uint32_t branch;
    const char *where;
} spin_loops[] = {
    { 0x562774, "EA::Audio::Core::BufferReference::ClearPending" },
    { 0x67a734, "EA::Ant::Anim::CacheLock::CacheLock (constprop)" },
    { 0x681f54, "EA::Ant::Anim::CacheLock::CacheLock" },
    { 0x6a140c, "EA::Ant::SceneOpNode::AddJobEvent" },
    { 0x6a1658, "EA::Ant::SceneOpNode::ScheduleTaskList" },
    { 0x6a17d0, "EA::Ant::SceneOpNode::RunFunc" },
    { 0xf37600, "EA::Ant::Interaction::InteractionManager::GetInteractionScene" },
};

static volatile uint32_t spin_sleeps;

static void spin_sleep(void) {
    __atomic_add_fetch(&spin_sleeps, 1, __ATOMIC_RELAXED);
    sceKernelDelayThread(100);
}

// Saves the caller-saved registers, calls spin_sleep with an 8-byte aligned
// stack, restores them and jumps back to the start of the loop (which
// recomputes the flags the branch tests)
static const uint32_t veneer_template[] = {
    0xe92d500f, // push {r0-r3, r12, lr}
    0xe1a0100d, // mov  r1, sp
    0xe3c10007, // bic  r0, r1, #7
    0xe1a0d000, // mov  sp, r0
    0xe92d0006, // push {r1, r2}
    0xe59fc010, // ldr  r12, [pc, #16]   (spin_sleep)
    0xe12fff3c, // blx  r12
    0xe8bd0006, // pop  {r1, r2}
    0xe1a0d001, // mov  sp, r1
    0xe8bd500f, // pop  {r0-r3, r12, lr}
    0xe59ff000, // ldr  pc, [pc]         (loop start)
    0,          // spin_sleep
    0,          // loop start
};
#define VENEER_WORDS (sizeof(veneer_template) / sizeof(veneer_template[0]))

void ssx_spinwait_patch(void) {
    int patched = 0;
    for (unsigned i = 0; i < sizeof(spin_loops) / sizeof(spin_loops[0]); i++) {
        uintptr_t branch = so_mod.text_base + spin_loops[i].branch;
        uint32_t ins = *(uint32_t *)branch;
        int32_t imm = (int32_t)(ins << 8) >> 8; // sign-extended imm24
        uintptr_t loop = branch + 8 + imm * 4;
        // Only the expected short backward B<cond>
        if ((ins & 0x0F000000) != 0x0A000000 || loop >= branch || branch - loop > 16) {
            l_error("[SPIN] unexpected instruction 0x%08X in %s, not patched", ins, spin_loops[i].where);
            continue;
        }

        uint32_t code[VENEER_WORDS];
        for (unsigned w = 0; w < VENEER_WORDS; w++)
            code[w] = veneer_template[w];
        code[VENEER_WORDS - 2] = (uint32_t)&spin_sleep;
        code[VENEER_WORDS - 1] = loop;
        if (hook_branch_veneer(&so_mod, branch, code, VENEER_WORDS))
            patched++;
        else
            l_error("[SPIN] no room for the veneer of %s", spin_loops[i].where);
    }
    l_info("[SPIN] %d busy-wait loops now sleep between checks", patched);
}

void ssx_spinwait_report(void) {
    l_info("[SPIN] %u sleeps in busy-wait loops so far", spin_sleeps);
}
