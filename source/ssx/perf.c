/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  perf.c
 * @brief Performance profiler: frame timing, CPU use per thread, texture
 *        memory and file I/O, logged periodically as [PERF] lines.
 *
 * - Frame timing (perf_frame, from eglSwapBuffers): average and worst frame
 *   time, and the share spent in vglSwapBuffers, i.e. waiting for the GPU and
 *   vsync. A frame that takes long with little swap time is CPU-bound.
 * - CPU per thread: every thread the game creates registers itself; its run
 *   time over the last report window gives a percentage of one core.
 * - Texture memory: vitaGL's textures by kind (compressed, 32-, 24-, 16-,
 *   8-bit) and the largest ones.
 * - File I/O: time really spent opening, reading and seeking assets.
 */

#include "ssx/perf.h"
#include "ssx/glprof.h"
#include "ssx/asprof.h"
#include "ssx/gfxcache.h"

#include <stdint.h>
#include <pthread.h>
#include <string.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>

#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <vitaGL.h>

#include "utils/logger.h"
#include <so_util/so_util.h>

extern so_module so_mod;

// --- Frame timing ---------------------------------------------------------

static uint64_t frame_last_us;
static uint64_t frame_sum_us, frame_max_us, swap_sum_us;
static uint32_t frame_count;

static void hitch_check(uint64_t now, uint64_t dt, uint32_t swap_us);

void perf_frame(uint32_t swap_us) {
    uint64_t now = sceKernelGetProcessTimeWide();
    if (frame_last_us) {
        uint64_t dt = now - frame_last_us;
        hitch_check(now, dt, swap_us);
        frame_sum_us += dt;
        if (dt > frame_max_us)
            frame_max_us = dt;
        swap_sum_us += swap_us;
        frame_count++;
    }
    frame_last_us = now;
}

uint32_t perf_frame_summary(char *out, int size) {
    uint32_t frames = frame_count;
    if (!frame_count) {
        out[0] = '\0';
        return 0;
    }
    sceClibSnprintf(out, size, "frame avg %.1f ms, worst %.1f ms, %.1f ms of it in swap (GPU/vsync wait)",
                    frame_sum_us / 1000.0f / frame_count, frame_max_us / 1000.0f,
                    swap_sum_us / 1000.0f / frame_count);
    frame_sum_us = frame_max_us = swap_sum_us = 0;
    frame_count = 0;
    return frames;
}

// --- CPU per thread -------------------------------------------------------

#define MAX_THREADS 64

static struct {
    SceUID uid;
    uintptr_t handle; // pthread_t, for pthread_setname_np
    char name[32];
    uint64_t last_run_us;
} threads[MAX_THREADS];
static int nthreads;
static SceKernelLwMutexWork threads_lock;
static int threads_lock_ready;
static uint64_t threads_last_report_us;

static void threads_init(void) {
    if (!__atomic_exchange_n(&threads_lock_ready, 1, __ATOMIC_SEQ_CST))
        sceKernelCreateLwMutex(&threads_lock, "perf_threads", 0, 0, NULL);
}

// With threads_lock held: the entry of a pthread_t, created if needed
static int thread_slot(uintptr_t handle) {
    if (handle) {
        for (int i = 0; i < nthreads; i++)
            if (threads[i].handle == handle)
                return i;
    }
    if (nthreads == MAX_THREADS)
        return -1;
    memset(&threads[nthreads], 0, sizeof(threads[nthreads]));
    threads[nthreads].handle = handle;
    strcpy(threads[nthreads].name, "thread");
    return nthreads++;
}

void perf_thread_register(uintptr_t handle, const char *name) {
    threads_init();
    sceKernelLockLwMutex(&threads_lock, 1, NULL);
    int i = thread_slot(handle);
    if (i >= 0) {
        threads[i].uid = sceKernelGetThreadId();
        // Keep a name pthread_setname_np gave before the thread got here
        if (name && !strcmp(threads[i].name, "thread"))
            strncpy(threads[i].name, name, sizeof(threads[i].name) - 1);
    }
    sceKernelUnlockLwMutex(&threads_lock, 1);
}

void perf_thread_set_name(uintptr_t handle, const char *name) {
    threads_init();
    sceKernelLockLwMutex(&threads_lock, 1, NULL);
    int i = thread_slot(handle); // the thread may not have registered yet
    if (i >= 0) {
        strncpy(threads[i].name, name, sizeof(threads[i].name) - 1);
        threads[i].name[sizeof(threads[i].name) - 1] = 0;
    }
    sceKernelUnlockLwMutex(&threads_lock, 1);
}

static void report_threads(void) {
    uint64_t now = sceKernelGetProcessTimeWide();
    uint64_t window = now - threads_last_report_us;
    int first = threads_last_report_us == 0;
    threads_last_report_us = now;

    struct { const char *name; float pct; } busy[MAX_THREADS];
    int nbusy = 0;
    float total = 0;
    sceKernelLockLwMutex(&threads_lock, 1, NULL);
    for (int i = 0; i < nthreads; i++) {
        if (!threads[i].uid)
            continue; // named, not started yet
        SceKernelThreadInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(threads[i].uid, &info) < 0)
            continue; // exited
        uint64_t run = info.runClocks;
        uint64_t delta = run - threads[i].last_run_us;
        threads[i].last_run_us = run;
        if (first || !window)
            continue;
        float pct = 100.0f * delta / window;
        total += pct;
        if (pct >= 2.0f) {
            busy[nbusy].name = threads[i].name;
            busy[nbusy].pct = pct;
            nbusy++;
        }
    }
    sceKernelUnlockLwMutex(&threads_lock, 1);
    if (first)
        return;

    // Busiest first (few entries: insertion sort)
    for (int i = 1; i < nbusy; i++)
        for (int j = i; j > 0 && busy[j].pct > busy[j - 1].pct; j--) {
            const char *n = busy[j].name; float p = busy[j].pct;
            busy[j] = busy[j - 1];
            busy[j - 1].name = n; busy[j - 1].pct = p;
        }
    char line[512];
    int len = sceClibSnprintf(line, sizeof(line), "[PERF] CPU (%% of one core): game threads %.0f%% total;", total);
    for (int i = 0; i < nbusy && len < (int)sizeof(line) - 40; i++)
        len += sceClibSnprintf(line + len, sizeof(line) - len, " %s %.0f%%", busy[i].name, busy[i].pct);
    l_info("%s", line);
}

