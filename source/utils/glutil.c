/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/glutil.h"
#include "ssx/fbprobe.h"

#include <stdarg.h>
#include <stdio.h>

#include "utils/utils.h"
#include "utils/dialog.h"
#include "utils/logger.h"
#include "ssx/perf.h"
#include "ssx/overlay.h"
#include "ssx/settings.h"
#include "ssx/glprof.h"
#include "jni/jni_ssx.h"

#include <so_util/so_util.h>
extern so_module so_mod;

#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include <stdlib.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/stat.h>

#define MOVIE_PHYCONT (20 * 1024 * 1024) // physically contiguous memory left to the movie decoder (it takes 18 MB)

// Helpers for our handling of shaders
GLboolean skip_next_compile = GL_FALSE;
char next_shader_fname[256];
void load_shader(GLuint shader, const char * string, size_t length);

void gl_preload() {
    if (!file_exists("ur0:/data/libshacccg.suprx")
        && !file_exists("ur0:/data/external/libshacccg.suprx")) {
        fatal_error("Error: libshacccg.suprx is not installed. "
                    "Google \"ShaRKBR33D\" for quick installation.");
    }

#ifdef USE_GLSL_SHADERS
    vglSetSemanticBindingMode(VGL_MODE_POSTPONED);
#endif
}

void gl_init() {
    // Called from both main() and the game's eglInitialize; init only once.
    static int initialized = 0;
    if (initialized)
        return;
    initialized = 1;

    // vitaGL compiles shaders at glLinkProgram time (VGL_MODE_POSTPONED) and
    // caches the results here (built with HAVE_SHADER_CACHE=1).
    // The cache is keyed on the GLSL source before vitaGL translates it, so
    // the folder name changes whenever the translator does (v2: globals of
    // struct types made static, see glsl_handle_struct_globals).
    vglSetShaderCachePath(DATA_PATH "shader_cache_v2");

    // vitaGL's scratch pool for per-frame data (client-side vertex arrays,
    // uniforms): 32 MB by default, a third per frame in flight, and part of it
    // spilled into VRAM. This port draws mostly from buffer objects; what
    // doesn't fit in a frame's slice falls back to a normal allocation. The
    // [FPS] line logs the peak use per frame: 0 bytes through menus, a race
    // and its results screen, so 1 MB per frame is plenty.
    vglSetCircularPoolSize(1024 * 1024);

    // MSAA as the player set it in the settings menu (ssx/settings.c; off by
    // default, 2x at most: with 4x some textures came out multicolored). It
    // applies to the game's render targets too.
    // Leave 16 MB of main RAM free (not 6): the game creates ~25 threads
    // after this, and their stacks come out of it.
    // The common dialog memory (~8.8 MB) becomes a vitaGL pool too: the game
    // shows no system dialogs, and a race with other riders ran every other
    // pool out (vglInitExtended leaves it unused).
    // MOVIE_PHYCONT of the 26 MB of physically contiguous memory stays out of
    // vitaGL for the movie decoder (jni/jni_video.c: 4 MB of output frames and
    // a 14 MB block). vitaGL used at most 8.9 MB of its pool there in a session
    // with races; what no longer fits goes to its other pools, among them the
    // common dialog one (8.8 MB, never used).
    SceGxmMultisampleMode msaa = ssx_msaa_active == 2 ? SCE_GXM_MULTISAMPLE_2X : SCE_GXM_MULTISAMPLE_NONE;
    vglInitWithCustomThreshold(0, 960, 544, 16 * 1024 * 1024, 0, MOVIE_PHYCONT, 0, msaa);
}

// Deletes from threads without the GL context. Android drops GL calls there,
// but Scaleform releases textures from its own threads (Texture destructor,
// game+0xA6A19C), which leaked them: the same race run twice ended with 30
// more textures, and the next race ran memory out. They are queued and done
// on the GL thread at the next swap (names aren't reused before: vitaGL gives
// a texture slot or buffer out again only once it is deleted).
enum { DEL_TEXTURE, DEL_BUFFER, DEL_FRAMEBUFFER, DEL_RENDERBUFFER };
#define MAX_DEFERRED 8192
static struct {
    uint8_t kind;
    GLuint name;
} deferred[MAX_DEFERRED];
static int num_deferred;
static SceKernelLwMutexWork deferred_lock;
static int deferred_lock_ready;
static uint32_t deferred_total, deferred_dropped;

static void defer_delete(int kind, GLsizei n, const GLuint *names) {
    if (!deferred_lock_ready) {
        sceKernelCreateLwMutex(&deferred_lock, "gl_deferred", 0, 0, NULL);
        deferred_lock_ready = 1;
    }
    sceKernelLockLwMutex(&deferred_lock, 1, NULL);
    for (GLsizei i = 0; i < n; i++) {
        if (!names[i])
            continue;
        if (num_deferred == MAX_DEFERRED) {
            deferred_dropped++;
            continue;
        }
        deferred[num_deferred].kind = kind;
        deferred[num_deferred].name = names[i];
        num_deferred++;
        deferred_total++;
    }
    sceKernelUnlockLwMutex(&deferred_lock, 1);
}

