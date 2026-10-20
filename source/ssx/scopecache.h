/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  scopecache.h
 * @brief Caches RNA::ScopeC::FindParm.
 */

#ifndef SSX_SCOPECACHE_H
#define SSX_SCOPECACHE_H

#include <stdint.h>

void ssx_scopecache_patch(void);
// A number that changes whenever any scope gains, loses or deletes a block or
// parameter; 0 when such changes can't be tracked (then nothing may be cached)
uint32_t ssx_scopecache_generation(void);
// Logs the hit rate since the last call
void ssx_scopecache_report(uint32_t frames);

#endif // SSX_SCOPECACHE_H