// --- Texture memory -------------------------------------------------------

static const char *const kind_names[VGL_SSX_TEX_KINDS] = { "compressed", "32-bit", "24-bit", "16-bit", "8-bit" };

static void report_textures(void) {
    uint32_t bytes[VGL_SSX_TEX_KINDS];
    vgl_ssx_tex_info top[8];
    int n = vglSsxTextureStats(bytes, top, 8);
    uint32_t total = 0;
    for (int k = 0; k < VGL_SSX_TEX_KINDS; k++)
        total += bytes[k];

    char line[512];
    int len = sceClibSnprintf(line, sizeof(line), "[PERF] textures: %d, %.1f MB:", n, total / 1048576.0f);
    for (int k = 0; k < VGL_SSX_TEX_KINDS; k++)
        len += sceClibSnprintf(line + len, sizeof(line) - len, " %s %.1f MB", kind_names[k], bytes[k] / 1048576.0f);
    l_info("%s", line);

    len = sceClibSnprintf(line, sizeof(line), "[PERF] largest textures:");
    for (int i = 0; i < 8 && top[i].bytes && len < (int)sizeof(line) - 48; i++)
        len += sceClibSnprintf(line + len, sizeof(line) - len, " #%u %ux%u fmt 0x%08X %u KB;",
                               (unsigned)top[i].id, (unsigned)top[i].w, (unsigned)top[i].h,
                               (unsigned)top[i].format, (unsigned)(top[i].bytes / 1024));
    l_info("%s", line);
}

// --- File I/O -------------------------------------------------------------

static volatile uint32_t io_opens, io_reads, io_seeks;
static volatile uint64_t io_open_us, io_read_us, io_seek_us, io_read_bytes;

// Running totals for hitch reports (never reset)
static struct {
    volatile uint32_t io_opens, io_reads, tex_uploads, links;
    volatile uint64_t io_open_us, io_read_us, io_bytes, tex_us, tex_bytes, link_us;
    volatile uint64_t wait_us[PERF_WAIT_KINDS];
} totals;

void perf_io_open(uint32_t us) {
    __atomic_add_fetch(&io_opens, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&io_open_us, us, __ATOMIC_RELAXED);
    __atomic_add_fetch(&totals.io_opens, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&totals.io_open_us, us, __ATOMIC_RELAXED);
}

void perf_io_read(uint32_t bytes, uint32_t us) {
    __atomic_add_fetch(&io_reads, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&io_read_bytes, bytes, __ATOMIC_RELAXED);
    __atomic_add_fetch(&io_read_us, us, __ATOMIC_RELAXED);
    __atomic_add_fetch(&totals.io_reads, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&totals.io_bytes, bytes, __ATOMIC_RELAXED);
    __atomic_add_fetch(&totals.io_read_us, us, __ATOMIC_RELAXED);
}

void perf_gl_upload(uint32_t bytes, uint32_t us) {
    __atomic_add_fetch(&totals.tex_uploads, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&totals.tex_bytes, bytes, __ATOMIC_RELAXED);
    __atomic_add_fetch(&totals.tex_us, us, __ATOMIC_RELAXED);
}

void perf_gl_link(uint32_t us) {
    __atomic_add_fetch(&totals.links, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&totals.link_us, us, __ATOMIC_RELAXED);
}

static char last_ui[48];
static volatile int sim_dump_armed; // see SIM_DUMP_US

void perf_note_ui(const char *view) {
    if (view) {
        strncpy(last_ui, view, sizeof(last_ui) - 1);
        if (!strcmp(view, "finishmessage") || strstr(view, "results"))
            sim_dump_armed = 1;
    }
}

void perf_io_seek(uint32_t us) {
    __atomic_add_fetch(&io_seeks, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&io_seek_us, us, __ATOMIC_RELAXED);
}

static void report_io(void) {
    uint32_t opens = __atomic_exchange_n(&io_opens, 0, __ATOMIC_RELAXED);
    uint32_t reads = __atomic_exchange_n(&io_reads, 0, __ATOMIC_RELAXED);
    uint32_t seeks = __atomic_exchange_n(&io_seeks, 0, __ATOMIC_RELAXED);
    uint64_t open_us = __atomic_exchange_n(&io_open_us, 0, __ATOMIC_RELAXED);
    uint64_t read_us = __atomic_exchange_n(&io_read_us, 0, __ATOMIC_RELAXED);
    uint64_t seek_us = __atomic_exchange_n(&io_seek_us, 0, __ATOMIC_RELAXED);
    uint64_t bytes = __atomic_exchange_n(&io_read_bytes, 0, __ATOMIC_RELAXED);
    if (!opens && !reads)
        return;
    l_info("[PERF] I/O: %u opens %.0f ms (avg %.1f), %u reads %.1f MB %.0f ms (%.1f MB/s), %u seeks %.0f ms",
           opens, open_us / 1000.0f, opens ? open_us / 1000.0f / opens : 0.0f,
           reads, bytes / 1048576.0f, read_us / 1000.0f,
           read_us ? bytes / 1.048576f / read_us : 0.0f, seeks, seek_us / 1000.0f);
}

// --- Report -----------------------------------------------------------------

void perf_report(void) {
    threads_init();
    report_threads();
    report_textures();
    report_io();
}

// --- Frame phases -----------------------------------------------------------
//
// Time spent in a few big functions of the game's frame, per frame on
// average. Phases nest (OnUpdate contains most of the others).

