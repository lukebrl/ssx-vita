/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/io.h"

#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <stdarg.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <utime.h>

#ifdef USE_SCELIBC_IO
#include <libc_bridge/libc_bridge.h>
#endif

#include "utils/logger.h"
#include "utils/utils.h"
#include "ssx/assets.h"
#include "reimpl/sparse_save.h"

// Includes the following inline utilities:
// int oflags_musl_to_newlib(int flags);
// dirent64_bionic * dirent_newlib_to_bionic(struct dirent* dirent_newlib);
// void stat_newlib_to_bionic(struct stat * src, stat64_bionic * dst);
#include "reimpl/bits/_struct_converters.c"

FILE * fopen_soloader(const char * filename, const char * mode) {
    if (strcmp(filename, "/proc/cpuinfo") == 0) {
        return fopen_soloader("app0:/cpuinfo", mode);
    } else if (strcmp(filename, "/proc/meminfo") == 0) {
        return fopen_soloader("app0:/meminfo", mode);
    }

    char vita_path[512], redirect[512];
    filename = ssx_path(filename, vita_path, sizeof(vita_path));
    if (mode[0] == 'r' && !strchr(mode, '+'))
        filename = ssx_redirect_big_file(filename, redirect, sizeof(redirect));

#ifdef USE_SCELIBC_IO
    FILE* ret = sceLibcBridge_fopen(filename, mode);
#else
    FILE* ret = fopen(filename, mode);
#endif

    if (ret)
        l_debug("fopen(%s, %s): %p", filename, mode, ret);
    else
        l_warn("fopen(%s, %s): %p", filename, mode, ret);

    return ret;
}

int open_soloader(const char * path, int oflag, ...) {
    if (strcmp(path, "/proc/cpuinfo") == 0) {
        return open_soloader("app0:/cpuinfo", oflag);
    } else if (strcmp(path, "/proc/meminfo") == 0) {
        return open_soloader("app0:/meminfo", oflag);
    } else if (strcmp(path, "/dev/urandom") == 0) {
        return open_soloader("app0:/urandom", oflag);
    }

    char vita_path[512], redirect[512];
    path = ssx_path(path, vita_path, sizeof(vita_path));
    if ((oflag & 3) == 0 /* O_RDONLY */ && !(oflag & BIONIC_O_CREAT))
        path = ssx_redirect_big_file(path, redirect, sizeof(redirect));

    mode_t mode = 0666;
    if (((oflag & BIONIC_O_CREAT) == BIONIC_O_CREAT) ||
        ((oflag & BIONIC_O_TMPFILE) == BIONIC_O_TMPFILE)) {
        va_list args;
        va_start(args, oflag);
        mode = (mode_t)(va_arg(args, int));
        va_end(args);
    }

    oflag = oflags_bionic_to_newlib(oflag);
    int ret = open(path, oflag, mode);
    if (ret >= 0) {
        fd_stats_opened(ret);
        fd_read_opened(ret, (oflag & O_ACCMODE) == O_RDONLY);
        sparse_opened(ret, path, oflag);
    }
    if (ret >= 0)
        l_debug("open(%s, %x): %i", path, oflag, ret);
    else
        l_warn("open(%s, %x): %i", path, oflag, ret);
    return ret;
}

int fstat_soloader(int fd, stat64_bionic * buf) {
    fd_write_flush(fd); // the size must include buffered writes
    struct stat st;
    int res = fstat(fd, &st);

    if (res == 0) {
        if (sparse_is(fd))
            st.st_size = sparse_size(fd);
        stat_newlib_to_bionic(&st, buf);
    }

    l_debug("fstat(%i): %i", fd, res);
    return res;
}

