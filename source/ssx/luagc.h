/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SSX_LUAGC_H
#define SSX_LUAGC_H

// Removes the full Lua collection before lua_close in the UI (from so_patch)
void ssx_luagc_patch(void);

#endif // SSX_LUAGC_H