#include <so_util/so_util.h>

extern so_module so_mod;

enum {
    PH_ONUPDATE, PH_PROCESS, PH_DRAW, PH_RENDERUI, PH_SF_UPDATE, PH_SF_RENDER,
    PH_PHYS_BEGIN, PH_PHYS_END, PH_AUDIO, PH_GL_DRAW, PH_COUNT
};
static const char *const phase_names[PH_COUNT] = {
    "frame (OnUpdate)", "GameRender::Process", "GameRender::Draw", "GameRender::RenderUi",
    "Scaleform update", "Scaleform render", "physics begin", "physics end", "audio update",
    "GL draw calls",
};
static volatile uint64_t phase_us[PH_COUNT];

void perf_phase_add(int phase, uint32_t us) {
    __atomic_add_fetch(&phase_us[phase], us, __ATOMIC_RELAXED);
}

#define NOW() sceKernelGetProcessTimeWide()
#define TIMED(ph, call) do { uint64_t _t0 = NOW(); call; perf_phase_add(ph, (uint32_t)(NOW() - _t0)); } while (0)

// Exact signatures (softfp: doubles in r2:r3, extra words on the stack);
// results come back in r0:r1, passed through as uint64_t
static uint64_t (*OnUpdate_orig)(void *, void *);
static uint64_t OnUpdate_hook(void *s, void *a) { uint64_t r; TIMED(PH_ONUPDATE, r = OnUpdate_orig(s, a)); return r; }
static uint64_t (*Process_orig)(void *, double, int);
static uint64_t Process_hook(void *s, double t, int b) { uint64_t r; TIMED(PH_PROCESS, r = Process_orig(s, t, b)); return r; }
static uint64_t (*Draw_orig)(void *, double, int, void *);
// GameRender::Draw runs on the render thread only. The game names that thread
// FGGameRender (prctl), which gives it its priority (reimpl/pthr.c), but
// since some build it no longer does: done here on its first frame instead.
static void render_thread_setup(void) {
    static int done;
    if (done)
        return;
    done = 1;
    if (sceKernelGetThreadCurrentPriority() > 0x98)
        sceKernelChangeThreadPriority(0, 0x98);
    perf_thread_set_name((uintptr_t)pthread_self(), "FGGameRender");
    l_info("[THREAD] render thread is 0x%x (Vita priority 0x%x)", (unsigned)sceKernelGetThreadId(),
           (unsigned)sceKernelGetThreadCurrentPriority());
}
#ifdef DEBUG_SOLOADER
// A/B test: the optimizations under test (see perf.h) switch on and off
// every AB_PERIOD_US, between two GameRender::Draw calls; Draw's time is
// added up by state and logged every AB_REPORT_US
#define AB_TEST 0 // 1 to switch them; 0: always on, Draw's time still logged
#define AB_PERIOD_US 10000000
#define AB_REPORT_US 60000000
volatile int ssx_ab_off;
static uint64_t ab_us[2];
static uint32_t ab_frames[2];

static void ab_draw(uint64_t start, uint32_t us) {
    static uint64_t period_start, report_start;
    if (!period_start)
        period_start = report_start = start;
    ab_us[ssx_ab_off] += us;
    ab_frames[ssx_ab_off]++;
    uint64_t now = start + us;
    if (AB_TEST && now - period_start >= AB_PERIOD_US) {
        ssx_ab_off = !ssx_ab_off;
        period_start = now;
    }
    if (now - report_start >= AB_REPORT_US) {
        if (AB_TEST)
            l_info("[ABTEST] GameRender::Draw over the last %u s: optimizations on %.2f ms (%u frames), off %.2f ms "
                   "(%u frames)",
                   (unsigned)((now - report_start) / 1000000), ab_frames[0] ? ab_us[0] / 1000.0f / ab_frames[0] : 0.0f,
                   ab_frames[0], ab_frames[1] ? ab_us[1] / 1000.0f / ab_frames[1] : 0.0f, ab_frames[1]);
        ab_us[0] = ab_us[1] = 0;
        ab_frames[0] = ab_frames[1] = 0;
        report_start = now;
    }
}
#endif

static uint64_t Draw_hook(void *s, double t, int b, void *m) {
    uint64_t r;
    render_thread_setup();
#ifdef DEBUG_SOLOADER
    uint64_t t0 = NOW();
    r = Draw_orig(s, t, b, m);
    uint32_t us = (uint32_t)(NOW() - t0);
    perf_phase_add(PH_DRAW, us);
    ab_draw(t0, us);
#else
    TIMED(PH_DRAW, r = Draw_orig(s, t, b, m));
#endif
    return r;
}
static uint64_t (*RenderUi_orig)(void *, double);
static uint64_t RenderUi_hook(void *s, double t) { uint64_t r; TIMED(PH_RENDERUI, r = RenderUi_orig(s, t)); return r; }
static uint64_t (*SfUpdate_orig)(void *);
static uint64_t SfUpdate_hook(void *s) { uint64_t r; TIMED(PH_SF_UPDATE, r = SfUpdate_orig(s)); return r; }
static uint64_t (*SfRender_orig)(void *);
static uint64_t SfRender_hook(void *s) { uint64_t r; TIMED(PH_SF_RENDER, r = SfRender_orig(s)); return r; }
static uint64_t (*PhysBegin_orig)(void *, void *, void *);
static uint64_t PhysBegin_hook(void *s, void *a, void *b) { uint64_t r; TIMED(PH_PHYS_BEGIN, r = PhysBegin_orig(s, a, b)); return r; }
static uint64_t (*PhysEnd_orig)(void *, void *);
static uint64_t PhysEnd_hook(void *s, void *a) { uint64_t r; TIMED(PH_PHYS_END, r = PhysEnd_orig(s, a)); return r; }
static uint64_t (*Audio_orig)(void *);
static uint64_t Audio_hook(void *s) { uint64_t r; TIMED(PH_AUDIO, r = Audio_orig(s)); return r; }

