/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  jni_egl.c
 * @brief javax.microedition.khronos.egl.EGL10 and com.ea.blast.EglAndroidDelegate.
 *
 * SSX does not link libEGL for setup: it drives EGL through the Java EGL10
 * interface, from native code. vitaGL needs no surface or context, so all of
 * this only has to answer plausibly, and eglSwapBuffers presents the frame.
 */

#include "ssx/glprof.h"
#include <psp2/kernel/threadmgr.h>
#include "jni/jni_ssx.h"

#include "utils/glutil.h"
#include "utils/logger.h"
#include "ssx/watchdog.h"

char jobj_surface_holder[16];
char jobj_egl[16];

volatile unsigned ssx_frames_presented = 0;

static char egl_display[16];
static char egl_config[16];
static char egl_context[16];
static char egl_surface[16];
static char egl_no_context[16];
static char egl_no_display[16];
static char egl_no_surface[16];

#define EGL_SUCCESS            0x3000
#define EGL_BUFFER_SIZE        0x3020
#define EGL_ALPHA_SIZE         0x3021
#define EGL_BLUE_SIZE          0x3022
#define EGL_GREEN_SIZE         0x3023
#define EGL_RED_SIZE           0x3024
#define EGL_DEPTH_SIZE         0x3025
#define EGL_STENCIL_SIZE       0x3026
#define EGL_CONFIG_CAVEAT      0x3027
#define EGL_CONFIG_ID          0x3028
#define EGL_SAMPLES            0x3031
#define EGL_SAMPLE_BUFFERS     0x3032
#define EGL_SURFACE_TYPE       0x3033
#define EGL_NONE               0x3038
#define EGL_RENDERABLE_TYPE    0x3040
#define EGL_VENDOR             0x3053
#define EGL_VERSION            0x3054
#define EGL_EXTENSIONS         0x3055
#define EGL_HEIGHT             0x3056
#define EGL_WIDTH              0x3057
#define EGL_CLIENT_APIS        0x308D
#define EGL_CONTEXT_CLIENT_VERSION 0x3098

#define EGL_WINDOW_BIT         0x0004
#define EGL_OPENGL_ES2_BIT     0x0004

/*
 * com.ea.blast.EglAndroidDelegate
 */

jobject EglDelegate_GetEgl(jmethodID id, va_list args) { return jobj_egl; }
jobject EglDelegate_GetSurface(jmethodID id, va_list args) { return jobj_surface_holder; }
jobject EglDelegate_GetEglNoContext(jmethodID id, va_list args) { return egl_no_context; }
jobject EglDelegate_GetEglNoDisplay(jmethodID id, va_list args) { return egl_no_display; }
jobject EglDelegate_GetEglNoSurface(jmethodID id, va_list args) { return egl_no_surface; }

/*
 * javax.microedition.khronos.egl.EGL10
 */

// EGLDisplay eglGetDisplay(Object native_display)
jobject EGL10_eglGetDisplay(jmethodID id, va_list args) {
    return egl_display;
}

// boolean eglInitialize(EGLDisplay display, int[] major_minor)
jboolean EGL10_eglInitialize(jmethodID id, va_list args) {
    va_arg(args, jobject);
    jint *version = jint_array(va_arg(args, jobject));

    l_info("[EGL10] eglInitialize");
    gl_init();

    if (version) {
        version[0] = 1;
        version[1] = 4;
    }
    return JNI_TRUE;
}

// boolean eglTerminate(EGLDisplay display)
jboolean EGL10_eglTerminate(jmethodID id, va_list args) {
    return JNI_TRUE;
}

// String eglQueryString(EGLDisplay display, int name)
jobject EGL10_eglQueryString(jmethodID id, va_list args) {
    va_arg(args, jobject);
    jint name = va_arg(args, jint);
    switch (name) {
        case EGL_VENDOR: return jstr("Sony");
        case EGL_VERSION: return jstr("1.4 vitaGL");
        case EGL_CLIENT_APIS: return jstr("OpenGL_ES");
        case EGL_EXTENSIONS:
        default: return jstr("");
    }
}

static jboolean return_one_config(jobject configs_arr, jint config_size, jobject num_config_arr) {
    JavaDynArray *configs = (JavaDynArray *)configs_arr;
    jint *num_config = jint_array(num_config_arr);

    if (configs && config_size > 0 && configs->len > 0)
        ((jobject *)configs->array)[0] = egl_config;
    if (num_config)
        num_config[0] = 1;
    return JNI_TRUE;
}

// boolean eglGetConfigs(EGLDisplay display, EGLConfig[] configs, int config_size, int[] num_config)
jboolean EGL10_eglGetConfigs(jmethodID id, va_list args) {
    va_arg(args, jobject);
    jobject configs = va_arg(args, jobject);
    jint config_size = va_arg(args, jint);
    jobject num_config = va_arg(args, jobject);
    return return_one_config(configs, config_size, num_config);
}

// boolean eglChooseConfig(EGLDisplay display, int[] attrib_list, EGLConfig[] configs, int config_size, int[] num_config)
jboolean EGL10_eglChooseConfig(jmethodID id, va_list args) {
    va_arg(args, jobject);
    va_arg(args, jobject);
    jobject configs = va_arg(args, jobject);
    jint config_size = va_arg(args, jint);
    jobject num_config = va_arg(args, jobject);
    return return_one_config(configs, config_size, num_config);
}

