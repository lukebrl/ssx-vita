/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  overlay.c
 * @brief FPS counter and the port's settings menu, drawn over the game.
 *
 * vitaGL calls overlay_draw with each finished frame (rendered, about to be
 * shown) from its display queue: the text is written straight into the
 * framebuffer with a small built-in font, a few thousand pixel writes and no
 * GPU work.
 *
 * The FPS counter shows the frames per second and the average frame time
 * over the last half second (setting "FPS counter", off by default), and
 * under it each core's use over the last second (setting "CPU usage", off by
 * default). The settings menu opens and closes with Select + Start
 * (reimpl/controls.c, which keeps the buttons from the game while it is
 * open): Up/Down pick a line, Left/Right or Cross change it, Circle closes.
 * Settings are saved at once (ssx/settings.c); MSAA and the 4th core
 * (ssx/core3.c) apply at the next launch, the frame rate in races (60 or 30)
 * at once.
 */

#include "ssx/overlay.h"
#include "ssx/settings.h"
#include "ssx/core3.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <vitaGL.h>

// 5x7 font: each glyph is 7 rows of 5 bits (most significant bit left)
static const struct {
    char c;
    uint8_t rows[7];
} font[] = {
    { '0', { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E } },
    { '1', { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E } },
    { '2', { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F } },
    { '3', { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E } },
    { '4', { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 } },
    { '5', { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E } },
    { '6', { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E } },
    { '7', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
    { '8', { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E } },
    { '9', { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C } },
    { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'B', { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E } },
    { 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } },
    { 'D', { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E } },
    { 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } },
    { 'F', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 } },
    { 'G', { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F } },
    { 'H', { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'I', { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E } },
    { 'J', { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C } },
    { 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } },
    { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F } },
    { 'M', { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 } },
    { 'N', { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 } },
    { 'O', { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
    { 'P', { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 } },
    { 'Q', { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D } },
    { 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } },
    { 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } },
    { 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
    { 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
    { 'V', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 } },
    { 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A } },
    { 'X', { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 } },
    { 'Y', { 0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04 } },
    { 'Z', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F } },
    { 'm', { 0x00, 0x00, 0x1A, 0x15, 0x15, 0x11, 0x11 } },
    { 's', { 0x00, 0x00, 0x0E, 0x10, 0x0E, 0x01, 0x1E } },
    { '.', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C } },
    { ':', { 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00 } },
    { '/', { 0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00 } },
    { '(', { 0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02 } },
    { ')', { 0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08 } },
    { '-', { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 } },
    { '+', { 0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00 } },
    { '>', { 0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08 } },
    { '%', { 0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03 } },
};

#define SCALE 2
#define GLYPH_W (6 * SCALE) // 5 pixels and a space
#define GLYPH_H (8 * SCALE)
#define MARGIN 4
#define SCREEN_W 960
#define SCREEN_H 544

#define WHITE 0xFFFFFFFF
#define GREY 0xFFA0A0A0
#define YELLOW 0xFF40E0FF // ABGR

extern int DISPLAY_STRIDE; // vitaGL, in pixels

static char text[32] = "";

// Frame statistics, updated from the render thread every half second
static uint64_t window_start;
static uint32_t window_frames;

// CPU use per core, updated every second: how long each core's idle thread
// ran (sceKernelGetSystemInfo, as PSVshell measures it), so the whole system's
// use, not only the game's. One system call a second.
static char cpu_text[32] = "";
static uint64_t cpu_start, idle_prev[4];

static void update_cpu(uint64_t now) {
    SceKernelSystemInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetSystemInfo(&info) < 0)
        return;
    uint64_t dt = now - cpu_start;
    if (cpu_start && dt) {
        int busy[4];
        for (int c = 0; c < 4; c++) {
            uint64_t idle = info.cpuInfo[c].idleClock - idle_prev[c];
            busy[c] = idle >= dt ? 0 : (int)(100 - idle * 100 / dt);
        }
        snprintf(cpu_text, sizeof(cpu_text), "CPU %3d%% %3d%% %3d%% %3d%%", busy[0], busy[1], busy[2], busy[3]);
    }
    cpu_start = now;
    for (int c = 0; c < 4; c++)
        idle_prev[c] = info.cpuInfo[c].idleClock;
}

