/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  glprof.c
 * @brief Guards and times every GL function the game calls.
 *
 * Before the game's imports are resolved, each gl* entry of the import table
 * is replaced by a small stub, which calls the real function with the
 * arguments untouched (registers restored, stack arguments in place).
 *
 * - Calls from threads other than the render thread are skipped (they
 *   return 0). The game has a single GL context, current on the render
 *   thread; on Android, GL calls from other threads (no current context) do
 *   nothing. On the Vita they reached vitaGL, which isn't thread-safe:
 *   GlobeRenderObj::RenderSetup's glTexSubImage2D from a worker thread
 *   crashed in SceGxm in the menu.
 * - Time spent in each function is added up; with SSX_GLPROF the [PERF] line
 *   lists the functions taking the most time per frame.
 */

#include "ssx/glprof.h"

#include <stdint.h>
#include <string.h>

#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

// Slots 0-255: GL imports (guarded); 256-511: game functions (fnprof)
#define GL_SLOTS 256
#define MAX_FUNCS 512

uintptr_t glprof_targets[MAX_FUNCS];
static const char *names[MAX_FUNCS];
static volatile uint64_t total_us[MAX_FUNCS]; // inclusive
static volatile uint64_t self_us[MAX_FUNCS];  // minus the timed functions it calls
static volatile uint32_t calls[MAX_FUNCS];
// The same, never reset, for fnprof_frame_* (per logic frame breakdowns)
static volatile uint64_t run_total_us[MAX_FUNCS], run_self_us[MAX_FUNCS];
static volatile uint32_t run_calls[MAX_FUNCS];
// Timed calls each function made (direct callees), for the hook overhead
// they put into its own time
static volatile uint32_t run_child_calls[MAX_FUNCS];
static float hook_overhead_us; // one enter + leave, measured at start
static volatile uint32_t skipped[MAX_FUNCS];
static uint8_t draw_funcs[MAX_FUNCS]; // glDrawArrays, glDrawElements, glClear
// glDelete*: let through from any thread, glutil.c defers them to the GL thread
static uint8_t deferred_funcs[MAX_FUNCS];
static int ngl, nfn;

// Per thread: return addresses and start times of the calls in progress
#define MAX_THREADS 32
#define MAX_DEPTH 512 // ActionScript and display-list code recurse deeply
static struct {
    uintptr_t tls;
    int depth;
    uintptr_t lr[MAX_DEPTH];
    uint32_t t0[MAX_DEPTH];
    uint32_t child[MAX_DEPTH]; // time in timed callees
    int idx[MAX_DEPTH];
} frames[MAX_THREADS];

static uintptr_t render_thread; // where the context is current (eglMakeCurrent)
static uintptr_t frame_thread;  // the logic thread, once fnprof_frame_start ran

static inline uintptr_t thread_key(void) {
    uintptr_t v;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(v));
    return v;
}

void glprof_set_render_thread(void) {
    __atomic_store_n(&render_thread, thread_key(), __ATOMIC_RELAXED);
}

int glprof_on_render_thread(void) {
    uintptr_t rt = __atomic_load_n(&render_thread, __ATOMIC_RELAXED);
    return !rt || thread_key() == rt;
}

uintptr_t glprof_render_thread(void) {
    return __atomic_load_n(&render_thread, __ATOMIC_RELAXED);
}