static const struct {
    uint32_t addr;
    void *hook;
    void **orig;
} phase_hooks[] = {
    { 0x015fff0, OnUpdate_hook, (void **)&OnUpdate_orig },   // EA::GameSkeleton::GameApplication::OnUpdate(void*)
    { 0x1041138, Process_hook, (void **)&Process_orig },     // GameRender::Process(double, bool)
    { 0x1046648, Draw_hook, (void **)&Draw_orig },           // GameRender::Draw(double, bool, Matrix44*)
    { 0x0dd90f8, RenderUi_hook, (void **)&RenderUi_orig },   // GameRender::RenderUi(double)
    { 0x0a9cacc, SfUpdate_hook, (void **)&SfUpdate_orig },   // EA::ScaleformBridge::ScaleformEngine::Update()
    { 0x0a9d7a4, SfRender_hook, (void **)&SfRender_orig },   // EA::ScaleformBridge::ScaleformEngine::RenderScaleform()
    { 0x10db2f4, PhysBegin_hook, (void **)&PhysBegin_orig }, // EA::Physics::Simulation::BeginFrame(void*, void*)
    { 0x10dbd64, PhysEnd_hook, (void **)&PhysEnd_orig },     // EA::Physics::Simulation::EndFrame(void*)
    { 0x11ef080, Audio_hook, (void **)&Audio_orig },         // AudioFramework::AudioSystem::Update()
};

void perf_patch(void) {
    int n = 0;
    for (unsigned i = 0; i < sizeof(phase_hooks) / sizeof(phase_hooks[0]); i++) {
        *phase_hooks[i].orig = (void *)hook_arm_trampoline(&so_mod, so_mod.text_base + phase_hooks[i].addr,
                                                           (uintptr_t)phase_hooks[i].hook);
        if (*phase_hooks[i].orig)
            n++;
        else
            l_warn("[PERF] could not time %s", phase_names[i]);
    }
    l_info("[PERF] timing %d frame phases", n);
}

void perf_phase_summary(char *out, int size, uint32_t frames) {
    int len = sceClibSnprintf(out, size, "ms per frame:");
    for (int i = 0; i < PH_COUNT; i++) {
        uint64_t us = __atomic_exchange_n(&phase_us[i], 0, __ATOMIC_RELAXED);
        if (frames && len < size - 40)
            len += sceClibSnprintf(out + len, size - len, " %s %.1f;", phase_names[i], us / 1000.0f / frames);
    }
}

void perf_gl_draw_time(uint32_t us) {
    perf_phase_add(PH_GL_DRAW, us);
}

// --- Render thread waits ----------------------------------------------------
//
// Time the render thread (the one presenting frames) spends blocked, by kind:
// in a game frame it otherwise runs flat out, so this is time lost.

static volatile SceUID render_uid;
static volatile uint64_t wait_us[PERF_WAIT_KINDS];
static const char *const wait_names[PERF_WAIT_KINDS] = { "locks", "condvars", "semaphores", "sleep/yield" };

#define STALL_US 200000

int perf_stack_words(const void *sp, int max) {
    SceKernelThreadInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) < 0)
        return 0;
    int words = ((uint8_t *)info.stack + info.stackSize - (const uint8_t *)sp) / 4;
    return words < 0 ? 0 : words > max ? max : words;
}

// A wait over STALL_US by the threads that drive a frame (FGGameSim,
// FGGameRender): logged with the libgame code on the stack (debug builds)
static void stall_report(int kind, uint32_t us) {
#ifdef DEBUG_SOLOADER
    SceUID me = sceKernelGetThreadId();
    const char *name = NULL;
    sceKernelLockLwMutex(&threads_lock, 1, NULL);
    for (int i = 0; i < nthreads; i++)
        if (threads[i].uid == me && !strncmp(threads[i].name, "FGGame", 6))
            name = threads[i].name;
    sceKernelUnlockLwMutex(&threads_lock, 1);
    if (!name)
        return;
    char trace[120] = "";
    const uint32_t *sp = (const uint32_t *)__builtin_frame_address(0);
    int words = perf_stack_words(sp, 512);
    for (int i = 0, n = 0, len = 0; i < words && n < 8 && len < (int)sizeof(trace) - 12; i++) {
        uint32_t v = sp[i];
        if (v > so_mod.text_base && v < so_mod.text_base + so_mod.text_size) {
            len += sceClibSnprintf(trace + len, sizeof(trace) - len, " 0x%X", (unsigned)(v - so_mod.text_base));
            n++;
        }
    }
    l_info("[STALL] %s waited %u ms on %s (last UI view: %s), game stack%s", name, us / 1000, wait_names[kind],
           last_ui[0] ? last_ui : "-", trace);
#else
    (void)kind;
    (void)us;
#endif
}

static volatile SceUID sim_uid; // FGGameSim, see perf_sim_patch
static volatile uint64_t sim_wait_us[PERF_WAIT_KINDS];

void perf_sim_waits(uint64_t out[PERF_WAIT_KINDS]) {
    for (int k = 0; k < PERF_WAIT_KINDS; k++)
        out[k] = sim_wait_us[k];
}

static void sim_stuck_check(int kind, uint32_t us);

#ifdef DEBUG_SOLOADER
// Where the simulation thread waits on semaphores ([SIMWAIT]): in races it
// spends ~15 of its ~20 ms per iteration there. Waits of SIMWAIT_MIN_US or
// more are grouped by the first SIMWAIT_DEPTH game addresses on the stack.
#define SIMWAIT_MIN_US 1000
#define SIMWAIT_DEPTH 6
#define SIMWAIT_SLOTS 12
static struct {
    uint32_t sig[SIMWAIT_DEPTH], n;
    uint64_t us;
} simwait[SIMWAIT_SLOTS];
static uint32_t simwait_other_n;
static uint64_t simwait_other_us;

