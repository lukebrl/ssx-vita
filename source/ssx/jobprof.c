/*
 * SSX port: EA::Jobs profiling
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  jobprof.c
 * @brief How long the render thread waits on EA::Jobs jobs, and runs them.
 *
 * Characters are skinned on the CPU in jobs (SkinnedRenderObject), and
 * SkinnedRenderObject::RenderSetup waits on the previous one with
 * JobInstanceHandle::WaitOn, which runs queued jobs on the waiting thread
 * (JobContextImpl::RunOneJob) before sleeping. CharacterRenderObj::RenderSetup
 * shows ~10 ms of its own time per frame for this; these hooks tell how much
 * is the render thread doing the skinning itself.
 */

#include "ssx/jobprof.h"

#include <stdint.h>

#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>

#include "ssx/glprof.h"
#include "utils/logger.h"

extern so_module so_mod;

// libgame.so v0.0.8430
#define OFF_JobInstanceHandle_WaitOn 0xf19e34 // EA::Jobs::JobInstanceHandle::WaitOn(WaitOnControl (*)(void*), void*, int) const
#define OFF_JobContextImpl_RunOneJob 0xf1872c // EA::Jobs::Detail::JobContextImpl::RunOneJob()
#define OFF_JobContextImpl_DispatchJob 0xf180a0 // EA::Jobs::Detail::JobContextImpl::DispatchJob(JobInstance*)
#define OFF_sIsAppPaused 0x1863d20 // EA::Jobs::JobContext::sIsAppPaused: job threads only sleep while set

typedef uint64_t (*Fn8)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
static Fn8 WaitOn_orig, RunOneJob_orig, DispatchJob_orig;

static struct {
    uint64_t wait_us, run_us;
    uint32_t waits, runs, worker_runs;
} stats;
static int waiting; // render thread inside WaitOn

static inline uintptr_t thread_key(void) {
    uintptr_t v;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(v));
    return v;
}

static inline int on_render_thread(void) {
    uintptr_t rt = glprof_render_thread();
    return rt && thread_key() == rt;
}

static uint64_t WaitOn_hook(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                            uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7) {
    if (!on_render_thread())
        return WaitOn_orig(a0, a1, a2, a3, a4, a5, a6, a7);
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    waiting++;
    uint64_t r = WaitOn_orig(a0, a1, a2, a3, a4, a5, a6, a7);
    waiting--;
    stats.wait_us += sceKernelGetProcessTimeWide() - t0;
    stats.waits++;
    return r;
}

static uint64_t RunOneJob_hook(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                               uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7) {
    if (!on_render_thread())
        return RunOneJob_orig(a0, a1, a2, a3, a4, a5, a6, a7);
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    uint64_t r = RunOneJob_orig(a0, a1, a2, a3, a4, a5, a6, a7);
    stats.run_us += sceKernelGetProcessTimeWide() - t0;
    stats.runs++;
    return r;
}

// Every job runs through here (worker threads call it from RunUntil)
static uint64_t DispatchJob_hook(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                                 uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7) {
    if (!on_render_thread())
        __atomic_add_fetch(&stats.worker_runs, 1, __ATOMIC_RELAXED);
    return DispatchJob_orig(a0, a1, a2, a3, a4, a5, a6, a7);
}

void ssx_jobprof_patch(void) {
    uintptr_t base = so_mod.text_base;
    WaitOn_orig = (Fn8)hook_arm_trampoline(&so_mod, base + OFF_JobInstanceHandle_WaitOn, (uintptr_t)WaitOn_hook);
    RunOneJob_orig = (Fn8)hook_arm_trampoline(&so_mod, base + OFF_JobContextImpl_RunOneJob, (uintptr_t)RunOneJob_hook);
    DispatchJob_orig = (Fn8)hook_arm_trampoline(&so_mod, base + OFF_JobContextImpl_DispatchJob, (uintptr_t)DispatchJob_hook);
    if (!WaitOn_orig || !RunOneJob_orig || !DispatchJob_orig)
        l_error("[JOBPROF] could not hook WaitOn/RunOneJob");
}

void ssx_jobprof_report(uint32_t frames) {
    if (frames && (stats.waits || stats.runs))
        l_info("[PERF] jobs: render thread in WaitOn %.2f ms per frame (%.1f calls), running jobs itself %.2f ms "
               "(%.1f jobs); other threads ran %.1f jobs per frame; sIsAppPaused %d",
               stats.wait_us / 1000.0f / frames, stats.waits / (float)frames, stats.run_us / 1000.0f / frames,
               stats.runs / (float)frames, stats.worker_runs / (float)frames,
               *(volatile uint8_t *)(so_mod.text_base + OFF_sIsAppPaused));
    stats.wait_us = stats.run_us = 0;
    stats.waits = stats.runs = 0;
    __atomic_store_n(&stats.worker_runs, 0, __ATOMIC_RELAXED);
}