void overlay_frame(void) {
    uint64_t now = sceKernelGetProcessTimeWide();
    if (!window_start)
        window_start = now;
    window_frames++;
    uint64_t dt = now - window_start;
    if (dt >= 500000) {
        float fps = window_frames * 1000000.0f / dt;
        snprintf(text, sizeof(text), "%.1f FPS %.1f ms", fps, dt / 1000.0f / window_frames);
        window_start = now;
        window_frames = 0;
    }
    if (!ssx_settings.cpu_usage) {
        cpu_start = 0; // starts over when turned on
        cpu_text[0] = 0;
    } else if (!cpu_start || now - cpu_start >= 1000000) {
        update_cpu(now);
    }
}

static const uint8_t *glyph(char c) {
    for (unsigned i = 0; i < sizeof(font) / sizeof(font[0]); i++)
        if (font[i].c == c)
            return font[i].rows;
    return NULL; // space and anything else: blank
}

// Darkens a box of the framebuffer (the background of text)
static void darken(uint32_t *fb, int stride, int x0, int y0, int w, int h) {
    for (int y = y0; y < y0 + h && y < SCREEN_H; y++)
        for (int x = x0; x < x0 + w && x < SCREEN_W; x++) {
            uint32_t *p = &fb[y * stride + x];
            *p = (*p >> 2 & 0x003F3F3F) | 0xFF000000;
        }
}

static void draw_text(uint32_t *fb, int stride, int x, int y, const char *s, uint32_t color) {
    for (int i = 0; s[i]; i++) {
        const uint8_t *rows = glyph(s[i]);
        if (!rows)
            continue;
        for (int r = 0; r < 7; r++)
            for (int col = 0; col < 5; col++) {
                if (!(rows[r] >> (4 - col) & 1))
                    continue;
                int x0 = x + i * GLYPH_W + col * SCALE, y0 = y + r * SCALE;
                for (int dy = 0; dy < SCALE; dy++)
                    for (int dx = 0; dx < SCALE; dx++)
                        if (x0 + dx < SCREEN_W && y0 + dy < SCREEN_H)
                            fb[(y0 + dy) * stride + x0 + dx] = color;
            }
    }
}

// --- The settings menu ---------------------------------------------------------

enum { ITEM_FPS, ITEM_CPU, ITEM_MSAA, ITEM_RACE_FPS, ITEM_CORE3, NITEMS };
static volatile int menu_open, menu_item;

int overlay_menu_is_open(void) {
    return menu_open;
}

void overlay_menu_toggle(void) {
    menu_open = !menu_open;
}

static const char *msaa_name(int v) {
    return v == 2 ? "2X" : "OFF";
}

static void change(int item, int dir) {
    if (item == ITEM_FPS) {
        ssx_settings.fps_counter = !ssx_settings.fps_counter;
    } else if (item == ITEM_CPU) {
        ssx_settings.cpu_usage = !ssx_settings.cpu_usage;
    } else if (item == ITEM_MSAA) {
        ssx_settings.msaa = ssx_settings.msaa ? 0 : 2;
    } else if (item == ITEM_RACE_FPS) {
        ssx_settings.race_fps = ssx_settings.race_fps == 30 ? 60 : 30;
    } else if (item == ITEM_CORE3) {
        ssx_settings.core3 = !ssx_settings.core3;
    }
    ssx_settings_save();
}

void overlay_menu_input(uint32_t pressed) {
    if (pressed & SCE_CTRL_UP)
        menu_item = (menu_item + NITEMS - 1) % NITEMS;
    if (pressed & SCE_CTRL_DOWN)
        menu_item = (menu_item + 1) % NITEMS;
    if (pressed & SCE_CTRL_LEFT)
        change(menu_item, -1);
    if (pressed & (SCE_CTRL_RIGHT | SCE_CTRL_CROSS))
        change(menu_item, 1);
    if (pressed & SCE_CTRL_CIRCLE)
        menu_open = 0;
}

