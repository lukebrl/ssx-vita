/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SSX_LIFECYCLE_H
#define SSX_LIFECYCLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Activity.onCreate(): starts EA's subsystems and the game application
void ssx_create(void);
// onResume(), surface creation and window focus; call once before frames
void ssx_resume(void);
// One iteration of com.ea.blast.MainThread.run()
void ssx_draw_frame(void);

// Input, as MainActivityGenerated forwards it from a "PLAYSTATION" pad
void ssx_key(int32_t android_keycode, int down);
void ssx_back(int down);
// Stick axes in [-1, 1], up is positive Y (Java already inverts the axis)
void ssx_sticks(float lx, float ly, float rx, float ry);

#ifdef __cplusplus
};
#endif

#endif // SSX_LIFECYCLE_H