// Live buffer storage as the game sees it (id -> size), to tell a buffer
// leak in the game from memory vitaGL doesn't give back. GL thread only.
#define BUF_MAP 16384 // power of two
#define BUF_TOMB 0xFFFFFFFFu
static struct { GLuint id; uint32_t size; } buf_map[BUF_MAP];
static uint32_t buf_live, buf_bytes;

static int buf_find(GLuint id, int insert) {
    uint32_t h = (id * 2654435761u) & (BUF_MAP - 1);
    int tomb = -1;
    for (int i = 0; i < BUF_MAP; i++, h = (h + 1) & (BUF_MAP - 1)) {
        if (buf_map[h].id == id)
            return h;
        if (buf_map[h].id == BUF_TOMB && tomb < 0)
            tomb = h;
        if (buf_map[h].id == 0)
            return insert ? (tomb >= 0 ? tomb : (int)h) : -1;
    }
    return insert ? tomb : -1;
}

static void buf_track_data(GLenum target, GLsizeiptr size) {
    GLint id = 0;
    glGetIntegerv(target == GL_ELEMENT_ARRAY_BUFFER ? GL_ELEMENT_ARRAY_BUFFER_BINDING : GL_ARRAY_BUFFER_BINDING, &id);
    if (!id)
        return;
    int i = buf_find(id, 0);
    if (i < 0) {
        i = buf_find(id, 1);
        if (i < 0)
            return;
        buf_map[i].id = id;
        buf_map[i].size = 0;
        buf_live++;
    }
    buf_bytes += size - buf_map[i].size;
    buf_map[i].size = size;
}

static void buf_track_delete(GLsizei n, const GLuint *names) {
    for (GLsizei k = 0; k < n; k++) {
        int i = names[k] ? buf_find(names[k], 0) : -1;
        if (i < 0)
            continue;
        buf_bytes -= buf_map[i].size;
        buf_live--;
        buf_map[i].id = BUF_TOMB;
    }
}

void glutil_mem_report(void) {
    static const char *const names[] = { "vram", "ram", "phycont", "cdlg" };
    static const vglMemType types[] = { VGL_MEM_VRAM, VGL_MEM_RAM, VGL_MEM_PHYCONT, VGL_MEM_BUDGET };
    char line[384];
    size_t used_total = 0;
    int len = sceClibSnprintf(line, sizeof(line), "[GLMEM] used:");
    for (int t = 0; t < 4; t++) {
        size_t total = vglMemTotal(types[t]), used = total - vglMemFree(types[t]);
        used_total += used;
        len += sceClibSnprintf(line + len, sizeof(line) - len, " %s %.1f/%.1f MB (peak %.1f)", names[t],
                               used / 1048576.0f, total / 1048576.0f, vglMemPeak(types[t]) / 1048576.0f);
    }
    uint32_t tex[VGL_SSX_TEX_KINDS], tex_total = 0;
    vglSsxTextureStats(tex, NULL, 0);
    for (int k = 0; k < VGL_SSX_TEX_KINDS; k++)
        tex_total += tex[k];
    sceClibSnprintf(line + len, sizeof(line) - len,
                    "; %.1f MB in all: textures %.1f MB, %u game buffers %.1f MB, other %.1f MB",
                    used_total / 1048576.0f, tex_total / 1048576.0f, buf_live, buf_bytes / 1048576.0f,
                    ((float)used_total - tex_total - buf_bytes) / 1048576.0f);
    l_info("%s", line);
}

static void run_deferred_deletes(void) {
    if (!deferred_lock_ready || !num_deferred)
        return;
    sceKernelLockLwMutex(&deferred_lock, 1, NULL);
    for (int i = 0; i < num_deferred; i++) {
        GLuint name = deferred[i].name;
        switch (deferred[i].kind) {
            case DEL_TEXTURE: glDeleteTextures(1, &name); break;
            case DEL_BUFFER: buf_track_delete(1, &name); glDeleteBuffers(1, &name); break;
            case DEL_FRAMEBUFFER: glDeleteFramebuffers(1, &name); break;
            case DEL_RENDERBUFFER: glDeleteRenderbuffers(1, &name); break;
        }
    }
    num_deferred = 0;
    sceKernelUnlockLwMutex(&deferred_lock, 1);
}

void glutil_deferred_report(void) {
    if (deferred_total || deferred_dropped)
        l_info("[GL] %u deletes from other threads done on the GL thread%s", deferred_total,
               deferred_dropped ? " (some dropped: queue full)" : "");
    deferred_total = 0;
}

#define DELETE_WRAPPER(fn, kind)                                       \
    void fn##_soloader(GLsizei n, const GLuint *names) {              \
        if (glprof_on_render_thread())                                 \
            fn(n, names);                                              \
        else                                                           \
            defer_delete(kind, n, names);                              \
    }
