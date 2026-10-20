/*
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/logger.h"

#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>

#define COLOR_RED    "\x1B[38;5;196m"
#define COLOR_PINK   "\x1B[38;5;212m"
#define COLOR_ORANGE "\x1B[38;5;202m"
#define COLOR_BLUE   "\x1B[38;5;32m"
#define COLOR_GREEN  "\x1B[32m"
#define COLOR_CYAN   "\x1B[36m"

#define COLOR_END    "\033[0m"

static SceKernelLwMutexWork _log_mutex;
static atomic_bool _log_mutex_ready = ATOMIC_VAR_INIT(false);

#ifdef LOG_TO_FILE
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>

/*
 * Mirrors every log line into DATA_PATH "log.txt" (ANSI colors stripped), so
 * logs can be pulled off the device over FTP without a debug console.
 *
 * Lines collect in a buffer that a background thread writes out every 200 ms;
 * errors and fatal lines are written out right away, so the lines before a
 * crash reach the file. Writing every line to the memory card under the log
 * lock made every thread that logs wait for it: thousands of lines while a
 * level loads. Warnings were written right away too: vitaGL's stats (ten
 * warning lines every 30 frames, from inside the swap) then held up the
 * render thread for 80-140 ms whenever the card was slow, the stutters seen
 * in races. Two buffers: the writer swaps them under the lock and writes
 * the full one without it, so a thread that logs never waits for the card.
 */
#define LOG_FILE_BUF_SIZE (256 * 1024)

static SceKernelLwMutexWork _log_file_mutex; // the buffers
static SceKernelLwMutexWork _log_io_mutex;   // the file, writes in order
static atomic_int _log_file_state = ATOMIC_VAR_INIT(0); // 0: none, 1: init, 2: ready
static SceUID _log_file_fd = -1;
static char _log_file_line[2048];
static char _log_file_buf[2][LOG_FILE_BUF_SIZE];
static int _log_file_cur, _log_file_pending_len;

// With _log_io_mutex held and _log_file_mutex held on entry: takes the
// pending lines, lets go of _log_file_mutex, writes them, takes it back
static void _log_file_flush_locked(void) {
    if (_log_file_pending_len <= 0 || _log_file_fd < 0) {
        _log_file_pending_len = 0;
        return;
    }
    char *buf = _log_file_buf[_log_file_cur];
    int len = _log_file_pending_len;
    _log_file_cur ^= 1;
    _log_file_pending_len = 0;
    sceKernelUnlockLwMutex(&_log_file_mutex, 1);
    sceIoWrite(_log_file_fd, buf, len);
    sceKernelLockLwMutex(&_log_file_mutex, 1, NULL);
}

void _log_file_flush(void) {
    if (atomic_load(&_log_file_state) != 2)
        return;
    sceKernelLockLwMutex(&_log_io_mutex, 1, NULL);
    sceKernelLockLwMutex(&_log_file_mutex, 1, NULL);
    _log_file_flush_locked();
    sceKernelUnlockLwMutex(&_log_file_mutex, 1);
    sceKernelUnlockLwMutex(&_log_io_mutex, 1);
}

static int _log_file_thread(SceSize args, void *argp) {
    while (1) {
        sceKernelDelayThread(200 * 1000);
        _log_file_flush();
    }
    return 0;
}

