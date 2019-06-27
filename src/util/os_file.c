/*
 * Copyright 2019 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "os_file.h"

#include <errno.h>
#include <stdlib.h>

#if defined(__linux__)

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>


static ssize_t
readN(int fd, char *buf, size_t len)
{
   int err = -ENODATA;
   size_t total = 0;
   do {
      ssize_t ret = read(fd, buf + total, len - total);

      if (ret < 0)
         ret = -errno;

      if (ret == -EINTR || ret == -EAGAIN)
         continue;

      if (ret <= 0) {
         err = ret;
         break;
      }

      total += ret;
   } while (total != len);

   return total ? total : err;
}

char *
os_read_file(const char *filename)
{
   /* Note that this also serves as a slight margin to avoid a 2x grow when
    * the file is just a few bytes larger when we read it than when we
    * fstat'ed it.
    * The string's NULL terminator is also included in here.
    */
   size_t len = 64;

   int fd = open(filename, O_RDONLY);
   if (fd == -1) {
      /* errno set by open() */
      return NULL;
   }

   /* Pre-allocate a buffer at least the size of the file if we can read
    * that information.
    */
   struct stat stat;
   if (fstat(fd, &stat) == 0)
      len += stat.st_size;

   char *buf = malloc(len);
   if (!buf) {
      close(fd);
      errno = -ENOMEM;
      return NULL;
   }

   ssize_t read;
   size_t offset = 0, remaining = len - 1;
   while ((read = readN(fd, buf + offset, remaining)) == remaining) {
      char *newbuf = realloc(buf, 2 * len);
      if (!newbuf) {
         free(buf);
         close(fd);
         errno = -ENOMEM;
         return NULL;
      }

      buf = newbuf;
      len *= 2;
      offset += read;
      remaining = len - offset - 1;
   }

   close(fd);

   if (read > 0)
      offset += read;

   /* Final resize to actual size */
   len = offset + 1;
   char *newbuf = realloc(buf, len);
   if (!newbuf) {
      free(buf);
      close(fd);
      errno = -ENOMEM;
      return NULL;
   }
   buf = newbuf;

   buf[offset] = '\0';

   return buf;
}

#else

char *
os_read_file(const char *filename)
{
   errno = -ENOSYS;
   return NULL;
}

#endif
