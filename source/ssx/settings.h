/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SSX_SETTINGS_H
#define SSX_SETTINGS_H

typedef struct {
    int fps_counter; // 0 or 1
    int cpu_usage;   // 0 or 1: each core's use under the FPS counter
    int msaa;        // 0 or 2 (samples), from the next launch (4x broke some textures)
    int race_fps;    // frame rate in races: 60 or 30 (30 leaves the CPU to the simulation)
    int core3;       // 0 or 1: background threads on the 4th core too (CapUnlocker), next launch
} ssx_settings_t;

extern ssx_settings_t ssx_settings;
// The MSAA vitaGL was started with (ssx_settings.msaa at boot)
extern int ssx_msaa_active;
// ssx_settings.core3 at boot (ssx/core3.c)
extern int ssx_core3_enabled;

// At boot, before vitaGL starts
void ssx_settings_load(void);
// After a change
void ssx_settings_save(void);

#endif // SSX_SETTINGS_H
