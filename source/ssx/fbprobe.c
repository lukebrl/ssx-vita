/*
 * SSX port: render pass probe
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  fbprobe.c
 * @brief Logs the render passes of one frame every few seconds (debug aid).
 *
 * GameRender runs at 960x544, but the game still creates 1280x720 colour
 * and depth targets. To see what renders into them, each pass of a sampled
 * frame (a run of draws into the same framebuffer) is logged with the
 * framebuffer's attachment size, the viewport, and its draw, clear and
 * copy counts.
 */

#include "ssx/fbprobe.h"

#include <stdint.h>
#include <string.h>

#include <psp2/kernel/clib.h>
#include <vitaGL.h>

#include "utils/logger.h"

#ifdef DEBUG_SOLOADER

#define MAX_IDS 16384 // texture names are small indices
#define MAX_OBJS 128  // framebuffer and renderbuffer names are pointers
#define MAX_PASSES 48
#define SAMPLE_EVERY 300 // frames

static uint32_t tex_size[MAX_IDS]; // w | h << 16, by texture id

typedef struct {
    GLuint id;
    uint32_t a, b; // renderbuffer: size; framebuffer: colour 0 and depth sizes
} obj_t;
static obj_t rbs[MAX_OBJS], fbos[MAX_OBJS];

static obj_t *obj(obj_t *t, GLuint id) {
    obj_t *free_slot = NULL;
    for (int i = 0; i < MAX_OBJS; i++) {
        if (t[i].id == id)
            return &t[i];
        if (!t[i].id && !free_slot)
            free_slot = &t[i];
    }
    if (free_slot) {
        memset(free_slot, 0, sizeof(*free_slot));
        free_slot->id = id;
    }
    return free_slot;
}

typedef struct {
    GLuint fbo;
    uint32_t color, depth;
    int16_t vx, vy, vw, vh;
    uint16_t draws, clears, copies;
    uint32_t copy_size;
} pass_t;

static pass_t passes[MAX_PASSES];
static int npasses, capturing, overflow;
static uint32_t frames;
static GLuint cur_fbo;
static GLint vp[4];

static uint32_t pack(int w, int h) {
    return (uint32_t)(w & 0xFFFF) | (uint32_t)(h & 0xFFFF) << 16;
}

static pass_t *pass(void) {
    if (!capturing)
        return NULL;
    pass_t *p = npasses ? &passes[npasses - 1] : NULL;
    if (p && p->fbo == cur_fbo && p->vw == vp[2] && p->vh == vp[3])
        return p;
    if (npasses == MAX_PASSES) {
        overflow++;
        return NULL;
    }
    p = &passes[npasses++];
    memset(p, 0, sizeof(*p));
    p->fbo = cur_fbo;
    obj_t *f = cur_fbo ? obj(fbos, cur_fbo) : NULL;
    p->color = f ? f->a : 0;
    p->depth = f ? f->b : 0;
    p->vx = vp[0];
    p->vy = vp[1];
    p->vw = vp[2];
    p->vh = vp[3];
    return p;
}

void fbprobe_tex_image(GLenum target, GLint level, GLsizei w, GLsizei h) {
    if (target != GL_TEXTURE_2D || level != 0)
        return;
    GLint id = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &id);
    if (id > 0 && id < MAX_IDS)
        tex_size[id] = pack(w, h);
}

void fbprobe_rb_storage(GLsizei w, GLsizei h) {
    GLint id = 0;
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &id);
    obj_t *r = id ? obj(rbs, id) : NULL;
    if (r)
        r->a = pack(w, h);
}

void glBindFramebuffer_fbprobe(GLenum target, GLuint fb) {
    glBindFramebuffer(target, fb);
    cur_fbo = fb;
}

void glFramebufferTexture2D_fbprobe(GLenum target, GLenum attachment, GLenum textarget, GLuint tex, GLint level) {
    glFramebufferTexture2D(target, attachment, textarget, tex, level);
    obj_t *f = cur_fbo ? obj(fbos, cur_fbo) : NULL;
    uint32_t size = tex && tex < MAX_IDS ? tex_size[tex] : 0;
    if (f) {
        if (attachment == GL_COLOR_ATTACHMENT0)
            f->a = size;
        else
            f->b = size;
    }
}

void glFramebufferRenderbuffer_fbprobe(GLenum target, GLenum attachment, GLenum rbtarget, GLuint rb) {
    glFramebufferRenderbuffer(target, attachment, rbtarget, rb);
    obj_t *f = cur_fbo ? obj(fbos, cur_fbo) : NULL;
    obj_t *r = rb ? obj(rbs, rb) : NULL;
    if (f) {
        if (attachment == GL_COLOR_ATTACHMENT0)
            f->a = r ? r->a : 0;
        else
            f->b = r ? r->a : 0;
    }
}

void glViewport_fbprobe(GLint x, GLint y, GLsizei w, GLsizei h) {
    glViewport(x, y, w, h);
    vp[0] = x;
    vp[1] = y;
    vp[2] = w;
    vp[3] = h;
}

void glCopyTexSubImage2D_fbprobe(GLenum target, GLint level, GLint xoff, GLint yoff, GLint x, GLint y,
                                 GLsizei w, GLsizei h) {
    glCopyTexSubImage2D(target, level, xoff, yoff, x, y, w, h);
    pass_t *p = pass();
    if (p) {
        p->copies++;
        p->copy_size = pack(w, h);
    }
}

void fbprobe_draw(void) {
    pass_t *p = pass();
    if (p)
        p->draws++;
}

void fbprobe_clear(void) {
    pass_t *p = pass();
    if (p)
        p->clears++;
}

void fbprobe_frame(void) {
    if (capturing) {
        char line[1536];
        int len = sceClibSnprintf(line, sizeof(line), "[FBPROBE] frame %u:", frames);
        for (int i = 0; i < npasses && len < (int)sizeof(line) - 120; i++) {
            pass_t *p = &passes[i];
            len += sceClibSnprintf(line + len, sizeof(line) - len, " | fb %x", p->fbo);
            if (p->fbo)
                len += sceClibSnprintf(line + len, sizeof(line) - len, " %ux%u/%ux%u", p->color & 0xFFFF,
                                       p->color >> 16, p->depth & 0xFFFF, p->depth >> 16);
            len += sceClibSnprintf(line + len, sizeof(line) - len, " vp %d,%d %dx%d: %u draws", p->vx, p->vy,
                                   p->vw, p->vh, p->draws);
            if (p->clears)
                len += sceClibSnprintf(line + len, sizeof(line) - len, " %u clears", p->clears);
            if (p->copies)
                len += sceClibSnprintf(line + len, sizeof(line) - len, " %u copies %ux%u", p->copies,
                                       p->copy_size & 0xFFFF, p->copy_size >> 16);
        }
        if (overflow)
            sceClibSnprintf(line + len, sizeof(line) - len, " | +%d more", overflow);
        l_info("%s", line);
        capturing = 0;
    }
    if (SSX_DIAG && ++frames % SAMPLE_EVERY == 0) {
        capturing = 1;
        npasses = 0;
        overflow = 0;
    }
}

#endif
