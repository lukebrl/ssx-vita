/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SSX_TEXMEM_H
#define SSX_TEXMEM_H

#ifdef __cplusplus
extern "C" {
#endif

// Frees the RAM copy of each texture once it is on the GPU (see texmem.c)
void ssx_texmem_patch(void);
// Logs how much texture memory that saved
void ssx_texmem_report(void);

#ifdef __cplusplus
};
#endif

#endif // SSX_TEXMEM_H