DELETE_WRAPPER(glDeleteTextures, DEL_TEXTURE)
void glDeleteBuffers_soloader(GLsizei n, const GLuint *names) {
    if (glprof_on_render_thread()) {
        buf_track_delete(n, names);
        glDeleteBuffers(n, names);
    } else {
        defer_delete(DEL_BUFFER, n, names);
    }
}
DELETE_WRAPPER(glDeleteFramebuffers, DEL_FRAMEBUFFER)
DELETE_WRAPPER(glDeleteRenderbuffers, DEL_RENDERBUFFER)

#ifdef DEBUG_SOLOADER
// Where the frame's present goes (debug): deletes, movie, swap, the rest
static uint32_t swap_parts_us[4], swap_parts_n;
void glutil_swap_parts_report(void) {
    if (!SSX_DIAG)
        return;
    uint32_t n = swap_parts_n ? swap_parts_n : 1;
    l_info("[PERF] gl_swap per frame: deletes %.2f, movie %.2f, "
           "vglSwapBuffers %.2f, after %.2f ms",
           swap_parts_us[0] / 1000.0f / n, swap_parts_us[1] / 1000.0f / n,
           swap_parts_us[2] / 1000.0f / n, swap_parts_us[3] / 1000.0f / n);
    swap_parts_us[0] = swap_parts_us[1] = swap_parts_us[2] = swap_parts_us[3] = swap_parts_n = 0;
}
#endif

void gl_swap() {
#ifdef DEBUG_SOLOADER
    uint32_t ta = sceKernelGetProcessTimeLow();
#endif
    run_deferred_deletes();
#ifdef DEBUG_SOLOADER
    uint32_t tb = sceKernelGetProcessTimeLow();
#endif
    movie_draw();
    uint64_t t0 = sceKernelGetProcessTimeWide();
    vglSwapBuffers(GL_FALSE);
#ifdef DEBUG_SOLOADER
    uint32_t tc = sceKernelGetProcessTimeLow();
#endif
    fbprobe_frame();
    perf_frame((uint32_t)(sceKernelGetProcessTimeWide() - t0));
    overlay_frame();
#ifdef DEBUG_SOLOADER
    uint32_t td = sceKernelGetProcessTimeLow();
    swap_parts_us[0] += tb - ta;
    swap_parts_us[1] += (uint32_t)t0 - tb;
    swap_parts_us[2] += tc - (uint32_t)t0;
    swap_parts_us[3] += td - tc;
    swap_parts_n++;
#endif
}

void glShaderSource_soloader(GLuint shader, GLsizei count,
                             const GLchar **string, const GLint *_length) {
#ifdef DEBUG_OPENGL
    sceClibPrintf("[gl_dbg] glShaderSource<%p>(shader: %i, count: %i, string: %p, length: %p)\n", __builtin_return_address(0), shader, count, string, _length);
#endif
    if (!string) {
        l_error("<%p> Shader source string is NULL, count: %i",
                   __builtin_return_address(0), count);
        skip_next_compile = GL_TRUE;
        return;
    } else if (!*string) {
        l_error("<%p> Shader source *string is NULL, count: %i",
                   __builtin_return_address(0), count);
        skip_next_compile = GL_TRUE;
        return;
    }

    size_t total_length = 0;

    for (int i = 0; i < count; ++i) {
        if (!_length) {
            total_length += strlen(string[i]);
        } else {
            total_length += _length[i];
        }
    }

    char * str = malloc(total_length+1);
    size_t l = 0;

    for (int i = 0; i < count; ++i) {
        if (!_length) {
            memcpy(str + l, string[i], strlen(string[i]));
            l += strlen(string[i]);
        } else {
            memcpy(str + l, string[i], _length[i]);
            l += _length[i];
        }
    }
    str[total_length] = '\0';

    load_shader(shader, str, total_length);

    free(str);
}

void glLinkProgram_soloader(GLuint program) {
    // With VGL_MODE_POSTPONED this is where shaders get translated/compiled
    uint64_t t0 = current_timestamp_ms();
    SceUInt64 t0_us = sceKernelGetProcessTimeWide();
    glLinkProgram(program);
    perf_gl_link((uint32_t)(sceKernelGetProcessTimeWide() - t0_us));
    uint64_t dt = current_timestamp_ms() - t0;
    if (dt >= 20)
        l_info("glLinkProgram(%u): %llu ms", program, dt);

    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char info[1024] = "";
        glGetProgramInfoLog(program, sizeof(info), NULL, info);
        l_error("glLinkProgram(%u) FAILED: %s", program, info);
    }
}

void glCompileShader_soloader(GLuint shader) {
#ifdef DEBUG_OPENGL
    sceClibPrintf("[gl_dbg] glCompileShader<%p>(shader: %i)\n", __builtin_return_address(0), shader);
#endif

#ifndef USE_GXP_SHADERS
    if (!skip_next_compile) {
        glCompileShader(shader);
#ifdef DUMP_COMPILED_SHADERS
        // Cache only shaders that compiled, or a failure would stick
        GLint ok = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (ok) {
            void *bin = vglMalloc(128 * 1024);
            GLsizei len = 0;
            vglGetShaderBinary(shader, 128 * 1024, &len, bin);
            if (len > 0)
                file_save(next_shader_fname, bin, len);
            vglFree(bin);
        } else {
            char info[512];
            glGetShaderInfoLog(shader, sizeof(info), NULL, info);
            l_error("Shader compile failed: %s", info);
        }
#endif
    }
    skip_next_compile = GL_FALSE;
#endif
}

