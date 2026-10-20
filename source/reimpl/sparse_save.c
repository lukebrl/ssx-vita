/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  sparse_save.c
 * @brief Compact storage for the 25 MB ghost save.
 *
 * "ssxvghosts <date>%Personal Best Ghosts" is a fixed 25 MB container that
 * is almost all zeros (two non-zero 4 KB pages on a fresh profile). On each
 * save the game copies the whole file into a new one 2 KB at a time, which
 * kept the saving icon up for 8 s at startup.
 *
 * The file is kept on the memory card in a sparse format instead: a header,
 * the indices of the non-zero pages and those pages' data. While the game
 * has it open, the file lives in RAM as a page table (zero pages are not
 * allocated), so reading or writing zeros costs no I/O. The file is written
 * back on fsync and on close, and stat/fstat report the size the game wrote.
 *
 * Files still in the plain format are read and written as before. A copy
 * the game makes of one is written in the sparse format, so the old format
 * disappears after the next save.
 *
 * On-disk layout (little-endian):
 *     char     magic[8]   "SSXSPRS1"
 *     uint64_t size       file size as the game sees it
 *     uint32_t count      number of stored pages
 *     uint32_t index[count], ascending
 *     uint8_t  pages[count][SPARSE_PAGE]
 */

#include "reimpl/sparse_save.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <psp2/kernel/threadmgr.h>

#include "utils/logger.h"

#define SPARSE_MAX_FD 64
#define SPARSE_PAGE 4096
#define SPARSE_MAGIC "SSXSPRS1"
#define SPARSE_HEADER 20

typedef struct {
    uint8_t **pages;  // NULL entries are all zeros
    uint32_t npages;  // capacity of pages[]
    uint64_t size;
    uint64_t pos;
    int writable;
    int dirty;
    uint32_t ops; // reads, writes and seeks, for the debug log
} sparse_file_t;

// Debug builds log the first LOG_OPS reads, writes and seeks of each open
#define LOG_OPS 24
#define LOG_OP(f, fd, fmt, ...)                                                                                        \
    do {                                                                                                               \
        if ((f)->ops++ < LOG_OPS)                                                                                      \
            l_debug("[SAVE] fd %d: " fmt, fd, __VA_ARGS__);                                                            \
    } while (0)

static sparse_file_t *sparse_files[SPARSE_MAX_FD];

int sparse_path(const char *path) {
    return path && strstr(path, "/ssxv/ssxvghosts ") != NULL;
}

static int read_all(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r <= 0)
            return -1;
        p += r;
        n -= r;
    }
    return 0;
}

static int write_all(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf;
    while (n) {
        ssize_t r = write(fd, p, n);
        if (r <= 0)
            return -1;
        p += r;
        n -= r;
    }
    return 0;
}

// Reads the header at the start of fd; 0 and the size if it is a sparse file
static int read_header(int fd, uint64_t *size, uint32_t *count) {
    uint8_t h[SPARSE_HEADER];
    if (lseek(fd, 0, SEEK_SET) != 0 || read_all(fd, h, sizeof(h)) < 0 ||
        memcmp(h, SPARSE_MAGIC, 8) != 0)
        return -1;
    memcpy(size, h + 8, 8);
    memcpy(count, h + 16, 4);
    return 0;
}

static int ensure_pages(sparse_file_t *f, uint64_t size) {
    uint32_t need = (uint32_t)((size + SPARSE_PAGE - 1) / SPARSE_PAGE);
    if (need <= f->npages)
        return 0;
    uint32_t cap = f->npages ? f->npages : 64;
    while (cap < need)
        cap *= 2;
    uint8_t **p = realloc(f->pages, cap * sizeof(*p));
    if (!p)
        return -1;
    memset(p + f->npages, 0, (cap - f->npages) * sizeof(*p));
    f->pages = p;
    f->npages = cap;
    return 0;
}

static void free_file(sparse_file_t *f) {
    for (uint32_t i = 0; i < f->npages; i++)
        free(f->pages[i]);
    free(f->pages);
    free(f);
}

static int load(sparse_file_t *f, int fd, uint64_t size, uint32_t count) {
    if (ensure_pages(f, size) < 0)
        return -1;
    f->size = size;
    if (!count)
        return 0;
    uint32_t *index = malloc(count * sizeof(uint32_t));
    if (!index || read_all(fd, index, count * sizeof(uint32_t)) < 0) {
        free(index);
        return -1;
    }
    int ret = 0;
    for (uint32_t i = 0; i < count && ret == 0; i++) {
        uint32_t page = index[i];
        if (page >= f->npages || !(f->pages[page] = malloc(SPARSE_PAGE)) ||
            read_all(fd, f->pages[page], SPARSE_PAGE) < 0)
            ret = -1;
    }
    free(index);
    return ret;
}

