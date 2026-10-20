/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  glprof.h
 * @brief Times every GL function the game calls.
 */

#ifndef SSX_GLPROF_H
#define SSX_GLPROF_H

#include <stddef.h>
#include <stdint.h>
#include <so_util/so_util.h>

// Per-frame GL timing in the [PERF] lines, off by default: it costs about
// 1 us per GL call, several ms per frame (the guarding stubs are always on)
#ifndef SSX_GLPROF
#define SSX_GLPROF 0
#endif

// Replaces the gl* entries of the import table with timing stubs (before
// the imports are resolved)
void glprof_wrap(so_default_dynlib *table, size_t count);
// The calling thread becomes the one GL calls are allowed from (eglMakeCurrent)
void glprof_set_render_thread(void);
// That thread's TLS key (TPIDRURO), 0 before eglMakeCurrent
uintptr_t glprof_render_thread(void);
// Whether the calling thread is that thread (or none is set yet)
int glprof_on_render_thread(void);
// The GL functions taking the most time per frame since the last call
void glprof_summary(char *out, int size, uint32_t frames);

// Game function timing: a lot of overhead on small functions, so off unless
// looking for hot spots (build with SSX_FNPROF=1)
#ifndef SSX_FNPROF
#define SSX_FNPROF 0
#endif
// Hooks the game functions listed in fnprof_list.h (from so_patch)
void fnprof_patch(void);
// The game functions with the most own time per frame since the last call
void fnprof_summary(char *out, int size, uint32_t frames);
// Per logic frame breakdown (perf.c, debug builds with SSX_FNPROF)
void fnprof_frame_start(void);
void fnprof_frame_report(const char *what);

#endif // SSX_GLPROF_H
