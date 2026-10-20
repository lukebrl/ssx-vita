/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  jni_video.c
 * @brief com.ea.VideoPlayer.PlayerAndroid on sceAvPlayer.
 *
 * On Android the game's movies play in a MediaPlayer on a SurfaceView put on
 * top of the GL view (setZOrderOnTop), full screen; the game keeps drawing
 * underneath and is told by OnCompletionNativeImpl when a movie ends. Here
 * the movie is decoded by sceAvPlayer and drawn over the game's frame just
 * before it is presented (movie_draw, from gl_swap).
 *
 * The APK's movies are MPEG-4 ASP at 1080p, which the Vita can't decode: the
 * data tool re-encodes them in place to H.264 at 960x544 with 48 kHz AAC
 * (datatool/ssxdata/movies.py). A movie that isn't H.264 (data built by an
 * older tool) or that can't start (no memory) is reported as finished
 * right away, so the game skips it as before.
 *
 * The game makes one PlayerAndroid at startup (PlayerAndroid.Startup) and
 * uses it for every movie: init() starts a movie, destroy() ends it, and the
 * object stays. (Freeing it in destroy() dropped every movie after the boot
 * logo.)
 *
 * The game calls these methods from its own threads; sceAvPlayer is opened,
 * read and closed only on the GL thread, from movie_draw, which carries out
 * what was asked (play, pause, stop). Its frame buffers come from physically
 * contiguous memory that vitaGL leaves free (utils/glutil.c, MOVIE_PHYCONT: 18 MB used),
 * so the decoder never takes from the game's memory pools.
 */

#include "jni/jni_ssx.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/audioout.h>
#include <psp2/avplayer.h>
#include <psp2/gxm.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>
#include <so_util/so_util.h>
#include <vitaGL.h>

#include "ssx/assets.h"
#include "ssx/memreport.h"
#include "utils/logger.h"

extern so_module so_mod;

#define PLAYER_MAGIC 0x504C4159 // "PLAY"
#define MAX_PLAYERS 8
#define FRAME_BUFFERS 5         // decoded frames in flight: at least 4 with vitaGL's triple buffering
#define PHYCONT_ALIGN (1024 * 1024)
#define START_TIMEOUT_MS 5000   // no first frame by then: the movie is skipped
#define AUDIO_GRAIN 1024        // samples per sceAvPlayer audio frame
#define APPBUNDLE "appbundle:/"
#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

enum { WANT_NONE, WANT_PLAY, WANT_PAUSE, WANT_STOP };

typedef struct {
    uint32_t magic;
    jint instance_id;
    char path[256];         // the movie on the Vita ("" if there is none)
    volatile int want;      // what the game asked for last (WANT_*)
    volatile int finished;  // ended, or skipped
    int completion_pending; // OnCompletionNativeImpl still to call
    uint32_t generation;    // counts init(): a movie still open from an older one is closed
} Player;

static jint next_instance_id = 0;
static Player *players[MAX_PLAYERS];
static SceKernelLwMutexWork lock;
static int lock_ready;

// The open movie, GL thread only (with the lock held)
static struct {
    Player *owner;
    uint32_t generation; // the owner's generation when it was opened
    SceAvPlayerHandle handle;
    int paused;
    int have_frame;
    uint32_t opened_ms;
    GLuint tex[FRAME_BUFFERS];
    SceGxmTexture *gxm[FRAME_BUFFERS];
    int frame;
    GLuint program, vao, vbo; // the full-screen quad, drawn like the game draws
    GLint tex_uniform;
    uint32_t cpu_bytes, gpu_bytes;
} movie;

static void make_quad(void);

static SceUID audio_thread = -1;
static int audio_port = -1;
static volatile int audio_run;

static void lock_init(void) {
    if (!lock_ready) {
        sceKernelCreateLwMutex(&lock, "ssx_movie", 0, 0, NULL);
        lock_ready = 1;
    }
}

static uint32_t now_ms(void) {
    return sceKernelGetProcessTimeLow() / 1000;
}

static Player *player_from(jobject obj) {
    Player *p = (Player *)obj;
    return (p && p->magic == PLAYER_MAGIC) ? p : NULL;
}

