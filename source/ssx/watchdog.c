/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  watchdog.c
 * @brief Debug aid for hangs: if the game stops completing frames, crash on
 *        purpose so the Vita writes a core dump showing where every thread is.
 */

#include "ssx/watchdog.h"

#include <stdatomic.h>

#include <psp2/ctrl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "utils/glutil.h"
#include "reimpl/io.h"
#include "utils/logger.h"
#include "ssx/perf.h"
#include "ssx/glprof.h"
#include "ssx/scopecache.h"
#include "ssx/vaocache.h"
#include "ssx/jobprof.h"
#include "ssx/audioguard.h"
#include "ssx/sfgc.h"
#include "ssx/terrainmerge.h"
#include "ssx/texload.h"
#include "ssx/allocprof.h"
#include "ssx/gfxcache.h"
#include "ssx/displaylist.h"
#include "ssx/movielife.h"
#include "ssx/hwskin.h"

// The first frame includes loading and, on first boot, compiling every shader
#define FIRST_FRAME_TIMEOUT_S (10 * 60)
#define HANG_TIMEOUT_S 30

static atomic_uint frames_done = 0;
static atomic_uint swaps_done = 0;

void watchdog_frame_done(void) {
    atomic_fetch_add(&frames_done, 1);
}

void watchdog_swap_done(void) {
    unsigned n = atomic_fetch_add(&swaps_done, 1) + 1;
    if (n <= 5)
        l_info("eglSwapBuffers #%u", n);

    // FPS over the last 5 seconds (only called from the render thread)
    static SceUInt64 window_start = 0;
    static unsigned window_frames = 0;
    SceUInt64 now = sceKernelGetProcessTimeWide();
    if (!window_start)
        window_start = now;
    window_frames++;
    if (now - window_start >= 5000000) {
        extern uint32_t vgl_circular_pool_ssx_peak;
        l_info("[FPS] %.1f, per frame: %u draws, %u clears, %u texture updates, %.2f MB scratch peak; vitaGL free: vram %.1f MB, ram %.1f MB, phycont %.1f MB",
               window_frames * 1000000.0f / (float)(now - window_start),
               gl_draw_calls / window_frames, gl_clear_calls / window_frames,
               gl_texsub_calls / window_frames, vgl_circular_pool_ssx_peak / 1048576.0f,
               vglMemFree(VGL_MEM_VRAM) / 1048576.0f, vglMemFree(VGL_MEM_RAM) / 1048576.0f,
               vglMemFree(VGL_MEM_PHYCONT) / 1048576.0f);
        vgl_circular_pool_ssx_peak = 0;
        extern uint32_t vgl_ssx_buffer_copies, vgl_ssx_buffer_inplace;
        if (vgl_ssx_buffer_copies || vgl_ssx_buffer_inplace)
            l_info("[PERF] buffer updates while in use: %u copied, %u written in place (memory full)",
                   vgl_ssx_buffer_copies, vgl_ssx_buffer_inplace);
        vgl_ssx_buffer_copies = vgl_ssx_buffer_inplace = 0;
        extern uint32_t vgl_ssx_map_copies, vgl_ssx_map_waits, vgl_ssx_map_bytes;
        if (vgl_ssx_map_copies || vgl_ssx_map_waits)
            l_info("[PERF] buffer maps while in use: %u copied (%.1f MB), %u waited for the GPU",
                   vgl_ssx_map_copies, vgl_ssx_map_bytes / 1048576.0f, vgl_ssx_map_waits);
        vgl_ssx_map_copies = vgl_ssx_map_waits = vgl_ssx_map_bytes = 0;
        if (vgl_ssx_ring_updates)
            l_info("[PERF] big buffer updates: %u, %.2f MB changed, %.2f MB copied, %u waited for the GPU, %u missed changes; "
                   "%.1f MB of extra storages",
                   vgl_ssx_ring_updates, vgl_ssx_ring_compared / 1048576.0f, vgl_ssx_ring_bytes / 1048576.0f,
                   vgl_ssx_ring_waits, vgl_ssx_ring_mismatches, vgl_ssx_ring_extra_bytes / 1048576.0f);
        vgl_ssx_ring_updates = vgl_ssx_ring_bytes = vgl_ssx_ring_compared = vgl_ssx_ring_waits = 0;
        vgl_ssx_ring_mismatches = 0;
        static uint32_t oom_seen;
        if (vgl_ssx_oom_count != oom_seen) {
            oom_seen = vgl_ssx_oom_count;
            l_error("[vitaGL] out of GPU memory: %u buffers left empty so far (last %u KB)", oom_seen,
                    vgl_ssx_oom_bytes / 1024);
        }
        char frame[2048];
        uint32_t frames = perf_frame_summary(frame, sizeof(frame));
        if (frame[0])
            l_info("[PERF] %s", frame);
        perf_phase_summary(frame, sizeof(frame), frames);
        if (frames)
            l_info("[PERF] %s", frame);
        perf_wait_summary(frame, sizeof(frame), frames);
        if (frames)
            l_info("[PERF] %s", frame);
        ssx_scopecache_report(frames);
        perf_sim_report();
        ssx_jobprof_report(frames);
        ssx_hwskin_report(frames);
        ssx_terrainmerge_report(frames);
        ssx_texload_report();
        ssx_allocprof_report(frames);
        ssx_gfxcache_report();
        static uint32_t audio_misses;
        if (ssx_audioguard_hits != audio_misses) {
            audio_misses = ssx_audioguard_hits;
            l_warn("[AUDIO] a music stream could not be created (%u times so far), retried later", audio_misses);
        }
        extern void audio_report(void);
        audio_report();
        static uint32_t pack_seen;
        if (vgl_ssx_pack_hits + vgl_ssx_pack_misses != pack_seen) {
            pack_seen = vgl_ssx_pack_hits + vgl_ssx_pack_misses;
            l_info("[GL] shader cache pack: %u found, %u not (so far)", vgl_ssx_pack_hits, vgl_ssx_pack_misses);
        }
        static uint32_t compiled_seen;
        if (vgl_ssx_compiled != compiled_seen) {
            compiled_seen = vgl_ssx_compiled;
            l_info("[GL] shaders compiled so far: %u in %.1f s (%.1f s of it writing the cache: file %.1f s, "
                   "replacing it %.1f s, pack %.1f s); %u damaged cache entries rejected",
                   vgl_ssx_compiled, vgl_ssx_compile_us / 1e6f, vgl_ssx_cache_write_us / 1e6f,
                   vgl_ssx_cache_file_us / 1e6f, vgl_ssx_cache_rename_us / 1e6f, vgl_ssx_cache_pack_us / 1e6f,
                   vgl_ssx_cache_rejected);
        }
        ssx_sfgc_report();
        ssx_displaylist_report();
        ssx_movielife_report();
        glutil_subdata_report(frames);
        glutil_deferred_report();
        glutil_mem_report();
#ifdef DEBUG_SOLOADER
        glutil_swap_parts_report();
        ssx_vaocache_report(frames);
#endif
#if SSX_GLPROF
        glprof_summary(frame, sizeof(frame), frames);
        if (frames)
            l_info("[PERF] %s", frame);
#endif
#if SSX_FNPROF
        fnprof_summary(frame, sizeof(frame), frames);
        if (frames)
            l_info("[PERF] %s", frame);
#endif
        gl_draw_calls = 0;
        gl_clear_calls = 0;
        gl_texsub_calls = 0;
        fd_stats_report_busy();
        window_start = now;
        window_frames = 0;
    }
}

