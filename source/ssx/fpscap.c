/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  fpscap.c
 * @brief The game at 60 FPS instead of 30.
 *
 * EA::Blast::FpsLimiter::OnEndOfFrame sleeps until 1000 / limit ms have
 * passed since the last frame. The limiter is made at 60, but the game sets
 * 30. The frame time it waits for is kept at FRAME_MS at most; vsync (one
 * vblank per swap) still caps at 60.
 *
 * That limiter wasn't what held 30: the render loop (LoopRender) and the
 * simulation (LoopSimulation) are GameKernel::Loop objects made with a
 * hard-coded 30.0 Hz, and Loop::Run sleeps until a period has passed since
 * the last iteration. A thread here sets their rate (SetLoopFrequency).
 *
 * In races the simulation stays at 30 Hz. It advances game time by exactly
 * one period per iteration and catches up at most one period of delay
 * (LoopPreProcess), so at 60 Hz, where it ran ~31 iterations a second (each
 * step waits ~22 ms for the riders' EA::Ant jobs, which run nearly one after
 * another), the race played at ~52% speed. The render loop stays at 60 Hz:
 * the render objects interpolate between simulation steps (Model, Skel and
 * ScopeRenderObjState::InterpolateState), so frames in between move. A race is when its world is
 * loaded: one ~24.6 MB block of the GAMEWORLD_SLOTALLOC heap (25 MB), from
 * the loading screen until the menus after the results; in menus all 25 MB
 * are free.
 *
 * At 60 Hz the render thread holds the GPU lock (GameRender::AcquireThread)
 * nearly all the time, so another thread asking for it waits ~16 ms, the end
 * of a frame. Asset unloads asked for it once per file: see assetunload.c.
 *
 * The loops measure themselves (LoopPostProcess, every 0.5 s or more):
 * iterations per second at +136, average iteration time at +128; debug
 * builds log both loops every 5 s ([LOOPS]).
 */

#include "ssx/fpscap.h"

#include <stdint.h>

#include <psp2/kernel/threadmgr.h>
#include <so_util/so_util.h>

#include "ssx/memreport.h"
#include "ssx/settings.h"
#include "ssx/perf.h"
#include "reimpl/fastmath.h"
#include "utils/logger.h"

extern so_module so_mod;

#define OFF_SetFpsLimit 0x10bd6c  // EA::Blast::FpsLimiter::SetFpsLimit(unsigned)
#define OFF_OnEndOfFrame 0x10bcb8 // EA::Blast::FpsLimiter::OnEndOfFrame()
#define OFF_Loop_ctor 0x105a03c   // GameKernel::Loop::Loop(float hz, ProcessType, unsigned)
#define MIN_FPS 60
#define FRAME_MS (1000 / MIN_FPS)
#define LOOP_SIM 0    // GameKernel::Loop::ProcessType of LoopSimulation
#define LOOP_RENDER 1 // and of LoopRender
// GameKernel::Loop fields (GameTime is in nanoseconds)
#define LOOP_HZ(l) (*(volatile float *)((uint8_t *)(l) + 28))
#define LOOP_ITERATIONS(l) (*(volatile uint32_t *)((uint8_t *)(l) + 96))
#define LOOP_AVG_NS(l) (*(volatile int64_t *)((uint8_t *)(l) + 128))
#define LOOP_RATE(l) (*(volatile float *)((uint8_t *)(l) + 136))
#define REPORT_MS 5000
#define OFF_SetLoopFrequency 0x105a190 // GameKernel::Loop::SetLoopFrequency(float)
#define CHECK_MS 100
#define WORLD_HEAP "GAMEWORLD_SLOTALLOC"
#define WORLD_LOADED (12 * 1024 * 1024) // its largest free block under this: a race world is loaded
#define RACE_HZ 30.0f

// FpsLimiter: the frame time in ms (64 bits) at +0, a Stopwatch at +8
typedef struct {
    uint64_t frame_ms;
} FpsLimiter;

static void (*SetFpsLimit_orig)(FpsLimiter *, unsigned);
static void (*OnEndOfFrame_orig)(FpsLimiter *);
static void *(*Loop_ctor_orig)(void *, float, int, unsigned);
static void (*SetLoopFrequency)(void *, float);
static void *volatile sim_loop, *volatile render_loop;

static void *Loop_ctor_hook(void *self, float hz, int type, unsigned arg) {
    l_info("[FPSCAP] game loop (process type %d) made at %.1f Hz", type, hz);
    if (type == LOOP_SIM)
        sim_loop = self;
    else if (type == LOOP_RENDER)
        render_loop = self;
    return Loop_ctor_orig(self, hz, type, arg);
}

static int loop_rate_thread(SceSize args, void *argp) {
    void *world = NULL;
    while (1) {
        sceKernelDelayThread(CHECK_MS * 1000);
        if (!sim_loop || !render_loop)
            continue;
        if (!world && !(world = ssx_find_allocator(WORLD_HEAP)))
            continue;
        int race = ssx_allocator_largest_free(world) < WORLD_LOADED;
        float hz = race ? RACE_HZ : (float)MIN_FPS;
        // Races at 30 FPS if the player chose it (settings menu): the render
        // thread draws half as many frames, CPU time left to the riders' jobs
        float render_hz = race && ssx_settings.race_fps == 30 ? 30.0f : (float)MIN_FPS;
        if (LOOP_HZ(sim_loop) != hz || LOOP_HZ(render_loop) != render_hz) {
            SetLoopFrequency(sim_loop, hz);
            SetLoopFrequency(render_loop, render_hz);
            l_info("[FPSCAP] simulation at %.0f Hz, render at %.0f Hz (%s)", hz, render_hz,
                   race ? "race world loaded" : "menus");
        }
    }
    return 0;
}