// The movie's file on the Vita, if it is one sceAvPlayer can play: the data
// tool's movies start with their index (moov) and have an H.264 track (avc1)
static int find_movie(const char *path, char *out, size_t size) {
    char buf[512], big[512];
    if (strncmp(path, APPBUNDLE, strlen(APPBUNDLE)) == 0) {
        ssx_asset_path(out, size, path + strlen(APPBUNDLE));
    } else {
        const char *p = ssx_redirect_big_file(ssx_path(path, buf, sizeof(buf)), big, sizeof(big));
        snprintf(out, size, "%s", p);
    }
    FILE *f = fopen(out, "rb");
    if (!f) {
        l_warn("[MOVIE] %s: no file at %s", path, out);
        return 0;
    }
    static uint8_t head[64 * 1024];
    size_t n = fread(head, 1, sizeof(head), f);
    fclose(f);
    for (size_t i = 0; i + 4 <= n; i++)
        if (memcmp(head + i, "avc1", 4) == 0)
            return 1;
    l_warn("[MOVIE] %s is not H.264 (build the data again with the data tool to get the movies)", out);
    return 0;
}

// sceAvPlayer memory: its own data from the newlib heap, frame buffers from
// physically contiguous memblocks mapped for the GPU
// The player's own data (3.8 MB for these movies) comes from a memory block
// made for each movie: the newlib heap is nearly all EA heaps, and a 1.25 MB
// allocation from it failed at the boot logo.
#define ARENA_SIZES { 6, 5, 4 } // MB, tried in turn
static SceUID arena_block = -1;
static SceClibMspace arena;
static uint8_t *arena_base;
static uint32_t arena_size;
static SceKernelLwMutexWork arena_lock; // the player allocates from its own threads

static void arena_open(void) {
    static int lock_made;
    if (!lock_made) {
        sceKernelCreateLwMutex(&arena_lock, "ssx_movie_arena", 0, 0, NULL);
        lock_made = 1;
    }
    static const int sizes[] = ARENA_SIZES;
    for (int i = 0; i < (int)(sizeof(sizes) / sizeof(sizes[0])) && arena_block < 0; i++) {
        arena_size = sizes[i] * 1024 * 1024;
        arena_block = sceKernelAllocMemBlock("ssx_movie_player", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, arena_size, NULL);
    }
    if (arena_block < 0) {
        l_warn("[MOVIE] no memory block for the player (0x%08X): using the heap", arena_block);
        return;
    }
    sceKernelGetMemBlockBase(arena_block, (void **)&arena_base);
    arena = sceClibMspaceCreate(arena_base, arena_size);
}

static void arena_close(void) {
    if (arena_block < 0)
        return;
    sceClibMspaceDestroy(arena);
    sceKernelFreeMemBlock(arena_block);
    arena_block = -1;
    arena = NULL;
}

static void *cpu_alloc(void *arg, uint32_t align, uint32_t size) {
    void *p = NULL;
    if (arena) {
        sceKernelLockLwMutex(&arena_lock, 1, NULL);
        p = sceClibMspaceMemalign(arena, align < 8 ? 8 : align, size);
        sceKernelUnlockLwMutex(&arena_lock, 1);
    }
    if (!p)
        p = memalign(align < 8 ? 8 : align, size);
    if (p)
        movie.cpu_bytes += size;
    else
        l_error("[MOVIE] no memory for the player (%u bytes)", size);
    return p;
}

static void cpu_free(void *arg, void *p) {
    if (arena && (uint8_t *)p >= arena_base && (uint8_t *)p < arena_base + arena_size) {
        sceKernelLockLwMutex(&arena_lock, 1, NULL);
        sceClibMspaceFree(arena, p);
        sceKernelUnlockLwMutex(&arena_lock, 1);
    } else {
        free(p);
    }
}

