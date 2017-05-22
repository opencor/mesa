/****************************************************************************
 * Copyright (C) 2015 Intel Corporation.   All Rights Reserved.
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 ***************************************************************************/

#include "swr_context.h"
#include "swr_memory.h"
#include "swr_screen.h"
#include "swr_resource.h"
#include "swr_scratch.h"
#include "swr_query.h"
#include "swr_fence.h"

#include "util/u_memory.h"
#include "util/u_inlines.h"
#include "util/u_format.h"
#include "util/u_atomic.h"
#include "util/u_upload_mgr.h"
#include "util/u_transfer.h"
#include "util/u_surface.h"

#include "api.h"
#include "backend.h"

static struct pipe_surface *
swr_create_surface(struct pipe_context *pipe,
                   struct pipe_resource *pt,
                   const struct pipe_surface *surf_tmpl)
{
   struct pipe_surface *ps;

   ps = CALLOC_STRUCT(pipe_surface);
   if (ps) {
      pipe_reference_init(&ps->reference, 1);
      pipe_resource_reference(&ps->texture, pt);
      ps->context = pipe;
      ps->format = surf_tmpl->format;
      if (pt->target != PIPE_BUFFER) {
         assert(surf_tmpl->u.tex.level <= pt->last_level);
         ps->width = u_minify(pt->width0, surf_tmpl->u.tex.level);
         ps->height = u_minify(pt->height0, surf_tmpl->u.tex.level);
         ps->u.tex.level = surf_tmpl->u.tex.level;
         ps->u.tex.first_layer = surf_tmpl->u.tex.first_layer;
         ps->u.tex.last_layer = surf_tmpl->u.tex.last_layer;
      } else {
         /* setting width as number of elements should get us correct
          * renderbuffer width */
         ps->width = surf_tmpl->u.buf.last_element
            - surf_tmpl->u.buf.first_element + 1;
         ps->height = pt->height0;
         ps->u.buf.first_element = surf_tmpl->u.buf.first_element;
         ps->u.buf.last_element = surf_tmpl->u.buf.last_element;
         assert(ps->u.buf.first_element <= ps->u.buf.last_element);
         assert(ps->u.buf.last_element < ps->width);
      }
   }
   return ps;
}

static void
swr_surface_destroy(struct pipe_context *pipe, struct pipe_surface *surf)
{
   assert(surf->texture);
   struct pipe_resource *resource = surf->texture;

   /* If the resource has been drawn to, store tiles. */
   swr_store_dirty_resource(pipe, resource, SWR_TILE_RESOLVED);

   pipe_resource_reference(&resource, NULL);
   FREE(surf);
}


static void *
swr_transfer_map(struct pipe_context *pipe,
                 struct pipe_resource *resource,
                 unsigned level,
                 unsigned usage,
                 const struct pipe_box *box,
                 struct pipe_transfer **transfer)
{
   struct swr_screen *screen = swr_screen(pipe->screen);
   struct swr_resource *spr = swr_resource(resource);
   struct pipe_transfer *pt;
   enum pipe_format format = resource->format;

   assert(resource);
   assert(level <= resource->last_level);

   /* If mapping an attached rendertarget, store tiles to surface and set
    * postStoreTileState to SWR_TILE_INVALID so tiles get reloaded on next use
    * and nothing needs to be done at unmap. */
   swr_store_dirty_resource(pipe, resource, SWR_TILE_INVALID);

   if (!(usage & PIPE_TRANSFER_UNSYNCHRONIZED)) {
      /* If resource is in use, finish fence before mapping.
       * Unless requested not to block, then if not done return NULL map */
      if (usage & PIPE_TRANSFER_DONTBLOCK) {
         if (swr_is_fence_pending(screen->flush_fence))
            return NULL;
      } else {
         if (spr->status) {
            /* But, if there's no fence pending, submit one.
             * XXX: Remove once draw timestamps are finished. */
            if (!swr_is_fence_pending(screen->flush_fence))
               swr_fence_submit(swr_context(pipe), screen->flush_fence);

            swr_fence_finish(pipe->screen, NULL, screen->flush_fence, 0);
            swr_resource_unused(resource);
         }
      }
   }