int stat_soloader(const char * path, stat64_bionic * buf) {
    if (strcmp(path, "/system/lib/libOpenSLES.so") == 0) {
        l_debug("stat(%s): returning 0 in case this is a check for OpenSLES support", path);
        return 0;
    }

    char vita_path[512], redirect[512];
    path = ssx_path(path, vita_path, sizeof(vita_path));
    path = ssx_redirect_big_file(path, redirect, sizeof(redirect));

    struct stat st;
    int res = stat(path, &st);

    if (res == 0) {
        uint64_t size;
        if (sparse_stat_size(path, &size) == 0)
            st.st_size = size;
        stat_newlib_to_bionic(&st, buf);
    }

    l_debug("stat(%s): %i", path, res);
    return res;
}

int fclose_soloader(FILE * f) {
#ifdef USE_SCELIBC_IO
    int ret = sceLibcBridge_fclose(f);
#else
    int ret = fclose(f);
#endif

    l_debug("fclose(%p): %i", f, ret);
    return ret;
}

int close_soloader(int fd) {
    sparse_close(fd);
    fd_write_release(fd);
    fd_read_release(fd);
    fd_stats_closing(fd);
    int ret = close(fd);
    l_debug("close(%i): %i", fd, ret);
    return ret;
}

DIR* opendir_soloader(char* _pathname) {
    char vita_path[512];
    const char *path = ssx_path(_pathname, vita_path, sizeof(vita_path));
    DIR* ret = opendir(path);
    l_debug("opendir(\"%s\"): %p", path, ret);
    return ret;
}

/*
 * SSX: other path-taking calls, so the Android paths the game is given
 * (see ssx/assets.c) work everywhere
 */

int mkdir_soloader(const char *path, mode_t mode) {
    char vita_path[512];
    path = ssx_path(path, vita_path, sizeof(vita_path));
    int ret = mkdir(path, mode);
    l_debug("mkdir(%s): %i", path, ret);
    return ret;
}

int rmdir_soloader(const char *path) {
    char vita_path[512];
    return rmdir(ssx_path(path, vita_path, sizeof(vita_path)));
}

int remove_soloader(const char *path) {
    char vita_path[512];
    return remove(ssx_path(path, vita_path, sizeof(vita_path)));
}

int unlink_soloader(const char *path) {
    char vita_path[512];
    return unlink(ssx_path(path, vita_path, sizeof(vita_path)));
}

int rename_soloader(const char *from, const char *to) {
    char vita_from[512], vita_to[512];
    return rename(ssx_path(from, vita_from, sizeof(vita_from)),
                  ssx_path(to, vita_to, sizeof(vita_to)));
}

int chmod_soloader(const char *path, mode_t mode) {
    char vita_path[512];
    return chmod(ssx_path(path, vita_path, sizeof(vita_path)), mode);
}

int utime_soloader(const char *path, const struct utimbuf *times) {
    char vita_path[512];
    return utime(ssx_path(path, vita_path, sizeof(vita_path)), times);
}

struct dirent64_bionic * readdir_soloader(DIR * dir) {
    static struct dirent64_bionic dirent_tmp;

    struct dirent* ret = readdir(dir);
    l_debug("readdir(%p): %p", dir, ret);

    if (ret) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(ret);
        memcpy(&dirent_tmp, entry_tmp, sizeof(dirent64_bionic));
        free(entry_tmp);
        return &dirent_tmp;
    }

    return NULL;
}

int readdir_r_soloader(DIR * dirp, dirent64_bionic * entry,
                       dirent64_bionic ** result) {
    struct dirent dirent_tmp;
    struct dirent * pdirent_tmp;

    int ret = readdir_r(dirp, &dirent_tmp, &pdirent_tmp);

    if (ret == 0) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(&dirent_tmp);
        memcpy(entry, entry_tmp, sizeof(dirent64_bionic));
        *result = (pdirent_tmp != NULL) ? entry : NULL;
        free(entry_tmp);
    }

    l_debug("readdir_r(%p, %p, %p): %i", dirp, entry, result, ret);
    return ret;
}

int closedir_soloader(DIR * dir) {
    int ret = closedir(dir);
    l_debug("closedir(%p): %i", dir, ret);
    return ret;
}