static void *gpu_alloc(void *arg, uint32_t align, uint32_t size) {
    size = ALIGN(size, PHYCONT_ALIGN);
    SceUID block = sceKernelAllocMemBlock("ssx_movie_frames", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, size, NULL);
    if (block < 0) {
        l_error("[MOVIE] no memory for video frames (%u KB, 0x%08X)", size / 1024, block);
        return NULL;
    }
    void *base;
    sceKernelGetMemBlockBase(block, &base);
    sceGxmMapMemory(base, size, SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE);
    movie.gpu_bytes += size;
    return base;
}

static void gpu_free(void *arg, void *p) {
    glFinish(); // the GPU may still read the last frames (this runs on the GL thread)
    sceGxmUnmapMemory(p);
    sceKernelFreeMemBlock(sceKernelFindMemBlockByAddr(p, 0));
}

static int audio_main(SceSize args, void *argp) {
    uint32_t rate = 48000, channels = 2;
    while (audio_run) {
        SceAvPlayerFrameInfo frame;
        if (!sceAvPlayerIsActive(movie.handle) || !sceAvPlayerGetAudioData(movie.handle, &frame)) {
            sceKernelDelayThread(1000);
            continue;
        }
        if (frame.details.audio.sampleRate != rate || frame.details.audio.channelCount != channels) {
            rate = frame.details.audio.sampleRate;
            channels = frame.details.audio.channelCount;
            sceAudioOutSetConfig(audio_port, AUDIO_GRAIN, rate,
                                 channels == 1 ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO);
        }
        sceAudioOutOutput(audio_port, frame.pData);
    }
    return 0;
}

static void finish(Player *p, const char *why) {
    if (!p->finished)
        l_info("[MOVIE] movie %d %s", p->instance_id, why);
    p->finished = 1;
    p->completion_pending = 1;
}

static void movie_close(void) {
    if (!movie.owner)
        return;
    if (audio_thread >= 0) {
        audio_run = 0;
        SceUInt timeout = 500 * 1000;
        sceKernelWaitThreadEnd(audio_thread, NULL, &timeout);
        sceKernelDeleteThread(audio_thread);
        audio_thread = -1;
    }
    sceAvPlayerStop(movie.handle);
    sceAvPlayerClose(movie.handle);
    arena_close();
    l_info("[MOVIE] movie %d closed (player memory %u KB, frames %u KB)", movie.owner->instance_id,
           movie.cpu_bytes / 1024, movie.gpu_bytes / 1024);
    movie.owner = NULL;
    movie.have_frame = 0;
}

static int movie_open(Player *p) {
    static int module_loaded;
    if (!module_loaded) {
        int r = sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);
        if (r < 0) {
            l_error("[MOVIE] could not load the AvPlayer module (0x%08X)", r);
            return 0;
        }
        module_loaded = 1;
    }
    if (!movie.tex[0]) {
        // GL textures whose data pointer is set to each decoded frame
        glGenTextures(FRAME_BUFFERS, movie.tex);
        for (int i = 0; i < FRAME_BUFFERS; i++) {
            glBindTexture(GL_TEXTURE_2D, movie.tex[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            movie.gxm[i] = vglGetGxmTexture(GL_TEXTURE_2D);
            vglFree(vglGetTexDataPointer(GL_TEXTURE_2D));
        }
        make_quad();
    }
    if (audio_port < 0) {
        audio_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, AUDIO_GRAIN, 48000, SCE_AUDIO_OUT_MODE_STEREO);
        if (audio_port < 0)
            l_error("[MOVIE] no audio port (0x%08X): movies play without sound", audio_port);
    }

    SceAvPlayerInitData init;
    memset(&init, 0, sizeof(init));
    init.memoryReplacement.allocate = cpu_alloc;
    init.memoryReplacement.deallocate = cpu_free;
    init.memoryReplacement.allocateTexture = gpu_alloc;
    init.memoryReplacement.deallocateTexture = gpu_free;
    init.basePriority = 0xA0;
    init.numOutputVideoFrameBuffers = FRAME_BUFFERS;
    init.autoStart = 1;
    movie.cpu_bytes = movie.gpu_bytes = 0;
    arena_open();
    movie.handle = sceAvPlayerInit(&init);
    // The handle is a pointer (0x8xxxxxxx); errors are 0x806Axxxx
    if (!movie.handle || ((uint32_t)movie.handle & 0xFFFF0000) == 0x806A0000) {
        l_error("[MOVIE] sceAvPlayerInit failed (0x%08X)", movie.handle);
        arena_close();
        return 0;
    }
    int r = sceAvPlayerAddSource(movie.handle, p->path);
    if (r < 0) {
        l_error("[MOVIE] %s: sceAvPlayerAddSource failed (0x%08X)", p->path, r);
        sceAvPlayerClose(movie.handle);
        arena_close();
        return 0;
    }
    movie.owner = p;
    movie.generation = p->generation;
    movie.paused = 0;
    movie.have_frame = 0;
    movie.opened_ms = now_ms();
    if (audio_port >= 0) {
        audio_run = 1;
        audio_thread = sceKernelCreateThread("ssx_movie_audio", audio_main, 0x10000100 - 10, 0x4000, 0, 0, NULL);
        if (audio_thread >= 0)
            sceKernelStartThread(audio_thread, 0, NULL);
    }
    l_info("[MOVIE] movie %d playing %s", p->instance_id, p->path);
    ssx_log_free_memory("movie start");
    return 1;
}

