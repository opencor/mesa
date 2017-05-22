/* -*- mode: C; c-file-style: "k&r"; tab-width 4; indent-tabs-mode: t; -*- */

/*
 * Copyright (C) 2012-2013 Rob Clark <robclark@freedesktop.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#ifndef FD2_RASTERIZER_H_
#define FD2_RASTERIZER_H_

#include "pipe/p_state.h"
#include "pipe/p_context.h"

struct fd2_rasterizer_stateobj {
	struct pipe_rasterizer_state base;
	uint32_t pa_sc_line_stipple;
	uint32_t pa_cl_clip_cntl;
	uint32_t pa_su_vtx_cntl;
	uint32_t pa_su_point_size;
	uint32_t pa_su_point_minmax;
	uint32_t pa_su_line_cntl;
	uint32_t pa_su_sc_mode_cntl;
};

static inline struct fd2_rasterizer_stateobj *
fd2_rasterizer_stateobj(struct pipe_rasterizer_state *rast)
{
	return (struct fd2_rasterizer_stateobj *)rast;
}

void * fd2_rasterizer_state_create(struct pipe_context *pctx,
		const struct pipe_rasterizer_state *cso);

#endif /* FD2_RASTERIZER_H_ */