static void simwait_add(uint32_t us) {
    uint32_t sig[SIMWAIT_DEPTH] = { 0 };
    const uint32_t *sp = (const uint32_t *)__builtin_frame_address(0);
    int words = perf_stack_words(sp, 1024);
    for (int i = 0, n = 0; i < words && n < SIMWAIT_DEPTH; i++)
        if (sp[i] > so_mod.text_base && sp[i] < so_mod.text_base + so_mod.text_size)
            sig[n++] = sp[i] - so_mod.text_base;
    for (int k = 0; k < SIMWAIT_SLOTS; k++) {
        if (!simwait[k].n)
            memcpy(simwait[k].sig, sig, sizeof(sig));
        if (!memcmp(simwait[k].sig, sig, sizeof(sig))) {
            simwait[k].n++;
            simwait[k].us += us;
            return;
        }
    }
    simwait_other_n++;
    simwait_other_us += us;
}

void perf_simwait_report(void) {
    // Only the simulation thread writes the table; a report racing a write
    // can be off by one wait
    for (int k = 0; k < SIMWAIT_SLOTS && simwait[k].n; k++) {
        char stack[96];
        int len = 0;
        for (int i = 0; i < SIMWAIT_DEPTH && simwait[k].sig[i]; i++)
            len += sceClibSnprintf(stack + len, sizeof(stack) - len, " 0x%X", (unsigned)simwait[k].sig[i]);
        l_info("[SIMWAIT] %u semaphore waits, %u ms (avg %u us), game stack%s", (unsigned)simwait[k].n,
               (unsigned)(simwait[k].us / 1000), (unsigned)(simwait[k].us / simwait[k].n), len ? stack : " -");
    }
    if (simwait_other_n)
        l_info("[SIMWAIT] %u other waits, %u ms", (unsigned)simwait_other_n, (unsigned)(simwait_other_us / 1000));
    memset(simwait, 0, sizeof(simwait));
    simwait_other_n = 0;
    simwait_other_us = 0;
}
#else
void perf_simwait_report(void) {}
#endif

void perf_wait_add(int kind, uint32_t us) {
    if (sim_uid && sceKernelGetThreadId() == sim_uid) {
        __atomic_add_fetch(&sim_wait_us[kind], us, __ATOMIC_RELAXED);
        sim_stuck_check(kind, us);
#ifdef DEBUG_SOLOADER
        if (SSX_DIAG && kind == PERF_WAIT_SEM && us >= SIMWAIT_MIN_US)
            simwait_add(us);
#endif
    }
    if (render_uid && sceKernelGetThreadId() == render_uid) {
        __atomic_add_fetch(&wait_us[kind], us, __ATOMIC_RELAXED);
        __atomic_add_fetch(&totals.wait_us[kind], us, __ATOMIC_RELAXED);
    }
    if (us > STALL_US && threads_lock_ready)
        stall_report(kind, us);
}

// --- Hitches ----------------------------------------------------------------
//
// A frame over HITCH_US logs what happened while it ran: CPU time of the
// busiest threads, the render thread's waits, file I/O (all threads), texture
// uploads and shader links, and the last UI view loaded. Debug builds only:
// it reads every thread's CPU time each frame.

#define HITCH_US 100000
#define SCAN_EVERY 30 // frames between two reads of all the threads' run times

static struct {
    uint32_t io_opens, io_reads, tex_uploads, links;
    uint64_t io_open_us, io_read_us, io_bytes, tex_us, tex_bytes, link_us;
    uint64_t wait_us[PERF_WAIT_KINDS];
    uint64_t run[MAX_THREADS];
    uint32_t preempt[MAX_THREADS], moves[MAX_THREADS];
} hitch_prev;

// The render and logic threads in a hitch frame: preemptions and core
// changes in it, affinity mask, the core they last ran on
typedef struct {
    int found;
    uint32_t run_us, preempt, moves, mask, cpu, prio;
} thread_sched_t;

