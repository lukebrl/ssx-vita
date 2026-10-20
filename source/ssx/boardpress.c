/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  boardpress.c
 * @brief Nose and tail presses on the ground, from Square and Circle.
 *
 * The console game's ground presses are still in the rider code and data:
 * EA::Ant::Ssx::CarvingController::GatherGameStateParameters reads two
 * inputs, BoardPressNoseInput and BoardPressTailInput (or their Classic
 * variants with classic controls), through evaluator objects of its asset
 * (+1432/+1436, +1440/+1444), stores them in its InputParameters (+217
 * nose, +218 tail) and right away decides from them, the speed (above
 * BoardPressStartSpeed, or StopSpeed once pressing) and the rider's state
 * whether the board presses (+216). On Android nothing drives the inputs.
 *
 * The two stores are patched to add Square (nose) and Circle (tail) to what
 * the evaluators gave: each store becomes a jump to a stub that ORs the
 * button in, stores, redoes the instruction the jump took the place of and
 * comes back.
 *
 * Only the player's rider gets them: the stubs compare the game state table
 * the controller reads with the one the pad data was last written to.
 * Debug builds with SSX_DIAG count the controllers and how often the board pressed
 * ([PRESS]).
 */

#include "ssx/boardpress.h"

#include <stdint.h>

#include <kubridge.h>
#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

// libgame.so v0.0.8430, in CarvingController::GatherGameStateParameters
#define OFF_Gather 0x7cb3b8        // the function (debug counters)
#define OFF_NOSE_STORE 0x7cbb28    // strb r0, [r4, #217]; cmp r3, #0
#define OFF_TAIL_STORE 0x7cbb54    // strb r0, [r4, #218]; ldr r3, [r5, #48]
#define NOSE_STORE_INSNS 0xe5c400d9, 0xe3530000
#define TAIL_STORE_INSNS 0xe5c400da, 0xe5953030
#define PARAM_PRESSING(p) (*(uint8_t *)((uint8_t *)(p) + 216))
#define PARAM_NOSE(p) (*(uint8_t *)((uint8_t *)(p) + 217))
#define PARAM_TAIL(p) (*(uint8_t *)((uint8_t *)(p) + 218))

// Where the stubs go back to (read by them)
uint32_t ssx_press_nose_ret, ssx_press_tail_ret;

// The player's rider: the game state table the pad data goes to.
// GameplayBase::GetClassicGamepadData passes it on to ClassicGamepadAsset::
// UpdateGamepadData for controller 0 only, every update.
#define OFF_UpdateGamepadData 0x12ac208 // Peripheral::ClassicGamepadAsset::UpdateGamepadData(Table&, float, float const*) const
const void *volatile ssx_press_player_table;
static void (*UpdateGamepadData_orig)(const void *asset, void *table, float dt, const float *data);
static void UpdateGamepadData_hook(const void *asset, void *table, float dt, const float *data) {
    ssx_press_player_table = table;
    UpdateGamepadData_orig(asset, table, dt, data);
}

// r0: what the evaluator gave, r4: InputParameters, r6: the rider's game state
// table. SCE_CTRL_SQUARE 0x8000, SCE_CTRL_CIRCLE 0x2000 in current_buttons
// (reimpl/controls.c).
__attribute__((naked, target("arm"))) static void nose_stub(void) {
    __asm__ volatile(
        "sub sp, sp, #4\n"
        "push {r1, r2}\n"
        "ldr r2, =ssx_press_player_table\n"
        "ldr r2, [r2]\n"
        "cmp r2, r6\n" // the player's rider only
        "bne 1f\n"
        "ldr r1, =current_buttons\n"
        "ldr r1, [r1]\n"
        "tst r1, #0x8000\n"
        "movne r0, #1\n"
        "1: strb r0, [r4, #217]\n"
        "ldr r2, =ssx_press_nose_ret\n"
        "ldr r2, [r2]\n"
        "str r2, [sp, #8]\n"
        "pop {r1, r2}\n"
        "cmp r3, #0\n" // the instruction the jump replaced
        "pop {pc}\n"
        ".ltorg\n");
}