   pt = CALLOC_STRUCT(pipe_transfer);
   if (!pt)
      return NULL;
   pipe_resource_reference(&pt->resource, resource);
   pt->usage = (pipe_transfer_usage)usage;
   pt->level = level;
   pt->box = *box;
   pt->stride = spr->swr.pitch;
   pt->layer_stride = spr->swr.qpitch * spr->swr.pitch;

   /* if we're mapping the depth/stencil, copy in stencil for the section
    * being read in
    */
   if (usage & PIPE_TRANSFER_READ && spr->has_depth && spr->has_stencil) {
      size_t zbase, sbase;
      for (int z = box->z; z < box->z + box->depth; z++) {
         zbase = (z * spr->swr.qpitch + box->y) * spr->swr.pitch +
            spr->mip_offsets[level];
         sbase = (z * spr->secondary.qpitch + box->y) * spr->secondary.pitch +
            spr->secondary_mip_offsets[level];
         for (int y = box->y; y < box->y + box->height; y++) {
            if (spr->base.format == PIPE_FORMAT_Z24_UNORM_S8_UINT) {
               for (int x = box->x; x < box->x + box->width; x++)
                  spr->swr.pBaseAddress[zbase + 4 * x + 3] =
                     spr->secondary.pBaseAddress[sbase + x];
            } else if (spr->base.format == PIPE_FORMAT_Z32_FLOAT_S8X24_UINT) {
               for (int x = box->x; x < box->x + box->width; x++)
                  spr->swr.pBaseAddress[zbase + 8 * x + 4] =
                     spr->secondary.pBaseAddress[sbase + x];
            }
            zbase += spr->swr.pitch;
            sbase += spr->secondary.pitch;
         }
      }
   }

   unsigned offset = box->z * pt->layer_stride +
      util_format_get_nblocksy(format, box->y) * pt->stride +
      util_format_get_stride(format, box->x);

   *transfer = pt;

   return spr->swr.pBaseAddress + offset + spr->mip_offsets[level];
}

static void
swr_transfer_flush_region(struct pipe_context *pipe,
                          struct pipe_transfer *transfer,
                          const struct pipe_box *flush_box)
{
   assert(transfer->resource);
   assert(transfer->usage & PIPE_TRANSFER_WRITE);

   struct swr_resource *spr = swr_resource(transfer->resource);
   if (!spr->has_depth || !spr->has_stencil)
      return;

   size_t zbase, sbase;
   struct pipe_box box = *flush_box;
   box.x += transfer->box.x;
   box.y += transfer->box.y;
   box.z += transfer->box.z;
   for (int z = box.z; z < box.z + box.depth; z++) {
      zbase = (z * spr->swr.qpitch + box.y) * spr->swr.pitch +
         spr->mip_offsets[transfer->level];
      sbase = (z * spr->secondary.qpitch + box.y) * spr->secondary.pitch +
         spr->secondary_mip_offsets[transfer->level];
      for (int y = box.y; y < box.y + box.height; y++) {
         if (spr->base.format == PIPE_FORMAT_Z24_UNORM_S8_UINT) {
            for (int x = box.x; x < box.x + box.width; x++)
               spr->secondary.pBaseAddress[sbase + x] =
                  spr->swr.pBaseAddress[zbase + 4 * x + 3];
         } else if (spr->base.format == PIPE_FORMAT_Z32_FLOAT_S8X24_UINT) {
            for (int x = box.x; x < box.x + box.width; x++)
               spr->secondary.pBaseAddress[sbase + x] =
                  spr->swr.pBaseAddress[zbase + 8 * x + 4];
         }
         zbase += spr->swr.pitch;
         sbase += spr->secondary.pitch;
      }
   }
}

