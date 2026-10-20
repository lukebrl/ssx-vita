/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SSX_MEMREPORT_H
#define SSX_MEMREPORT_H

#ifdef __cplusplus
extern "C" {
#endif

// Free Vita memory (main RAM, CDRAM, phycont)
void ssx_log_free_memory(const char *when);
// Size, peak and current use of every EA MemoryFramework heap
void ssx_log_heaps(void);
// The IAllocator of the MemoryFramework heap with that name, or NULL
void *ssx_find_allocator(const char *name);
// Its largest free block, in bytes
unsigned ssx_allocator_largest_free(void *allocator);
// Logs the heaps every 10 seconds from a background thread
void ssx_memreport_start(void);

#ifdef __cplusplus
};
#endif

#endif // SSX_MEMREPORT_H