int fcntl_soloader(int fd, int cmd, ...) {
    l_warn("fcntl(%i, %i, ...): not implemented", fd, cmd);
    return 0;
}

int ioctl_soloader(int fd, int request, ...) {
    l_warn("ioctl(%i, %i, ...): not implemented", fd, request);
    return 0;
}

int fsync_soloader(int fd) {
    if (sparse_is(fd))
        return sparse_sync(fd);
    fd_write_flush(fd);
    uint64_t t0 = sceKernelGetProcessTimeWide();
    int ret = fsync(fd);
    if (fd >= 0 && fd < 64)
        fd_stats_fsync(fd, sceKernelGetProcessTimeWide() - t0);
    l_debug("fsync(%i): %i", fd, ret);
    return ret;
}

/*
 * SSX: per-descriptor I/O statistics (debug aid: saving a ~300 KB ghost file
 * takes 30-40 s). Logged on close when slow or chatty, and every few seconds
 * for descriptors that stay busy.
 */


#define FD_STATS_MAX 64

typedef struct {
    uint32_t reads, writes, seeks, fsyncs;
    uint64_t read_bytes, write_bytes;
    uint64_t read_us, write_us, seek_us, fsync_us;
    uint64_t opened_us;
} fd_stats_t;

static fd_stats_t fd_stats[FD_STATS_MAX];

static void fd_stats_log(int fd, const char *why) {
    fd_stats_t *s = &fd_stats[fd];
    l_info("[FDSTAT] fd %d %s: %u reads (%llu KB, %llu ms), %u writes (%llu KB, %llu ms), "
           "%u seeks (%llu ms), %u fsyncs (%llu ms), %llu ms since open",
           fd, why, s->reads, s->read_bytes / 1024, s->read_us / 1000,
           s->writes, s->write_bytes / 1024, s->write_us / 1000,
           s->seeks, s->seek_us / 1000, s->fsyncs, s->fsync_us / 1000,
           (sceKernelGetProcessTimeWide() - s->opened_us) / 1000);
}

void fd_stats_opened(int fd) {
    if (fd < 0 || fd >= FD_STATS_MAX)
        return;
    memset(&fd_stats[fd], 0, sizeof(fd_stats[fd]));
    fd_stats[fd].opened_us = sceKernelGetProcessTimeWide();
}

void fd_stats_closing(int fd) {
    if (fd < 0 || fd >= FD_STATS_MAX)
        return;
    fd_stats_t *s = &fd_stats[fd];
    uint64_t busy = s->read_us + s->write_us + s->seek_us + s->fsync_us;
    if (busy > 100000 || s->reads + s->writes + s->seeks > 500)
        fd_stats_log(fd, "closed");
    s->opened_us = 0; // stop "still open" reports
}

void fd_stats_report_busy(void) {
    for (int fd = 3; fd < FD_STATS_MAX; fd++) {
        fd_stats_t *s = &fd_stats[fd];
        if (s->opened_us && s->reads + s->writes + s->seeks > 500)
            fd_stats_log(fd, "still open");
    }
}

#define FD_TIMED(counter, bytes_field, us_field, call) ({ \
    uint64_t _t0 = sceKernelGetProcessTimeWide(); \
    __typeof__(call) _r = (call); \
    if (fd >= 0 && fd < FD_STATS_MAX) { \
        fd_stats[fd].counter++; \
        bytes_field; \
        fd_stats[fd].us_field += sceKernelGetProcessTimeWide() - _t0; \
    } \
    _r; \
})

/*
 * SSX: write-back buffering. The save system copies its 25 MB ghost file in
 * 2 KB writes, ~2.6 ms each on the memory card (30-40 s per save). Writes are
 * gathered per descriptor and written in 64 KB blocks; the buffer is flushed
 * before anything that could observe the file (read, seek, fstat, ftruncate,
 * fsync, close).
 */

#define WRITE_BUF_SIZE (64 * 1024)

typedef struct {
    uint8_t *data;
    uint32_t len;
} write_buf_t;