static void
swr_transfer_unmap(struct pipe_context *pipe, struct pipe_transfer *transfer)
{
   assert(transfer->resource);

   struct swr_resource *spr = swr_resource(transfer->resource);
   /* if we're mapping the depth/stencil, copy in stencil for the section
    * being written out
    */
   if (transfer->usage & PIPE_TRANSFER_WRITE &&
       !(transfer->usage & PIPE_TRANSFER_FLUSH_EXPLICIT) &&
       spr->has_depth && spr->has_stencil) {
      struct pipe_box box;
      u_box_3d(0, 0, 0, transfer->box.width, transfer->box.height,
               transfer->box.depth, &box);
      swr_transfer_flush_region(pipe, transfer, &box);
   }

   pipe_resource_reference(&transfer->resource, NULL);
   FREE(transfer);
}


static void
swr_resource_copy(struct pipe_context *pipe,
                  struct pipe_resource *dst,
                  unsigned dst_level,
                  unsigned dstx,
                  unsigned dsty,
                  unsigned dstz,
                  struct pipe_resource *src,
                  unsigned src_level,
                  const struct pipe_box *src_box)
{
   struct swr_screen *screen = swr_screen(pipe->screen);

   /* If either the src or dst is a renderTarget, store tiles before copy */
   swr_store_dirty_resource(pipe, src, SWR_TILE_RESOLVED);
   swr_store_dirty_resource(pipe, dst, SWR_TILE_RESOLVED);

   swr_fence_finish(pipe->screen, NULL, screen->flush_fence, 0);
   swr_resource_unused(src);
   swr_resource_unused(dst);

   if ((dst->target == PIPE_BUFFER && src->target == PIPE_BUFFER)
       || (dst->target != PIPE_BUFFER && src->target != PIPE_BUFFER)) {
      util_resource_copy_region(
         pipe, dst, dst_level, dstx, dsty, dstz, src, src_level, src_box);
      return;
   }

   debug_printf("unhandled swr_resource_copy\n");
}


/* XXX: This resolve is incomplete and suboptimal. It will be removed once the
 * pipelined resolve blit works. */
void
swr_do_msaa_resolve(struct pipe_resource *src_resource,
                    struct pipe_resource *dst_resource)
{
   /* This is a pretty dumb inline resolve.  It only supports 8-bit formats
    * (ex RGBA8/BGRA8) - which are most common display formats anyway.
    */

   /* quick check for 8-bit and number of components */
   uint8_t bits_per_component =
      util_format_get_component_bits(src_resource->format,
            UTIL_FORMAT_COLORSPACE_RGB, 0);

   /* Unsupported resolve format */
   assert(src_resource->format == dst_resource->format);
   assert(bits_per_component == 8);
   if ((src_resource->format != dst_resource->format) ||
       (bits_per_component != 8)) {
      return;
   }

   uint8_t src_num_comps = util_format_get_nr_components(src_resource->format);

   SWR_SURFACE_STATE *src_surface = &swr_resource(src_resource)->swr;
   SWR_SURFACE_STATE *dst_surface = &swr_resource(dst_resource)->swr;

   uint32_t *src, *dst, offset;
   uint32_t num_samples = src_surface->numSamples;
   float recip_num_samples = 1.0f / num_samples;
   for (uint32_t y = 0; y < src_surface->height; y++) {
      for (uint32_t x = 0; x < src_surface->width; x++) {
         float r = 0.0f;
         float g = 0.0f;
         float b = 0.0f;
         float a = 0.0f;
         for (uint32_t sampleNum = 0;  sampleNum < num_samples; sampleNum++) {
            offset = ComputeSurfaceOffset<false>(x, y, 0, 0, sampleNum, 0, src_surface);
            src = (uint32_t *) src_surface->pBaseAddress + offset/src_num_comps;
            const uint32_t sample = *src;
            r += (float)((sample >> 24) & 0xff) / 255.0f * recip_num_samples;
            g += (float)((sample >> 16) & 0xff) / 255.0f * recip_num_samples;
            b += (float)((sample >>  8) & 0xff) / 255.0f * recip_num_samples;
            a += (float)((sample      ) & 0xff) / 255.0f * recip_num_samples;
         }
         uint32_t result = 0;
         result  = ((uint8_t)(r * 255.0f) & 0xff) << 24;
         result |= ((uint8_t)(g * 255.0f) & 0xff) << 16;
         result |= ((uint8_t)(b * 255.0f) & 0xff) <<  8;
         result |= ((uint8_t)(a * 255.0f) & 0xff);
         offset = ComputeSurfaceOffset<false>(x, y, 0, 0, 0, 0, src_surface);
         dst = (uint32_t *) dst_surface->pBaseAddress + offset/src_num_comps;
         *dst = result;
      }
   }
}


