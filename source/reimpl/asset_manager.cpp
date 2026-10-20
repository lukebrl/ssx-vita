#include "reimpl/asset_manager.h"
#include "utils/logger.h"
#include "ssx/assets.h"
#include "ssx/perf.h"

#include <pthread.h>
#include <malloc.h>
#include <cstring>
#include <cstdio>
#include <string>
#include <fcntl.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

/*
 * SSX port: assets are read with sceIo directly. Through stdio, 64 KB at a
 * time as the game asks, they came at 3-7 MB/s, and UI screens (dozens of
 * small files each) froze the game for up to a second.
 *   - Files up to SMALL_MAX are read whole at open, in one read, and kept
 *     in a cache of recently used ones (CACHE_MAX bytes): images are opened
 *     three times in a row (header twice, then the data), screens reopen the
 *     same components.
 *   - Bigger files are read through a READAHEAD buffer; reads at least that
 *     big go straight to the caller's memory.
 */
#define SMALL_MAX (256 * 1024)
#define CACHE_MAX (2 * 1024 * 1024)
#define READAHEAD (256 * 1024)

typedef struct assetManager {
    int dummy = 0; // TODO: mb we will need to store something here in future
    pthread_mutex_t mLock;
} assetManager;

typedef struct cacheEntry {
    char *path;
    uint8_t *data;
    size_t size;
    int refs;     // open assets using data
    uint64_t used; // for eviction (least recently used first)
    cacheEntry *next;
} cacheEntry;

typedef struct aAsset {
    char *filename;
    SceUID fd = -1;   // big files only
    size_t fileSize = 0;
    size_t pos = 0;    // read position
    cacheEntry *small = nullptr; // whole content, small files
    uint8_t *buf = nullptr;      // read-ahead, big files
    size_t buf_start = 0, buf_len = 0; // file offset and length of buf's data
    size_t fd_pos = 0;           // where fd is
    bool in_pak = false;         // read from the pack at pak_off, as a big file
    uint32_t pak_off = 0;
    bool opened = false;
    // SSX: timing, logged on close when slow
    uint64_t t_open_us = 0;
    uint64_t read_us = 0;
    uint32_t reads = 0;
} asset;

static AAssetManager * g_AAssetManager = nullptr;

AAssetManager * AAssetManager_create() {
    if (g_AAssetManager) return g_AAssetManager;

    assetManager am;

    pthread_mutex_init(&am.mLock, nullptr);

    g_AAssetManager = (AAssetManager *) malloc(sizeof(assetManager));
    memcpy(g_AAssetManager, &am, sizeof(assetManager));

    return g_AAssetManager;
}

// --- Small file cache ---------------------------------------------------------

static cacheEntry *cache_head;
static size_t cache_bytes;
static uint64_t cache_clock;
static SceKernelLwMutexWork cache_lock;
static volatile int cache_lock_state; // 0, 1 creating, 2 ready

