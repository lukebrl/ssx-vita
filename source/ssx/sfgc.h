/*
 * SSX port: Scaleform ActionScript garbage collection probe
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SSX_SFGC_H
#define SSX_SFGC_H

// Raises the AS2 heap limit; in debug builds also times collections (from so_patch)
void ssx_sfgc_patch(void);
// Collections since the last call (watchdog)
void ssx_sfgc_report(void);

#endif // SSX_SFGC_H
