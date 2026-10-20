/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SOLOADER_SPARSE_SAVE_H
#define SOLOADER_SPARSE_SAVE_H

#include <stdint.h>
#include <sys/types.h>

// Compact storage for the ghost save, see sparse_save.c. fd is a real
// descriptor; oflag uses newlib flags.

int sparse_path(const char *path);
void sparse_opened(int fd, const char *path, int oflag);
int sparse_is(int fd);
ssize_t sparse_read(int fd, void *buf, size_t count);
ssize_t sparse_write(int fd, const void *buf, size_t count);
off_t sparse_lseek(int fd, off_t offset, int whence);
int sparse_ftruncate(int fd, off_t length);
int sparse_sync(int fd);
int sparse_close(int fd);
uint64_t sparse_size(int fd);
// 0 and the size the game wrote if path is a sparse file
int sparse_stat_size(const char *path, uint64_t *size);

#endif // SOLOADER_SPARSE_SAVE_H
