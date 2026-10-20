/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SSX_UNLOCKWAIT_H
#define SSX_UNLOCKWAIT_H

// The post-race campaign sequence without its waits (from so_patch)
void ssx_unlockwait_patch(void);

#endif // SSX_UNLOCKWAIT_H