static int is_zero(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (p[i])
            return 0;
    return 1;
}

// The file is written through a static buffer. Building it in one malloc
// (~400 KB with five ghosts) failed when memory was short in a race: the new
// record's ghost was never written, and the game, which had saved it,
// reported the file as corrupted when it read that slot back.
#define STORE_BUF (64 * 1024)
static uint8_t store_buf[STORE_BUF];
static int store_busy; // store_buf in use (stores are rare: a spin lock is enough)

typedef struct {
    int fd;
    size_t len; // bytes in store_buf
    int failed;
} writer_t;

static void flush(writer_t *w) {
    if (w->len && !w->failed && write_all(w->fd, store_buf, w->len) < 0)
        w->failed = 1;
    w->len = 0;
}

static void put(writer_t *w, const void *data, size_t n) {
    const uint8_t *p = data;
    while (n) {
        size_t k = STORE_BUF - w->len < n ? STORE_BUF - w->len : n;
        memcpy(store_buf + w->len, p, k);
        w->len += k;
        p += k;
        n -= k;
        if (w->len == STORE_BUF)
            flush(w);
    }
}

static int store(sparse_file_t *f, int fd) {
    uint32_t used = (uint32_t)((f->size + SPARSE_PAGE - 1) / SPARSE_PAGE);
    uint32_t count = 0;
    for (uint32_t i = 0; i < used; i++) {
        // Pages that were written back to zeros don't need storing
        if (f->pages[i] && is_zero(f->pages[i], SPARSE_PAGE)) {
            free(f->pages[i]);
            f->pages[i] = NULL;
        }
        if (f->pages[i])
            count++;
    }
    size_t len = SPARSE_HEADER + count * (sizeof(uint32_t) + SPARSE_PAGE);

    while (__atomic_exchange_n(&store_busy, 1, __ATOMIC_ACQUIRE))
        sceKernelDelayThread(1000);
    writer_t w = { fd, 0, lseek(fd, 0, SEEK_SET) != 0 };
    put(&w, SPARSE_MAGIC, 8);
    put(&w, &f->size, 8);
    put(&w, &count, 4);
    for (uint32_t i = 0; i < used; i++)
        if (f->pages[i])
            put(&w, &i, sizeof(i));
    for (uint32_t i = 0; i < used; i++)
        if (f->pages[i])
            put(&w, f->pages[i], SPARSE_PAGE);
    flush(&w);
    __atomic_store_n(&store_busy, 0, __ATOMIC_RELEASE);

    int ret = !w.failed && ftruncate(fd, len) == 0 ? 0 : -1;
    if (ret < 0)
        l_error("[SAVE] could not write the sparse ghost save (fd %d, %u pages, errno %d)", fd, count, errno);
    else
        l_info("[SAVE] ghost save written: %llu KB as %u KB (%u non-zero pages)", f->size / 1024,
               (unsigned)(len / 1024), count);
    return ret;
}

void sparse_opened(int fd, const char *path, int oflag) {
    if (fd < 0 || fd >= SPARSE_MAX_FD || !sparse_path(path))
        return;
    sparse_file_t *f = calloc(1, sizeof(*f));
    if (!f)
        return;
    f->writable = (oflag & O_ACCMODE) != O_RDONLY;

    struct stat st;
    uint64_t size;
    uint32_t count;
    if (fstat(fd, &st) == 0 && st.st_size == 0) {
        // New or truncated: sparse from the start, and stored even if
        // nothing gets written, so the empty file has a header
        f->dirty = f->writable;
    } else if (read_header(fd, &size, &count) == 0) {
        if (load(f, fd, size, count) < 0) {
            l_error("[SAVE] %s: sparse ghost save is damaged, reading it as a plain file", path);
            free_file(f);
            lseek(fd, 0, SEEK_SET);
            return;
        }
    } else {
        // Old plain file: left as it is
        free_file(f);
        lseek(fd, 0, SEEK_SET);
        return;
    }
    if (oflag & O_APPEND)
        f->pos = f->size;
    sparse_files[fd] = f;
    l_debug("[SAVE] %s opened as a sparse file (%llu KB)", path, f->size / 1024);
}

int sparse_is(int fd) {
    return fd >= 0 && fd < SPARSE_MAX_FD && sparse_files[fd];
}

