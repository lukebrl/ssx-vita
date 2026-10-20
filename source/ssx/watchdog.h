/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SSX_WATCHDOG_H
#define SSX_WATCHDOG_H

#ifdef __cplusplus
extern "C" {
#endif

// Starts a thread that crashes the app if no frame completes for 30 s
// (10 min for the first frame, which includes loading)
void watchdog_start(void);
void watchdog_frame_done(void);
// Counts (and logs the first few) eglSwapBuffers calls
void watchdog_swap_done(void);

#ifdef __cplusplus
};
#endif

#endif // SSX_WATCHDOG_H
