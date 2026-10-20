/*
 * SSX port: Scaleform mesh cache uploads
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  sfmesh.h
 * @brief Uploads only the changed parts of Scaleform's mesh cache buffers.
 */

#ifndef SSX_SFMESH_H
#define SSX_SFMESH_H

#include <vitaGL.h>

// Hooks MeshCache::allocBuffer and MeshBuffer::Unmap (from so_patch)
void ssx_sfmesh_patch(void);
// The game's glBufferSubData
void ssx_sfmesh_buffer_sub_data(GLenum target, GLintptr offset, GLsizeiptr size, const void *data);

#endif // SSX_SFMESH_H