#if defined(USE_GLSL_SHADERS) && defined(DUMP_COMPILED_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char gxp_path[256];
    snprintf(gxp_path, sizeof(gxp_path), DATA_PATH"gxp/%s.gxp", sha_name);

    if (file_exists(gxp_path)) {
        uint8_t *buffer;
        size_t size;

        file_load(gxp_path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
        skip_next_compile = GL_TRUE;
    } else {
        glShaderSource(shader, 1, &string, &length);
        strcpy(next_shader_fname, gxp_path);
    }

    free(sha_name);
}
#elif defined(USE_GLSL_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    glShaderSource(shader, 1, &string, &length);
}
#elif defined(USE_CG_SHADERS) && defined(DUMP_COMPILED_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char gxp_path[256];
    char cg_path[256];
    snprintf(gxp_path, sizeof(gxp_path), DATA_PATH"gxp/%s.gxp", sha_name);
    snprintf(cg_path, sizeof(cg_path), DATA_PATH"cg/%s.cg", sha_name);

    if (file_exists(gxp_path)) {
        uint8_t *buffer;
        size_t size;

        file_load(gxp_path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
        skip_next_compile = GL_TRUE;
    } else if (file_exists(cg_path)) {
        char *buffer;
        size_t size;

        file_load(cg_path, (uint8_t **) &buffer, &size);

        glShaderSource(shader, 1, &string, &size);
        strcpy(next_shader_fname, gxp_path);

        free(buffer);
        skip_next_compile = GL_FALSE;
    } else {
        l_warn("Encountered an untranslated shader %s, saving GLSL "
               "and using a dummy shader.", sha_name);

        char glsl_path[256];
        snprintf(glsl_path, sizeof(glsl_path), DATA_PATH"glsl/%s.glsl", sha_name);
        file_mkpath(glsl_path, 0777);
        file_save(glsl_path, (const uint8_t *) string, length);

        if (strstr(string, "gl_FragColor")) {
            const char *dummy_shader = "float4 main() { return float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        } else {
            const char *dummy_shader = "void main(float4 out gl_Position : POSITION ) { gl_Position = float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        }

        skip_next_compile = GL_FALSE;
    }

    free(sha_name);
}
#elif defined(USE_CG_SHADERS) || defined(USE_GXP_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char path[256];
#ifdef USE_CG_SHADERS
    snprintf(path, sizeof(path), DATA_PATH"cg/%s.cg", sha_name);
#else
    snprintf(path, sizeof(path), DATA_PATH"gxp/%s.gxp", sha_name);
#endif

    if (file_exists(path)) {
#ifdef USE_CG_SHADERS
        char *buffer;
        size_t size;

        file_load(path, (uint8_t **) &buffer, &size);

        glShaderSource(shader, 1, &string, &size);

        free(buffer);
#else
        uint8_t *buffer;
        size_t size;

        file_load(path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
#endif
    } else {
        l_warn("Encountered an untranslated shader %s, saving GLSL "
               "and using a dummy shader.", sha_name);

        char glsl_path[256];
        snprintf(glsl_path, sizeof(glsl_path), DATA_PATH"glsl/%s.glsl", sha_name);
        file_mkpath(glsl_path, 0777);
        file_save(glsl_path, (const uint8_t *) string, length);

        if (strstr(string, "gl_FragColor")) {
            const char *dummy_shader = "float4 main() { return float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        } else {
            const char *dummy_shader = "void main(float4 out gl_Position : POSITION ) { gl_Position = float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        }
    }

    free(sha_name);
}
#else
#error "Define one of (USE_GLSL_SHADERS, USE_CG_SHADERS, USE_GXP_SHADERS)"
#endif

// Draw call counters, logged with the FPS (debug aid)
volatile unsigned gl_draw_calls = 0;
volatile unsigned gl_clear_calls = 0;

void glDrawArrays_soloader(GLenum mode, GLint first, GLsizei count) {
    gl_draw_calls++;
    fbprobe_draw();
    uint64_t t0 = sceKernelGetProcessTimeWide();
    glDrawArrays(mode, first, count);
    perf_gl_draw_time((uint32_t)(sceKernelGetProcessTimeWide() - t0));
}

void glDrawElements_soloader(GLenum mode, GLsizei count, GLenum type, const void *indices) {
    gl_draw_calls++;
    fbprobe_draw();
    uint64_t t0 = sceKernelGetProcessTimeWide();
    glDrawElements(mode, count, type, indices);
    perf_gl_draw_time((uint32_t)(sceKernelGetProcessTimeWide() - t0));
}

void glClear_soloader(GLbitfield mask) {
    fbprobe_clear();
    gl_clear_calls++;
    glClear(mask);
}

static void log_gl_alloc(uintptr_t caller, const char *what, uint32_t bytes, const char *fmt, ...);
static uint32_t gl_pixel_bytes(GLenum format, GLenum type);

// Texture upload timing (debug aid: frames take seconds during front-end load)
// Screen-sized images (1280x720 and similar: loading screens, menu
// backgrounds) are stored at the Vita's resolution, 960 wide: the screen
// can't show more of them, and they take 44% less video memory. Done once at
// upload (bilinear, row by row); texture coordinates are normalized, so the
// game doesn't notice. Later uploads to the same texture (mip levels,
// glTexSubImage2D) are scaled the same way.
#define SCREEN_IMAGE_W 960
#ifndef SSX_SCALE_SCREEN_IMAGES
#define SSX_SCALE_SCREEN_IMAGES 0
#endif
#define MAX_SCALED 64

static struct {
    GLuint id;
    uint16_t sw, sh, dw, dh; // original and stored size of level 0
} scaled[MAX_SCALED];

static int is_screen_image(GLsizei w, GLsizei h) {
    return w >= 1200 && w <= 1280 && h >= 576 && h <= 720;
}

static GLuint bound_texture_2d(void) {
    GLint id = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &id);
    return (GLuint)id;
}

static int scaled_find(GLuint id) {
    for (int i = 0; i < MAX_SCALED; i++)
        if (scaled[i].id == id)
            return i;
    return -1;
}

static void scaled_forget(GLuint id) {
    int i = scaled_find(id);
    if (i >= 0)
        scaled[i].id = 0;
}

// Bytes per pixel of the formats handled, 0 for others
static int scale_pixel_bytes(GLenum format, GLenum type) {
    if (type == GL_UNSIGNED_BYTE) {
        switch (format) {
            case GL_RGBA: return 4;
            case GL_RGB: return 3;
            case GL_LUMINANCE_ALPHA: return 2;
            case GL_ALPHA:
            case GL_LUMINANCE: return 1;
        }
        return 0;
    }
    if ((type == GL_UNSIGNED_SHORT_5_6_5 && format == GL_RGB) ||
        ((type == GL_UNSIGNED_SHORT_4_4_4_4 || type == GL_UNSIGNED_SHORT_5_5_5_1) && format == GL_RGBA))
        return 2;
    return 0;
}

// One row to 4 channels of 8 bits (unused ones 0)
static void unpack_row(const uint8_t *src, int w, GLenum type, int bpp, uint8_t *out) {
    for (int x = 0; x < w; x++, out += 4) {
        if (type == GL_UNSIGNED_BYTE) {
            for (int c = 0; c < 4; c++)
                out[c] = c < bpp ? src[x * bpp + c] : 0;
            continue;
        }
        uint16_t v = ((const uint16_t *)src)[x];
        if (type == GL_UNSIGNED_SHORT_5_6_5) {
            out[0] = (v >> 11 & 31) * 255 / 31; out[1] = (v >> 5 & 63) * 255 / 63;
            out[2] = (v & 31) * 255 / 31; out[3] = 0;
        } else if (type == GL_UNSIGNED_SHORT_4_4_4_4) {
            out[0] = (v >> 12 & 15) * 17; out[1] = (v >> 8 & 15) * 17;
            out[2] = (v >> 4 & 15) * 17; out[3] = (v & 15) * 17;
        } else { // 5_5_5_1
            out[0] = (v >> 11 & 31) * 255 / 31; out[1] = (v >> 6 & 31) * 255 / 31;
            out[2] = (v >> 1 & 31) * 255 / 31; out[3] = (v & 1) * 255;
        }
    }
}

static void pack_pixel(const int *c, GLenum type, int bpp, uint8_t *dst) {
    if (type == GL_UNSIGNED_BYTE) {
        for (int i = 0; i < bpp; i++)
            dst[i] = c[i];
        return;
    }
    uint16_t v;
    if (type == GL_UNSIGNED_SHORT_5_6_5)
        v = ((c[0] * 31 + 127) / 255) << 11 | ((c[1] * 63 + 127) / 255) << 5 | ((c[2] * 31 + 127) / 255);
    else if (type == GL_UNSIGNED_SHORT_4_4_4_4)
        v = ((c[0] * 15 + 127) / 255) << 12 | ((c[1] * 15 + 127) / 255) << 8 | ((c[2] * 15 + 127) / 255) << 4 | ((c[3] * 15 + 127) / 255);
    else
        v = ((c[0] * 31 + 127) / 255) << 11 | ((c[1] * 31 + 127) / 255) << 6 | ((c[2] * 31 + 127) / 255) << 1 | (c[3] >= 128);
    *(uint16_t *)dst = v;
}

// Bilinear resample of a sw x sh image to dw x dh (rows 4-byte aligned, the
// GL default unpack alignment); NULL if out of memory
static void *resample(const void *src, int sw, int sh, int dw, int dh, GLenum type, int bpp) {
    int src_stride = (sw * bpp + 3) & ~3, dst_stride = (dw * bpp + 3) & ~3;
    uint8_t *dst = malloc((size_t)dst_stride * dh);
    uint8_t *rows = malloc((size_t)sw * 4 * 2);
    if (!dst || !rows) {
        free(dst);
        free(rows);
        return NULL;
    }
    uint8_t *row[2] = { rows, rows + sw * 4 };
    int cached[2] = { -1, -1 };
    for (int y = 0; y < dh; y++) {
        // Source position of the pixel centre, in 1/256
        int fy = (int)(((int64_t)(2 * y + 1) * sh * 128) / dh) - 128;
        if (fy < 0) fy = 0;
        int y0 = fy >> 8, wy = fy & 255;
        int y1 = y0 + 1 < sh ? y0 + 1 : y0;
        for (int k = 0; k < 2; k++) {
            int want = k ? y1 : y0;
            if (cached[k] != want) {
                unpack_row((const uint8_t *)src + (size_t)want * src_stride, sw, type, bpp, row[k]);
                cached[k] = want;
            }
        }
        uint8_t *out = dst + (size_t)y * dst_stride;
        for (int x = 0; x < dw; x++) {
            int fx = (int)(((int64_t)(2 * x + 1) * sw * 128) / dw) - 128;
            if (fx < 0) fx = 0;
            int x0 = fx >> 8, wx = fx & 255;
            int x1 = x0 + 1 < sw ? x0 + 1 : x0;
            int c[4];
            for (int i = 0; i < 4; i++) {
                int top = row[0][x0 * 4 + i] * (256 - wx) + row[0][x1 * 4 + i] * wx;
                int bot = row[1][x0 * 4 + i] * (256 - wx) + row[1][x1 * 4 + i] * wx;
                c[i] = (top * (256 - wy) + bot * wy + 32768) >> 16;
            }
            pack_pixel(c, type, bpp, out + x * bpp);
        }
    }
    free(rows);
    return dst;
}

// Size of a level of a scaled texture, as stored
static void scaled_level_size(int i, GLint level, int *w, int *h) {
    *w = scaled[i].dw >> level;
    *h = scaled[i].dh >> level;
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
}

static int nscaled; // entries in use, to skip the lookups when there are none

// Level 0 of a 2D texture is (re)specified: scale it if it is a screen image.
// Returns the data to upload (data, or a scaled copy to free) and the size.
static void *scale_tex_image(GLenum target, GLint level, GLenum format, GLenum type,
                             GLsizei *width, GLsizei *height, const void *data) {
    // Off: a 1280x720 texture the game uploads with data turned out to be a
    // render target, and shrinking it showed the 3D scene zoomed and off
    // centre. Screen-sized image files are shrunk offline instead
    // (tools/shrink_images.py).
    if (!SSX_SCALE_SCREEN_IMAGES || target != GL_TEXTURE_2D)
        return NULL;
    int bpp = scale_pixel_bytes(format, type);
    if (level == 0) {
        if (!nscaled && !is_screen_image(*width, *height))
            return NULL;
        GLuint id = bound_texture_2d();
        int i = scaled_find(id);
        if (i >= 0) {
            scaled[i].id = 0;
            nscaled--;
        }
        if (!data || !bpp || !is_screen_image(*width, *height))
            return NULL;
        i = scaled_find(0);
        if (i < 0)
            return NULL;
        int dw = SCREEN_IMAGE_W, dh = (*height * SCREEN_IMAGE_W + *width / 2) / *width;
        void *out = resample(data, *width, *height, dw, dh, type, bpp);
        if (!out)
            return NULL;
        scaled[i].id = id;
        scaled[i].sw = *width; scaled[i].sh = *height;
        scaled[i].dw = dw; scaled[i].dh = dh;
        nscaled++;
        *width = dw;
        *height = dh;
        return out;
    }
    if (!nscaled)
        return NULL;
    int i = scaled_find(bound_texture_2d());
    if (i < 0)
        return NULL;
    // A mip level of a scaled texture: scale it to match level 0
    int lw, lh;
    scaled_level_size(i, level, &lw, &lh);
    void *out = data && bpp ? resample(data, *width, *height, lw, lh, type, bpp) : NULL;
    *width = lw;
    *height = lh;
    return out;
}

void glTexImage2D_soloader(GLenum target, GLint level, GLint internalformat, GLsizei width,
                           GLsizei height, GLint border, GLenum format, GLenum type, const void *data) {
    void *scaled_data = scale_tex_image(target, level, format, type, &width, &height, data);
    if (scaled_data)
        data = scaled_data;
    log_gl_alloc((uintptr_t)__builtin_return_address(0), "glTexImage2D", (uint32_t)(width * height * gl_pixel_bytes(format, type)),
                 "%dx%d fmt 0x%x type 0x%x level %d%s", width, height, format, type, level,
                 data ? "" : ", no data (render target?)");
    uint64_t t0 = current_timestamp_ms();
    SceUInt64 t0_us = sceKernelGetProcessTimeWide();
    glTexImage2D(target, level, internalformat, width, height, border, format, type, data);
    fbprobe_tex_image(target, level, width, height);
    perf_gl_upload((uint32_t)(width * height * gl_pixel_bytes(format, type)),
                   (uint32_t)(sceKernelGetProcessTimeWide() - t0_us));
    free(scaled_data);
    uint64_t dt = current_timestamp_ms() - t0;
    if (dt >= 30)
        l_info("[SLOW] glTexImage2D %dx%d fmt 0x%x type 0x%x level %d: %llu ms",
               width, height, format, type, level, dt);
}

void glCompressedTexImage2D_soloader(GLenum target, GLint level, GLenum internalformat, GLsizei width,
                                     GLsizei height, GLint border, GLsizei imageSize, const void *data) {
    log_gl_alloc((uintptr_t)__builtin_return_address(0), "glCompressedTexImage2D", (uint32_t)imageSize, "%dx%d fmt 0x%x level %d",
                 width, height, internalformat, level);
    uint64_t t0 = current_timestamp_ms();
    if (nscaled && target == GL_TEXTURE_2D && level == 0) {
        int i = scaled_find(bound_texture_2d()); // re-specified, now compressed
        if (i >= 0) {
            scaled[i].id = 0;
            nscaled--;
        }
    }
    SceUInt64 t0_us = sceKernelGetProcessTimeWide();
    glCompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, data);
    perf_gl_upload((uint32_t)imageSize, (uint32_t)(sceKernelGetProcessTimeWide() - t0_us));
    uint64_t dt = current_timestamp_ms() - t0;
    if (dt >= 30)
        l_info("[SLOW] glCompressedTexImage2D %dx%d fmt 0x%x level %d: %llu ms",
               width, height, internalformat, level, dt);
}

