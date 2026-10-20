/*
 * SSX port: EA::Jobs profiling
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  jobprof.h
 * @brief How long the render thread waits on EA::Jobs jobs, and runs them.
 */

#ifndef SSX_JOBPROF_H
#define SSX_JOBPROF_H

#include <stdint.h>

// Hooks JobInstanceHandle::WaitOn and JobContextImpl::RunOneJob (from so_patch)
void ssx_jobprof_patch(void);
// Logs the times per frame since the last call
void ssx_jobprof_report(uint32_t frames);

#endif // SSX_JOBPROF_H