static write_buf_t write_bufs[FD_STATS_MAX];
static SceKernelLwMutexWork write_buf_lock;

__attribute__((constructor)) static void write_buf_lock_create(void) {
    sceKernelCreateLwMutex(&write_buf_lock, "write_buf_lock", 0, 0, NULL);
}

// Caller holds write_buf_lock
static int write_buf_flush_locked(int fd) {
    write_buf_t *b = &write_bufs[fd];
    uint32_t off = 0;
    while (off < b->len) {
        ssize_t n = write(fd, b->data + off, b->len - off);
        if (n <= 0) {
            l_error("write-back flush of fd %d failed: %d", fd, (int)n);
            b->len = 0;
            return -1;
        }
        off += n;
    }
    b->len = 0;
    return 0;
}

void fd_write_flush(int fd) {
    if (fd < 0 || fd >= FD_STATS_MAX || !write_bufs[fd].len)
        return;
    sceKernelLockLwMutex(&write_buf_lock, 1, NULL);
    write_buf_flush_locked(fd);
    sceKernelUnlockLwMutex(&write_buf_lock, 1);
}

void fd_write_release(int fd) {
    if (fd < 0 || fd >= FD_STATS_MAX)
        return;
    fd_write_flush(fd);
    sceKernelLockLwMutex(&write_buf_lock, 1, NULL);
    free(write_bufs[fd].data);
    write_bufs[fd].data = NULL;
    sceKernelUnlockLwMutex(&write_buf_lock, 1);
}

static ssize_t buffered_write(int fd, const void *buf, size_t count) {
    if (fd < 3 || fd >= FD_STATS_MAX)
        return write(fd, buf, count);

    sceKernelLockLwMutex(&write_buf_lock, 1, NULL);
    write_buf_t *b = &write_bufs[fd];
    ssize_t ret = count;
    if (b->len + count > WRITE_BUF_SIZE && write_buf_flush_locked(fd) < 0) {
        ret = -1;
    } else if (count >= WRITE_BUF_SIZE) {
        ret = write(fd, buf, count);
    } else {
        if (!b->data)
            b->data = malloc(WRITE_BUF_SIZE);
        memcpy(b->data + b->len, buf, count);
        b->len += count;
    }
    sceKernelUnlockLwMutex(&write_buf_lock, 1);
    return ret;
}

/*
 * SSX: read-ahead for read-only descriptors read sequentially in small
 * pieces. The save system copies its 25 MB ghost file 2 KB at a time, and
 * each sceIoRead costs ~0.4 ms however little it reads: 5 of the 8 s the
 * "saving" icon stays up at startup. After READ_SEQ_MIN small reads in a row
 * with no seek, reads are served from a READ_BUF_SIZE block. A seek outside
 * the block, or a read of a block or more, drops it. Descriptors read in big
 * pieces or at random (rwfilesys) never get a buffer.
 */

#define READ_BUF_SIZE (128 * 1024)
#define READ_SMALL (16 * 1024)
#define READ_SEQ_MIN 4

typedef struct {
    uint8_t *data;
    uint32_t len, pos;   // valid bytes in data, next byte to hand out
    uint8_t readonly, seq;
} read_buf_t;

static read_buf_t read_bufs[FD_STATS_MAX];

void fd_read_opened(int fd, int readonly) {
    if (fd < 0 || fd >= FD_STATS_MAX)
        return;
    read_buf_t *b = &read_bufs[fd];
    b->len = b->pos = 0;
    b->seq = 0;
    b->readonly = readonly;
}

void fd_read_release(int fd) {
    if (fd < 0 || fd >= FD_STATS_MAX)
        return;
    read_buf_t *b = &read_bufs[fd];
    free(b->data);
    b->data = NULL;
    b->len = b->pos = 0;
    b->seq = 0;
    b->readonly = 0;
}