// GPU allocation log (debug aid, SSX_DIAG: VRAM filled up while the front end
// loaded). Logs every allocation of 256 KB or more with a running total.
static uint64_t gl_alloc_total = 0;

static void log_gl_alloc(uintptr_t caller, const char *what, uint32_t bytes, const char *fmt, ...) {
    gl_alloc_total += bytes;
    if (!SSX_DIAG || bytes < 256 * 1024)
        return;
    char desc[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(desc, sizeof(desc), fmt, args);
    va_end(args);
    // Calls go through EA::Graphics::OpenGLES20 wrappers, so for big allocations
    // also list the libgame code addresses on the stack (a rough backtrace)
    char trace[256] = "";
    if (bytes >= 1024 * 1024) {
        uint32_t *sp = (uint32_t *)__builtin_frame_address(0);
        // Stay inside this thread's stack (reading past its top faults)
        int words = 256;
        SceKernelThreadInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) >= 0) {
            uint8_t *top = (uint8_t *)info.stack + info.stackSize;
            words = (top - (uint8_t *)sp) / 4;
            if (words > 1024)
                words = 1024;
        }
        uint32_t seen[16];
        int n = 0, len = 0;
        for (int i = 0; i < words && n < 16; i++) {
            uint32_t v = sp[i];
            int dup = 0;
            for (int k = 0; k < n; k++)
                dup |= seen[k] == v;
            if (!dup && v > so_mod.text_base && v < so_mod.text_base + so_mod.text_size) {
                seen[n] = v;
                len += snprintf(trace + len, sizeof(trace) - len, " 0x%X", (unsigned)(v - so_mod.text_base));
                n++;
            }
        }
    }
    l_info("[GPUALLOC] %s %u KB (%s), total %llu KB, from game+0x%X%s%s", what, bytes / 1024, desc,
           gl_alloc_total / 1024, (unsigned)(caller - so_mod.text_base), trace[0] ? ", stack" : "", trace);
}