static void
swr_blit(struct pipe_context *pipe, const struct pipe_blit_info *blit_info)
{
   struct swr_context *ctx = swr_context(pipe);
   /* Make a copy of the const blit_info, so we can modify it */
   struct pipe_blit_info info = *blit_info;

   if (info.render_condition_enable && !swr_check_render_cond(pipe))
      return;

   if (info.src.resource->nr_samples > 1 && info.dst.resource->nr_samples <= 1
       && !util_format_is_depth_or_stencil(info.src.resource->format)
       && !util_format_is_pure_integer(info.src.resource->format)) {
      debug_printf("swr_blit: color resolve : %d -> %d\n",
            info.src.resource->nr_samples, info.dst.resource->nr_samples);

      /* Because the resolve is being done inline (not pipelined),
       * resources need to be stored out of hottiles and the pipeline empty.
       *
       * Resources are marked unused following fence finish because all
       * pipeline operations are complete.  Validation of the blit will mark
       * them are read/write again.
       */
      swr_store_dirty_resource(pipe, info.src.resource, SWR_TILE_RESOLVED);
      swr_store_dirty_resource(pipe, info.dst.resource, SWR_TILE_RESOLVED);
      swr_fence_finish(pipe->screen, NULL, swr_screen(pipe->screen)->flush_fence, 0);
      swr_resource_unused(info.src.resource);
      swr_resource_unused(info.dst.resource);

      struct pipe_resource *src_resource = info.src.resource;
      struct pipe_resource *resolve_target =
         swr_resource(src_resource)->resolve_target;

      /* Inline resolve samples into resolve target resource, then continue
       * the blit. */
      swr_do_msaa_resolve(src_resource, resolve_target);

      /* The resolve target becomes the new source for the blit.  */
      info.src.resource = resolve_target;
   }

   if (util_try_blit_via_copy_region(pipe, &info)) {
      return; /* done */
   }

   if (info.mask & PIPE_MASK_S) {
      debug_printf("swr: cannot blit stencil, skipping\n");
      info.mask &= ~PIPE_MASK_S;
   }

   if (!util_blitter_is_blit_supported(ctx->blitter, &info)) {
      debug_printf("swr: blit unsupported %s -> %s\n",
                   util_format_short_name(info.src.resource->format),
                   util_format_short_name(info.dst.resource->format));
      return;
   }

   if (ctx->active_queries) {
      SwrEnableStatsFE(ctx->swrContext, FALSE);
      SwrEnableStatsBE(ctx->swrContext, FALSE);
   }

   util_blitter_save_vertex_buffer_slot(ctx->blitter, ctx->vertex_buffer);
   util_blitter_save_vertex_elements(ctx->blitter, (void *)ctx->velems);
   util_blitter_save_vertex_shader(ctx->blitter, (void *)ctx->vs);
   util_blitter_save_geometry_shader(ctx->blitter, (void*)ctx->gs);
   util_blitter_save_so_targets(
      ctx->blitter,
      ctx->num_so_targets,
      (struct pipe_stream_output_target **)ctx->so_targets);
   util_blitter_save_rasterizer(ctx->blitter, (void *)ctx->rasterizer);
   util_blitter_save_viewport(ctx->blitter, &ctx->viewport);
   util_blitter_save_scissor(ctx->blitter, &ctx->scissor);
   util_blitter_save_fragment_shader(ctx->blitter, ctx->fs);
   util_blitter_save_blend(ctx->blitter, (void *)ctx->blend);
   util_blitter_save_depth_stencil_alpha(ctx->blitter,
                                         (void *)ctx->depth_stencil);
   util_blitter_save_stencil_ref(ctx->blitter, &ctx->stencil_ref);
   util_blitter_save_sample_mask(ctx->blitter, ctx->sample_mask);
   util_blitter_save_framebuffer(ctx->blitter, &ctx->framebuffer);
   util_blitter_save_fragment_sampler_states(
      ctx->blitter,
      ctx->num_samplers[PIPE_SHADER_FRAGMENT],
      (void **)ctx->samplers[PIPE_SHADER_FRAGMENT]);
   util_blitter_save_fragment_sampler_views(
      ctx->blitter,
      ctx->num_sampler_views[PIPE_SHADER_FRAGMENT],
      ctx->sampler_views[PIPE_SHADER_FRAGMENT]);
   util_blitter_save_render_condition(ctx->blitter,
                                      ctx->render_cond_query,
                                      ctx->render_cond_cond,
                                      ctx->render_cond_mode);

   util_blitter_blit(ctx->blitter, &info);

   if (ctx->active_queries) {
      SwrEnableStatsFE(ctx->swrContext, TRUE);
      SwrEnableStatsBE(ctx->swrContext, TRUE);
   }
}