static int frame_slot(void) {
    uintptr_t key = thread_key();
    for (int i = 0; i < MAX_THREADS; i++)
        if (frames[i].tls == key)
            return i;
    for (int i = 0; i < MAX_THREADS; i++) {
        uintptr_t expected = 0;
        if (__atomic_compare_exchange_n(&frames[i].tls, &expected, key, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return i;
    }
    return MAX_THREADS - 1; // more threads than slots: cannot happen in practice
}

// Light stubs (GL timing off): whether GL function idx may run on this thread
int glprof_allow(int idx, uintptr_t lr) {
    uintptr_t rt = __atomic_load_n(&render_thread, __ATOMIC_RELAXED);
    if (!rt || thread_key() == rt || deferred_funcs[idx])
        return 1;
    if (draw_funcs[idx]) {
        l_warn("[GL] %s from another thread (from game+0x%X): it becomes the GL thread",
               names[idx], (unsigned)(lr - so_mod.text_base));
        glprof_set_render_thread();
        return 1;
    }
    if (__atomic_fetch_add(&skipped[idx], 1, __ATOMIC_RELAXED) == 0)
        l_warn("[GL] %s called from another thread (from game+0x%X): skipped, as on Android",
               names[idx], (unsigned)(lr - so_mod.text_base));
    return 0;
}

// Called by the stubs: function idx is starting, lr is where it returns.
// Returns 0 to skip the call.
int glprof_enter(int idx, uintptr_t lr) {
    int s = frame_slot();
    int d = frames[s].depth < MAX_DEPTH ? frames[s].depth++ : MAX_DEPTH - 1;
    frames[s].lr[d] = lr;
    frames[s].idx[d] = idx;
    frames[s].child[d] = 0;
    frames[s].t0[d] = sceKernelGetProcessTimeLow();
    if (idx >= GL_SLOTS)
        return 1; // a game function: always called
    uintptr_t rt = __atomic_load_n(&render_thread, __ATOMIC_RELAXED);
    if (!rt || thread_key() == rt || deferred_funcs[idx])
        return 1;
    // A thread that draws or clears is the renderer whatever eglMakeCurrent
    // said (a game would not draw where it has no context)
    if (draw_funcs[idx]) {
        l_warn("[GL] %s from another thread (from game+0x%X): it becomes the GL thread",
               names[idx], (unsigned)(lr - so_mod.text_base));
        glprof_set_render_thread();
        return 1;
    }
    if (__atomic_fetch_add(&skipped[idx], 1, __ATOMIC_RELAXED) == 0)
        l_warn("[GL] %s called from another thread (from game+0x%X): skipped, as on Android",
               names[idx], (unsigned)(lr - so_mod.text_base));
    return 0;
}

// Called by the stubs after the function returned (or was skipped): the
// caller's return address
uintptr_t glprof_leave(void) {
    int s = frame_slot();
    int d = frames[s].depth > 0 ? --frames[s].depth : 0;
    int idx = frames[s].idx[d];
    uint32_t elapsed = sceKernelGetProcessTimeLow() - frames[s].t0[d];
    total_us[idx] += elapsed;
    self_us[idx] += elapsed - frames[s].child[d];
    // Per logic frame reports: the logic thread's calls only (Scaleform loads
    // movies on its own threads meanwhile)
    if (!frame_thread || frames[s].tls == frame_thread) {
        run_total_us[idx] += elapsed;
        run_self_us[idx] += elapsed - frames[s].child[d];
        run_calls[idx]++;
        if (d > 0)
            run_child_calls[frames[s].idx[d - 1]]++;
    }
    if (d > 0)
        frames[s].child[d - 1] += elapsed;
    calls[idx]++;
    return frames[s].lr[d];
}

// One stub per slot; slot base + a*64 + b*16 + c*4 + d is glprof_stub_base_abcd
#define FULLSTUB(base, a, b, c, d)                                                     \
    __attribute__((naked, used)) static void glprof_stub_##base##_##a##b##c##d(void) { \
        __asm__ volatile(                                                         \
            "push {r0-r3}\n"                                                      \
            "movw r0, #(" #base "+" #a "*64+" #b "*16+" #c "*4+" #d ")\n"         \
            "mov r1, lr\n"                                                        \
            "bl glprof_enter\n"                                                   \
            "mov r12, r0\n"                                                       \
            "pop {r0-r3}\n"                                                       \
            "cmp r12, #0\n"                                                       \
            "beq 1f\n"                                                            \
            "ldr r12, =glprof_targets+4*(" #base "+" #a "*64+" #b "*16+" #c "*4+" #d ")\n" \
            "ldr r12, [r12]\n"                                                    \
            "blx r12\n"                                                           \
            "2: push {r0, r1}\n"                                                  \
            "bl glprof_leave\n"                                                   \
            "mov r12, r0\n"                                                       \
            "pop {r0, r1}\n"                                                      \
            "bx r12\n"                                                            \
            "1: movs r0, #0\n" /* skipped: returns 0 */                           \
            "movs r1, #0\n"                                                       \
            "b 2b\n"                                                              \
            ".ltorg\n");                                                          \
    }
// Light stub, used for GL when GL timing is off: checks the thread, then
// jumps to the function, which returns straight to the caller
#define LSTUB(base, a, b, c, d)                                                 \
    __attribute__((naked, used)) static void glprof_stub_##base##_##a##b##c##d(void) {\
        __asm__ volatile(                                                       \
            "push {r0-r3, r12, lr}\n"                                           \
            "movw r0, #(" #base "+" #a "*64+" #b "*16+" #c "*4+" #d ")\n"       \
            "mov r1, lr\n"                                                      \
            "bl glprof_allow\n"                                                 \
            "mov r12, r0\n"                                                     \
            "cmp r12, #0\n"                                                     \
            "pop {r0-r3, r12, lr}\n"                                            \
            "beq 1f\n"                                                          \
            "ldr r12, =glprof_targets+4*(" #base "+" #a "*64+" #b "*16+" #c "*4+" #d ")\n"\
            "ldr r12, [r12]\n"                                                  \
            "bx r12\n"                                                          \
            "1: movs r0, #0\n" /* skipped: returns 0 */                         \
            "movs r1, #0\n"                                                     \
            "bx lr\n"                                                           \
            ".ltorg\n");                                                        \
    }