void _log_file_write(const char *s, int urgent) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&_log_file_state, &expected, 1)) {
        sceKernelCreateLwMutex(&_log_file_mutex, "log_file_lock", 0, 0, NULL);
        sceKernelCreateLwMutex(&_log_io_mutex, "log_io_lock", 0, 0, NULL);
        _log_file_fd = sceIoOpen(DATA_PATH "log.txt",
                                 SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        SceUID thid = sceKernelCreateThread("ssx_log_writer", _log_file_thread, 0x10000100, 0x2000, 0, 0, NULL);
        if (thid >= 0)
            sceKernelStartThread(thid, 0, NULL);
        atomic_store(&_log_file_state, 2);
    }
    while (atomic_load(&_log_file_state) != 2)
        sceKernelDelayThread(100);

    if (_log_file_fd < 0)
        return;

    // A full buffer or an urgent line: this thread writes, in order with the writer thread
    int io = urgent;
    if (io)
        sceKernelLockLwMutex(&_log_io_mutex, 1, NULL);
    sceKernelLockLwMutex(&_log_file_mutex, 1, NULL);
    // Seconds since launch, to see where time goes
    SceUInt64 us = sceKernelGetProcessTimeWide();
    int n = sceClibSnprintf(_log_file_line, sizeof(_log_file_line), "[%4u.%03u]",
                            (unsigned)(us / 1000000), (unsigned)(us / 1000 % 1000));
    for (const char *p = s; *p && n < (int)sizeof(_log_file_line) - 1; p++) {
        if (*p == '\x1B') {
            while (*p && *p != 'm') p++;
            if (!*p) break;
            continue;
        }
        _log_file_line[n++] = *p;
    }
    if (_log_file_pending_len + n > LOG_FILE_BUF_SIZE) {
        if (!io) {
            // Lock order: io first
            sceKernelUnlockLwMutex(&_log_file_mutex, 1);
            sceKernelLockLwMutex(&_log_io_mutex, 1, NULL);
            sceKernelLockLwMutex(&_log_file_mutex, 1, NULL);
            io = 1;
        }
        if (_log_file_pending_len + n > LOG_FILE_BUF_SIZE)
            _log_file_flush_locked();
    }
    memcpy(_log_file_buf[_log_file_cur] + _log_file_pending_len, _log_file_line, n);
    _log_file_pending_len += n;
    if (urgent)
        _log_file_flush_locked();
    sceKernelUnlockLwMutex(&_log_file_mutex, 1);
    if (io)
        sceKernelUnlockLwMutex(&_log_io_mutex, 1);
}
#else
void _log_file_write(const char *s, int urgent) { (void)s; (void)urgent; }
void _log_file_flush(void) {}
#endif

// Buffer A is used to adjust the format string.
static char buffer_a[2048];
// Buffer B is used to compile the final log using the updated format string.
static char buffer_b[2048];

void _log_print(int t, const char* fmt, ...) {
    if (!atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
        int ret = sceKernelCreateLwMutex(&_log_mutex, "log_lock", 0, 0, NULL);
        if (ret < 0) {
            sceClibPrintf("Error: failed to create log mutex: 0x%x\n", ret);
            return;
        }
        atomic_store_explicit(&_log_mutex_ready, true, memory_order_relaxed);
    }
    sceKernelLockLwMutex(&_log_mutex, 1, NULL);

    switch (t) {
        case LT_DEBUG:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s• debug%s    %s\n",
                            COLOR_PINK, COLOR_END, fmt); break;
        case LT_INFO:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %sℹ info%s     %s\n",
                            COLOR_BLUE, COLOR_END, fmt); break;
        case LT_WARN:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s⚠ warning%s  %s\n",
                            COLOR_ORANGE, COLOR_END, fmt); break;
        case LT_ERROR:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s⨯ error%s    %s\n",
                            COLOR_RED, COLOR_END, fmt); break;
        case LT_FATAL:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s! fatal%s    %s\n",
                            COLOR_RED, COLOR_END, fmt); break;
        case LT_SUCCESS:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s! success%s  %s\n",
                            COLOR_GREEN, COLOR_END, fmt); break;
        case LT_WAIT:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s… waiting%s  %s\n",
                            COLOR_CYAN, COLOR_END, fmt); break;
        default:
            if (atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
                sceKernelUnlockLwMutex(&_log_mutex, 1);
            }
            return;
    }

    va_list list;
    va_start(list, fmt);
    sceClibVsnprintf(buffer_b, sizeof(buffer_b), buffer_a, list);
    va_end(list);
    // The debug console only gets info and above: debug lines (every file
    // open and stat) are many, and each print is a system call
    if (t != LT_DEBUG)
        sceClibPrintf(buffer_b);
    _log_file_write(buffer_b, t == LT_ERROR || t == LT_FATAL);

    if (atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
        sceKernelUnlockLwMutex(&_log_mutex, 1);
    }
}

// vitaGL diagnostics (built with LOG_ERRORS, see lib/vitagl/source/utils/debug_utils.h)
void ssx_vgl_log(const char *fmt, ...) {
    char msg[1024];
    va_list list;
    va_start(list, fmt);
    sceClibVsnprintf(msg, sizeof(msg), fmt, list);
    va_end(list);

    size_t len = strlen(msg);
    while (len > 0 && (msg[len - 1] == '\n' || msg[len - 1] == '\r'))
        msg[--len] = '\0';
    _log_print(LT_WARN, "[vitaGL] %s", msg);
}