static void
swr_destroy(struct pipe_context *pipe)
{
   struct swr_context *ctx = swr_context(pipe);
   struct swr_screen *screen = swr_screen(pipe->screen);

   if (ctx->blitter)
      util_blitter_destroy(ctx->blitter);

   for (unsigned i = 0; i < PIPE_MAX_COLOR_BUFS; i++) {
      pipe_surface_reference(&ctx->framebuffer.cbufs[i], NULL);
   }

   pipe_surface_reference(&ctx->framebuffer.zsbuf, NULL);

   for (unsigned i = 0; i < ARRAY_SIZE(ctx->sampler_views[0]); i++) {
      pipe_sampler_view_reference(&ctx->sampler_views[PIPE_SHADER_FRAGMENT][i], NULL);
   }

   for (unsigned i = 0; i < ARRAY_SIZE(ctx->sampler_views[0]); i++) {
      pipe_sampler_view_reference(&ctx->sampler_views[PIPE_SHADER_VERTEX][i], NULL);
   }

   if (ctx->pipe.stream_uploader)
      u_upload_destroy(ctx->pipe.stream_uploader);

   /* Idle core after destroying buffer resources, but before deleting
    * context.  Destroying resources has potentially called StoreTiles.*/
   SwrWaitForIdle(ctx->swrContext);

   if (ctx->swrContext)
      SwrDestroyContext(ctx->swrContext);

   delete ctx->blendJIT;

   swr_destroy_scratch_buffers(ctx);

   /* Only update screen->pipe if current context is being destroyed */
   assert(screen);
   if (screen->pipe == pipe)
      screen->pipe = NULL;

   AlignedFree(ctx);
}


static void
swr_render_condition(struct pipe_context *pipe,
                     struct pipe_query *query,
                     boolean condition,
                     enum pipe_render_cond_flag mode)
{
   struct swr_context *ctx = swr_context(pipe);

   ctx->render_cond_query = query;
   ctx->render_cond_mode = mode;
   ctx->render_cond_cond = condition;
}

static void
swr_UpdateStats(HANDLE hPrivateContext, const SWR_STATS *pStats)
{
   swr_draw_context *pDC = (swr_draw_context*)hPrivateContext;

   if (!pDC)
      return;

   struct swr_query_result *pqr = (struct swr_query_result *)pDC->pStats;

   SWR_STATS *pSwrStats = &pqr->core;

   pSwrStats->DepthPassCount += pStats->DepthPassCount;
   pSwrStats->PsInvocations += pStats->PsInvocations;
   pSwrStats->CsInvocations += pStats->CsInvocations;
}

