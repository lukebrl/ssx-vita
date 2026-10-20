/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  io.h
 * @brief Wrappers and implementations for some of the IO functions.
 */

#ifndef SOLOADER_IO_H
#define SOLOADER_IO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <sys/dirent.h>
#include <sys/syslimits.h>
#include <sys/fcntl.h>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#ifndef DT_DIR
#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12
#define DT_WHT 14
#endif

// Bionic's 32-bit ARM layouts, spelled with fixed-size types: newlib's nlink_t,
// uid_t and gid_t are 16-bit and its timespec is 64-bit, and the old packed
// definitions put st_size at offset 38 instead of 48 (SSX's AssetStream then
// got garbage file sizes and never read the file).

typedef struct timespec_bionic {
    int32_t tv_sec;
    int32_t tv_nsec;
} timespec_bionic;

typedef struct stat64_bionic {
    uint64_t st_dev;           // 0
    unsigned char __pad0[4];   // 8
    uint32_t __st_ino;         // 12
    uint32_t st_mode;          // 16
    uint32_t st_nlink;         // 20
    uint32_t st_uid;           // 24
    uint32_t st_gid;           // 28
    uint64_t st_rdev;          // 32
    unsigned char __pad3[4];   // 40
    int64_t st_size;           // 48
    uint32_t st_blksize;       // 56
    uint64_t st_blocks;        // 64
    timespec_bionic st_atim;   // 72
    timespec_bionic st_mtim;   // 80
    timespec_bionic st_ctim;   // 88
    uint64_t st_ino;           // 96
} stat64_bionic;               // 104 bytes

_Static_assert(sizeof(stat64_bionic) == 104, "bionic struct stat is 104 bytes");
_Static_assert(__builtin_offsetof(stat64_bionic, st_size) == 48, "bionic st_size is at 48");

// Same layout as bionic's struct dirent on 32-bit ARM
typedef struct dirent64_bionic {
    uint64_t d_ino;            // 0
    int64_t d_off;             // 8
    uint16_t d_reclen;         // 16
    unsigned char d_type;      // 18
    char d_name[256];          // 19
} dirent64_bionic;

_Static_assert(__builtin_offsetof(dirent64_bionic, d_name) == 19, "bionic d_name is at 19");

int open_soloader(const char * path, int oflag, ...);

FILE * fopen_soloader(const char * filename, const char * mode);

DIR *opendir_soloader(char *name);

struct utimbuf;
int mkdir_soloader(const char *path, mode_t mode);
int rmdir_soloader(const char *path);
int remove_soloader(const char *path);
int unlink_soloader(const char *path);
int rename_soloader(const char *from, const char *to);
int chmod_soloader(const char *path, mode_t mode);
int utime_soloader(const char *path, const struct utimbuf *times);

// SSX: per-descriptor I/O statistics
void fd_stats_opened(int fd);
void fd_stats_closing(int fd);
void fd_stats_fsync(int fd, uint64_t us);
void fd_stats_report_busy(void);
ssize_t read_soloader(int fd, void *buf, size_t count);
ssize_t write_soloader(int fd, const void *buf, size_t count);
off_t lseek_soloader(int fd, off_t offset, int whence);
int ftruncate_soloader(int fd, off_t length);
void fd_write_flush(int fd);
void fd_write_release(int fd);
void fd_read_opened(int fd, int readonly);
void fd_read_release(int fd);

int stat_soloader(const char * path, stat64_bionic * buf);

int fstat_soloader(int fd, stat64_bionic * buf);

struct dirent64_bionic * readdir_soloader(DIR *dir);

int readdir_r_soloader(DIR * dirp, dirent64_bionic * entry,
                       dirent64_bionic ** result);

int close_soloader(int fd);

int fclose_soloader(FILE *f);

int closedir_soloader(DIR *dir);

int fcntl_soloader(int fd, int cmd, ...);

int ioctl_soloader(int fd, int request, ... /* arg */);

int fsync_soloader(int fd);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_IO_H