static ssize_t buffered_read(int fd, void *buf, size_t count) {
    if (fd < 3 || fd >= FD_STATS_MAX || !read_bufs[fd].readonly)
        return read(fd, buf, count);
    read_buf_t *b = &read_bufs[fd];

    size_t done = 0;
    uint32_t avail = b->len - b->pos;
    if (avail) {
        size_t n = count < avail ? count : avail;
        memcpy(buf, b->data + b->pos, n);
        b->pos += n;
        done = n;
        if (done == count)
            return done;
    }

    // Buffer empty from here on
    if (count - done > READ_SMALL || b->seq < READ_SEQ_MIN) {
        if (count <= READ_SMALL && b->seq < READ_SEQ_MIN)
            b->seq++;
        else if (count > READ_SMALL)
            b->seq = 0;
        ssize_t r = read(fd, (uint8_t *)buf + done, count - done);
        if (r < 0)
            return done ? (ssize_t)done : r;
        return done + r;
    }

    if (!b->data && !(b->data = malloc(READ_BUF_SIZE))) {
        ssize_t r = read(fd, (uint8_t *)buf + done, count - done);
        return r < 0 ? (done ? (ssize_t)done : r) : (ssize_t)(done + r);
    }
    ssize_t r = read(fd, b->data, READ_BUF_SIZE);
    b->pos = b->len = 0;
    if (r <= 0)
        return done ? (ssize_t)done : r;
    b->len = r;
    size_t n = count - done < (size_t)r ? count - done : (size_t)r;
    memcpy((uint8_t *)buf + done, b->data, n);
    b->pos = n;
    return done + n;
}

// lseek on a descriptor with read-ahead: the kernel position is b->len - b->pos
// bytes past the position the game sees
static off_t buffered_lseek(int fd, off_t offset, int whence) {
    read_buf_t *b = (fd >= 0 && fd < FD_STATS_MAX) ? &read_bufs[fd] : NULL;
    if (!b || b->len == 0) {
        if (b)
            b->seq = 0;
        return lseek(fd, offset, whence);
    }
    uint32_t ahead = b->len - b->pos;
    if (whence == SEEK_CUR && offset == 0)
        return lseek(fd, 0, SEEK_CUR) - ahead; // tell
    off_t real = lseek(fd, 0, SEEK_CUR);
    off_t block_start = real - b->len;
    off_t target = whence == SEEK_SET ? offset
                 : whence == SEEK_CUR ? real - ahead + offset
                 : -1;
    if (target >= block_start && target <= real) {
        b->pos = target - block_start;
        return target;
    }
    b->len = b->pos = 0;
    b->seq = 0;
    if (whence == SEEK_CUR)
        return lseek(fd, target, SEEK_SET);
    return lseek(fd, offset, whence);
}

ssize_t read_soloader(int fd, void *buf, size_t count) {
    if (sparse_is(fd))
        return sparse_read(fd, buf, count);
    fd_write_flush(fd);
    return FD_TIMED(reads, if (_r > 0) fd_stats[fd].read_bytes += _r, read_us, buffered_read(fd, buf, count));
}

ssize_t write_soloader(int fd, const void *buf, size_t count) {
    if (sparse_is(fd))
        return sparse_write(fd, buf, count);
    return FD_TIMED(writes, if (_r > 0) fd_stats[fd].write_bytes += _r, write_us, buffered_write(fd, buf, count));
}

off_t lseek_soloader(int fd, off_t offset, int whence) {
    if (sparse_is(fd))
        return sparse_lseek(fd, offset, whence);
    fd_write_flush(fd);
    return FD_TIMED(seeks, (void)0, seek_us, buffered_lseek(fd, offset, whence));
}

int ftruncate_soloader(int fd, off_t length) {
    if (sparse_is(fd))
        return sparse_ftruncate(fd, length);
    fd_write_flush(fd);
    return ftruncate(fd, length);
}

void fd_stats_fsync(int fd, uint64_t us) {
    fd_stats[fd].fsyncs++;
    fd_stats[fd].fsync_us += us;
}