static uint32_t gl_pixel_bytes(GLenum format, GLenum type) {
    switch (type) {
        case GL_UNSIGNED_SHORT_5_6_5:
        case GL_UNSIGNED_SHORT_4_4_4_4:
        case GL_UNSIGNED_SHORT_5_5_5_1:
            return 2;
        case GL_FLOAT:
            return 16;
        case 0x8D61: // GL_HALF_FLOAT_OES
            return 8;
    }
    switch (format) {
        case GL_ALPHA:
        case GL_LUMINANCE: return 1;
        case GL_LUMINANCE_ALPHA: return 2;
        case GL_RGB: return 3;
        default: return 4;
    }
}

void glBufferData_soloader(GLenum target, GLsizeiptr size, const void *data, GLenum usage) {
    log_gl_alloc((uintptr_t)__builtin_return_address(0), "glBufferData", (uint32_t)size, "target 0x%x usage 0x%x", target, usage);
    glBufferData(target, size, data, usage);
    buf_track_data(target, size);
}

// Logs where the game updates the big buffers that get a ring in vitaGL
// (updated while in use, see ssx_ring_update), once per buffer
// The libgame code addresses on the stack from frame (a rough backtrace: GL
// calls come through EA::Graphics::OpenGLES20 and the GL guard stubs)
static void game_stack(const uint32_t *sp, char *out, int size) {
    out[0] = 0;
    int words = perf_stack_words(sp, 256);
    for (int i = 0, n = 0, len = 0; i < words && n < 8 && len < size; i++) {
        uint32_t v = sp[i];
        if (v > so_mod.text_base && v < so_mod.text_base + so_mod.text_size) {
            len += snprintf(out + len, size - len, " 0x%X", (unsigned)(v - so_mod.text_base));
            n++;
        }
    }
}

