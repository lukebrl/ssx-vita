/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SSX_DISPLAYLIST_H
#define SSX_DISPLAYLIST_H

// Faster DisplayList::GetDisplayObjectByName (from so_patch)
void ssx_displaylist_patch(void);
// Debug: lookup counts since the last call (watchdog)
void ssx_displaylist_report(void);

#endif // SSX_DISPLAYLIST_H