static void SetFpsLimit_hook(FpsLimiter *self, unsigned fps) {
    l_info("[FPSCAP] the game sets the frame limit to %u FPS%s", fps, fps < MIN_FPS ? ", kept at 60" : "");
    SetFpsLimit_orig(self, fps < MIN_FPS ? MIN_FPS : fps);
}

static void OnEndOfFrame_hook(FpsLimiter *self) {
    if (self->frame_ms > FRAME_MS)
        self->frame_ms = FRAME_MS;
    OnEndOfFrame_orig(self);
}

#ifdef DEBUG_SOLOADER
static int loop_report_thread(SceSize args, void *argp) {
    uint32_t sim_prev = 0, render_prev = 0;
    uint64_t waits_prev[PERF_WAIT_KINDS] = {0};
    while (1) {
        sceKernelDelayThread(REPORT_MS * 1000);
        if (!sim_loop || !render_loop)
            continue;
        uint32_t sim_n = LOOP_ITERATIONS(sim_loop), render_n = LOOP_ITERATIONS(render_loop);
        uint64_t waits[PERF_WAIT_KINDS];
        perf_sim_waits(waits);
        uint32_t ds = sim_n - sim_prev, dr = render_n - render_prev;
        float secs = REPORT_MS / 1000.0f;
        float w[PERF_WAIT_KINDS]; // ms waited per iteration
        for (int k = 0; k < PERF_WAIT_KINDS; k++)
            w[k] = ds ? (waits[k] - waits_prev[k]) / 1000.0f / ds : 0;
        l_info("[LOOPS] simulation %.0f Hz: %.1f iterations a second (game at %.0f%% speed), %.1f ms each "
               "(last window); waited per iteration: locks %.1f, condvars %.1f, semaphores %.1f, sleep/yield %.1f ms. "
               "Render %.0f Hz: %.1f frames a second, %.1f ms each",
               LOOP_HZ(sim_loop), ds / secs, LOOP_HZ(sim_loop) > 0 ? ds / secs / LOOP_HZ(sim_loop) * 100 : 0,
               LOOP_AVG_NS(sim_loop) / 1e6f, w[0], w[1], w[2], w[3], LOOP_HZ(render_loop), dr / secs,
               LOOP_AVG_NS(render_loop) / 1e6f);
        if (SSX_DIAG) {
            perf_simwait_report();
            static uint32_t math_prev[MATH_KINDS];
            uint32_t m[MATH_KINDS];
            for (int k = 0; k < MATH_KINDS; k++) {
                m[k] = (ssx_math_calls[k] - math_prev[k]) / (REPORT_MS / 1000);
                math_prev[k] = ssx_math_calls[k];
            }
            l_info("[MATH] calls a second: sqrtf %u, floorf %u, sinf %u, cosf %u, powf %u, atan2f %u, acosf %u, expf %u",
                   m[MATH_SQRTF], m[MATH_FLOORF], m[MATH_SINF], m[MATH_COSF], m[MATH_POWF], m[MATH_ATAN2F],
                   m[MATH_ACOSF], m[MATH_EXPF]);
        }
        sim_prev = sim_n;
        render_prev = render_n;
        for (int k = 0; k < PERF_WAIT_KINDS; k++)
            waits_prev[k] = waits[k];
    }
    return 0;
}
#endif

void ssx_fpscap_patch(void) {
    SetFpsLimit_orig = (void *)hook_arm_trampoline(&so_mod, so_mod.text_base + OFF_SetFpsLimit,
                                                   (uintptr_t)SetFpsLimit_hook);
    OnEndOfFrame_orig = (void *)hook_arm_trampoline(&so_mod, so_mod.text_base + OFF_OnEndOfFrame,
                                                    (uintptr_t)OnEndOfFrame_hook);
    Loop_ctor_orig = (void *)hook_arm_trampoline(&so_mod, so_mod.text_base + OFF_Loop_ctor, (uintptr_t)Loop_ctor_hook);
    if (!SetFpsLimit_orig || !OnEndOfFrame_orig || !Loop_ctor_orig)
        l_error("[FPSCAP] hooks failed (%p %p %p)", SetFpsLimit_orig, OnEndOfFrame_orig, Loop_ctor_orig);
    SetLoopFrequency = (void *)(so_mod.text_base + OFF_SetLoopFrequency);
    SceUID rate = sceKernelCreateThread("ssx_looprate", loop_rate_thread, 0x10000100, 0x2000, 0, 0, NULL);
    if (rate >= 0)
        sceKernelStartThread(rate, 0, NULL);
#ifdef DEBUG_SOLOADER
    SceUID thid = sceKernelCreateThread("ssx_loops", loop_report_thread, 0x10000100, 0x2000, 0, 0, NULL);
    if (thid >= 0)
        sceKernelStartThread(thid, 0, NULL);
#endif
}