#define STUB FULLSTUB
#define STUB4(x, a, b, c) STUB(x, a, b, c, 0) STUB(x, a, b, c, 1) STUB(x, a, b, c, 2) STUB(x, a, b, c, 3)
#define STUB16(x, a, b) STUB4(x, a, b, 0) STUB4(x, a, b, 1) STUB4(x, a, b, 2) STUB4(x, a, b, 3)
#define STUB64(x, a) STUB16(x, a, 0) STUB16(x, a, 1) STUB16(x, a, 2) STUB16(x, a, 3)
#if !SSX_GLPROF
#undef STUB
#define STUB LSTUB
#endif
STUB64(0, 0) STUB64(0, 1) STUB64(0, 2) STUB64(0, 3)
#undef STUB
#define STUB FULLSTUB
STUB64(256, 0) STUB64(256, 1) STUB64(256, 2) STUB64(256, 3)

#define S(x, a, b, c, d) (uintptr_t)glprof_stub_##x##_##a##b##c##d
#define S4(x, a, b, c) S(x, a, b, c, 0), S(x, a, b, c, 1), S(x, a, b, c, 2), S(x, a, b, c, 3)
#define S16(x, a, b) S4(x, a, b, 0), S4(x, a, b, 1), S4(x, a, b, 2), S4(x, a, b, 3)
#define S64(x, a) S16(x, a, 0), S16(x, a, 1), S16(x, a, 2), S16(x, a, 3)
static const uintptr_t stubs[MAX_FUNCS] = {
    S64(0, 0), S64(0, 1), S64(0, 2), S64(0, 3),
    S64(256, 0), S64(256, 1), S64(256, 2), S64(256, 3),
};

void glprof_wrap(so_default_dynlib *table, size_t count) {
    for (size_t i = 0; i < count && ngl < GL_SLOTS; i++) {
        const char *name = table[i].symbol;
        if (!name || name[0] != 'g' || name[1] != 'l' || !(name[2] >= 'A' && name[2] <= 'Z'))
            continue;
        glprof_targets[ngl] = table[i].func;
        draw_funcs[ngl] = !strcmp(name, "glDrawArrays") || !strcmp(name, "glDrawElements") || !strcmp(name, "glClear");
        deferred_funcs[ngl] = !strcmp(name, "glDeleteTextures") || !strcmp(name, "glDeleteBuffers") ||
                              !strcmp(name, "glDeleteFramebuffers") || !strcmp(name, "glDeleteRenderbuffers");
        names[ngl] = name;
        table[i].func = stubs[ngl];
        ngl++;
    }
    l_info("[GL] %d GL functions guarded (render thread only) and timed", ngl);
}

// --- fnprof: game functions -------------------------------------------------

static const struct {
    uint32_t addr;
    const char *name;
} fn_list[] = {
#include "ssx/fnprof_list.h"
};

void fnprof_patch(void) {
    int refused = 0;
    for (unsigned i = 0; i < sizeof(fn_list) / sizeof(fn_list[0]) && nfn < MAX_FUNCS - GL_SLOTS; i++) {
        int slot = GL_SLOTS + nfn;
        uintptr_t tramp = hook_arm_trampoline(&so_mod, so_mod.text_base + fn_list[i].addr, stubs[slot]);
        if (!tramp) {
            refused++; // starts with PC-relative code
            continue;
        }
        glprof_targets[slot] = tramp;
        names[slot] = fn_list[i].name;
        nfn++;
    }
    // What a hooked call costs its caller (enter + leave), to take it off the
    // callers' own times: hot functions are called tens of thousands of times
    // per screen load. Measured on a spare slot, then cleared.
    int spare = GL_SLOTS + nfn;
    if (spare < MAX_FUNCS) {
        const int n = 20000;
        uint32_t t0 = sceKernelGetProcessTimeLow();
        for (int k = 0; k < n; k++) {
            glprof_enter(spare, 0);
            glprof_leave();
        }
        hook_overhead_us = (float)(sceKernelGetProcessTimeLow() - t0) / n;
        total_us[spare] = self_us[spare] = 0;
        calls[spare] = 0;
        run_total_us[spare] = run_self_us[spare] = 0;
        run_calls[spare] = run_child_calls[spare] = 0;
    }
    l_info("[FNPROF] timing %d game functions (%d could not be hooked); a hooked call costs ~%.2f us", nfn, refused,
           hook_overhead_us);
}