// The quad that shows the frame: a shader, a vertex buffer and a vertex array
// object of its own, drawn through the same path as the game's draws
// (vitaGL's fixed-function path drew nothing over the game's frame)
static const char *const quad_vs =
    "attribute vec2 position;\n"
    "attribute vec2 texcoord;\n"
    "varying vec2 v_texcoord;\n"
    "void main() {\n"
    "    v_texcoord = texcoord;\n"
    "    gl_Position = vec4(position, 0.0, 1.0);\n"
    "}\n";
static const char *const quad_fs =
    "precision mediump float;\n"
    "varying vec2 v_texcoord;\n"
    "uniform sampler2D frame;\n"
    "void main() {\n"
    "    gl_FragColor = vec4(texture2D(frame, v_texcoord).rgb, 1.0);\n"
    "}\n";

static GLuint compile(GLenum type, const char *src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok)
        l_error("[MOVIE] the %s shader did not compile", type == GL_VERTEX_SHADER ? "vertex" : "fragment");
    return sh;
}

static void make_quad(void) {
    GLint program, array_buffer;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer);
    movie.program = glCreateProgram();
    glAttachShader(movie.program, compile(GL_VERTEX_SHADER, quad_vs));
    glAttachShader(movie.program, compile(GL_FRAGMENT_SHADER, quad_fs));
    glBindAttribLocation(movie.program, 0, "position");
    glBindAttribLocation(movie.program, 1, "texcoord");
    glLinkProgram(movie.program);
    GLint ok = 0;
    glGetProgramiv(movie.program, GL_LINK_STATUS, &ok);
    if (!ok)
        l_error("[MOVIE] the quad's shaders did not link");
    movie.tex_uniform = glGetUniformLocation(movie.program, "frame");

    // Triangle strip: top left, top right, bottom left, bottom right (x, y, u, v)
    static const float quad[16] = { -1, 1, 0, 0, 1, 1, 1, 0, -1, -1, 0, 1, 1, -1, 1, 1 };
    glGenVertexArrays(1, &movie.vao);
    glBindVertexArray(movie.vao);
    glGenBuffers(1, &movie.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, movie.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glBindVertexArray(0); // the game draws with the default one (it uses no vertex array objects)
    glBindBuffer(GL_ARRAY_BUFFER, array_buffer);
    glUseProgram(program);
}

