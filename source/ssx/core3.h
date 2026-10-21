/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  core3.h
 * @brief The fourth CPU core for the game's background threads, when the
 *        CapUnlocker plugin makes it available.
 */

#ifndef SSX_CORE3_H
#define SSX_CORE3_H

// From the thread itself, once the game has named it (prctl)
void ssx_core3_thread_named(const char *name);

// CORE3_UNKNOWN until a thread has asked for it
enum { CORE3_UNKNOWN = -1, CORE3_UNAVAILABLE = 0, CORE3_USED = 1, CORE3_OFF = 2 };
int ssx_core3_state(void);

#endif // SSX_CORE3_H