static void hitch_check(uint64_t now, uint64_t dt, uint32_t swap_us) {
#ifdef DEBUG_SOLOADER
    (void)now;
    render_uid = sceKernelGetThreadId(); // perf_frame runs on the render thread
    struct { const char *name; uint32_t us; } busy[MAX_THREADS];
    int nbusy = 0;
    thread_sched_t rs = { 0 }, ss = { 0 };
    static int table_logged;
    int log_table = dt > HITCH_US && !table_logged;
    if (log_table)
        table_logged = 1;
    threads_init();
    // sceKernelGetThreadInfo for every thread took ~2 ms a frame: the threads
    // are read every SCAN_EVERY frames and on a hitch (their time then since
    // the last read)
    static uint32_t scan_n;
    int scan = dt > HITCH_US || log_table || ++scan_n % SCAN_EVERY == 0;
    sceKernelLockLwMutex(&threads_lock, 1, NULL);
    for (int i = 0; scan && i < nthreads; i++) {
        if (!threads[i].uid)
            continue;
        SceKernelThreadInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(threads[i].uid, &info) < 0)
            continue;
        uint64_t run = info.runClocks;
        uint32_t delta = (uint32_t)(run - hitch_prev.run[i]);
        int had = hitch_prev.run[i] != 0;
        hitch_prev.run[i] = run;
        uint32_t d_preempt = info.threadPreemptCount - hitch_prev.preempt[i];
        uint32_t d_moves = (uint32_t)info.changeCpuCount - hitch_prev.moves[i];
        hitch_prev.preempt[i] = info.threadPreemptCount;
        hitch_prev.moves[i] = (uint32_t)info.changeCpuCount;
        thread_sched_t *ts = threads[i].uid == render_uid ? &rs : threads[i].uid == sim_uid ? &ss : NULL;
        if (ts && had) {
            ts->found = 1;
            ts->run_us = delta;
            ts->preempt = d_preempt;
            ts->moves = d_moves;
            ts->mask = (uint32_t)info.currentCpuAffinityMask;
            ts->cpu = (uint32_t)info.lastExecutedCpuId;
            ts->prio = (uint32_t)info.currentPriority;
        }
        if (log_table)
            l_info("[SCHED] thread %s: priority 0x%x, affinity mask 0x%x (initial 0x%x), last on CPU %d, %u core changes",
                   threads[i].name, (unsigned)info.currentPriority, (unsigned)info.currentCpuAffinityMask,
                   (unsigned)info.initCpuAffinityMask, (int)info.lastExecutedCpuId, (unsigned)info.changeCpuCount);
        if (dt > HITCH_US && had && delta >= 5000) {
            busy[nbusy].name = threads[i].name;
            busy[nbusy].us = delta;
            nbusy++;
        }
    }
    sceKernelUnlockLwMutex(&threads_lock, 1);

    #define TAKE(f) uint64_t d_##f = totals.f - hitch_prev.f; hitch_prev.f = totals.f
    TAKE(io_opens); TAKE(io_reads); TAKE(tex_uploads); TAKE(links);
    TAKE(io_open_us); TAKE(io_read_us); TAKE(io_bytes); TAKE(tex_us); TAKE(tex_bytes); TAKE(link_us);
    #undef TAKE
    uint64_t d_wait[PERF_WAIT_KINDS];
    for (int k = 0; k < PERF_WAIT_KINDS; k++) {
        d_wait[k] = totals.wait_us[k] - hitch_prev.wait_us[k];
        hitch_prev.wait_us[k] = totals.wait_us[k];
    }
    if (dt <= HITCH_US)
        return;

    for (int i = 1; i < nbusy; i++)
        for (int j = i; j > 0 && busy[j].us > busy[j - 1].us; j--) {
            const char *n = busy[j].name; uint32_t u = busy[j].us;
            busy[j] = busy[j - 1];
            busy[j - 1].name = n; busy[j - 1].us = u;
        }
    char line[640];
    int len = sceClibSnprintf(line, sizeof(line), "[HITCH] %u ms frame (last UI view: %s): CPU",
                              (unsigned)(dt / 1000), last_ui[0] ? last_ui : "-");
    for (int i = 0; i < nbusy && i < 6 && len < (int)sizeof(line) - 60; i++)
        len += sceClibSnprintf(line + len, sizeof(line) - len, " %s %u", busy[i].name, busy[i].us / 1000);
    len += sceClibSnprintf(line + len, sizeof(line) - len, " ms; render waited");
    for (int k = 0; k < PERF_WAIT_KINDS; k++)
        len += sceClibSnprintf(line + len, sizeof(line) - len, " %s %u", wait_names[k], (unsigned)(d_wait[k] / 1000));
    len += sceClibSnprintf(line + len, sizeof(line) - len,
                    " ms; I/O %u opens %u ms, %u reads %u KB %u ms; %u texture uploads %u KB %u ms; %u links %u ms",
                    (unsigned)d_io_opens, (unsigned)(d_io_open_us / 1000), (unsigned)d_io_reads,
                    (unsigned)(d_io_bytes / 1024), (unsigned)(d_io_read_us / 1000), (unsigned)d_tex_uploads,
                    (unsigned)(d_tex_bytes / 1024), (unsigned)(d_tex_us / 1000), (unsigned)d_links,
                    (unsigned)(d_link_us / 1000));
    l_info("%s", line);
    // Where the render thread's frame went: running, waiting (tracked), in
    // the swap, and the rest (not running though ready, or a wait not tracked)
    uint64_t waited = 0;
    for (int k = 0; k < PERF_WAIT_KINDS; k++)
        waited += d_wait[k];
    int64_t rest = (int64_t)dt - rs.run_us - (int64_t)waited - swap_us;
    l_info("[HITCH] ... render thread: ran %u ms, waited %u ms, swap %u ms, rest %d ms; preempted %u times, %u core "
           "changes, mask 0x%x, CPU %u, priority 0x%x | logic thread: ran %u ms, preempted %u, %u core changes, mask "
           "0x%x, CPU %u, priority 0x%x",
           rs.run_us / 1000, (unsigned)(waited / 1000), swap_us / 1000, (int)(rest / 1000), rs.preempt, rs.moves,
           rs.mask, rs.cpu, rs.prio, ss.run_us / 1000, ss.preempt, ss.moves, ss.mask, ss.cpu, ss.prio);
#else
    (void)now;
    (void)dt;
    (void)swap_us;
#endif
}

// --- Game logic frames --------------------------------------------------------
//
// GameLoop::GameLoopProcess runs once per frame on both FGGameRender and
// FGGameSim; on FGGameSim, a frame (start to next start) over SIM_HITCH_US is
// logged with the time spent in the frame's own work, the waits in it, and
// file I/O meanwhile: a freeze of the game logic doesn't show as a long
// render frame (the render thread keeps presenting the same picture).

#define SIM_HITCH_US 150000
#define OFF_GameLoopProcess 0x10598fc // GameLoop::GameLoopProcess(float)
// Debug aid, 0 = off: crash for a core dump when a logic frame after a race
// runs longer than this
#define SIM_DUMP_US 0

static uint64_t (*GameLoopProcess_orig)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
static uint64_t sim_last_start, sim_busy_us;
static volatile uint64_t sim_frame_start; // 0 outside GameLoopProcess

// A logic frame running over SIM_STUCK_US: each wait it makes logs the game
// code on the stack, at most once per SIM_STUCK_EVERY_US, to see what it is
// stuck on (the waits can be short and many, under the [STALL] threshold)
#define SIM_STUCK_US 1000000
#define SIM_STUCK_EVERY_US 1000000