__attribute__((naked, target("arm"))) static void tail_stub(void) {
    __asm__ volatile(
        "sub sp, sp, #4\n"
        "push {r1, r2}\n"
        "ldr r2, =ssx_press_player_table\n"
        "ldr r2, [r2]\n"
        "cmp r2, r6\n" // the player's rider only
        "bne 1f\n"
        "ldr r1, =current_buttons\n"
        "ldr r1, [r1]\n"
        "tst r1, #0x2000\n"
        "movne r0, #1\n"
        "1: strb r0, [r4, #218]\n"
        "ldr r2, =ssx_press_tail_ret\n"
        "ldr r2, [r2]\n"
        "str r2, [sp, #8]\n"
        "pop {r1, r2}\n"
        "ldr r3, [r5, #48]\n" // the instruction the jump replaced
        "pop {pc}\n"
        ".ltorg\n");
}

#ifdef DEBUG_SOLOADER
static void (*Gather_orig)(void *ctl, const void *table, void *params);
#define MAX_SEEN 16
static const void *seen[MAX_SEEN];
static int nseen;
static uint32_t calls, player_calls, nose, tail, pressing;

static void Gather_hook(void *ctl, const void *table, void *params) {
    Gather_orig(ctl, table, params);
    int k = 0;
    while (k < nseen && seen[k] != ctl)
        k++;
    if (k == nseen && nseen < MAX_SEEN)
        seen[nseen++] = ctl;
    calls++;
    player_calls += table == ssx_press_player_table;
    nose += PARAM_NOSE(params) != 0;
    tail += PARAM_TAIL(params) != 0;
    pressing += PARAM_PRESSING(params) != 0;
    static uint32_t last;
    uint32_t now = sceKernelGetProcessTimeLow();
    if (now - last >= 5000000) {
        if (calls)
            l_info("[PRESS] last 5 s: %u carving controllers, %u updates (%u the player's); nose input %u, tail "
                   "input %u; board pressing %u",
                   nseen, calls, player_calls, nose, tail, pressing);
        last = now;
        nseen = 0;
        calls = player_calls = nose = tail = pressing = 0;
    }
}
#endif

static int patch_store(uint32_t off, const uint32_t expected[2], void (*stub)(void), uint32_t *ret) {
    uint32_t *p = (uint32_t *)(so_mod.text_base + off);
    if (p[0] != expected[0] || p[1] != expected[1]) {
        l_error("[PRESS] unexpected code at game+0x%X: 0x%08X 0x%08X", (unsigned)off, (unsigned)p[0], (unsigned)p[1]);
        return 0;
    }
    *ret = so_mod.text_base + off + 8;
    uint32_t jump[2] = { 0xe51ff004, (uint32_t)(uintptr_t)stub }; // ldr pc, [pc, #-4]; stub
    kuKernelCpuUnrestrictedMemcpy(p, jump, sizeof(jump));
    kuKernelFlushCaches(p, sizeof(jump));
    return 1;
}

void ssx_boardpress_patch(void) {
    static const uint32_t nose_insns[2] = { NOSE_STORE_INSNS }, tail_insns[2] = { TAIL_STORE_INSNS };
    UpdateGamepadData_orig =
        (void *)hook_arm_trampoline(&so_mod, so_mod.text_base + OFF_UpdateGamepadData, (uintptr_t)UpdateGamepadData_hook);
    if (!UpdateGamepadData_orig) {
        l_error("[PRESS] could not hook ClassicGamepadAsset::UpdateGamepadData: no ground presses");
        return;
    }
    if (!patch_store(OFF_NOSE_STORE, nose_insns, nose_stub, &ssx_press_nose_ret) ||
        !patch_store(OFF_TAIL_STORE, tail_insns, tail_stub, &ssx_press_tail_ret))
        return;
#ifdef DEBUG_SOLOADER
    if (SSX_DIAG)
        Gather_orig = (void *)hook_arm_trampoline(&so_mod, so_mod.text_base + OFF_Gather, (uintptr_t)Gather_hook);
#endif
    l_info("[PRESS] ground presses on Square (nose) and Circle (tail)");
}