static void
swr_UpdateStatsFE(HANDLE hPrivateContext, const SWR_STATS_FE *pStats)
{
   swr_draw_context *pDC = (swr_draw_context*)hPrivateContext;

   if (!pDC)
      return;

   struct swr_query_result *pqr = (struct swr_query_result *)pDC->pStats;

   SWR_STATS_FE *pSwrStats = &pqr->coreFE;
   p_atomic_add(&pSwrStats->IaVertices, pStats->IaVertices);
   p_atomic_add(&pSwrStats->IaPrimitives, pStats->IaPrimitives);
   p_atomic_add(&pSwrStats->VsInvocations, pStats->VsInvocations);
   p_atomic_add(&pSwrStats->HsInvocations, pStats->HsInvocations);
   p_atomic_add(&pSwrStats->DsInvocations, pStats->DsInvocations);
   p_atomic_add(&pSwrStats->GsInvocations, pStats->GsInvocations);
   p_atomic_add(&pSwrStats->CInvocations, pStats->CInvocations);
   p_atomic_add(&pSwrStats->CPrimitives, pStats->CPrimitives);
   p_atomic_add(&pSwrStats->GsPrimitives, pStats->GsPrimitives);

   for (unsigned i = 0; i < 4; i++) {
      p_atomic_add(&pSwrStats->SoPrimStorageNeeded[i],
            pStats->SoPrimStorageNeeded[i]);
      p_atomic_add(&pSwrStats->SoNumPrimsWritten[i],
            pStats->SoNumPrimsWritten[i]);
   }
}

struct pipe_context *
swr_create_context(struct pipe_screen *p_screen, void *priv, unsigned flags)
{
   struct swr_context *ctx = (struct swr_context *)
      AlignedMalloc(sizeof(struct swr_context), KNOB_SIMD_BYTES);
   memset(ctx, 0, sizeof(struct swr_context));

   ctx->blendJIT =
      new std::unordered_map<BLEND_COMPILE_STATE, PFN_BLEND_JIT_FUNC>;

   SWR_CREATECONTEXT_INFO createInfo;
   memset(&createInfo, 0, sizeof(createInfo));
   createInfo.privateStateSize = sizeof(swr_draw_context);
   createInfo.pfnLoadTile = swr_LoadHotTile;
   createInfo.pfnStoreTile = swr_StoreHotTile;
   createInfo.pfnClearTile = swr_StoreHotTileClear;
   createInfo.pfnUpdateStats = swr_UpdateStats;
   createInfo.pfnUpdateStatsFE = swr_UpdateStatsFE;
   ctx->swrContext = SwrCreateContext(&createInfo);

   /* Init Load/Store/ClearTiles Tables */
   swr_InitMemoryModule();

   InitBackendFuncTables();

   if (ctx->swrContext == NULL)
      goto fail;

   ctx->pipe.screen = p_screen;
   ctx->pipe.destroy = swr_destroy;
   ctx->pipe.priv = priv;
   ctx->pipe.create_surface = swr_create_surface;
   ctx->pipe.surface_destroy = swr_surface_destroy;
   ctx->pipe.transfer_map = swr_transfer_map;
   ctx->pipe.transfer_unmap = swr_transfer_unmap;
   ctx->pipe.transfer_flush_region = swr_transfer_flush_region;

   ctx->pipe.buffer_subdata = u_default_buffer_subdata;
   ctx->pipe.texture_subdata = u_default_texture_subdata;

   ctx->pipe.clear_texture = util_clear_texture;
   ctx->pipe.resource_copy_region = swr_resource_copy;
   ctx->pipe.render_condition = swr_render_condition;

   swr_state_init(&ctx->pipe);
   swr_clear_init(&ctx->pipe);
   swr_draw_init(&ctx->pipe);
   swr_query_init(&ctx->pipe);

   ctx->pipe.stream_uploader = u_upload_create_default(&ctx->pipe);
   if (!ctx->pipe.stream_uploader)
      goto fail;
   ctx->pipe.const_uploader = ctx->pipe.stream_uploader;

   ctx->pipe.blit = swr_blit;
   ctx->blitter = util_blitter_create(&ctx->pipe);
   if (!ctx->blitter)
      goto fail;

   swr_init_scratch_buffers(ctx);

   return &ctx->pipe;

fail:
   /* Should really validate the init steps and fail gracefully */
   swr_destroy(&ctx->pipe);
   return NULL;
}
