/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  perf.h
 * @brief Performance profiler: frame timing, CPU use per thread, texture
 *        memory and file I/O.
 */

#ifndef SSX_PERF_H
#define SSX_PERF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A/B test (debug builds): 1 while the optimizations under test are off
// (switched every 10 s, see perf.c); always 0 in release builds
#ifdef DEBUG_SOLOADER
extern volatile int ssx_ab_off;
#else
#define ssx_ab_off 0
#endif

// Once per presented frame, with the time vglSwapBuffers took
void perf_frame(uint32_t swap_us);
// Frame timing since the last call, as text (empty if no frame); returns the
// number of frames
uint32_t perf_frame_summary(char *out, int size);
// Time per frame in the timed phases since the last call
void perf_phase_summary(char *out, int size, uint32_t frames);
// Hooks the timed phases (from so_patch)
void perf_patch(void);
// Time spent in one GL draw call
void perf_gl_draw_time(uint32_t us);

// From the new thread itself; handle is its pthread_t (0 if none)
void perf_thread_register(uintptr_t handle, const char *name);
void perf_thread_set_name(uintptr_t handle, const char *name);

void perf_io_open(uint32_t us);
// Hitch report inputs (see hitch_check in perf.c)
void perf_gl_upload(uint32_t bytes, uint32_t us);
void perf_gl_link(uint32_t us);
void perf_note_ui(const char *view);
// Logs game logic frames over 150 ms (from so_patch, debug builds)
void perf_sim_patch(void);
// When the current logic frame started (sceKernelGetProcessTimeWide), 0 outside one
uint64_t perf_sim_frame_start(void);
void perf_io_read(uint32_t bytes, uint32_t us);
void perf_io_seek(uint32_t us);

// Logs CPU per thread, texture memory and I/O since the last report
void perf_report(void);

// Time a thread spent blocked; only the render thread's is counted
enum { PERF_WAIT_MUTEX, PERF_WAIT_COND, PERF_WAIT_SEM, PERF_WAIT_SLEEP, PERF_WAIT_KINDS };
void perf_wait_add(int kind, uint32_t us);
// Microseconds the simulation thread has waited so far, by kind
void perf_sim_waits(uint64_t out[PERF_WAIT_KINDS]);

// How many 32-bit words can be read upward from sp without leaving the
// calling thread's stack, at most max (for the debug stack scans)
int perf_stack_words(const void *sp, int max);

// Debug: logs where the simulation thread waited on semaphores since the
// last call ([SIMWAIT]), and starts over
void perf_simwait_report(void);
// The render thread's blocked time per frame since the last call (call it
// from the render thread)
void perf_wait_summary(char *out, int size, uint32_t frames);

#include <time.h>
#include <sys/types.h>
// nanosleep, usleep and sched_yield for the game, timed
int perf_nanosleep(const struct timespec *req, struct timespec *rem);
int perf_usleep(useconds_t us);
int perf_sched_yield(void);

#ifdef __cplusplus
}
#endif

// Logic frame counts since the last call (watchdog, debug builds)
void perf_sim_report(void);

#endif // SSX_PERF_H