// Draws the frame over everything the game drew; the GL state the game may
// rely on is put back afterwards
static void draw_frame(void) {
    GLint program, active, texture, framebuffer, array_buffer, viewport[4];
    GLboolean color_mask[4];
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
    static const GLenum caps[] = { GL_BLEND, GL_DEPTH_TEST, GL_CULL_FACE, GL_SCISSOR_TEST, GL_STENCIL_TEST };
    GLboolean enabled[sizeof(caps) / sizeof(caps[0])];
    for (int i = 0; i < (int)(sizeof(caps) / sizeof(caps[0])); i++) {
        enabled[i] = glIsEnabled(caps[i]);
        glDisable(caps[i]);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, SCREEN_W, SCREEN_H);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(movie.program);
    glUniform1i(movie.tex_uniform, 0);
    glBindTexture(GL_TEXTURE_2D, movie.tex[movie.frame]);
    glBindVertexArray(movie.vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    for (int i = 0; i < (int)(sizeof(caps) / sizeof(caps[0])); i++)
        if (enabled[i])
            glEnable(caps[i]);
    glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glBindBuffer(GL_ARRAY_BUFFER, array_buffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glBindTexture(GL_TEXTURE_2D, texture);
    glActiveTexture(active);
    glUseProgram(program);
}

void movie_draw(void) {
    lock_init();
    sceKernelLockLwMutex(&lock, 1, NULL);

    // Start a movie the game asked for
    if (!movie.owner)
        for (int i = 0; i < MAX_PLAYERS; i++) {
            Player *p = players[i];
            if (p && !p->finished && p->want == WANT_PLAY) {
                if (!p->path[0] || !movie_open(p))
                    finish(p, "skipped");
                break;
            }
        }

    if (movie.owner) {
        Player *p = movie.owner;
        if (p->want == WANT_STOP || p->generation != movie.generation) {
            movie_close();
        } else {
            if (p->want == WANT_PAUSE && !movie.paused) {
                sceAvPlayerPause(movie.handle);
                movie.paused = 1;
            } else if (p->want == WANT_PLAY && movie.paused) {
                sceAvPlayerResume(movie.handle);
                movie.paused = 0;
            }
            if (sceAvPlayerIsActive(movie.handle)) {
                SceAvPlayerFrameInfo frame;
                if (sceAvPlayerGetVideoData(movie.handle, &frame)) {
                    movie.frame = (movie.frame + 1) % FRAME_BUFFERS;
                    SceGxmTexture *t = movie.gxm[movie.frame];
                    sceGxmTextureInitLinear(t, frame.pData, SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1,
                                            frame.details.video.width, frame.details.video.height, 0);
                    sceGxmTextureSetMinFilter(t, SCE_GXM_TEXTURE_FILTER_LINEAR);
                    sceGxmTextureSetMagFilter(t, SCE_GXM_TEXTURE_FILTER_LINEAR);
                    if (!movie.have_frame)
                        l_info("[MOVIE] movie %d: first frame %ux%u after %u ms", p->instance_id,
                               frame.details.video.width, frame.details.video.height, now_ms() - movie.opened_ms);
                    movie.have_frame = 1;
                }
            } else if (movie.have_frame && !movie.paused) {
                movie_close();
                finish(p, "ended");
            }
            if (movie.owner && !movie.have_frame && now_ms() - movie.opened_ms > START_TIMEOUT_MS) {
                movie_close();
                finish(p, "skipped: no frame decoded");
            }
            if (movie.owner && movie.have_frame)
                draw_frame();
        }
    }

    sceKernelUnlockLwMutex(&lock, 1);
}

// new PlayerAndroid()
jobject PlayerAndroid_new(jmethodID id, va_list args) {
    lock_init();
    Player *p = calloc(1, sizeof(Player));
    p->magic = PLAYER_MAGIC;
    p->instance_id = -1;
    sceKernelLockLwMutex(&lock, 1, NULL);
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (!players[i]) {
            players[i] = p;
            break;
        }
    sceKernelUnlockLwMutex(&lock, 1);
    return p;
}

// void init(String path, int x, int y, int width, int height, int uiMode)
void PlayerAndroid_init(jmethodID id, va_list args) {
    Player *p = player_from(jni_current_object());
    jstring jpath = va_arg(args, jstring);
    jint x = va_arg(args, jint), y = va_arg(args, jint), w = va_arg(args, jint), h = va_arg(args, jint);
    jint ui_mode = va_arg(args, jint);
    if (!p)
        return;

    const char *path = jni->GetStringUTFChars(&jni, jpath, NULL);
    char file[sizeof(p->path)];
    if (!find_movie(path, file, sizeof(file)))
        file[0] = 0;
    l_info("[MOVIE] init(%s, %d,%d %dx%d, ui mode %d): %s", path, x, y, w, h, ui_mode, file[0] ? file : "skipped");
    jni->ReleaseStringUTFChars(&jni, jpath, (char *)path);

    // A new movie on the same object; Java does mInstanceID = gInstanceCount++
    sceKernelLockLwMutex(&lock, 1, NULL);
    p->want = WANT_NONE;
    p->finished = 0;
    p->completion_pending = 0;
    p->generation++;
    memcpy(p->path, file, sizeof(p->path));
    p->instance_id = next_instance_id++;
    sceKernelUnlockLwMutex(&lock, 1);
    setIntFieldValueById((jfieldID)FIELD_PLAYER_INSTANCE_ID, p->instance_id);
}

// void play(int position)
void PlayerAndroid_play(jmethodID id, va_list args) {
    Player *p = player_from(jni_current_object());
    jint position = va_arg(args, jint);
    if (!p)
        return;
    l_info("[MOVIE] play(movie %d, at %d ms)", p->instance_id, position);
    p->want = WANT_PLAY;
}

// void stop()
void PlayerAndroid_stop(jmethodID id, va_list args) {
    Player *p = player_from(jni_current_object());
    if (p && p->want != WANT_STOP) {
        l_info("[MOVIE] stop(movie %d)", p->instance_id);
        p->want = WANT_STOP;
        p->finished = 1;
    }
}

// int pause(): the position in ms
jint PlayerAndroid_pause(jmethodID id, va_list args) {
    Player *p = player_from(jni_current_object());
    if (!p)
        return -1;
    p->want = WANT_PAUSE;
    uint64_t t = 0;
    sceKernelLockLwMutex(&lock, 1, NULL);
    if (movie.owner == p)
        t = sceAvPlayerCurrentTime(movie.handle);
    sceKernelUnlockLwMutex(&lock, 1);
    return (jint)t;
}

// boolean isPlaying()
jboolean PlayerAndroid_isPlaying(jmethodID id, va_list args) {
    Player *p = player_from(jni_current_object());
    return (p && p->want == WANT_PLAY && !p->finished) ? JNI_TRUE : JNI_FALSE;
}

// void setViewFrame(int x, int y, int width, int height)
void PlayerAndroid_setViewFrame(jmethodID id, va_list args) {
    jint x = va_arg(args, jint), y = va_arg(args, jint), w = va_arg(args, jint), h = va_arg(args, jint);
    l_info("[MOVIE] setViewFrame(%d,%d %dx%d): drawn full screen", x, y, w, h);
}

// void destroy()
void PlayerAndroid_destroy(jmethodID id, va_list args) {
    Player *p = player_from(jni_current_object());
    if (!p)
        return;
    l_info("[MOVIE] destroy(movie %d)", p->instance_id);
    sceKernelLockLwMutex(&lock, 1, NULL);
    p->want = WANT_STOP; // the movie closes on the GL thread; the object stays for the next one
    p->finished = 1;
    p->completion_pending = 0;
    sceKernelUnlockLwMutex(&lock, 1);
}

// Called once per frame from the main loop. Completion is reported from
// here, as Java reports it from the UI thread, not from the GL thread.
void video_poll(void) {
    static void (*OnCompletionNativeImpl)(JNIEnv *, jobject, jint) = NULL;
    if (!OnCompletionNativeImpl)
        OnCompletionNativeImpl = (void *)so_symbol(&so_mod, "Java_com_ea_VideoPlayer_PlayerAndroid_OnCompletionNativeImpl");
    lock_init();
    for (int i = 0; i < MAX_PLAYERS; i++) {
        sceKernelLockLwMutex(&lock, 1, NULL);
        Player *p = players[i];
        int complete = p && p->completion_pending;
        if (complete)
            p->completion_pending = 0;
        sceKernelUnlockLwMutex(&lock, 1);
        if (complete) {
            l_info("[MOVIE] movie %d completed", p->instance_id);
            OnCompletionNativeImpl(&jni, (jobject)p, p->instance_id);
        }
    }
}
