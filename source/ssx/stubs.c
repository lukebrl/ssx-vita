/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  stubs.c
 * @brief Replacements for imports of libgame.so / libgnustl_shared.so that the
 *        boilerplate does not cover: EA Mercury (DLC downloader) and Nimble
 *        (telemetry) entry points, and a handful of Bionic libc functions.
 */

#include "ssx/stubs.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <psp2/kernel/threadmgr.h>
#include <so_util/so_util.h>

#include "reimpl/io.h"
#include "reimpl/pthr.h"
#include "utils/logger.h"

extern so_module so_mod;
extern so_module gnustl_mod;

/*
 * EA Mercury. libgame only reaches it through MERCURY::MercuryManager, which
 * patch.c turns into no-ops, so these only have to be safe if called anyway.
 */

static uint32_t mercury_base_dummy[64];

void *MercuryBase_GetInstance(void) {
    return mercury_base_dummy;
}

/*
 * EA Nimble tracking. `Tracking::getComponent()` returns a small handle by
 * value (hidden result pointer in r0); `isEnabled()` on that handle gates
 * every tracking call, so reporting "disabled" switches telemetry off.
 */

void *NimbleTracking_getComponent(void *ret) {
    if (ret)
        memset(ret, 0, sizeof(uint32_t));
    return ret;
}

int NimbleTracking_isEnabled(void *self) {
    (void)self;
    return 0;
}

/*
 * Bionic libc
 */

int isnan_soloader(double x) {
    return __builtin_isnan(x);
}

uint32_t inet_addr_soloader(const char *cp) {
    // Only used by EA::Sockets::PingHTTP; networking is not supported.
    (void)cp;
    return 0xFFFFFFFF; // INADDR_NONE
}

ssize_t readlink_soloader(const char *path, char *buf, size_t bufsiz) {
    // EA::StdC::GetCurrentProcessPath reads /proc/self/exe
    (void)path; (void)buf; (void)bufsiz;
    errno = ENOENT;
    return -1;
}

// Layout of Bionic's 32-bit struct statfs
typedef struct {
    uint32_t f_type;
    uint32_t f_bsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t f_files;
    uint64_t f_ffree;
    int32_t f_fsid[2];
    uint32_t f_namelen;
    uint32_t f_frsize;
    uint32_t f_flags;
    uint32_t f_spare[4];
} statfs_bionic;

int statfs_soloader(const char *path, void *out) {
    // Used by EA::IO::GetDriveFreeSpace; report 1 GB free.
    (void)path;
    statfs_bionic *buf = out;
    if (!buf) {
        errno = EFAULT;
        return -1;
    }
    memset(buf, 0, sizeof(*buf));
    buf->f_bsize = 4096;
    buf->f_frsize = 4096;
    buf->f_blocks = (1024ull * 1024 * 1024) / 4096;
    buf->f_bfree = buf->f_blocks;
    buf->f_bavail = buf->f_blocks;
    buf->f_namelen = 255;
    return 0;
}

// Bionic struct iovec has the same layout as the POSIX one
struct iovec_bionic {
    void *iov_base;
    size_t iov_len;
};

ssize_t writev_soloader(int fd, const void *iov_, int iovcnt) {
    const struct iovec_bionic *iov = iov_;
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        ssize_t n = write_soloader(fd, iov[i].iov_base, iov[i].iov_len); // keeps write-back buffers in order
        if (n < 0)
            return total ? total : -1;
        total += n;
        if ((size_t)n < iov[i].iov_len)
            break;
    }
    return total;
}

/*
 * pthread extras. pthr.c keeps a pointer to the real newlib attr in the Bionic
 * struct; the `stack_size` and `guard_size` fields are unused there, so
 * pthread_getattr_np stores the calling thread's stack bounds in them.
 */

int pthread_getattr_np_soloader(pthread_t thread, pthread_attr_t_bionic *attr) {
    (void)thread; // EAThread only asks about the current thread
    if (!attr)
        return EINVAL;

    SceKernelThreadInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) < 0)
        return EINVAL;

    attr->guard_size = (size_t)info.stack;
    attr->stack_size = (size_t)info.stackSize;
    return 0;
}

int pthread_attr_getstack_soloader(const pthread_attr_t_bionic *attr, void **stackaddr, size_t *stacksize) {
    if (!attr)
        return EINVAL;
    if (stackaddr) *stackaddr = (void *)attr->guard_size;
    if (stacksize) *stacksize = attr->stack_size;
    return 0;
}

int pthread_attr_setstack_soloader(pthread_attr_t_bionic *attr, void *stackaddr, size_t stacksize) {
    // newlib threads can't run on a caller-provided stack; keep only the size.
    (void)stackaddr;
    return pthread_attr_setstacksize_soloader(attr, stacksize);
}

int pthread_attr_setschedpolicy_soloader(pthread_attr_t_bionic *attr, int policy) {
    (void)attr; (void)policy;
    return 0;
}

/*
 * ARM EHABI: libgnustl_shared.so's unwinder asks the "dynamic linker" for the
 * exception index table covering a PC.
 */

uintptr_t __gnu_Unwind_Find_exidx_soloader(uintptr_t pc, int *pcount) {
    const so_module *mods[] = { &so_mod, &gnustl_mod };
    for (int i = 0; i < 2; i++) {
        const so_module *m = mods[i];
        if (pc >= m->text_base && pc < m->text_base + m->text_size) {
            if (pcount) *pcount = (int)(m->exidx_size / 8);
            return m->exidx_base;
        }
    }
    if (pcount) *pcount = 0;
    return 0;
}

/*
 * Data symbols
 */

long timezone_soloader = 0;
char *tzname_soloader[2] = { "UTC", "UTC" };