ssize_t sparse_read(int fd, void *buf, size_t count) {
    sparse_file_t *f = sparse_files[fd];
    if (f->pos >= f->size) {
        LOG_OP(f, fd, "read %u at %llu: end of file", (unsigned)count, f->pos);
        return 0;
    }
    LOG_OP(f, fd, "read %u at %llu", (unsigned)count, f->pos);
    if (count > f->size - f->pos)
        count = f->size - f->pos;
    uint8_t *out = buf;
    size_t done = 0;
    while (done < count) {
        uint64_t pos = f->pos + done;
        uint32_t page = pos / SPARSE_PAGE, off = pos % SPARSE_PAGE;
        size_t n = SPARSE_PAGE - off;
        if (n > count - done)
            n = count - done;
        if (f->pages[page])
            memcpy(out + done, f->pages[page] + off, n);
        else
            memset(out + done, 0, n);
        done += n;
    }
    f->pos += done;
    return done;
}

ssize_t sparse_write(int fd, const void *buf, size_t count) {
    sparse_file_t *f = sparse_files[fd];
    LOG_OP(f, fd, "write %u at %llu", (unsigned)count, f->pos);
    if (!f->writable) {
        errno = EBADF;
        return -1;
    }
    if (ensure_pages(f, f->pos + count) < 0) {
        errno = ENOMEM;
        return -1;
    }
    const uint8_t *in = buf;
    size_t done = 0;
    while (done < count) {
        uint64_t pos = f->pos + done;
        uint32_t page = pos / SPARSE_PAGE, off = pos % SPARSE_PAGE;
        size_t n = SPARSE_PAGE - off;
        if (n > count - done)
            n = count - done;
        if (!f->pages[page] && !is_zero(in + done, n)) {
            if (!(f->pages[page] = calloc(1, SPARSE_PAGE))) {
                l_error("[SAVE] fd %d: no memory for a page of the ghost save", fd);
                errno = ENOMEM;
                return done ? (ssize_t)done : -1;
            }
        }
        if (f->pages[page])
            memcpy(f->pages[page] + off, in + done, n);
        done += n;
    }
    f->pos += done;
    if (f->pos > f->size)
        f->size = f->pos;
    f->dirty = 1;
    return done;
}

off_t sparse_lseek(int fd, off_t offset, int whence) {
    sparse_file_t *f = sparse_files[fd];
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)f->pos : (int64_t)f->size;
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
        errno = EINVAL;
        return -1;
    }
    int64_t pos = base + offset;
    if (pos < 0) {
        errno = EINVAL;
        return -1;
    }
    LOG_OP(f, fd, "seek to %lld (whence %d)", (long long)pos, whence);
    f->pos = pos;
    return pos;
}

int sparse_ftruncate(int fd, off_t length) {
    sparse_file_t *f = sparse_files[fd];
    if (!f->writable || length < 0) {
        errno = EINVAL;
        return -1;
    }
    if (ensure_pages(f, length) < 0) {
        errno = ENOMEM;
        return -1;
    }
    uint32_t keep = (uint32_t)((length + SPARSE_PAGE - 1) / SPARSE_PAGE);
    for (uint32_t i = keep; i < f->npages; i++) {
        free(f->pages[i]);
        f->pages[i] = NULL;
    }
    // Bytes past the new end of the last page read as zeros if the file grows again
    if (length % SPARSE_PAGE && f->pages[keep - 1])
        memset(f->pages[keep - 1] + length % SPARSE_PAGE, 0, SPARSE_PAGE - length % SPARSE_PAGE);
    f->size = length;
    f->dirty = 1;
    return 0;
}

int sparse_sync(int fd) {
    sparse_file_t *f = sparse_files[fd];
    if (!f->dirty)
        return 0;
    if (store(f, fd) < 0) {
        errno = EIO;
        return -1;
    }
    f->dirty = 0;
    return 0;
}

int sparse_close(int fd) {
    if (!sparse_is(fd))
        return 0;
    sparse_file_t *f = sparse_files[fd];
    int ret = sparse_sync(fd);
    l_debug("[SAVE] fd %d closed after %u reads, writes and seeks%s", fd, f->ops, ret < 0 ? "; writing it failed" : "");
    sparse_files[fd] = NULL;
    free_file(f);
    return ret;
}

uint64_t sparse_size(int fd) {
    return sparse_files[fd]->size;
}

int sparse_stat_size(const char *path, uint64_t *size) {
    if (!sparse_path(path))
        return -1;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    uint32_t count;
    int ret = read_header(fd, size, &count);
    close(fd);
    return ret;
}