static void sim_stuck_check(int kind, uint32_t us) {
#ifdef DEBUG_SOLOADER
    static uint64_t last_log;
    uint64_t start = sim_frame_start, now = sceKernelGetProcessTimeWide();
    if (!start || now - start < SIM_STUCK_US || now - last_log < SIM_STUCK_EVERY_US)
        return;
    last_log = now;
    char trace[200] = "";
    const uint32_t *sp = (const uint32_t *)__builtin_frame_address(0);
    int words = perf_stack_words(sp, 512); // 512 words ran past a thread's stack top and faulted
    for (int i = 0, n = 0, len = 0; i < words && n < 14 && len < (int)sizeof(trace) - 12; i++) {
        uint32_t v = sp[i];
        if (v > so_mod.text_base && v < so_mod.text_base + so_mod.text_size) {
            len += sceClibSnprintf(trace + len, sizeof(trace) - len, " 0x%X", (unsigned)(v - so_mod.text_base));
            n++;
        }
    }
    l_info("[SIMSTUCK] logic frame running for %u ms, waited %u us on %s (last UI view: %s), game stack%s",
           (unsigned)((now - start) / 1000), us, wait_names[kind], last_ui[0] ? last_ui : "-", trace);
#else
    (void)kind;
    (void)us;
#endif
}
static struct {
    uint32_t io_opens, io_reads;
    uint64_t io_open_us, io_read_us, io_bytes, wait_us[PERF_WAIT_KINDS];
} sim_prev;

// Slow stretches: logic frames over SLOW_FRAME_US in a row add up to a
// freeze even when no single one passes SIM_HITCH_US (the picture stops
// moving while the render thread keeps presenting it)
#define SLOW_FRAME_US 50000
#define SLOW_RUN_US 250000
static struct {
    uint64_t start, total_us, max_us, cpu0;
    uint32_t frames, io_reads0, io_opens0;
    uint64_t io_bytes0, io_read_us0, wait0[PERF_WAIT_KINDS];
    char ui[48];
} slow;
static volatile uint32_t sim_frames_window, sim_slow_frames_window;

static uint64_t thread_cpu_us(SceUID uid) {
    SceKernelThreadInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(uid, &info) < 0)
        return 0;
    return info.runClocks;
}

static void slow_frame(uint64_t now, uint64_t frame_us) {
    sim_frames_window++;
    if (frame_us > SLOW_FRAME_US) {
        sim_slow_frames_window++;
        if (!slow.frames) {
            slow.start = now - frame_us;
            slow.total_us = slow.max_us = 0;
            // the CPU time of the frame that just ended can't be split off: approximate from here
            slow.cpu0 = thread_cpu_us(sim_uid);
            slow.io_reads0 = totals.io_reads;
            slow.io_opens0 = totals.io_opens;
            slow.io_bytes0 = totals.io_bytes;
            slow.io_read_us0 = totals.io_read_us;
            for (int k = 0; k < PERF_WAIT_KINDS; k++)
                slow.wait0[k] = sim_wait_us[k];
            strncpy(slow.ui, last_ui[0] ? last_ui : "-", sizeof(slow.ui) - 1);
        }
        slow.frames++;
        slow.total_us += frame_us;
        if (frame_us > slow.max_us)
            slow.max_us = frame_us;
        return;
    }
    if (slow.frames && slow.total_us >= SLOW_RUN_US) {
        uint64_t cpu = thread_cpu_us(sim_uid) - slow.cpu0;
        uint64_t w[PERF_WAIT_KINDS];
        for (int k = 0; k < PERF_WAIT_KINDS; k++)
            w[k] = sim_wait_us[k] - slow.wait0[k];
        l_info("[SIMSLOW] %u ms of slow logic frames (%u frames, longest %u ms; started on UI view %s, now %s): "
               "sim CPU ~%u ms; waited locks %u condvars %u semaphores %u sleep/yield %u ms; I/O %u opens, %u reads "
               "%u KB %u ms",
               (unsigned)(slow.total_us / 1000), slow.frames, (unsigned)(slow.max_us / 1000), slow.ui,
               last_ui[0] ? last_ui : "-", (unsigned)(cpu / 1000), (unsigned)(w[0] / 1000), (unsigned)(w[1] / 1000),
               (unsigned)(w[2] / 1000), (unsigned)(w[3] / 1000), totals.io_opens - slow.io_opens0,
               totals.io_reads - slow.io_reads0, (unsigned)((totals.io_bytes - slow.io_bytes0) / 1024),
               (unsigned)((totals.io_read_us - slow.io_read_us0) / 1000));
    }
    slow.frames = 0;
}

void perf_sim_report(void) {
    uint32_t n = sim_frames_window, s = sim_slow_frames_window;
    sim_frames_window = sim_slow_frames_window = 0;
    if (n)
        l_info("[PERF] logic frames: %u in this window, %u over %u ms", n, s, SLOW_FRAME_US / 1000);
}

static int is_sim_thread(void) {
    SceUID me = sceKernelGetThreadId();
    if (sim_uid)
        return me == sim_uid;
    int found = 0;
    sceKernelLockLwMutex(&threads_lock, 1, NULL);
    for (int i = 0; i < nthreads; i++)
        if (threads[i].uid == me && !strcmp(threads[i].name, "FGGameSim"))
            found = 1;
    sceKernelUnlockLwMutex(&threads_lock, 1);
    if (found)
        sim_uid = me;
    return found;
}

