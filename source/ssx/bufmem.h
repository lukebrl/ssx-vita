/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  bufmem.h
 * @brief Frees the RAM copy the engine keeps of every static vertex and
 *        index buffer.
 */

#ifndef SSX_BUFMEM_H
#define SSX_BUFMEM_H

void ssx_bufmem_patch(void);
void ssx_bufmem_report(void);

#endif // SSX_BUFMEM_H
