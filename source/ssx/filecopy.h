/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SSX_FILECOPY_H
#define SSX_FILECOPY_H

// Replaces EA::IO::File::Copy with a copy of the files as stored on disk (from so_patch)
void ssx_filecopy_patch(void);

#endif // SSX_FILECOPY_H
