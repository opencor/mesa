/**************************************************************************
 *
 * Copyright 2007 VMware, Inc.
 * Copyright (c) 2008-2010 VMware, Inc.
 * Copyright (c) 2019 Google, LLC
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "state_tracker/st_context.h"
#include "state_tracker/st_format.h"
#include "state_tracker/st_texture.h"
#include "util/u_format.h"
#include <stdbool.h>

static bool
is_format_supported(struct pipe_screen *pscreen,
                    enum pipe_format format,
                    enum pipe_texture_target target,
                    unsigned sample_count,
                    unsigned storage_sample_count,
                    unsigned usage)
{
   return true;
}

int main(int argc, char **argv)
{
   struct pipe_screen screen = {
      .is_format_supported = is_format_supported,
   };
   struct pipe_context pctx = {
      .screen = &screen,
   };
   struct st_context local_st = {
      .pipe = &pctx,
   };
   struct st_context *st = &local_st;

   GLuint i;

   /* test all Mesa formats */
   for (i = 1; i < MESA_FORMAT_COUNT; i++) {
      enum pipe_format pf;

      if (st_compressed_format_fallback(st, i))
         continue;

      pf = st_mesa_format_to_pipe_format(st, i);
      if (pf != PIPE_FORMAT_NONE) {
         mesa_format mf = st_pipe_format_to_mesa_format(pf);
         if (mf != i) {
            fprintf(stderr, "Round-tripping %s -> %s -> %s failed\n",
                    _mesa_get_format_name(i), util_format_short_name(pf),
                    _mesa_get_format_name(mf));
            return 1;
         }
      }
   }

   /* Test all Gallium formats */
   for (i = 1; i < PIPE_FORMAT_COUNT; i++) {
      mesa_format mf = st_pipe_format_to_mesa_format(i);
      if (st_compressed_format_fallback(st, mf))
         continue;

      if (mf != MESA_FORMAT_NONE) {
         enum pipe_format pf =
            st_mesa_format_to_pipe_format(st, mf);
         if (pf != i) {
            fprintf(stderr, "Round-tripping %s -> %s -> %s failed\n",
                    util_format_short_name(i),
                    _mesa_get_format_name(pf),
                    util_format_short_name(pf));
            return 1;
         }
      }
   }

   return 0;
}