static uint64_t GameLoopProcess_hook(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                                     uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7) {
    if (!threads_lock_ready || !is_sim_thread())
        return GameLoopProcess_orig(a0, a1, a2, a3, a4, a5, a6, a7);
    uint64_t t0 = sceKernelGetProcessTimeWide();
    if (sim_last_start && t0 - sim_last_start > SIM_HITCH_US) {
        uint64_t frame = t0 - sim_last_start;
        uint64_t w[PERF_WAIT_KINDS];
        for (int k = 0; k < PERF_WAIT_KINDS; k++)
            w[k] = sim_wait_us[k] - sim_prev.wait_us[k];
        l_info("[SIMHITCH] %u ms logic frame (last UI view: %s): %u ms in GameLoopProcess, %u ms outside; waited "
               "locks %u condvars %u semaphores %u sleep/yield %u ms; I/O %u opens %u ms, %u reads %u KB %u ms",
               (unsigned)(frame / 1000), last_ui[0] ? last_ui : "-", (unsigned)(sim_busy_us / 1000),
               (unsigned)((frame - sim_busy_us) / 1000), (unsigned)(w[0] / 1000), (unsigned)(w[1] / 1000),
               (unsigned)(w[2] / 1000), (unsigned)(w[3] / 1000), (unsigned)(totals.io_opens - sim_prev.io_opens),
               (unsigned)((totals.io_open_us - sim_prev.io_open_us) / 1000), (unsigned)(totals.io_reads - sim_prev.io_reads),
               (unsigned)((totals.io_bytes - sim_prev.io_bytes) / 1024),
               (unsigned)((totals.io_read_us - sim_prev.io_read_us) / 1000));
    }
    if (sim_last_start)
        slow_frame(t0, t0 - sim_last_start);
    {
        static int asprof_thread_set;
        if (!asprof_thread_set) {
            asprof_set_thread(sim_uid);
            gfxcache_set_thread();
            asprof_thread_set = 1;
        }
        uint32_t prev_ms = sim_last_start ? (uint32_t)((t0 - sim_last_start) / 1000) : 0;
        asprof_frame(prev_ms, last_ui[0] ? last_ui : "-", sim_last_start && t0 - sim_last_start > SIM_HITCH_US);
        gfxcache_frame(prev_ms, last_ui[0] ? last_ui : "-", sim_last_start && t0 - sim_last_start > SIM_HITCH_US);
    }
#if SSX_FNPROF
    // What the frame that just ended spent its time on, when it was long
    if (sim_last_start && t0 - sim_last_start > SIM_HITCH_US) {
        char what[96];
        sceClibSnprintf(what, sizeof(what), "%u ms logic frame on %s", (unsigned)((t0 - sim_last_start) / 1000),
                        last_ui[0] ? last_ui : "-");
        fnprof_frame_report(what);
    }
    fnprof_frame_start();
#endif
    sim_last_start = t0;
    for (int k = 0; k < PERF_WAIT_KINDS; k++)
        sim_prev.wait_us[k] = sim_wait_us[k];
    sim_prev.io_opens = totals.io_opens;
    sim_prev.io_reads = totals.io_reads;
    sim_prev.io_open_us = totals.io_open_us;
    sim_prev.io_read_us = totals.io_read_us;
    sim_prev.io_bytes = totals.io_bytes;
    sim_frame_start = t0;
    uint64_t r = GameLoopProcess_orig(a0, a1, a2, a3, a4, a5, a6, a7);
    sim_frame_start = 0;
    sim_busy_us = sceKernelGetProcessTimeWide() - t0;
    return r;
}

#if SIM_DUMP_US && defined(DEBUG_SOLOADER)
// Crashes once, from its own thread, when a logic frame on a results screen
// runs past SIM_DUMP_US: the core dump then holds FGGameSim's stack in the
// middle of the freeze (the HITCH lines show no I/O or waits to explain it).
static int sim_dump_thread(SceSize args, void *argp) {
    while (1) {
        sceKernelDelayThread(50 * 1000);
        uint64_t start = sim_frame_start;
        if (sim_dump_armed && start && sceKernelGetProcessTimeWide() - start > SIM_DUMP_US) {
            sceClibPrintf("[SIMHITCH] logic frame over %u ms on %s; crashing on purpose to get a core dump\n",
                          SIM_DUMP_US / 1000, last_ui);
            *(volatile int *)0 = 0;
        }
    }
    return 0;
}
#endif

uint64_t perf_sim_frame_start(void) {
    return sim_frame_start;
}

void perf_sim_patch(void) {
#ifdef DEBUG_SOLOADER
    GameLoopProcess_orig = (void *)hook_arm_trampoline(&so_mod, so_mod.text_base + OFF_GameLoopProcess,
                                                       (uintptr_t)GameLoopProcess_hook);
    if (!GameLoopProcess_orig)
        l_error("[PERF] could not hook GameLoop::GameLoopProcess");
#if SIM_DUMP_US && defined(DEBUG_SOLOADER)
    SceUID thid = sceKernelCreateThread("ssx_sim_dump", sim_dump_thread, 64, 0x1000, 0, 0, NULL);
    if (thid >= 0)
        sceKernelStartThread(thid, 0, NULL);
#endif
#endif
}

void perf_wait_summary(char *out, int size, uint32_t frames) {
    if (!frames)
        frames = 1;
    int len = sceClibSnprintf(out, size, "render thread blocked per frame:");
    for (int k = 0; k < PERF_WAIT_KINDS; k++) {
        uint64_t us = __atomic_exchange_n(&wait_us[k], 0, __ATOMIC_RELAXED);
        len += sceClibSnprintf(out + len, size - len, " %s %.1f ms;", wait_names[k], us / 1000.0f / frames);
    }
}

// Sleep and yield as the game calls them, timed (render thread waits)
int perf_nanosleep(const struct timespec *req, struct timespec *rem) {
    uint64_t t0 = NOW();
    int r = nanosleep(req, rem);
    perf_wait_add(PERF_WAIT_SLEEP, (uint32_t)(NOW() - t0));
    return r;
}

int perf_usleep(useconds_t us) {
    uint64_t t0 = NOW();
    int r = usleep(us);
    perf_wait_add(PERF_WAIT_SLEEP, (uint32_t)(NOW() - t0));
    return r;
}

int perf_sched_yield(void) {
    uint64_t t0 = NOW();
    int r = sched_yield();
    perf_wait_add(PERF_WAIT_SLEEP, (uint32_t)(NOW() - t0));
    return r;
}
