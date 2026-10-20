/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SSX_SHADERBIND_H
#define SSX_SHADERBIND_H

// RNAX::OGLESProgram::Bind and SendParmsToGPU without their per-parameter
// string compare (from so_patch)
void ssx_shaderbind_patch(void);

#endif // SSX_SHADERBIND_H