static void draw_menu(uint32_t *fb, int stride) {
    char lines[14][48];
    uint32_t colors[14];
    int n = 0;
    snprintf(lines[n], sizeof(lines[n]), "SSX VITA SETTINGS");
    colors[n++] = WHITE;
    lines[n][0] = 0;
    colors[n++] = WHITE;
    snprintf(lines[n], sizeof(lines[n]), "%c FPS COUNTER   %s", menu_item == ITEM_FPS ? '>' : ' ',
             ssx_settings.fps_counter ? "ON" : "OFF");
    colors[n++] = menu_item == ITEM_FPS ? YELLOW : WHITE;
    snprintf(lines[n], sizeof(lines[n]), "%c CPU USAGE     %s", menu_item == ITEM_CPU ? '>' : ' ',
             ssx_settings.cpu_usage ? "ON" : "OFF");
    colors[n++] = menu_item == ITEM_CPU ? YELLOW : WHITE;
    snprintf(lines[n], sizeof(lines[n]), "%c MSAA          %s", menu_item == ITEM_MSAA ? '>' : ' ',
             msaa_name(ssx_settings.msaa));
    colors[n++] = menu_item == ITEM_MSAA ? YELLOW : WHITE;
    if (ssx_settings.msaa != ssx_msaa_active) {
        snprintf(lines[n], sizeof(lines[n]), "  (NOW %s: RESTART THE GAME TO APPLY)", msaa_name(ssx_msaa_active));
        colors[n++] = GREY;
    }
    snprintf(lines[n], sizeof(lines[n]), "%c RACE FPS      %d", menu_item == ITEM_RACE_FPS ? '>' : ' ',
             ssx_settings.race_fps);
    colors[n++] = menu_item == ITEM_RACE_FPS ? YELLOW : WHITE;
    snprintf(lines[n], sizeof(lines[n]), "%c 4TH CORE      %s", menu_item == ITEM_CORE3 ? '>' : ' ',
             ssx_settings.core3 ? "ON" : "OFF");
    colors[n++] = menu_item == ITEM_CORE3 ? YELLOW : WHITE;
    int core3 = ssx_core3_state();
    const char *core3_note = ssx_settings.core3 != ssx_core3_enabled ? "  (RESTART THE GAME TO APPLY)"
                             : core3 == CORE3_USED                 ? "  (CAPUNLOCKER FOUND: IN USE)"
                             : core3 == CORE3_UNAVAILABLE          ? "  (NEEDS THE CAPUNLOCKER PLUGIN)"
                                                                   : NULL;
    if (core3_note) {
        snprintf(lines[n], sizeof(lines[n]), "%s", core3_note);
        colors[n++] = GREY;
    }
    lines[n][0] = 0;
    colors[n++] = WHITE;
    snprintf(lines[n], sizeof(lines[n]), "UP/DOWN: SELECT  LEFT/RIGHT: CHANGE");
    colors[n++] = GREY;
    snprintf(lines[n], sizeof(lines[n]), "CIRCLE OR SELECT+START: CLOSE");
    colors[n++] = GREY;

    int width = 0;
    for (int i = 0; i < n; i++) {
        int len = (int)strlen(lines[i]);
        if (len > width)
            width = len;
    }
    int box_w = width * GLYPH_W + 4 * MARGIN, box_h = n * GLYPH_H + 4 * MARGIN;
    int x0 = (SCREEN_W - box_w) / 2, y0 = (SCREEN_H - box_h) / 2;
    darken(fb, stride, x0, y0, box_w, box_h);
    for (int i = 0; i < n; i++)
        draw_text(fb, stride, x0 + 2 * MARGIN, y0 + 2 * MARGIN + i * GLYPH_H, lines[i], colors[i]);
}

static void overlay_draw(void *framebuf) {
    uint32_t *fb = framebuf;
    int stride = DISPLAY_STRIDE;
    // Top left: the FPS counter, the CPU use under it
    const char *top[2] = { ssx_settings.fps_counter ? text : "", ssx_settings.cpu_usage ? cpu_text : "" };
    int y = 0;
    for (int i = 0; i < 2; i++) {
        char shown[32];
        strncpy(shown, top[i], sizeof(shown) - 1);
        shown[sizeof(shown) - 1] = 0;
        if (!shown[0])
            continue;
        darken(fb, stride, 0, y, (int)strlen(shown) * GLYPH_W + 2 * MARGIN, GLYPH_H + 2 * MARGIN);
        draw_text(fb, stride, MARGIN, y + MARGIN, shown, WHITE);
        y += GLYPH_H + 2 * MARGIN;
    }
    if (menu_open)
        draw_menu(fb, stride);
}

void overlay_init(void) {
    vglSetDisplayCallback(overlay_draw);
}
