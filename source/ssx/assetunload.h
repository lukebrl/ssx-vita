/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SSX_ASSETUNLOAD_H
#define SSX_ASSETUNLOAD_H

// Asset classes unloaded under one GPU lock (from so_patch)
void ssx_assetunload_patch(void);

#endif // SSX_ASSETUNLOAD_H
