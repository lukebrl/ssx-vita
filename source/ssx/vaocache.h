/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SSX_VAOCACHE_H
#define SSX_VAOCACHE_H

#include <stdint.h>

// Mesh vertex setups kept in vertex array objects (from so_patch)
void ssx_vaocache_patch(void);
// How mesh binds went since the last call
void ssx_vaocache_report(uint32_t frames);

#endif // SSX_VAOCACHE_H