// glBufferSubData statistics per [PERF] window: calls, bytes, and the slowest
// call (what the 4 to 9 ms per frame in races is)
static struct {
    uint32_t calls, max_us, max_size, max_offset, max_buffer, max_target;
    uint64_t bytes, us;
    char max_trace[128];
} subdata;

void glBufferSubData_soloader(GLenum target, GLintptr offset, GLsizeiptr size, const void *data) {
    uint32_t rings = vgl_ssx_rings_created;
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    glBufferSubData(target, offset, size, data);
    uint32_t us = (uint32_t)(sceKernelGetProcessTimeWide() - t0);
    subdata.calls++;
    subdata.bytes += size;
    subdata.us += us;
    if (us > subdata.max_us || vgl_ssx_rings_created != rings) {
        GLint buffer_size = 0;
        glGetBufferParameteriv(target, GL_BUFFER_SIZE, &buffer_size);
        char trace[128];
        game_stack((const uint32_t *)__builtin_frame_address(0), trace, sizeof(trace));
        if (us > subdata.max_us) {
            subdata.max_us = us;
            subdata.max_size = size;
            subdata.max_offset = offset;
            subdata.max_buffer = buffer_size;
            subdata.max_target = target;
            strcpy(subdata.max_trace, trace);
        }
        if (vgl_ssx_rings_created != rings)
            l_info("[GPUALLOC] ring for a %d KB buffer (target 0x%x) updated while in use, %d bytes at %d, stack%s",
                   buffer_size / 1024, target, (int)size, (int)offset, trace);
    }
}

