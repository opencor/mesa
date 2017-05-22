/*
 * Copyright © 2014 NVIDIA Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef LIBDRM_LIBDRM_H
#define LIBDRM_LIBDRM_H

#include "util/macros.h"


#include <sys/mman.h>

#if defined(ANDROID) && !defined(__LP64__)
#include <errno.h> /* for EINVAL */

extern void *__mmap2(void *, size_t, int, int, int, size_t);

static inline void *drm_mmap(void *addr, size_t length, int prot, int flags,
                             int fd, loff_t offset)
{
   /* offset must be aligned to 4096 (not necessarily the page size) */
   if (offset & 4095) {
      errno = EINVAL;
      return MAP_FAILED;
   }

   return __mmap2(addr, length, prot, flags, fd, (size_t) (offset >> 12));
}

#  define drm_munmap(addr, length) \
              munmap(addr, length)


#else

/* assume large file support exists */
#  define drm_mmap(addr, length, prot, flags, fd, offset) \
              mmap(addr, length, prot, flags, fd, offset)


static inline int drm_munmap(void *addr, size_t length)
{
   /* Copied from configure code generated by AC_SYS_LARGEFILE */
#define LARGE_OFF_T ((((off_t) 1 << 31) << 31) - 1 + \
                     (((off_t) 1 << 31) << 31))
   STATIC_ASSERT(LARGE_OFF_T % 2147483629 == 721 &&
                 LARGE_OFF_T % 2147483647 == 1);
#undef LARGE_OFF_T

   return munmap(addr, length);
}
#endif

#endif
