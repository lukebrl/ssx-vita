/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "ssx/perf.h"
#include "ssx/overlay.h"
#include "utils/init.h"
#include "utils/glutil.h"
#include "utils/logger.h"

#include <psp2/kernel/threadmgr.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

#include "jni/jni_ssx.h"
#include "reimpl/controls.h"
#include "ssx/lifecycle.h"
#include "ssx/memreport.h"
#include "ssx/watchdog.h"

// The Android build budgets ~450 MB of heaps (assets/common/config/memory.cfg).
// The Vita has ~365 MB for the app (ATTRIBUTE2=12). vitaGL takes whatever is
// left after this heap and the ~26 MB of game code for its RAM pool; textures
// mostly live in the separate 128 MB of CDRAM.
//
// 296 MB leaves vitaGL ~11 MB of main RAM instead of ~27. Its main RAM pool
// mostly held vertex buffers the game rewrites every frame, which then spill
// into CDRAM (~54 MB free in races); the game's heaps need the RAM more.
int _newlib_heap_size_user = 296 * 1024 * 1024;

#ifdef USE_SCELIBC_IO
int sceLibcHeapSize = 4 * 1024 * 1024;
#endif

so_module so_mod;

int main() {
    perf_thread_register(0, "main"); // the game's main and render thread
    soloader_init_all();

    // vitaGL must be up before the game touches GL; its own eglInitialize
    // call (through Java EGL10) then becomes a no-op.
    ssx_log_free_memory("before vitaGL init");
    gl_init();
    overlay_init();
    ssx_log_free_memory("before game init");

#ifdef DEBUG_SOLOADER
    ssx_memreport_start(); // only logs
#endif
    watchdog_start(); // crashes on purpose on a freeze, for a core dump
    ssx_create();
    ssx_resume();

    while (1) {
        controls_poll();
        ssx_draw_frame();
        video_poll();
    }

    sceKernelExitDeleteThread(0);
}

void controls_handler_key(int32_t keycode, ControlsAction action) {
    ssx_key(keycode, action == CONTROLS_ACTION_DOWN);
}

void controls_handler_touch(int32_t id, float x, float y, ControlsAction action) {
    // TODO: the game also supports touch (TouchScreenAndroid); not wired yet
}

void controls_handler_analog(ControlsStickId which, float x, float y, ControlsAction action) {
    static float lx, ly, rx, ry;

    // Vita stick Y grows downwards; the game expects up = positive
    if (which == CONTROLS_STICK_LEFT) {
        lx = x;
        ly = -y;
    } else {
        rx = x;
        ry = -y;
    }
    ssx_sticks(lx, ly, rx, ry);
}