void glutil_subdata_report(uint32_t frames) {
    if (frames && subdata.calls)
        l_info("[PERF] glBufferSubData: %.1f calls, %.0f KB, %.2f ms per frame; slowest %.2f ms: %u bytes at %u "
               "of a %u KB buffer (target 0x%x), stack%s",
               subdata.calls / (float)frames, subdata.bytes / 1024.0f / frames, subdata.us / 1000.0f / frames,
               subdata.max_us / 1000.0f, subdata.max_size, subdata.max_offset, subdata.max_buffer / 1024,
               subdata.max_target, subdata.max_trace);
    sceClibMemset(&subdata, 0, sizeof(subdata));
}

void glRenderbufferStorage_soloader(GLenum target, GLenum internalformat, GLsizei width, GLsizei height) {
    log_gl_alloc((uintptr_t)__builtin_return_address(0), "glRenderbufferStorage", (uint32_t)(width * height * 4),
                 "%dx%d fmt 0x%x", width, height, internalformat);
    glRenderbufferStorage(target, internalformat, width, height);
    fbprobe_rb_storage(width, height);
}

volatile unsigned gl_texsub_calls = 0;

void glTexSubImage2D_soloader(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                              GLsizei height, GLenum format, GLenum type, const void *pixels) {
    gl_texsub_calls++;
    int i = nscaled && target == GL_TEXTURE_2D ? scaled_find(bound_texture_2d()) : -1;
    int bpp = scale_pixel_bytes(format, type);
    if (i >= 0 && pixels && bpp) {
        // A part of a scaled texture: scale the rectangle and its pixels
        int lw, lh, ow = scaled[i].sw >> level, oh = scaled[i].sh >> level;
        scaled_level_size(i, level, &lw, &lh);
        if (ow < 1) ow = 1;
        if (oh < 1) oh = 1;
        int x0 = xoffset * lw / ow, y0 = yoffset * lh / oh;
        int x1 = (xoffset + width) * lw / ow, y1 = (yoffset + height) * lh / oh;
        if (x1 <= x0 || y1 <= y0)
            return;
        void *part = resample(pixels, width, height, x1 - x0, y1 - y0, type, bpp);
        if (part) {
            glTexSubImage2D(target, level, x0, y0, x1 - x0, y1 - y0, format, type, part);
            free(part);
        }
        return;
    }
    glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
}

// The game calls glFinish (RNA::DevicePlatC::BlockUntilGpuIdle) once a frame:
// on vitaGL that ends the current scene and waits for the GPU to finish
// everything, 10-17 ms per frame in a race. vitaGL doesn't need it: memory
// the GPU may still read is freed only once the GPU is done with it, buffers
// are copied on write, and reading GPU results (glReadPixels) waits by
// itself. So it does nothing, and the CPU and GPU keep working in parallel.
void glFinish_soloader(void) {
}
