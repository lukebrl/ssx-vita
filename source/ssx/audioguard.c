/*
 * SSX port: audio stream guard
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  audioguard.c
 * @brief Survives a music stream that can't be created.
 *
 * Audio::PathStream::Load uses what PATH_createstreamimp returns without
 * checking it. It returned NULL once at boot (front-end music,
 * ssx_fe.mpf), and the game crashed reading through it. Load now returns
 * early in that case, as when the stream's asset is missing: the stream
 * stays not loaded, and PathProject::Update tries again later.
 */

#include "ssx/audioguard.h"

#include <stdint.h>

#include <kubridge.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

// libgame.so v0.0.8430, in Audio::PathStream::Load()
#define OFF_UseStream 0x62805c // ldr r3, [r0]; mov r5, r0 (r0 = PATH_createstreamimp(...))
#define OFF_Continue  0x628064
#define OFF_Return    0x6280a0 // pop {r3, r4, r5, pc}, as when the asset is missing

uintptr_t ssx_audioguard_continue, ssx_audioguard_return;
volatile uint32_t ssx_audioguard_hits;

__attribute__((naked, target("arm"))) static void stream_guard(void) {
    __asm__ volatile(
        "cmp r0, #0\n"
        "beq 1f\n"
        "ldr r3, [r0]\n" // the two instructions replaced
        "mov r5, r0\n"
        "ldr r12, =ssx_audioguard_continue\n"
        "ldr pc, [r12]\n"
        "1: ldr r12, =ssx_audioguard_hits\n"
        "ldr r3, [r12]\n"
        "add r3, r3, #1\n"
        "str r3, [r12]\n"
        "ldr r12, =ssx_audioguard_return\n"
        "ldr pc, [r12]\n"
        ".ltorg\n");
}

void ssx_audioguard_patch(void) {
    uintptr_t base = so_mod.text_base;
    uint32_t *code = (uint32_t *)(base + OFF_UseStream);
    if (code[0] != 0xE5903000 || code[1] != 0xE1A05000) { // ldr r3, [r0]; mov r5, r0
        l_error("[AUDIO] PathStream::Load is not as expected: not guarded");
        return;
    }
    ssx_audioguard_continue = base + OFF_Continue;
    ssx_audioguard_return = base + OFF_Return;
    code[0] = 0xE51FF004; // ldr pc, [pc, #-4]
    code[1] = (uint32_t)(uintptr_t)stream_guard;
    kuKernelFlushCaches(code, 8);
}