// boolean eglGetConfigAttrib(EGLDisplay display, EGLConfig config, int attribute, int[] value)
jboolean EGL10_eglGetConfigAttrib(jmethodID id, va_list args) {
    va_arg(args, jobject);
    va_arg(args, jobject);
    jint attribute = va_arg(args, jint);
    jint *value = jint_array(va_arg(args, jobject));
    if (!value)
        return JNI_FALSE;

    switch (attribute) {
        case EGL_BUFFER_SIZE: value[0] = 32; break;
        case EGL_RED_SIZE:
        case EGL_GREEN_SIZE:
        case EGL_BLUE_SIZE:
        case EGL_ALPHA_SIZE: value[0] = 8; break;
        case EGL_DEPTH_SIZE: value[0] = 24; break;
        case EGL_STENCIL_SIZE: value[0] = 8; break;
        case EGL_CONFIG_CAVEAT: value[0] = EGL_NONE; break;
        case EGL_CONFIG_ID: value[0] = 1; break;
        case EGL_SURFACE_TYPE: value[0] = EGL_WINDOW_BIT; break;
        case EGL_RENDERABLE_TYPE: value[0] = EGL_OPENGL_ES2_BIT; break;
        case EGL_SAMPLES:
        case EGL_SAMPLE_BUFFERS:
        default: value[0] = 0; break;
    }
    return JNI_TRUE;
}

// EGLSurface eglCreateWindowSurface(EGLDisplay display, EGLConfig config, Object native_window, int[] attrib_list)
jobject EGL10_eglCreateWindowSurface(jmethodID id, va_list args) {
    l_info("[EGL10] eglCreateWindowSurface");
    return egl_surface;
}

jobject EGL10_eglCreatePbufferSurface(jmethodID id, va_list args) {
    l_warn("[EGL10] eglCreatePbufferSurface: not supported");
    return egl_surface;
}

jobject EGL10_eglCreatePixmapSurface(jmethodID id, va_list args) {
    l_warn("[EGL10] eglCreatePixmapSurface: not supported");
    return egl_surface;
}

// EGLContext eglCreateContext(EGLDisplay display, EGLConfig config, EGLContext share_context, int[] attrib_list)
jobject EGL10_eglCreateContext(jmethodID id, va_list args) {
    l_info("[EGL10] eglCreateContext");
    return egl_context;
}

// boolean eglMakeCurrent(EGLDisplay display, EGLSurface draw, EGLSurface read, EGLContext context)
jboolean EGL10_eglMakeCurrent(jmethodID id, va_list args) {
    va_arg(args, jobject); // display
    va_arg(args, jobject); // draw
    va_arg(args, jobject); // read
    jobject context = va_arg(args, jobject);
    // GL calls are only let through from the thread the context is current on
    // (see glprof.c): with no context, Android ignores them
    // (the game makes it current with each GPU lock, many times a second and
    // from its loading threads too: each thread is logged the first time)
    if (context) {
        static SceUID seen[16];
        static int nseen;
        SceUID thread = sceKernelGetThreadId();
        int k = 0;
        while (k < nseen && seen[k] != thread)
            k++;
        if (k == nseen && nseen < 16) {
            seen[nseen++] = thread;
            l_info("[EGL10] eglMakeCurrent: GL on thread 0x%X (first time)", (unsigned)thread);
        }
        glprof_set_render_thread();
    }
    return JNI_TRUE;
}

// boolean eglSwapBuffers(EGLDisplay display, EGLSurface surface)
jboolean EGL10_eglSwapBuffers(jmethodID id, va_list args) {
    gl_swap();
    ssx_frames_presented++;
    watchdog_swap_done();
    return JNI_TRUE;
}

// boolean eglQuerySurface(EGLDisplay display, EGLSurface surface, int attribute, int[] value)
jboolean EGL10_eglQuerySurface(jmethodID id, va_list args) {
    va_arg(args, jobject);
    va_arg(args, jobject);
    jint attribute = va_arg(args, jint);
    jint *value = jint_array(va_arg(args, jobject));
    if (!value)
        return JNI_FALSE;

    switch (attribute) {
        case EGL_WIDTH: value[0] = SCREEN_W; break;
        case EGL_HEIGHT: value[0] = SCREEN_H; break;
        case EGL_CONFIG_ID: value[0] = 1; break;
        default: value[0] = 0; break;
    }
    return JNI_TRUE;
}

// boolean eglQueryContext(EGLDisplay display, EGLContext context, int attribute, int[] value)
jboolean EGL10_eglQueryContext(jmethodID id, va_list args) {
    va_arg(args, jobject);
    va_arg(args, jobject);
    jint attribute = va_arg(args, jint);
    jint *value = jint_array(va_arg(args, jobject));
    if (!value)
        return JNI_FALSE;

    value[0] = (attribute == EGL_CONTEXT_CLIENT_VERSION) ? 2 : 1;
    return JNI_TRUE;
}

jint EGL10_eglGetError(jmethodID id, va_list args) {
    return EGL_SUCCESS;
}

// eglDestroyContext, eglDestroySurface, eglWaitGL, eglWaitNative, eglCopyBuffers
jboolean EGL10_true(jmethodID id, va_list args) {
    return JNI_TRUE;
}
