/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SSX_MOVIELIFE_H
#define SSX_MOVIELIFE_H

// Debug: logs Scaleform movies made and destroyed (from so_patch)
void ssx_movielife_patch(void);
// Debug: alive counts, when they changed (watchdog)
void ssx_movielife_report(void);

#endif // SSX_MOVIELIFE_H
