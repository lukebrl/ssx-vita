/*
 * SSX port: GPU skinning
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  hwskin.h
 * @brief Skins the characters on the GPU instead of the CPU.
 */

#ifndef SSX_HWSKIN_H
#define SSX_HWSKIN_H

// Makes CharacterRenderObj::Init build GpuSkinningFeatureImpl (from so_patch)
void ssx_hwskin_patch(void);
// Logs GPU skinning counts per frame since the last call
#include <stdint.h>
void ssx_hwskin_report(uint32_t frames);

#endif // SSX_HWSKIN_H
