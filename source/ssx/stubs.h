/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SSX_STUBS_H
#define SSX_STUBS_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "reimpl/pthr.h"

#ifdef __cplusplus
extern "C" {
#endif

void *MercuryBase_GetInstance(void);
void *NimbleTracking_getComponent(void *ret);
int NimbleTracking_isEnabled(void *self);

int isnan_soloader(double x);
uint32_t inet_addr_soloader(const char *cp);
ssize_t readlink_soloader(const char *path, char *buf, size_t bufsiz);
int statfs_soloader(const char *path, void *buf);
ssize_t writev_soloader(int fd, const void *iov, int iovcnt);

int pthread_getattr_np_soloader(pthread_t thread, pthread_attr_t_bionic *attr);
int pthread_attr_getstack_soloader(const pthread_attr_t_bionic *attr, void **stackaddr, size_t *stacksize);
int pthread_attr_setstack_soloader(pthread_attr_t_bionic *attr, void *stackaddr, size_t stacksize);
int pthread_attr_setschedpolicy_soloader(pthread_attr_t_bionic *attr, int policy);

uintptr_t __gnu_Unwind_Find_exidx_soloader(uintptr_t pc, int *pcount);

extern long timezone_soloader;
extern char *tzname_soloader[2];

#ifdef __cplusplus
};
#endif

#endif // SSX_STUBS_H
