/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  settings.c
 * @brief The port's own settings (FPS counter, CPU use, MSAA, frame rate in
 *        races, 4th core), kept on the memory card.
 *
 * Changed from the in-game settings menu (ssx/overlay.c) and saved at once
 * to SETTINGS_PATH, one "key=value" per line. MSAA is set when vitaGL starts
 * (utils/glutil.c), so a new MSAA setting applies at the next launch, as does
 * the 4th core (threads are set up as the game starts them); the
 * frame rate in races applies at once (ssx/fpscap.c).
 */

#include "ssx/settings.h"

#include <stdio.h>
#include <string.h>

#include "utils/logger.h"

#define SETTINGS_PATH "ux0:data/ssx/port_settings.txt"

ssx_settings_t ssx_settings = { .fps_counter = 0, .cpu_usage = 0, .msaa = 0, .race_fps = 60, .core3 = 1 };
int ssx_msaa_active;
int ssx_core3_enabled;

static int valid_msaa(int v) {
    return v == 0 || v == 2; // 4x left out: some textures loaded wrong (multicolored)
}

void ssx_settings_load(void) {
    FILE *f = fopen(SETTINGS_PATH, "r");
    if (f) {
        char line[64];
        int v;
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "fps_counter=%d", &v) == 1)
                ssx_settings.fps_counter = v != 0;
            else if (sscanf(line, "cpu_usage=%d", &v) == 1)
                ssx_settings.cpu_usage = v != 0;
            else if (sscanf(line, "msaa=%d", &v) == 1 && valid_msaa(v))
                ssx_settings.msaa = v;
            else if (sscanf(line, "race_fps=%d", &v) == 1 && (v == 30 || v == 60))
                ssx_settings.race_fps = v;
            else if (sscanf(line, "core3=%d", &v) == 1)
                ssx_settings.core3 = v != 0;
        }
        fclose(f);
    }
    ssx_msaa_active = ssx_settings.msaa;
    ssx_core3_enabled = ssx_settings.core3;
    l_info("[SETTINGS] FPS counter %s, MSAA %s, %d FPS in races, 4th core %s",
           ssx_settings.fps_counter ? "on" : "off", ssx_settings.msaa ? "2x" : "off", ssx_settings.race_fps,
           ssx_settings.core3 ? "on" : "off");
}

void ssx_settings_save(void) {
    FILE *f = fopen(SETTINGS_PATH, "w");
    if (!f) {
        l_error("[SETTINGS] could not write %s", SETTINGS_PATH);
        return;
    }
    fprintf(f, "fps_counter=%d\ncpu_usage=%d\nmsaa=%d\nrace_fps=%d\ncore3=%d\n", ssx_settings.fps_counter,
            ssx_settings.cpu_usage, ssx_settings.msaa, ssx_settings.race_fps, ssx_settings.core3);
    fclose(f);
}