// The n slots in [first, last) with the most time in times[]
static int top_slots(volatile uint64_t *times, int first, int last, int *top, int n) {
    int ntop = 0;
    for (int k = 0; k < n; k++) {
        int best = -1;
        for (int i = first; i < last; i++) {
            int taken = 0;
            for (int j = 0; j < ntop; j++)
                taken |= top[j] == i;
            if (!taken && times[i] && (best < 0 || times[i] > times[best]))
                best = i;
        }
        if (best < 0)
            break;
        top[ntop++] = best;
    }
    return ntop;
}

static void reset(int first, int last) {
    for (int i = first; i < last; i++) {
        total_us[i] = 0;
        self_us[i] = 0;
        calls[i] = 0;
    }
}

void glprof_summary(char *out, int size, uint32_t frames) {
    int top[10];
    int ntop = top_slots(total_us, 0, ngl, top, 10);
    uint64_t all = 0;
    for (int i = 0; i < ngl; i++)
        all += total_us[i];
    if (!frames)
        frames = 1;
    int len = sceClibSnprintf(out, size, "GL %.1f ms per frame:", all / 1000.0f / frames);
    for (int k = 0; k < ntop && len < size - 64; k++) {
        int i = top[k];
        len += sceClibSnprintf(out + len, size - len, " %s %.2f ms (%u calls);", names[i],
                               total_us[i] / 1000.0f / frames, (unsigned)(calls[i] / frames));
    }
    reset(0, ngl);
}

// Per logic frame: fnprof_frame_start copies the running totals, and
// fnprof_frame_report logs the functions that took the most own time since
static uint64_t snap_total[MAX_FUNCS], snap_self[MAX_FUNCS];
static uint32_t snap_calls[MAX_FUNCS], snap_child_calls[MAX_FUNCS];

void fnprof_frame_start(void) {
    frame_thread = thread_key(); // called on the logic thread
    for (int i = GL_SLOTS; i < GL_SLOTS + nfn; i++) {
        snap_total[i] = run_total_us[i];
        snap_self[i] = run_self_us[i];
        snap_calls[i] = run_calls[i];
        snap_child_calls[i] = run_child_calls[i];
    }
}

void fnprof_frame_report(const char *what) {
    static uint64_t d_self[MAX_FUNCS];
    uint64_t timed = 0, overhead = 0;
    for (int i = GL_SLOTS; i < GL_SLOTS + nfn; i++) {
        // Own time without the overhead of the hooked calls it made
        uint64_t own = run_self_us[i] - snap_self[i];
        uint64_t ovh = (uint64_t)((run_child_calls[i] - snap_child_calls[i]) * hook_overhead_us);
        d_self[i] = own > ovh ? own - ovh : 0;
        overhead += ovh < own ? ovh : own;
        timed += d_self[i];
    }
    int top[30];
    int ntop = top_slots(d_self, GL_SLOTS, GL_SLOTS + nfn, top, 30);
    char out[2048];
    int len = sceClibSnprintf(out, sizeof(out), "[FRAMEPROF] %s; %.0f ms in timed functions (plus ~%.0f ms of hook overhead, taken off), "
                              "by own time (ms own / with callees, calls):",
                              what, timed / 1000.0f, overhead / 1000.0f);
    for (int k = 0; k < ntop; k++) {
        int i = top[k];
        // Log lines hold 2048 characters: the rest on a line of its own
        if (len > 1700) {
            l_info("%s", out);
            len = sceClibSnprintf(out, sizeof(out), "[FRAMEPROF] ... continued:");
        }
        len += sceClibSnprintf(out + len, sizeof(out) - len, " %s %.1f/%.1f (%u);", names[i], d_self[i] / 1000.0f,
                               (run_total_us[i] - snap_total[i]) / 1000.0f, (unsigned)(run_calls[i] - snap_calls[i]));
    }
    l_info("%s", out);
}

void fnprof_summary(char *out, int size, uint32_t frames) {
    int top[24];
    int ntop = top_slots(self_us, GL_SLOTS, GL_SLOTS + nfn, top, 24);
    if (!frames)
        frames = 1;
    int len = sceClibSnprintf(out, size, "game functions by own time per frame (ms own / with callees, calls):");
    for (int k = 0; k < ntop && len < size - 120; k++) {
        int i = top[k];
        len += sceClibSnprintf(out + len, size - len, " %s %.2f/%.2f (%u);", names[i],
                               self_us[i] / 1000.0f / frames, total_us[i] / 1000.0f / frames,
                               (unsigned)(calls[i] / frames));
    }
    reset(GL_SLOTS, GL_SLOTS + nfn);
}
