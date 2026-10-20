/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  overlay.c
 * @brief FPS counter drawn over the game.
 *
 * vitaGL calls overlay_draw with each finished frame (rendered, about to be
 * shown) from its display queue: the text is written straight into the
 * framebuffer with a small built-in font, a few thousand pixel writes and no
 * GPU work. It shows the frames per second and the average frame time over
 * the last half second. Hidden by default; holding Select and pressing
 * Start shows or hides it (reimpl/controls.c).
 */

#include "ssx/overlay.h"

#include <stdint.h>
#include <stdio.h>

#include <psp2/kernel/processmgr.h>
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
    { '.', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C } },
    { 'F', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 } },
    { 'P', { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 } },
    { 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } },
    { 'm', { 0x00, 0x00, 0x1A, 0x15, 0x15, 0x11, 0x11 } },
    { 's', { 0x00, 0x00, 0x0E, 0x10, 0x0E, 0x01, 0x1E } },
};

#define SCALE 2
#define GLYPH_W (6 * SCALE) // 5 pixels and a space
#define GLYPH_H (8 * SCALE)
#define MARGIN 4

extern int DISPLAY_STRIDE; // vitaGL, in pixels

static char text[32] = "";
static volatile int visible; // hidden until the player turns it on (Select + Start)

// Frame statistics, updated from the render thread every half second
static uint64_t window_start;
static uint32_t window_frames;

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
}

static const uint8_t *glyph(char c) {
    for (unsigned i = 0; i < sizeof(font) / sizeof(font[0]); i++)
        if (font[i].c == c)
            return font[i].rows;
    return NULL; // space and anything else: blank
}

void overlay_toggle(void) {
    visible = !visible;
}

static void overlay_draw(void *framebuf) {
    if (!visible)
        return;
    uint32_t *fb = framebuf;
    int stride = DISPLAY_STRIDE;
    char shown[32];
    int n = 0;
    while (text[n] && n < (int)sizeof(shown) - 1) {
        shown[n] = text[n];
        n++;
    }
    if (!n)
        return;

    // Darken the box behind the text, then draw it in white
    int w = n * GLYPH_W + 2 * MARGIN, h = GLYPH_H + 2 * MARGIN;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t *p = &fb[y * stride + x];
            *p = (*p >> 2 & 0x003F3F3F) | 0xFF000000;
        }
    for (int i = 0; i < n; i++) {
        const uint8_t *rows = glyph(shown[i]);
        if (!rows)
            continue;
        for (int r = 0; r < 7; r++)
            for (int col = 0; col < 5; col++) {
                if (!(rows[r] >> (4 - col) & 1))
                    continue;
                int x0 = MARGIN + i * GLYPH_W + col * SCALE, y0 = MARGIN + r * SCALE;
                for (int dy = 0; dy < SCALE; dy++)
                    for (int dx = 0; dx < SCALE; dx++)
                        fb[(y0 + dy) * stride + x0 + dx] = 0xFFFFFFFF;
            }
    }
}

void overlay_init(void) {
    vglSetDisplayCallback(overlay_draw);
}
