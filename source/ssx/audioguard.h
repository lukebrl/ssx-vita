/*
 * SSX port: audio stream guard
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  audioguard.h
 * @brief Survives a music stream that can't be created.
 */

#ifndef SSX_AUDIOGUARD_H
#define SSX_AUDIOGUARD_H

#include <stdint.h>

// Patches Audio::PathStream::Load (from so_patch)
void ssx_audioguard_patch(void);
// Times Load found no stream (to log)
extern volatile uint32_t ssx_audioguard_hits;

#endif // SSX_AUDIOGUARD_H