// Crashes without going through the logger: in a hang, the thread holding the
// log lock may be the stuck one, and logging first would block us forever.
// The core dump shows this thread crashing in here.
static void crash_for_dump(const char *why) {
    sceClibPrintf("[WATCHDOG] %s; crashing on purpose to get a core dump\n", why);
    *(volatile int *)0 = 0;
}

static int watchdog_thread(SceSize args, void *argp) {
    unsigned last = atomic_load(&frames_done);
    int idle_s = 0;
    int combo_s = 0;
    while (1) {
        sceKernelDelayThread(1000 * 1000);

        // Manual dump: hold L + R + Select for 2 s (for hangs where frames
        // still get drawn, e.g. waiting on another thread)
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        const unsigned combo = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_SELECT;
        combo_s = ((pad.buttons & combo) == combo) ? combo_s + 1 : 0;
        if (combo_s >= 2)
            crash_for_dump("L+R+Select held");

        unsigned now = atomic_load(&frames_done);
        idle_s = (now == last) ? idle_s + 1 : 0;
        int timeout = now ? HANG_TIMEOUT_S : FIRST_FRAME_TIMEOUT_S;
        if (idle_s >= timeout)
            crash_for_dump("no frame finished in time");
        last = now;
    }
    return 0;
}

void watchdog_start(void) {
    // Highest user priority: it mostly sleeps, and has to run even when game
    // threads spin on every core
    SceUID thid = sceKernelCreateThread("ssx_watchdog", watchdog_thread, 64, 0x4000, 0, 0, NULL);
    if (thid >= 0)
        sceKernelStartThread(thid, 0, NULL);
}