static void cache_lock_take() {
    int expected = 0;
    if (__atomic_compare_exchange_n(&cache_lock_state, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        sceKernelCreateLwMutex(&cache_lock, "asset_cache", 0, 0, NULL);
        __atomic_store_n(&cache_lock_state, 2, __ATOMIC_RELEASE);
    }
    while (__atomic_load_n(&cache_lock_state, __ATOMIC_ACQUIRE) != 2)
        sceKernelDelayThread(100);
    sceKernelLockLwMutex(&cache_lock, 1, NULL);
}

static void cache_lock_give() {
    sceKernelUnlockLwMutex(&cache_lock, 1);
}

// Locked: the entry for path, with a reference taken
static cacheEntry *cache_find(const char *path) {
    for (cacheEntry *e = cache_head; e; e = e->next) {
        if (strcmp(e->path, path) == 0) {
            e->refs++;
            e->used = ++cache_clock;
            return e;
        }
    }
    return nullptr;
}

// Locked: drops unreferenced entries, oldest first, until extra bytes fit
static void cache_make_room(size_t extra) {
    while (cache_bytes + extra > CACHE_MAX) {
        cacheEntry **victim = nullptr;
        for (cacheEntry **pe = &cache_head; *pe; pe = &(*pe)->next)
            if ((*pe)->refs == 0 && (!victim || (*pe)->used < (*victim)->used))
                victim = pe;
        if (!victim)
            return; // everything in use: the cache grows past CACHE_MAX for now
        cacheEntry *e = *victim;
        *victim = e->next;
        cache_bytes -= e->size;
        free(e->data);
        free(e->path);
        delete e;
    }
}

static void cache_release(cacheEntry *e) {
    cache_lock_take();
    e->refs--;
    cache_make_room(0);
    cache_lock_give();
}

// Locked: adds data (malloc'd, now the cache's) for path, with a reference
// taken; if another thread added it meanwhile, frees data and returns that one
static cacheEntry *cache_insert(const char *path, uint8_t *data, size_t size) {
    cacheEntry *e = cache_find(path);
    if (e) {
        free(data);
        return e;
    }
    cache_make_room(size);
    e = new cacheEntry;
    e->path = strdup(path);
    e->data = data;
    e->size = size;
    e->refs = 1;
    e->used = ++cache_clock;
    e->next = cache_head;
    cache_head = e;
    cache_bytes += e->size;
    return e;
}

// --- Asset pack -----------------------------------------------------------------

/*
 * An open on the memory card costs 10-100 ms whatever the file's size, and a
 * UI screen opens a few dozen small files (its movies, their images, view
 * models): a third of the freeze when the race results come up. assets.pak
 * (written by the data tool, datatool/ssxdata/layout.py) holds every asset
 * up to 256 KB, read from one file kept open; the loose files stay for
 * everything else (AAsset_openFileDescriptor, files missing from the pack).
 */
#define PAK_PATH DATA_PATH "assets.pak"
#define ASSETS_PREFIX DATA_PATH "assets/"

typedef struct pakEntry {
    uint32_t h1, h2, off, size; // hashes of the lowercase path under assets/
} pakEntry;

static pakEntry *pak_index;
static uint32_t pak_count;
static SceUID pak_fd = -1;
static volatile int pak_state; // 0, 1 loading, 2 done

static void pak_load() {
    int expected = 0;
    if (__atomic_compare_exchange_n(&pak_state, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        SceUID fd = sceIoOpen(PAK_PATH, SCE_O_RDONLY, 0);
        uint8_t hdr[24];
        if (fd >= 0 && sceIoRead(fd, hdr, sizeof(hdr)) == (int)sizeof(hdr) && memcmp(hdr, "SSXPAK01", 8) == 0) {
            uint32_t count, index_off;
            memcpy(&count, hdr + 8, 4);
            memcpy(&index_off, hdr + 12, 4);
            pakEntry *index = (pakEntry *)malloc(count * sizeof(pakEntry));
            if (index && sceIoPread(fd, index, count * sizeof(pakEntry), index_off) == (int)(count * sizeof(pakEntry))) {
                pak_index = index;
                pak_count = count;
                pak_fd = fd;
                l_info("[PAK] %s: %u files", PAK_PATH, count);
            } else {
                free(index);
            }
        }
        if (pak_fd < 0) {
            l_info("[PAK] no usable %s: every asset is opened on its own", PAK_PATH);
            if (fd >= 0)
                sceIoClose(fd);
        }
        __atomic_store_n(&pak_state, 2, __ATOMIC_RELEASE);
    }
    while (__atomic_load_n(&pak_state, __ATOMIC_ACQUIRE) != 2)
        sceKernelDelayThread(100);
}

// The pack's entry for path (a full path as ssx_asset_path gives it), if any
static const pakEntry *pak_find(const char *path) {
    pak_load();
    size_t n = sizeof(ASSETS_PREFIX) - 1;
    if (!pak_index || strncasecmp(path, ASSETS_PREFIX, n) != 0)
        return nullptr;
    uint32_t h1 = 0x811C9DC5, h2 = 0x1505; // as in tools/build_asset_pack.py
    for (const char *p = path + n; *p; p++) {
        uint8_t c = *p >= 'A' && *p <= 'Z' ? *p - 'A' + 'a' : *p;
        h1 = (h1 ^ c) * 0x01000193;
        h2 = (h2 * 33) ^ c;
    }
    uint32_t lo = 0, hi = pak_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        const pakEntry *e = &pak_index[mid];
        if (e->h1 < h1 || (e->h1 == h1 && e->h2 < h2))
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < pak_count && pak_index[lo].h1 == h1 && pak_index[lo].h2 == h2)
        return &pak_index[lo];
    return nullptr;
}

// --- Assets ---------------------------------------------------------------------

AAsset* AAssetManager_open(AAssetManager* mgr, const char* filename, int mode) {
    uint64_t t_fopen = sceKernelGetProcessTimeWide(); // path lookup included
    char path[512];
    ssx_asset_path(path, sizeof(path), filename);

    auto * a = new aAsset;
    a->filename = strdup(path);
    a->t_open_us = sceKernelGetProcessTimeWide();

    cache_lock_take();
    a->small = cache_find(path);
    cache_lock_give();
    const pakEntry *pe = a->small ? nullptr : pak_find(path);
    if (a->small) {
        a->fileSize = a->small->size;
        a->opened = true;
    } else if (pe) {
        // From the pack: no open of its own
        uint8_t *data = (uint8_t *)malloc(pe->size ? pe->size : 1);
        uint64_t t0 = sceKernelGetProcessTimeWide();
        int got = data ? sceIoPread(pak_fd, data, pe->size, pe->off) : -1;
        uint64_t read_us = sceKernelGetProcessTimeWide() - t0;
        if (got == (int)pe->size) {
            a->read_us += read_us;
            a->reads++;
            perf_io_read(got, (uint32_t)read_us);
            a->fileSize = pe->size;
            a->opened = true;
            cache_lock_take();
            a->small = cache_insert(path, data, pe->size);
            cache_lock_give();
        } else if (!data) {
            // No memory for the whole file: read it from the pack as it is
            // asked for, through the read-ahead path. (The newlib heap is
            // nearly all EA heaps; these allocations failed during race loads
            // and the file was lost.)
            l_warn("[PAK] %s: no memory for %u bytes, read from the pack as needed", path, pe->size);
            a->in_pak = true;
            a->pak_off = pe->off;
            a->fileSize = pe->size;
            a->opened = true;
        } else {
            l_error("[PAK] %s: read %d of %u bytes at %u", path, got, pe->size, pe->off);
            free(data);
        }
    }
    if (!a->opened) {
        a->fd = sceIoOpen(path, SCE_O_RDONLY, 0);
        if (a->fd >= 0) {
            SceOff size = sceIoLseek(a->fd, 0, SCE_SEEK_END);
            sceIoLseek(a->fd, 0, SCE_SEEK_SET);
            a->fileSize = size > 0 ? (size_t)size : 0;
            a->opened = true;
            if (a->fileSize <= SMALL_MAX) {
                // Whole file now, in one read, shared through the cache
                uint8_t *data = (uint8_t *)malloc(a->fileSize ? a->fileSize : 1);
                uint64_t t0 = sceKernelGetProcessTimeWide();
                int got = data ? sceIoRead(a->fd, data, a->fileSize) : -1;
                uint64_t read_us = sceKernelGetProcessTimeWide() - t0;
                if (data && got == (int)a->fileSize) {
                    a->read_us += read_us;
                    a->reads++;
                    perf_io_read(got, (uint32_t)read_us);
                    sceIoClose(a->fd);
                    a->fd = -1;
                    cache_lock_take();
                    a->small = cache_insert(path, data, a->fileSize); // another thread may have read it meanwhile
                    cache_lock_give();
                } else {
                    // Short read: fall back to reading it as a big file
                    free(data);
                    sceIoLseek(a->fd, 0, SCE_SEEK_SET);
                }
            }
        }
    }

    if (!a->opened) {
        free(a->filename);
        delete a;
        a = nullptr;
    }
    uint32_t open_us = (uint32_t)(sceKernelGetProcessTimeWide() - t_fopen);
    perf_io_open(open_us); // lookup, open, size
    if (open_us > 10000 && a)
        l_info("[SLOWOPEN] %s: %u ms (%u ms of it reading %u KB; lookup %u ms)", path, open_us / 1000,
               (unsigned)(a->read_us / 1000), (unsigned)(a->fileSize / 1024),
               (unsigned)((a->t_open_us - t_fopen) / 1000));

    l_debug("AAssetManager_open<%p>(%p, %s, %i): %p", __builtin_return_address(0), mgr, path, mode, a);
    return (AAsset *) a;
}

void AAsset_close(AAsset* asset) {
    l_debug("AAsset_close<%p>(%p)", __builtin_return_address(0), asset);

    if (asset) {
        auto * a = (aAsset *) asset;
        uint64_t total_us = sceKernelGetProcessTimeWide() - a->t_open_us;
        if (total_us > 100000)
            l_info("[SLOW] asset %s: %u KB in %u reads, %llu ms reading, %llu ms open",
                   a->filename, (unsigned)(a->pos / 1024), (unsigned)a->reads,
                   a->read_us / 1000, total_us / 1000);
        if (a->small)
            cache_release(a->small);
        if (a->fd >= 0)
            sceIoClose(a->fd);
        free(a->buf);
        free(a->filename);
        delete a;
    }
}

// Reads count bytes at a->fd's offset `at` into dst; returns bytes read
static int fd_read_at(aAsset *a, size_t at, void *dst, size_t count) {
    if (a->in_pak) {
        if (at >= a->fileSize)
            return 0;
        if (count > a->fileSize - at)
            count = a->fileSize - at;
        uint64_t t0 = sceKernelGetProcessTimeWide();
        int got = sceIoPread(pak_fd, dst, count, a->pak_off + at);
        uint64_t read_us = sceKernelGetProcessTimeWide() - t0;
        a->read_us += read_us;
        a->reads++;
        perf_io_read(got > 0 ? got : 0, (uint32_t)read_us);
        return got;
    }
    if (a->fd_pos != at) {
        sceIoLseek(a->fd, at, SCE_SEEK_SET);
        a->fd_pos = at;
    }
    uint64_t t0 = sceKernelGetProcessTimeWide();
    int got = sceIoRead(a->fd, dst, count);
    uint64_t read_us = sceKernelGetProcessTimeWide() - t0;
    a->read_us += read_us;
    a->reads++;
    perf_io_read(got > 0 ? got : 0, (uint32_t)read_us);
    if (got > 0)
        a->fd_pos += got;
    return got;
}

int AAsset_read(AAsset* asset, void* buf, size_t count) {
    if (!asset) {
        return -1;
    }

    auto * a = (aAsset *) asset;

    if (!a->opened) {
        return -1;
    }
    if (a->pos >= a->fileSize || !count)
        return 0;
    if (count > a->fileSize - a->pos)
        count = a->fileSize - a->pos;

    if (a->small) {
        memcpy(buf, a->small->data + a->pos, count);
        a->pos += count;
        return (int)count;
    }

    uint8_t *out = (uint8_t *)buf;
    size_t done = 0;
    while (done < count) {
        size_t at = a->pos;
        // From the read-ahead buffer
        if (a->buf && at >= a->buf_start && at < a->buf_start + a->buf_len) {
            size_t n = a->buf_start + a->buf_len - at;
            if (n > count - done)
                n = count - done;
            memcpy(out + done, a->buf + (at - a->buf_start), n);
            done += n;
            a->pos += n;
            continue;
        }
        size_t left = count - done;
        if (left >= READAHEAD) {
            // Big read: straight into the caller's memory
            int got = fd_read_at(a, at, out + done, left);
            if (got <= 0)
                break;
            done += got;
            a->pos += got;
            continue;
        }
        if (!a->buf) {
            a->buf = (uint8_t *)malloc(READAHEAD);
            if (!a->buf) {
                int got = fd_read_at(a, at, out + done, left);
                if (got <= 0)
                    break;
                done += got;
                a->pos += got;
                continue;
            }
        }
        int got = fd_read_at(a, at, a->buf, READAHEAD);
        if (got <= 0)
            break;
        a->buf_start = at;
        a->buf_len = got;
    }
    return done ? (int)done : -1;
}

off_t AAsset_seek(AAsset* asset, off_t offset, int whence) {
    if (!asset) {
        return (off_t) -1;
    }

    auto * a = (aAsset *) asset;

    if (!a->opened) {
        return -1;
    }

    // Android returns the new position (like lseek)
    off_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (off_t)a->pos : (off_t)a->fileSize;
    off_t to = base + offset;
    if (to < 0)
        return -1;
    a->pos = (size_t)to;
    perf_io_seek(0);
    return to;
}

off_t AAsset_getRemainingLength(AAsset* asset) {
    l_debug("AAsset_getRemainingLength");
    if (!asset) {
        return (off_t) -1;
    }

    auto * a = (aAsset *) asset;

    if (!a->opened) {
        return -1;
    }

    return a->pos < a->fileSize ? (off_t)(a->fileSize - a->pos) : 0;
}

off_t AAsset_getLength(AAsset* asset) {
    l_debug("AAsset_getLength");
    if (!asset) {
        return (off_t) -1;
    }

    auto * a = (aAsset *) asset;

    return (off_t)a->fileSize;
}

AAssetDir* AAssetManager_openDir(AAssetManager* mgr, const char* dirName) {
    l_error("UNIMPLEMENTED: AAssetManager_openDir: %s", dirName);
    return (AAssetDir *)strdup("dummy");
}

const char* AAssetDir_getNextFileName(AAssetDir* assetDir) {
    l_error("UNIMPLEMENTED: AAssetDir_getNextFileName: %p", assetDir);
    return "";
}

void AAssetDir_close(AAssetDir* assetDir) {
    l_error("UNIMPLEMENTED: AAssetDir_close");
    free(assetDir);
}

int AAsset_openFileDescriptor(AAsset* asset, off_t* outStart, off_t* outLength) {
    if (!asset) {
        l_warn("AAsset_openFileDescriptor(%p, %p, %p): asset is null", asset, outStart, outLength);
        return -1;
    }
    auto * a = (aAsset *) asset;
    if (outStart) *outStart = 0;
    if (outLength) *outLength = a->fileSize;
    // The caller reads the file itself from now on
    if (a->fd >= 0) {
        sceIoClose(a->fd);
        a->fd = -1;
    }
    if (a->small) {
        cache_release(a->small);
        a->small = nullptr;
    }
    a->opened = false;
    int ret = open(a->filename, O_RDONLY);
    l_debug("AAsset_openFileDescriptor(%p/\"%s\", %p, %p): ret %i", asset, a->filename, outStart, outLength, ret);
    return ret;
}
