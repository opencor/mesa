/*
 * Copyright © 2016 Intel Corporation
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
 */

#include "anv_private.h"

static bool
lookup_blorp_shader(struct blorp_batch *batch,
                    const void *key, uint32_t key_size,
                    uint32_t *kernel_out, void *prog_data_out)
{
   struct blorp_context *blorp = batch->blorp;
   struct anv_device *device = blorp->driver_ctx;

   /* The default cache must be a real cache */
   assert(device->default_pipeline_cache.cache);

   struct anv_shader_bin *bin =
      anv_pipeline_cache_search(&device->default_pipeline_cache, key, key_size);
   if (!bin)
      return false;

   /* The cache already has a reference and it's not going anywhere so there
    * is no need to hold a second reference.
    */
   anv_shader_bin_unref(device, bin);

   *kernel_out = bin->kernel.offset;
   *(const struct brw_stage_prog_data **)prog_data_out = bin->prog_data;

   return true;
}

static bool
upload_blorp_shader(struct blorp_batch *batch,
                    const void *key, uint32_t key_size,
                    const void *kernel, uint32_t kernel_size,
                    const struct brw_stage_prog_data *prog_data,
                    uint32_t prog_data_size,
                    uint32_t *kernel_out, void *prog_data_out)
{
   struct blorp_context *blorp = batch->blorp;
   struct anv_device *device = blorp->driver_ctx;

   /* The blorp cache must be a real cache */
   assert(device->default_pipeline_cache.cache);

   struct anv_pipeline_bind_map bind_map = {
      .surface_count = 0,
      .sampler_count = 0,
   };

   struct anv_shader_bin *bin =
      anv_pipeline_cache_upload_kernel(&device->default_pipeline_cache,
                                       key, key_size, kernel, kernel_size,
                                       NULL, 0,
                                       prog_data, prog_data_size,
                                       NULL, 0, NULL, &bind_map);

   if (!bin)
      return false;

   /* The cache already has a reference and it's not going anywhere so there
    * is no need to hold a second reference.
    */
   anv_shader_bin_unref(device, bin);

   *kernel_out = bin->kernel.offset;
   *(const struct brw_stage_prog_data **)prog_data_out = bin->prog_data;

   return true;
}

void
anv_device_init_blorp(struct anv_device *device)
{
   blorp_init(&device->blorp, device, &device->isl_dev);
   device->blorp.compiler = device->instance->physicalDevice.compiler;
   device->blorp.lookup_shader = lookup_blorp_shader;
   device->blorp.upload_shader = upload_blorp_shader;
   switch (device->info.gen) {
   case 7:
      if (device->info.is_haswell) {
         device->blorp.exec = gen75_blorp_exec;
      } else {
         device->blorp.exec = gen7_blorp_exec;
      }
      break;
   case 8:
      device->blorp.exec = gen8_blorp_exec;
      break;
   case 9:
      device->blorp.exec = gen9_blorp_exec;
      break;
   case 10:
      device->blorp.exec = gen10_blorp_exec;
      break;
   case 11:
      device->blorp.exec = gen11_blorp_exec;
      break;
   case 12:
      device->blorp.exec = gen12_blorp_exec;
      break;
   default:
      unreachable("Unknown hardware generation");
   }
}

void
anv_device_finish_blorp(struct anv_device *device)
{
   blorp_finish(&device->blorp);
}

static void
get_blorp_surf_for_anv_buffer(struct anv_device *device,
                              struct anv_buffer *buffer, uint64_t offset,
                              uint32_t width, uint32_t height,
                              uint32_t row_pitch, enum isl_format format,
                              struct blorp_surf *blorp_surf,
                              struct isl_surf *isl_surf)
{
   const struct isl_format_layout *fmtl =
      isl_format_get_layout(format);
   bool ok UNUSED;

   /* ASTC is the only format which doesn't support linear layouts.
    * Create an equivalently sized surface with ISL to get around this.
    */
   if (fmtl->txc == ISL_TXC_ASTC) {
      /* Use an equivalently sized format */
      format = ISL_FORMAT_R32G32B32A32_UINT;
      assert(fmtl->bpb == isl_format_get_layout(format)->bpb);

      /* Shrink the dimensions for the new format */
      width = DIV_ROUND_UP(width, fmtl->bw);
      height = DIV_ROUND_UP(height, fmtl->bh);
   }

   *blorp_surf = (struct blorp_surf) {
      .surf = isl_surf,
      .addr = {
         .buffer = buffer->address.bo,
         .offset = buffer->address.offset + offset,
         .mocs = anv_mocs_for_bo(device, buffer->address.bo),
      },
   };

   ok = isl_surf_init(&device->isl_dev, isl_surf,
                     .dim = ISL_SURF_DIM_2D,
                     .format = format,
                     .width = width,
                     .height = height,
                     .depth = 1,
                     .levels = 1,
                     .array_len = 1,
                     .samples = 1,
                     .row_pitch_B = row_pitch,
                     .usage = ISL_SURF_USAGE_TEXTURE_BIT |
                              ISL_SURF_USAGE_RENDER_TARGET_BIT,
                     .tiling_flags = ISL_TILING_LINEAR_BIT);
   assert(ok);
}

/* Pick something high enough that it won't be used in core and low enough it
 * will never map to an extension.
 */
#define ANV_IMAGE_LAYOUT_EXPLICIT_AUX (VkImageLayout)10000000

static struct blorp_address
anv_to_blorp_address(struct anv_address addr)
{
   return (struct blorp_address) {
      .buffer = addr.bo,
      .offset = addr.offset,
   };
}

static void
get_blorp_surf_for_anv_image(const struct anv_device *device,
                             const struct anv_image *image,
                             VkImageAspectFlags aspect,
                             VkImageLayout layout,
                             enum isl_aux_usage aux_usage,
                             struct blorp_surf *blorp_surf)
{
   uint32_t plane = anv_image_aspect_to_plane(image->aspects, aspect);

   if (layout != ANV_IMAGE_LAYOUT_EXPLICIT_AUX)
      aux_usage = anv_layout_to_aux_usage(&device->info, image, aspect, layout);

   const struct anv_surface *surface = &image->planes[plane].surface;
   *blorp_surf = (struct blorp_surf) {
      .surf = &surface->isl,
      .addr = {
         .buffer = image->planes[plane].address.bo,
         .offset = image->planes[plane].address.offset + surface->offset,
         .mocs = anv_mocs_for_bo(device, image->planes[plane].address.bo),
      },
   };

   if (aux_usage != ISL_AUX_USAGE_NONE) {
      const struct anv_surface *aux_surface = &image->planes[plane].aux_surface;
      blorp_surf->aux_surf = &aux_surface->isl,
      blorp_surf->aux_addr = (struct blorp_address) {
         .buffer = image->planes[plane].address.bo,
         .offset = image->planes[plane].address.offset + aux_surface->offset,
         .mocs = anv_mocs_for_bo(device, image->planes[plane].address.bo),
      };
      blorp_surf->aux_usage = aux_usage;

      /* If we're doing a partial resolve, then we need the indirect clear
       * color.  If we are doing a fast clear and want to store/update the
       * clear color, we also pass the address to blorp, otherwise it will only
       * stomp the CCS to a particular value and won't care about format or
       * clear value
       */
      if (aspect & VK_IMAGE_ASPECT_ANY_COLOR_BIT_ANV) {
         const struct anv_address clear_color_addr =
            anv_image_get_clear_color_addr(device, image, aspect);
         blorp_surf->clear_color_addr = anv_to_blorp_address(clear_color_addr);
      } else if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT
                 && device->info.gen >= 10) {
         /* Vulkan always clears to 1.0. On gen < 10, we set that directly in
          * the state packet. For gen >= 10, must provide the clear value in a
          * buffer. We have a single global buffer that stores the 1.0 value.
          */
         const struct anv_address clear_color_addr = (struct anv_address) {
            .bo = (struct anv_bo *)&device->hiz_clear_bo
         };
         blorp_surf->clear_color_addr = anv_to_blorp_address(clear_color_addr);
      }
   }
}

static bool
get_blorp_surf_for_anv_shadow_image(const struct anv_device *device,
                                    const struct anv_image *image,
                                    VkImageAspectFlags aspect,
                                    struct blorp_surf *blorp_surf)
{

   uint32_t plane = anv_image_aspect_to_plane(image->aspects, aspect);
   if (image->planes[plane].shadow_surface.isl.size_B == 0)
      return false;

   *blorp_surf = (struct blorp_surf) {
      .surf = &image->planes[plane].shadow_surface.isl,
      .addr = {
         .buffer = image->planes[plane].address.bo,
         .offset = image->planes[plane].address.offset +
                   image->planes[plane].shadow_surface.offset,
         .mocs = anv_mocs_for_bo(device, image->planes[plane].address.bo),
      },
   };

   return true;
}

void anv_CmdCopyImage(
    VkCommandBuffer                             commandBuffer,
    VkImage                                     srcImage,
    VkImageLayout                               srcImageLayout,
    VkImage                                     dstImage,
    VkImageLayout                               dstImageLayout,
    uint32_t                                    regionCount,
    const VkImageCopy*                          pRegions)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_image, src_image, srcImage);
   ANV_FROM_HANDLE(anv_image, dst_image, dstImage);

   struct blorp_batch batch;
   blorp_batch_init(&cmd_buffer->device->blorp, &batch, cmd_buffer, 0);

   for (unsigned r = 0; r < regionCount; r++) {
      VkOffset3D srcOffset =
         anv_sanitize_image_offset(src_image->type, pRegions[r].srcOffset);
      VkOffset3D dstOffset =
         anv_sanitize_image_offset(dst_image->type, pRegions[r].dstOffset);
      VkExtent3D extent =
         anv_sanitize_image_extent(src_image->type, pRegions[r].extent);

      const uint32_t dst_level = pRegions[r].dstSubresource.mipLevel;
      unsigned dst_base_layer, layer_count;
      if (dst_image->type == VK_IMAGE_TYPE_3D) {
         dst_base_layer = pRegions[r].dstOffset.z;
         layer_count = pRegions[r].extent.depth;
      } else {
         dst_base_layer = pRegions[r].dstSubresource.baseArrayLayer;
         layer_count =
            anv_get_layerCount(dst_image, &pRegions[r].dstSubresource);
      }

      const uint32_t src_level = pRegions[r].srcSubresource.mipLevel;
      unsigned src_base_layer;
      if (src_image->type == VK_IMAGE_TYPE_3D) {
         src_base_layer = pRegions[r].srcOffset.z;
      } else {
         src_base_layer = pRegions[r].srcSubresource.baseArrayLayer;
         assert(layer_count ==
                anv_get_layerCount(src_image, &pRegions[r].srcSubresource));
      }

      VkImageAspectFlags src_mask = pRegions[r].srcSubresource.aspectMask,
         dst_mask = pRegions[r].dstSubresource.aspectMask;

      assert(anv_image_aspects_compatible(src_mask, dst_mask));

      if (util_bitcount(src_mask) > 1) {
         uint32_t aspect_bit;
         anv_foreach_image_aspect_bit(aspect_bit, src_image, src_mask) {
            struct blorp_surf src_surf, dst_surf;
            get_blorp_surf_for_anv_image(cmd_buffer->device,
                                         src_image, 1UL << aspect_bit,
                                         srcImageLayout, ISL_AUX_USAGE_NONE,
                                         &src_surf);
            get_blorp_surf_for_anv_image(cmd_buffer->device,
                                         dst_image, 1UL << aspect_bit,
                                         dstImageLayout, ISL_AUX_USAGE_NONE,
                                         &dst_surf);
            anv_cmd_buffer_mark_image_written(cmd_buffer, dst_image,
                                              1UL << aspect_bit,
                                              dst_surf.aux_usage, dst_level,
                                              dst_base_layer, layer_count);

            for (unsigned i = 0; i < layer_count; i++) {
               blorp_copy(&batch, &src_surf, src_level, src_base_layer + i,
                          &dst_surf, dst_level, dst_base_layer + i,
                          srcOffset.x, srcOffset.y,
                          dstOffset.x, dstOffset.y,
                          extent.width, extent.height);
            }

            struct blorp_surf dst_shadow_surf;
            if (get_blorp_surf_for_anv_shadow_image(cmd_buffer->device,
                                                    dst_image,
                                                    1UL << aspect_bit,
                                                    &dst_shadow_surf)) {
               for (unsigned i = 0; i < layer_count; i++) {
                  blorp_copy(&batch, &src_surf, src_level, src_base_layer + i,
                             &dst_shadow_surf, dst_level, dst_base_layer + i,
                             srcOffset.x, srcOffset.y,
                             dstOffset.x, dstOffset.y,
                             extent.width, extent.height);
               }
            }
         }
      } else {
         struct blorp_surf src_surf, dst_surf;
         get_blorp_surf_for_anv_image(cmd_buffer->device, src_image, src_mask,
                                      srcImageLayout, ISL_AUX_USAGE_NONE,
                                      &src_surf);
         get_blorp_surf_for_anv_image(cmd_buffer->device, dst_image, dst_mask,
                                      dstImageLayout, ISL_AUX_USAGE_NONE,
                                      &dst_surf);
         anv_cmd_buffer_mark_image_written(cmd_buffer, dst_image, dst_mask,
                                           dst_surf.aux_usage, dst_level,
                                           dst_base_layer, layer_count);

         for (unsigned i = 0; i < layer_count; i++) {
            blorp_copy(&batch, &src_surf, src_level, src_base_layer + i,
                       &dst_surf, dst_level, dst_base_layer + i,
                       srcOffset.x, srcOffset.y,
                       dstOffset.x, dstOffset.y,
                       extent.width, extent.height);
         }

         struct blorp_surf dst_shadow_surf;
         if (get_blorp_surf_for_anv_shadow_image(cmd_buffer->device,
                                                 dst_image, dst_mask,
                                                 &dst_shadow_surf)) {
            for (unsigned i = 0; i < layer_count; i++) {
               blorp_copy(&batch, &src_surf, src_level, src_base_layer + i,
                          &dst_shadow_surf, dst_level, dst_base_layer + i,
                          srcOffset.x, srcOffset.y,
                          dstOffset.x, dstOffset.y,
                          extent.width, extent.height);
            }
         }
      }
   }

   blorp_batch_finish(&batch);
}

static void
copy_buffer_to_image(struct anv_cmd_buffer *cmd_buffer,
                     struct anv_buffer *anv_buffer,
                     struct anv_image *anv_image,
                     VkImageLayout image_layout,
                     uint32_t regionCount,
                     const VkBufferImageCopy* pRegions,
                     bool buffer_to_image)
{
   struct blorp_batch batch;
   blorp_batch_init(&cmd_buffer->device->blorp, &batch, cmd_buffer, 0);

   struct {
      struct blorp_surf surf;
      uint32_t level;
      VkOffset3D offset;
   } image, buffer, *src, *dst;

   buffer.level = 0;
   buffer.offset = (VkOffset3D) { 0, 0, 0 };

   if (buffer_to_image) {
      src = &buffer;
      dst = &image;
   } else {
      src = &image;
      dst = &buffer;
   }

   for (unsigned r = 0; r < regionCount; r++) {
      const VkImageAspectFlags aspect = pRegions[r].imageSubresource.aspectMask;

      get_blorp_surf_for_anv_image(cmd_buffer->device, anv_image, aspect,
                                   image_layout, ISL_AUX_USAGE_NONE,
                                   &image.surf);
      image.offset =
         anv_sanitize_image_offset(anv_image->type, pRegions[r].imageOffset);
      image.level = pRegions[r].imageSubresource.mipLevel;

      VkExtent3D extent =
         anv_sanitize_image_extent(anv_image->type, pRegions[r].imageExtent);
      if (anv_image->type != VK_IMAGE_TYPE_3D) {
         image.offset.z = pRegions[r].imageSubresource.baseArrayLayer;
         extent.depth =
            anv_get_layerCount(anv_image, &pRegions[r].imageSubresource);
      }

      const enum isl_format buffer_format =
         anv_get_isl_format(&cmd_buffer->device->info, anv_image->vk_format,
                            aspect, VK_IMAGE_TILING_LINEAR);

      const VkExtent3D bufferImageExtent = {
         .width  = pRegions[r].bufferRowLength ?
                   pRegions[r].bufferRowLength : extent.width,
         .height = pRegions[r].bufferImageHeight ?
                   pRegions[r].bufferImageHeight : extent.height,
      };

      const struct isl_format_layout *buffer_fmtl =
         isl_format_get_layout(buffer_format);

      const uint32_t buffer_row_pitch =
         DIV_ROUND_UP(bufferImageExtent.width, buffer_fmtl->bw) *
         (buffer_fmtl->bpb / 8);

      const uint32_t buffer_layer_stride =
         DIV_ROUND_UP(bufferImageExtent.height, buffer_fmtl->bh) *
         buffer_row_pitch;

      struct isl_surf buffer_isl_surf;
      get_blorp_surf_for_anv_buffer(cmd_buffer->device,
                                    anv_buffer, pRegions[r].bufferOffset,
                                    extent.width, extent.height,
                                    buffer_row_pitch, buffer_format,
                                    &buffer.surf, &buffer_isl_surf);

      bool dst_has_shadow = false;
      struct blorp_surf dst_shadow_surf;
      if (&image == dst) {
         anv_cmd_buffer_mark_image_written(cmd_buffer, anv_image,
                                           aspect, dst->surf.aux_usage,
                                           dst->level,
                                           dst->offset.z, extent.depth);

         dst_has_shadow =
            get_blorp_surf_for_anv_shadow_image(cmd_buffer->device,
                                                anv_image, aspect,
                                                &dst_shadow_surf);
      }

      for (unsigned z = 0; z < extent.depth; z++) {
         blorp_copy(&batch, &src->surf, src->level, src->offset.z,
                    &dst->surf, dst->level, dst->offset.z,
                    src->offset.x, src->offset.y, dst->offset.x, dst->offset.y,
                    extent.width, extent.height);

         if (dst_has_shadow) {
            blorp_copy(&batch, &src->surf, src->level, src->offset.z,
                       &dst_shadow_surf, dst->level, dst->offset.z,
                       src->offset.x, src->offset.y,
                       dst->offset.x, dst->offset.y,
                       extent.width, extent.height);
         }

         image.offset.z++;
         buffer.surf.addr.offset += buffer_layer_stride;
      }
   }

   blorp_batch_finish(&batch);
}

void anv_CmdCopyBufferToImage(
    VkCommandBuffer                             commandBuffer,
    VkBuffer                                    srcBuffer,
    VkImage                                     dstImage,
    VkImageLayout                               dstImageLayout,
    uint32_t                                    regionCount,
    const VkBufferImageCopy*                    pRegions)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_buffer, src_buffer, srcBuffer);
   ANV_FROM_HANDLE(anv_image, dst_image, dstImage);

   copy_buffer_to_image(cmd_buffer, src_buffer, dst_image, dstImageLayout,
                        regionCount, pRegions, true);
}

void anv_CmdCopyImageToBuffer(
    VkCommandBuffer                             commandBuffer,
    VkImage                                     srcImage,
    VkImageLayout                               srcImageLayout,
    VkBuffer                                    dstBuffer,
    uint32_t                                    regionCount,
    const VkBufferImageCopy*                    pRegions)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_image, src_image, srcImage);
   ANV_FROM_HANDLE(anv_buffer, dst_buffer, dstBuffer);

   copy_buffer_to_image(cmd_buffer, dst_buffer, src_image, srcImageLayout,
                        regionCount, pRegions, false);

   cmd_buffer->state.pending_pipe_bits |= ANV_PIPE_RENDER_TARGET_BUFFER_WRITES;
}

static bool
flip_coords(unsigned *src0, unsigned *src1, unsigned *dst0, unsigned *dst1)
{
   bool flip = false;
   if (*src0 > *src1) {
      unsigned tmp = *src0;
      *src0 = *src1;
      *src1 = tmp;
      flip = !flip;
   }

   if (*dst0 > *dst1) {
      unsigned tmp = *dst0;
      *dst0 = *dst1;
      *dst1 = tmp;
      flip = !flip;
   }

   return flip;
}

void anv_CmdBlitImage(
    VkCommandBuffer                             commandBuffer,
    VkImage                                     srcImage,
    VkImageLayout                               srcImageLayout,
    VkImage                                     dstImage,
    VkImageLayout                               dstImageLayout,
    uint32_t                                    regionCount,
    const VkImageBlit*                          pRegions,
    VkFilter                                    filter)

{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_image, src_image, srcImage);
   ANV_FROM_HANDLE(anv_image, dst_image, dstImage);

   struct blorp_surf src, dst;

   enum blorp_filter blorp_filter;
   switch (filter) {
   case VK_FILTER_NEAREST:
      blorp_filter = BLORP_FILTER_NEAREST;
      break;
   case VK_FILTER_LINEAR:
      blorp_filter = BLORP_FILTER_BILINEAR;
      break;
   default:
      unreachable("Invalid filter");
   }

   struct blorp_batch batch;
   blorp_batch_init(&cmd_buffer->device->blorp, &batch, cmd_buffer, 0);

   for (unsigned r = 0; r < regionCount; r++) {
      const VkImageSubresourceLayers *src_res = &pRegions[r].srcSubresource;
      const VkImageSubresourceLayers *dst_res = &pRegions[r].dstSubresource;

      assert(anv_image_aspects_compatible(src_res->aspectMask,
                                          dst_res->aspectMask));

      uint32_t aspect_bit;
      anv_foreach_image_aspect_bit(aspect_bit, src_image, src_res->aspectMask) {
         get_blorp_surf_for_anv_image(cmd_buffer->device,
                                      src_image, 1U << aspect_bit,
                                      srcImageLayout, ISL_AUX_USAGE_NONE, &src);
         get_blorp_surf_for_anv_image(cmd_buffer->device,
                                      dst_image, 1U << aspect_bit,
                                      dstImageLayout, ISL_AUX_USAGE_NONE, &dst);

         struct anv_format_plane src_format =
            anv_get_format_plane(&cmd_buffer->device->info, src_image->vk_format,
                                 1U << aspect_bit, src_image->tiling);
         struct anv_format_plane dst_format =
            anv_get_format_plane(&cmd_buffer->device->info, dst_image->vk_format,
                                 1U << aspect_bit, dst_image->tiling);

         unsigned dst_start, dst_end;
         if (dst_image->type == VK_IMAGE_TYPE_3D) {
            assert(dst_res->baseArrayLayer == 0);
            dst_start = pRegions[r].dstOffsets[0].z;
            dst_end = pRegions[r].dstOffsets[1].z;
         } else {
            dst_start = dst_res->baseArrayLayer;
            dst_end = dst_start + anv_get_layerCount(dst_image, dst_res);
         }

         unsigned src_start, src_end;
         if (src_image->type == VK_IMAGE_TYPE_3D) {
            assert(src_res->baseArrayLayer == 0);
            src_start = pRegions[r].srcOffsets[0].z;
            src_end = pRegions[r].srcOffsets[1].z;
         } else {
            src_start = src_res->baseArrayLayer;
            src_end = src_start + anv_get_layerCount(src_image, src_res);
         }

         bool flip_z = flip_coords(&src_start, &src_end, &dst_start, &dst_end);
         float src_z_step = (float)(src_end + 1 - src_start) /
            (float)(dst_end + 1 - dst_start);

         if (flip_z) {
            src_start = src_end;
            src_z_step *= -1;
         }

         unsigned src_x0 = pRegions[r].srcOffsets[0].x;
         unsigned src_x1 = pRegions[r].srcOffsets[1].x;
         unsigned dst_x0 = pRegions[r].dstOffsets[0].x;
         unsigned dst_x1 = pRegions[r].dstOffsets[1].x;
         bool flip_x = flip_coords(&src_x0, &src_x1, &dst_x0, &dst_x1);

         unsigned src_y0 = pRegions[r].srcOffsets[0].y;
         unsigned src_y1 = pRegions[r].srcOffsets[1].y;
         unsigned dst_y0 = pRegions[r].dstOffsets[0].y;
         unsigned dst_y1 = pRegions[r].dstOffsets[1].y;
         bool flip_y = flip_coords(&src_y0, &src_y1, &dst_y0, &dst_y1);

         const unsigned num_layers = dst_end - dst_start;
         anv_cmd_buffer_mark_image_written(cmd_buffer, dst_image,
                                           1U << aspect_bit,
                                           dst.aux_usage,
                                           dst_res->mipLevel,
                                           dst_start, num_layers);

         for (unsigned i = 0; i < num_layers; i++) {
            unsigned dst_z = dst_start + i;
            unsigned src_z = src_start + i * src_z_step;

            blorp_blit(&batch, &src, src_res->mipLevel, src_z,
                       src_format.isl_format, src_format.swizzle,
                       &dst, dst_res->mipLevel, dst_z,
                       dst_format.isl_format, dst_format.swizzle,
                       src_x0, src_y0, src_x1, src_y1,
                       dst_x0, dst_y0, dst_x1, dst_y1,
                       blorp_filter, flip_x, flip_y);
         }
      }
   }

   blorp_batch_finish(&batch);
}

static enum isl_format
isl_format_for_size(unsigned size_B)
{
   switch (size_B) {
   case 4:  return ISL_FORMAT_R32_UINT;
   case 8:  return ISL_FORMAT_R32G32_UINT;
   case 16: return ISL_FORMAT_R32G32B32A32_UINT;
   default:
      unreachable("Not a power-of-two format size");
   }
}

/**
 * Returns the greatest common divisor of a and b that is a power of two.
 */
static uint64_t
gcd_pow2_u64(uint64_t a, uint64_t b)
{
   assert(a > 0 || b > 0);

   unsigned a_log2 = ffsll(a) - 1;
   unsigned b_log2 = ffsll(b) - 1;

   /* If either a or b is 0, then a_log2 or b_log2 till be UINT_MAX in which
    * case, the MIN2() will take the other one.  If both are 0 then we will
    * hit the assert above.
    */
   return 1 << MIN2(a_log2, b_log2);
}

/* This is maximum possible width/height our HW can handle */
#define MAX_SURFACE_DIM (1ull << 14)

void anv_CmdCopyBuffer(
    VkCommandBuffer                             commandBuffer,
    VkBuffer                                    srcBuffer,
    VkBuffer                                    dstBuffer,
    uint32_t                                    regionCount,
    const VkBufferCopy*                         pRegions)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_buffer, src_buffer, srcBuffer);
   ANV_FROM_HANDLE(anv_buffer, dst_buffer, dstBuffer);

   struct blorp_batch batch;
   blorp_batch_init(&cmd_buffer->device->blorp, &batch, cmd_buffer, 0);

   for (unsigned r = 0; r < regionCount; r++) {
      struct blorp_address src = {
         .buffer = src_buffer->address.bo,
         .offset = src_buffer->address.offset + pRegions[r].srcOffset,
         .mocs = anv_mocs_for_bo(cmd_buffer->device, src_buffer->address.bo),
      };
      struct blorp_address dst = {
         .buffer = dst_buffer->address.bo,
         .offset = dst_buffer->address.offset + pRegions[r].dstOffset,
         .mocs = anv_mocs_for_bo(cmd_buffer->device, dst_buffer->address.bo),
      };

      blorp_buffer_copy(&batch, src, dst, pRegions[r].size);
   }

   blorp_batch_finish(&batch);

   cmd_buffer->state.pending_pipe_bits |= ANV_PIPE_RENDER_TARGET_BUFFER_WRITES;
}

void anv_CmdUpdateBuffer(
    VkCommandBuffer                             commandBuffer,
    VkBuffer                                    dstBuffer,
    VkDeviceSize                                dstOffset,
    VkDeviceSize                                dataSize,
    const void*                                 pData)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_buffer, dst_buffer, dstBuffer);

   struct blorp_batch batch;
   blorp_batch_init(&cmd_buffer->device->blorp, &batch, cmd_buffer, 0);

   /* We can't quite grab a full block because the state stream needs a
    * little data at the top to build its linked list.
    */
   const uint32_t max_update_size =
      cmd_buffer->device->dynamic_state_pool.block_size - 64;

   assert(max_update_size < MAX_SURFACE_DIM * 4);

   /* We're about to read data that was written from the CPU.  Flush the
    * texture cache so we don't get anything stale.
    */
   cmd_buffer->state.pending_pipe_bits |= ANV_PIPE_TEXTURE_CACHE_INVALIDATE_BIT;

   while (dataSize) {
      const uint32_t copy_size = MIN2(dataSize, max_update_size);

      struct anv_state tmp_data =
         anv_cmd_buffer_alloc_dynamic_state(cmd_buffer, copy_size, 64);

      memcpy(tmp_data.map, pData, copy_size);

      struct blorp_address src = {
         .buffer = cmd_buffer->device->dynamic_state_pool.block_pool.bo,
         .offset = tmp_data.offset,
         .mocs = cmd_buffer->device->default_mocs,
      };
      struct blorp_address dst = {
         .buffer = dst_buffer->address.bo,
         .offset = dst_buffer->address.offset + dstOffset,
         .mocs = anv_mocs_for_bo(cmd_buffer->device, dst_buffer->address.bo),
      };

      blorp_buffer_copy(&batch, src, dst, copy_size);

      dataSize -= copy_size;
      dstOffset += copy_size;
      pData = (void *)pData + copy_size;
   }

   blorp_batch_finish(&batch);

   cmd_buffer->state.pending_pipe_bits |= ANV_PIPE_RENDER_TARGET_BUFFER_WRITES;
}

void anv_CmdFillBuffer(
    VkCommandBuffer                             commandBuffer,
    VkBuffer                                    dstBuffer,
    VkDeviceSize                                dstOffset,
    VkDeviceSize                                fillSize,
    uint32_t                                    data)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_buffer, dst_buffer, dstBuffer);
   struct blorp_surf surf;
   struct isl_surf isl_surf;

   struct blorp_batch batch;
   blorp_batch_init(&cmd_buffer->device->blorp, &batch, cmd_buffer, 0);

   fillSize = anv_buffer_get_range(dst_buffer, dstOffset, fillSize);

   /* From the Vulkan spec:
    *
    *    "size is the number of bytes to fill, and must be either a multiple
    *    of 4, or VK_WHOLE_SIZE to fill the range from offset to the end of
    *    the buffer. If VK_WHOLE_SIZE is used and the remaining size of the
    *    buffer is not a multiple of 4, then the nearest smaller multiple is
    *    used."
    */
   fillSize &= ~3ull;

   /* First, we compute the biggest format that can be used with the
    * given offsets and size.
    */
   int bs = 16;
   bs = gcd_pow2_u64(bs, dstOffset);
   bs = gcd_pow2_u64(bs, fillSize);
   enum isl_format isl_format = isl_format_for_size(bs);

   union isl_color_value color = {
      .u32 = { data, data, data, data },
   };

   const uint64_t max_fill_size = MAX_SURFACE_DIM * MAX_SURFACE_DIM * bs;
   while (fillSize >= max_fill_size) {
      get_blorp_surf_for_anv_buffer(cmd_buffer->device,
                                    dst_buffer, dstOffset,
                                    MAX_SURFACE_DIM, MAX_SURFACE_DIM,
                                    MAX_SURFACE_DIM * bs, isl_format,
                                    &surf, &isl_surf);

      blorp_clear(&batch, &surf, isl_format, ISL_SWIZZLE_IDENTITY,
                  0, 0, 1, 0, 0, MAX_SURFACE_DIM, MAX_SURFACE_DIM,
                  color, NULL);
      fillSize -= max_fill_size;
      dstOffset += max_fill_size;
   }

   uint64_t height = fillSize / (MAX_SURFACE_DIM * bs);
   assert(height < MAX_SURFACE_DIM);
   if (height != 0) {
      const uint64_t rect_fill_size = height * MAX_SURFACE_DIM * bs;
      get_blorp_surf_for_anv_buffer(cmd_buffer->device,
                                    dst_buffer, dstOffset,
                                    MAX_SURFACE_DIM, height,
                                    MAX_SURFACE_DIM * bs, isl_format,
                                    &surf, &isl_surf);

      blorp_clear(&batch, &surf, isl_format, ISL_SWIZZLE_IDENTITY,
                  0, 0, 1, 0, 0, MAX_SURFACE_DIM, height,
                  color, NULL);
      fillSize -= rect_fill_size;
      dstOffset += rect_fill_size;
   }

   if (fillSize != 0) {
      const uint32_t width = fillSize / bs;
      get_blorp_surf_for_anv_buffer(cmd_buffer->device,
                                    dst_buffer, dstOffset,
                                    width, 1,
                                    width * bs, isl_format,
                                    &surf, &isl_surf);

      blorp_clear(&batch, &surf, isl_format, ISL_SWIZZLE_IDENTITY,
                  0, 0, 1, 0, 0, width, 1,
                  color, NULL);
   }

   blorp_batch_finish(&batch);

   cmd_buffer->state.pending_pipe_bits |= ANV_PIPE_RENDER_TARGET_BUFFER_WRITES;
}

void anv_CmdClearColorImage(
    VkCommandBuffer                             commandBuffer,
    VkImage                                     _image,
    VkImageLayout                               imageLayout,
    const VkClearColorValue*                    pColor,
    uint32_t                                    rangeCount,
    const VkImageSubresourceRange*              pRanges)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_image, image, _image);

   static const bool color_write_disable[4] = { false, false, false, false };

   struct blorp_batch batch;
   blorp_batch_init(&cmd_buffer->device->blorp, &batch, cmd_buffer, 0);


   for (unsigned r = 0; r < rangeCount; r++) {
      if (pRanges[r].aspectMask == 0)
         continue;

      assert(pRanges[r].aspectMask & VK_IMAGE_ASPECT_ANY_COLOR_BIT_ANV);

      struct blorp_surf surf;
      get_blorp_surf_for_anv_image(cmd_buffer->device,
                                   image, pRanges[r].aspectMask,
                                   imageLayout, ISL_AUX_USAGE_NONE, &surf);

      struct anv_format_plane src_format =
         anv_get_format_plane(&cmd_buffer->device->info, image->vk_format,
                              VK_IMAGE_ASPECT_COLOR_BIT, image->tiling);

      unsigned base_layer = pRanges[r].baseArrayLayer;
      unsigned layer_count = anv_get_layerCount(image, &pRanges[r]);

      for (unsigned i = 0; i < anv_get_levelCount(image, &pRanges[r]); i++) {
         const unsigned level = pRanges[r].baseMipLevel + i;
         const unsigned level_width = anv_minify(image->extent.width, level);
         const unsigned level_height = anv_minify(image->extent.height, level);

         if (image->type == VK_IMAGE_TYPE_3D) {
            base_layer = 0;
            layer_count = anv_minify(image->extent.depth, level);
         }

         anv_cmd_buffer_mark_image_written(cmd_buffer, image,
                                           pRanges[r].aspectMask,
                                           surf.aux_usage, level,
                                           base_layer, layer_count);

         blorp_clear(&batch, &surf,
                     src_format.isl_format, src_format.swizzle,
                     level, base_layer, layer_count,
                     0, 0, level_width, level_height,
                     vk_to_isl_color(*pColor), color_write_disable);
      }
   }

   blorp_batch_finish(&batch);
}

void anv_CmdClearDepthStencilImage(
    VkCommandBuffer                             commandBuffer,
    VkImage                                     image_h,
    VkImageLayout                               imageLayout,
    const VkClearDepthStencilValue*             pDepthStencil,
    uint32_t                                    rangeCount,
    const VkImageSubresourceRange*              pRanges)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_image, image, image_h);

   struct blorp_batch batch;
   blorp_batch_init(&cmd_buffer->device->blorp, &batch, cmd_buffer, 0);

   struct blorp_surf depth, stencil, stencil_shadow;
   if (image->aspects & VK_IMAGE_ASPECT_DEPTH_BIT) {
      get_blorp_surf_for_anv_image(cmd_buffer->device,
                                   image, VK_IMAGE_ASPECT_DEPTH_BIT,
                                   imageLayout, ISL_AUX_USAGE_NONE, &depth);
   } else {
      memset(&depth, 0, sizeof(depth));
   }

   bool has_stencil_shadow = false;
   if (image->aspects & VK_IMAGE_ASPECT_STENCIL_BIT) {
      get_blorp_surf_for_anv_image(cmd_buffer->device,
                                   image, VK_IMAGE_ASPECT_STENCIL_BIT,
                                   imageLayout, ISL_AUX_USAGE_NONE, &stencil);

      has_stencil_shadow =
         get_blorp_surf_for_anv_shadow_image(cmd_buffer->device, image,
                                             VK_IMAGE_ASPECT_STENCIL_BIT,
                                             &stencil_shadow);
   } else {
      memset(&stencil, 0, sizeof(stencil));
   }

   for (unsigned r = 0; r < rangeCount; r++) {
      if (pRanges[r].aspectMask == 0)
         continue;

      bool clear_depth = pRanges[r].aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT;
      bool clear_stencil = pRanges[r].aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT;

      unsigned base_layer = pRanges[r].baseArrayLayer;
      unsigned layer_count = anv_get_layerCount(image, &pRanges[r]);

      for (unsigned i = 0; i < anv_get_levelCount(image, &pRanges[r]); i++) {
         const unsigned level = pRanges[r].baseMipLevel + i;
         const unsigned level_width = anv_minify(image->extent.width, level);
         const unsigned level_height = anv_minify(image->extent.height, level);

         if (image->type == VK_IMAGE_TYPE_3D)
            layer_count = anv_minify(image->extent.depth, level);

         blorp_clear_depth_stencil(&batch, &depth, &stencil,
                                   level, base_layer, layer_count,
                                   0, 0, level_width, level_height,
                                   clear_depth, pDepthStencil->depth,
                                   clear_stencil ? 0xff : 0,
                                   pDepthStencil->stencil);

         if (clear_stencil && has_stencil_shadow) {
            union isl_color_value stencil_color = {
               .u32 = { pDepthStencil->stencil, },
            };
            blorp_clear(&batch, &stencil_shadow,
                        ISL_FORMAT_R8_UINT, ISL_SWIZZLE_IDENTITY,
                        level, base_layer, layer_count,
                        0, 0, level_width, level_height,
                        stencil_color, NULL);
         }
      }
   }

   blorp_batch_finish(&batch);
}

VkResult
anv_cmd_buffer_alloc_blorp_binding_table(struct anv_cmd_buffer *cmd_buffer,
                                         uint32_t num_entries,
                                         uint32_t *state_offset,
                                         struct anv_state *bt_state)
{
   *bt_state = anv_cmd_buffer_alloc_binding_table(cmd_buffer, num_entries,
                                                  state_offset);
   if (bt_state->map == NULL) {
      /* We ran out of space.  Grab a new binding table block. */
      VkResult result = anv_cmd_buffer_new_binding_table_block(cmd_buffer);
      if (result != VK_SUCCESS)
         return result;

      /* Re-emit state base addresses so we get the new surface state base
       * address before we start emitting binding tables etc.
       */
      anv_cmd_buffer_emit_state_base_address(cmd_buffer);

      *bt_state = anv_cmd_buffer_alloc_binding_table(cmd_buffer, num_entries,
                                                     state_offset);
      assert(bt_state->map != NULL);
   }

   return VK_SUCCESS;
}

static VkResult
binding_table_for_surface_state(struct anv_cmd_buffer *cmd_buffer,
                                struct anv_state surface_state,
                                uint32_t *bt_offset)
{
   uint32_t state_offset;
   struct anv_state bt_state;

   VkResult result =
      anv_cmd_buffer_alloc_blorp_binding_table(cmd_buffer, 1, &state_offset,
                                               &bt_state);
   if (result != VK_SUCCESS)
      return result;

   uint32_t *bt_map = bt_state.map;
   bt_map[0] = surface_state.offset + state_offset;

   *bt_offset = bt_state.offset;
   return VK_SUCCESS;
}

static void
clear_color_attachment(struct anv_cmd_buffer *cmd_buffer,
                       struct blorp_batch *batch,
                       const VkClearAttachment *attachment,
                       uint32_t rectCount, const VkClearRect *pRects)
{
   const struct anv_subpass *subpass = cmd_buffer->state.subpass;
   const uint32_t color_att = attachment->colorAttachment;
   assert(color_att < subpass->color_count);
   const uint32_t att_idx = subpass->color_attachments[color_att].attachment;

   if (att_idx == VK_ATTACHMENT_UNUSED)
      return;

   struct anv_render_pass_attachment *pass_att =
      &cmd_buffer->state.pass->attachments[att_idx];
   struct anv_attachment_state *att_state =
      &cmd_buffer->state.attachments[att_idx];

   uint32_t binding_table;
   VkResult result =
      binding_table_for_surface_state(cmd_buffer, att_state->color.state,
                                      &binding_table);
   if (result != VK_SUCCESS)
      return;

   union isl_color_value clear_color =
      vk_to_isl_color(attachment->clearValue.color);

   /* If multiview is enabled we ignore baseArrayLayer and layerCount */
   if (subpass->view_mask) {
      uint32_t view_idx;
      for_each_bit(view_idx, subpass->view_mask) {
         for (uint32_t r = 0; r < rectCount; ++r) {
            const VkOffset2D offset = pRects[r].rect.offset;
            const VkExtent2D extent = pRects[r].rect.extent;
            blorp_clear_attachments(batch, binding_table,
                                    ISL_FORMAT_UNSUPPORTED, pass_att->samples,
                                    view_idx, 1,
                                    offset.x, offset.y,
                                    offset.x + extent.width,
                                    offset.y + extent.height,
                                    true, clear_color, false, 0.0f, 0, 0);
         }
      }
      return;
   }

   for (uint32_t r = 0; r < rectCount; ++r) {
      const VkOffset2D offset = pRects[r].rect.offset;
      const VkExtent2D extent = pRects[r].rect.extent;
      assert(pRects[r].layerCount != VK_REMAINING_ARRAY_LAYERS);
      blorp_clear_attachments(batch, binding_table,
                              ISL_FORMAT_UNSUPPORTED, pass_att->samples,
                              pRects[r].baseArrayLayer,
                              pRects[r].layerCount,
                              offset.x, offset.y,
                              offset.x + extent.width, offset.y + extent.height,
                              true, clear_color, false, 0.0f, 0, 0);
   }
}

static void
clear_depth_stencil_attachment(struct anv_cmd_buffer *cmd_buffer,
                               struct blorp_batch *batch,
                               const VkClearAttachment *attachment,
                               uint32_t rectCount, const VkClearRect *pRects)
{
   static const union isl_color_value color_value = { .u32 = { 0, } };
   const struct anv_subpass *subpass = cmd_buffer->state.subpass;
   if (!subpass->depth_stencil_attachment)
      return;

   const uint32_t att_idx = subpass->depth_stencil_attachment->attachment;
   assert(att_idx != VK_ATTACHMENT_UNUSED);
   struct anv_render_pass_attachment *pass_att =
      &cmd_buffer->state.pass->attachments[att_idx];

   bool clear_depth = attachment->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT;
   bool clear_stencil = attachment->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT;

   enum isl_format depth_format = ISL_FORMAT_UNSUPPORTED;
   if (clear_depth) {
      depth_format = anv_get_isl_format(&cmd_buffer->device->info,
                                        pass_att->format,
                                        VK_IMAGE_ASPECT_DEPTH_BIT,
                                        VK_IMAGE_TILING_OPTIMAL);
   }

   uint32_t binding_table;
   VkResult result =
      binding_table_for_surface_state(cmd_buffer,
                                      cmd_buffer->state.null_surface_state,
                                      &binding_table);
   if (result != VK_SUCCESS)
      return;

   /* If multiview is enabled we ignore baseArrayLayer and layerCount */
   if (subpass->view_mask) {
      uint32_t view_idx;
      for_each_bit(view_idx, subpass->view_mask) {
         for (uint32_t r = 0; r < rectCount; ++r) {
            const VkOffset2D offset = pRects[r].rect.offset;
            const VkExtent2D extent = pRects[r].rect.extent;
            VkClearDepthStencilValue value = attachment->clearValue.depthStencil;
            blorp_clear_attachments(batch, binding_table,
                                    depth_format, pass_att->samples,
                                    view_idx, 1,
                                    offset.x, offset.y,
                                    offset.x + extent.width,
                                    offset.y + extent.height,
                                    false, color_value,
                                    clear_depth, value.depth,
                                    clear_stencil ? 0xff : 0, value.stencil);
         }
      }
      return;
   }

   for (uint32_t r = 0; r < rectCount; ++r) {
      const VkOffset2D offset = pRects[r].rect.offset;
      const VkExtent2D extent = pRects[r].rect.extent;
      VkClearDepthStencilValue value = attachment->clearValue.depthStencil;
      assert(pRects[r].layerCount != VK_REMAINING_ARRAY_LAYERS);
      blorp_clear_attachments(batch, binding_table,
                              depth_format, pass_att->samples,
                              pRects[r].baseArrayLayer,
                              pRects[r].layerCount,
                              offset.x, offset.y,
                              offset.x + extent.width, offset.y + extent.height,
                              false, color_value,
                              clear_depth, value.depth,
                              clear_stencil ? 0xff : 0, value.stencil);
   }
}

void anv_CmdClearAttachments(
    VkCommandBuffer                             commandBuffer,
    uint32_t                                    attachmentCount,
    const VkClearAttachment*                    pAttachments,
    uint32_t                                    rectCount,
    const VkClearRect*                          pRects)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);

   /* Because this gets called within a render pass, we tell blorp not to
    * trash our depth and stencil buffers.
    */
   struct blorp_batch batch;
   enum blorp_batch_flags flags = BLORP_BATCH_NO_EMIT_DEPTH_STENCIL;
   if (cmd_buffer->state.conditional_render_enabled) {
      anv_cmd_emit_conditional_render_predicate(cmd_buffer);
      flags |= BLORP_BATCH_PREDICATE_ENABLE;
   }
   blorp_batch_init(&cmd_buffer->device->blorp, &batch, cmd_buffer, flags);

   for (uint32_t a = 0; a < attachmentCount; ++a) {
      if (pAttachments[a].aspectMask & VK_IMAGE_ASPECT_ANY_COLOR_BIT_ANV) {
         assert(pAttachments[a].aspectMask == VK_IMAGE_ASPECT_COLOR_BIT);
         clear_color_attachment(cmd_buffer, &batch,
                                &pAttachments[a],
                                rectCount, pRects);
      } else {
         clear_depth_stencil_attachment(cmd_buffer, &batch,
                                        &pAttachments[a],
                                        rectCount, pRects);
      }
   }

   blorp_batch_finish(&batch);
}

enum subpass_stage {
   SUBPASS_STAGE_LOAD,
   SUBPASS_STAGE_DRAW,
   SUBPASS_STAGE_RESOLVE,
};

void
anv_image_msaa_resolve(struct anv_cmd_buffer *cmd_buffer,
                       const struct anv_image *src_image,
                       enum isl_aux_usage src_aux_usage,
                       uint32_t src_level, uint32_t src_base_layer,
                       const struct anv_image *dst_image,
                       enum isl_aux_usage dst_aux_usage,
                       uint32_t dst_level, uint32_t dst_base_layer,
                       VkImageAspectFlagBits aspect,
                       uint32_t src_x, uint32_t src_y,
                       uint32_t dst_x, uint32_t dst_y,
                       uint32_t width, uint32_t height,
                       uint32_t layer_count,
                       enum blorp_filter filter)
{
   struct blorp_batch batch;
   blorp_batch_init(&cmd_buffer->device->blorp, &batch, cmd_buffer, 0);

   assert(src_image->type == VK_IMAGE_TYPE_2D);
   assert(src_image->samples > 1);
   assert(dst_image->type == VK_IMAGE_TYPE_2D);
   assert(dst_image->samples == 1);
   assert(src_image->n_planes == dst_image->n_planes);
   assert(!src_image->format->can_ycbcr);
   assert(!dst_image->format->can_ycbcr);

   struct blorp_surf src_surf, dst_surf;
   get_blorp_surf_for_anv_image(cmd_buffer->device, src_image, aspect,
                                ANV_IMAGE_LAYOUT_EXPLICIT_AUX,
                                src_aux_usage, &src_surf);
   if (src_aux_usage == ISL_AUX_USAGE_MCS) {
      src_surf.clear_color_addr = anv_to_blorp_address(
         anv_image_get_clear_color_addr(cmd_buffer->device, src_image,
                                        VK_IMAGE_ASPECT_COLOR_BIT));
   }
   get_blorp_surf_for_anv_image(cmd_buffer->device, dst_image, aspect,
                                ANV_IMAGE_LAYOUT_EXPLICIT_AUX,
                                dst_aux_usage, &dst_surf);
   anv_cmd_buffer_mark_image_written(cmd_buffer, dst_image,
                                     aspect, dst_aux_usage,
                                     dst_level, dst_base_layer, layer_count);

   if (filter == BLORP_FILTER_NONE) {
      /* If no explicit filter is provided, then it's implied by the type of
       * the source image.
       */
      if ((src_surf.surf->usage & ISL_SURF_USAGE_DEPTH_BIT) ||
          (src_surf.surf->usage & ISL_SURF_USAGE_STENCIL_BIT) ||
          isl_format_has_int_channel(src_surf.surf->format)) {
         filter = BLORP_FILTER_SAMPLE_0;
      } else {
         filter = BLORP_FILTER_AVERAGE;
      }
   }

   for (uint32_t l = 0; l < layer_count; l++) {
      blorp_blit(&batch,
                 &src_surf, src_level, src_base_layer + l,
                 ISL_FORMAT_UNSUPPORTED, ISL_SWIZZLE_IDENTITY,
                 &dst_surf, dst_level, dst_base_layer + l,
                 ISL_FORMAT_UNSUPPORTED, ISL_SWIZZLE_IDENTITY,
                 src_x, src_y, src_x + width, src_y + height,
                 dst_x, dst_y, dst_x + width, dst_y + height,
                 filter, false, false);
   }

   blorp_batch_finish(&batch);
}

void anv_CmdResolveImage(
    VkCommandBuffer                             commandBuffer,
    VkImage                                     srcImage,
    VkImageLayout                               srcImageLayout,
    VkImage                                     dstImage,
    VkImageLayout                               dstImageLayout,
    uint32_t                                    regionCount,
    const VkImageResolve*                       pRegions)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_image, src_image, srcImage);
   ANV_FROM_HANDLE(anv_image, dst_image, dstImage);

   assert(!src_image->format->can_ycbcr);

   for (uint32_t r = 0; r < regionCount; r++) {
      assert(pRegions[r].srcSubresource.aspectMask ==
             pRegions[r].dstSubresource.aspectMask);
      assert(anv_get_layerCount(src_image, &pRegions[r].srcSubresource) ==
             anv_get_layerCount(dst_image, &pRegions[r].dstSubresource));

      const uint32_t layer_count =
         anv_get_layerCount(dst_image, &pRegions[r].dstSubresource);

      uint32_t aspect_bit;
      anv_foreach_image_aspect_bit(aspect_bit, src_image,
                                   pRegions[r].srcSubresource.aspectMask) {
         enum isl_aux_usage src_aux_usage =
            anv_layout_to_aux_usage(&cmd_buffer->device->info, src_image,
                                    (1 << aspect_bit), srcImageLayout);
         enum isl_aux_usage dst_aux_usage =
            anv_layout_to_aux_usage(&cmd_buffer->device->info, dst_image,
                                    (1 << aspect_bit), dstImageLayout);

         anv_image_msaa_resolve(cmd_buffer,
                                src_image, src_aux_usage,
                                pRegions[r].srcSubresource.mipLevel,
                                pRegions[r].srcSubresource.baseArrayLayer,
                                dst_image, dst_aux_usage,
                                pRegions[r].dstSubresource.mipLevel,
                                pRegions[r].dstSubresource.baseArrayLayer,
                                (1 << aspect_bit),
                                pRegions[r].srcOffset.x,
                                pRegions[r].srcOffset.y,
                                pRegions[r].dstOffset.x,
                                pRegions[r].dstOffset.y,
                                pRegions[r].extent.width,
                                pRegions[r].extent.height,
                                layer_count, BLORP_FILTER_NONE);
      }
   }
}

static enum isl_aux_usage
fast_clear_aux_usage(const struct anv_image *image,
                     VkImageAspectFlagBits aspect)
{
   uint32_t plane = anv_image_aspect_to_plane(image->aspects, aspect);
   if (image->planes[plane].aux_usage == ISL_AUX_USAGE_NONE)
      return ISL_AUX_USAGE_CCS_D;
   else
      return image->planes[plane].aux_usage;
}

void
anv_image_copy_to_shadow(struct anv_cmd_buffer *cmd_buffer,
                         const struct anv_image *image,
                         VkImageAspectFlagBits aspect,
                         uint32_t base_level, uint32_t level_count,
                         uint32_t base_layer, uint32_t layer_count)
{
   struct blorp_batch batch;
   blorp_batch_init(&cmd_buffer->device->blorp, &batch, cmd_buffer, 0);

   /* We don't know who touched the main surface last so flush a bunch of
    * caches to ensure we get good data.
    */
   cmd_buffer->state.pending_pipe_bits |=
      ANV_PIPE_DEPTH_CACHE_FLUSH_BIT |
      ANV_PIPE_DATA_CACHE_FLUSH_BIT |
      ANV_PIPE_RENDER_TARGET_CACHE_FLUSH_BIT |
      ANV_PIPE_TEXTURE_CACHE_INVALIDATE_BIT;

   struct blorp_surf surf;
   get_blorp_surf_for_anv_image(cmd_buffer->device,
                                image, aspect,
                                VK_IMAGE_LAYOUT_GENERAL,
                                ISL_AUX_USAGE_NONE, &surf);
   assert(surf.aux_usage == ISL_AUX_USAGE_NONE);

   struct blorp_surf shadow_surf;
   get_blorp_surf_for_anv_shadow_image(cmd_buffer->device,
                                       image, aspect, &shadow_surf);

   for (uint32_t l = 0; l < level_count; l++) {
      const uint32_t level = base_level + l;

      const VkExtent3D extent = {
         .width = anv_minify(image->extent.width, level),
         .height = anv_minify(image->extent.height, level),
         .depth = anv_minify(image->extent.depth, level),
      };

      if (image->type == VK_IMAGE_TYPE_3D)
         layer_count = extent.depth;

      for (uint32_t a = 0; a < layer_count; a++) {
         const uint32_t layer = base_layer + a;

         blorp_copy(&batch, &surf, level, layer,
                    &shadow_surf, level, layer,
                    0, 0, 0, 0, extent.width, extent.height);
      }
   }

   /* We just wrote to the buffer with the render cache.  Flush it. */
   cmd_buffer->state.pending_pipe_bits |=
      ANV_PIPE_RENDER_TARGET_CACHE_FLUSH_BIT;

   blorp_batch_finish(&batch);
}

void
anv_image_clear_color(struct anv_cmd_buffer *cmd_buffer,
                      const struct anv_image *image,
                      VkImageAspectFlagBits aspect,
                      enum isl_aux_usage aux_usage,
                      enum isl_format format, struct isl_swizzle swizzle,
                      uint32_t level, uint32_t base_layer, uint32_t layer_count,
                      VkRect2D area, union isl_color_value clear_color)
{
   assert(image->aspects == VK_IMAGE_ASPECT_COLOR_BIT);

   /* We don't support planar images with multisampling yet */
   assert(image->n_planes == 1);

   struct blorp_batch batch;
   blorp_batch_init(&cmd_buffer->device->blorp, &batch, cmd_buffer, 0);

   struct blorp_surf surf;
   get_blorp_surf_for_anv_image(cmd_buffer->device, image, aspect,
                                ANV_IMAGE_LAYOUT_EXPLICIT_AUX,
                                aux_usage, &surf);
   anv_cmd_buffer_mark_image_written(cmd_buffer, image, aspect, aux_usage,
                                     level, base_layer, layer_count);

   blorp_clear(&batch, &surf, format, anv_swizzle_for_render(swizzle),
               level, base_layer, layer_count,
               area.offset.x, area.offset.y,
               area.offset.x + area.extent.width,
               area.offset.y + area.extent.height,
               clear_color, NULL);

   blorp_batch_finish(&batch);
}

void
anv_image_clear_depth_stencil(struct anv_cmd_buffer *cmd_buffer,
                              const struct anv_image *image,
                              VkImageAspectFlags aspects,
                              enum isl_aux_usage depth_aux_usage,
                              uint32_t level,
                              uint32_t base_layer, uint32_t layer_count,
                              VkRect2D area,
                              float depth_value, uint8_t stencil_value)
{
   assert(image->aspects & (VK_IMAGE_ASPECT_DEPTH_BIT |
                            VK_IMAGE_ASPECT_STENCIL_BIT));

   struct blorp_batch batch;
   blorp_batch_init(&cmd_buffer->device->blorp, &batch, cmd_buffer, 0);

   struct blorp_surf depth = {};
   if (aspects & VK_IMAGE_ASPECT_DEPTH_BIT) {
      get_blorp_surf_for_anv_image(cmd_buffer->device,
                                   image, VK_IMAGE_ASPECT_DEPTH_BIT,
                                   ANV_IMAGE_LAYOUT_EXPLICIT_AUX,
                                   depth_aux_usage, &depth);
      depth.clear_color.f32[0] = ANV_HZ_FC_VAL;
   }

   struct blorp_surf stencil = {};
   if (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) {
      get_blorp_surf_for_anv_image(cmd_buffer->device,
                                   image, VK_IMAGE_ASPECT_STENCIL_BIT,
                                   ANV_IMAGE_LAYOUT_EXPLICIT_AUX,
                                   ISL_AUX_USAGE_NONE, &stencil);
   }

   /* Blorp may choose to clear stencil using RGBA32_UINT for better
    * performance.  If it does this, we need to flush it out of the depth
    * cache before rendering to it.
    */
   cmd_buffer->state.pending_pipe_bits |=
      ANV_PIPE_DEPTH_CACHE_FLUSH_BIT | ANV_PIPE_CS_STALL_BIT;

   blorp_clear_depth_stencil(&batch, &depth, &stencil,
                             level, base_layer, layer_count,
                             area.offset.x, area.offset.y,
                             area.offset.x + area.extent.width,
                             area.offset.y + area.extent.height,
                             aspects & VK_IMAGE_ASPECT_DEPTH_BIT,
                             depth_value,
                             (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) ? 0xff : 0,
                             stencil_value);

   /* Blorp may choose to clear stencil using RGBA32_UINT for better
    * performance.  If it does this, we need to flush it out of the render
    * cache before someone starts trying to do stencil on it.
    */
   cmd_buffer->state.pending_pipe_bits |=
      ANV_PIPE_RENDER_TARGET_CACHE_FLUSH_BIT | ANV_PIPE_CS_STALL_BIT;

   struct blorp_surf stencil_shadow;
   if ((aspects & VK_IMAGE_ASPECT_STENCIL_BIT) &&
       get_blorp_surf_for_anv_shadow_image(cmd_buffer->device, image,
                                           VK_IMAGE_ASPECT_STENCIL_BIT,
                                           &stencil_shadow)) {
      union isl_color_value stencil_color = {
         .u32 = { stencil_value },
      };
      blorp_clear(&batch, &stencil_shadow,
                  ISL_FORMAT_R8_UINT, ISL_SWIZZLE_IDENTITY,
                  level, base_layer, layer_count,
                  area.offset.x, area.offset.y,
                  area.offset.x + area.extent.width,
                  area.offset.y + area.extent.height,
                  stencil_color, NULL);
   }

   blorp_batch_finish(&batch);
}

void
anv_image_hiz_op(struct anv_cmd_buffer *cmd_buffer,
                 const struct anv_image *image,
                 VkImageAspectFlagBits aspect, uint32_t level,
                 uint32_t base_layer, uint32_t layer_count,
                 enum isl_aux_op hiz_op)
{
   assert(aspect == VK_IMAGE_ASPECT_DEPTH_BIT);
   assert(base_layer + layer_count <= anv_image_aux_layers(image, aspect, level));
   assert(anv_image_aspect_to_plane(image->aspects,
                                    VK_IMAGE_ASPECT_DEPTH_BIT) == 0);

   struct blorp_batch batch;
   blorp_batch_init(&cmd_buffer->device->blorp, &batch, cmd_buffer, 0);

   struct blorp_surf surf;
   get_blorp_surf_for_anv_image(cmd_buffer->device,
                                image, VK_IMAGE_ASPECT_DEPTH_BIT,
                                ANV_IMAGE_LAYOUT_EXPLICIT_AUX,
                                ISL_AUX_USAGE_HIZ, &surf);
   surf.clear_color.f32[0] = ANV_HZ_FC_VAL;

   blorp_hiz_op(&batch, &surf, level, base_layer, layer_count, hiz_op);

   blorp_batch_finish(&batch);
}

void
anv_image_hiz_clear(struct anv_cmd_buffer *cmd_buffer,
                    const struct anv_image *image,
                    VkImageAspectFlags aspects,
                    uint32_t level,
                    uint32_t base_layer, uint32_t layer_count,
                    VkRect2D area, uint8_t stencil_value)
{
   assert(image->aspects & (VK_IMAGE_ASPECT_DEPTH_BIT |
                            VK_IMAGE_ASPECT_STENCIL_BIT));

   struct blorp_batch batch;
   blorp_batch_init(&cmd_buffer->device->blorp, &batch, cmd_buffer, 0);

   struct blorp_surf depth = {};
   if (aspects & VK_IMAGE_ASPECT_DEPTH_BIT) {
      assert(base_layer + layer_count <=
             anv_image_aux_layers(image, VK_IMAGE_ASPECT_DEPTH_BIT, level));
      get_blorp_surf_for_anv_image(cmd_buffer->device,
                                   image, VK_IMAGE_ASPECT_DEPTH_BIT,
                                   ANV_IMAGE_LAYOUT_EXPLICIT_AUX,
                                   ISL_AUX_USAGE_HIZ, &depth);
      depth.clear_color.f32[0] = ANV_HZ_FC_VAL;
   }

   struct blorp_surf stencil = {};
   if (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) {
      get_blorp_surf_for_anv_image(cmd_buffer->device,
                                   image, VK_IMAGE_ASPECT_STENCIL_BIT,
                                   ANV_IMAGE_LAYOUT_EXPLICIT_AUX,
                                   ISL_AUX_USAGE_NONE, &stencil);
   }

   /* From the Sky Lake PRM Volume 7, "Depth Buffer Clear":
    *
    *    "The following is required when performing a depth buffer clear with
    *    using the WM_STATE or 3DSTATE_WM:
    *
    *       * If other rendering operations have preceded this clear, a
    *         PIPE_CONTROL with depth cache flush enabled, Depth Stall bit
    *         enabled must be issued before the rectangle primitive used for
    *         the depth buffer clear operation.
    *       * [...]"
    *
    * Even though the PRM only says that this is required if using 3DSTATE_WM
    * and a 3DPRIMITIVE, the GPU appears to also need this to avoid occasional
    * hangs when doing a clear with WM_HZ_OP.
    */
   cmd_buffer->state.pending_pipe_bits |=
      ANV_PIPE_DEPTH_CACHE_FLUSH_BIT | ANV_PIPE_DEPTH_STALL_BIT;

   blorp_hiz_clear_depth_stencil(&batch, &depth, &stencil,
                                 level, base_layer, layer_count,
                                 area.offset.x, area.offset.y,
                                 area.offset.x + area.extent.width,
                                 area.offset.y + area.extent.height,
                                 aspects & VK_IMAGE_ASPECT_DEPTH_BIT,
                                 ANV_HZ_FC_VAL,
                                 aspects & VK_IMAGE_ASPECT_STENCIL_BIT,
                                 stencil_value);

   blorp_batch_finish(&batch);

   /* From the SKL PRM, Depth Buffer Clear:
    *
    *    "Depth Buffer Clear Workaround
    *
    *    Depth buffer clear pass using any of the methods (WM_STATE,
    *    3DSTATE_WM or 3DSTATE_WM_HZ_OP) must be followed by a PIPE_CONTROL
    *    command with DEPTH_STALL bit and Depth FLUSH bits “set” before
    *    starting to render.  DepthStall and DepthFlush are not needed between
    *    consecutive depth clear passes nor is it required if the depth-clear
    *    pass was done with “full_surf_clear” bit set in the
    *    3DSTATE_WM_HZ_OP."
    *
    * Even though the PRM provides a bunch of conditions under which this is
    * supposedly unnecessary, we choose to perform the flush unconditionally
    * just to be safe.
    */
   cmd_buffer->state.pending_pipe_bits |=
      ANV_PIPE_DEPTH_CACHE_FLUSH_BIT | ANV_PIPE_DEPTH_STALL_BIT;
}

void
anv_image_mcs_op(struct anv_cmd_buffer *cmd_buffer,
                 const struct anv_image *image,
                 enum isl_format format,
                 VkImageAspectFlagBits aspect,
                 uint32_t base_layer, uint32_t layer_count,
                 enum isl_aux_op mcs_op, union isl_color_value *clear_value,
                 bool predicate)
{
   assert(image->aspects == VK_IMAGE_ASPECT_COLOR_BIT);
   assert(image->samples > 1);
   assert(base_layer + layer_count <= anv_image_aux_layers(image, aspect, 0));

   /* Multisampling with multi-planar formats is not supported */
   assert(image->n_planes == 1);

   struct blorp_batch batch;
   blorp_batch_init(&cmd_buffer->device->blorp, &batch, cmd_buffer,
                    BLORP_BATCH_PREDICATE_ENABLE * predicate +
                    BLORP_BATCH_NO_UPDATE_CLEAR_COLOR * !clear_value);

   struct blorp_surf surf;
   get_blorp_surf_for_anv_image(cmd_buffer->device, image, aspect,
                                ANV_IMAGE_LAYOUT_EXPLICIT_AUX,
                                ISL_AUX_USAGE_MCS, &surf);

   /* Blorp will store the clear color for us if we provide the clear color
    * address and we are doing a fast clear. So we save the clear value into
    * the blorp surface.
    */
   if (clear_value)
      surf.clear_color = *clear_value;

   /* From the Sky Lake PRM Vol. 7, "Render Target Fast Clear":
    *
    *    "After Render target fast clear, pipe-control with color cache
    *    write-flush must be issued before sending any DRAW commands on
    *    that render target."
    *
    * This comment is a bit cryptic and doesn't really tell you what's going
    * or what's really needed.  It appears that fast clear ops are not
    * properly synchronized with other drawing.  This means that we cannot
    * have a fast clear operation in the pipe at the same time as other
    * regular drawing operations.  We need to use a PIPE_CONTROL to ensure
    * that the contents of the previous draw hit the render target before we
    * resolve and then use a second PIPE_CONTROL after the resolve to ensure
    * that it is completed before any additional drawing occurs.
    */
   cmd_buffer->state.pending_pipe_bits |=
      ANV_PIPE_RENDER_TARGET_CACHE_FLUSH_BIT | ANV_PIPE_CS_STALL_BIT;

   switch (mcs_op) {
   case ISL_AUX_OP_FAST_CLEAR:
      blorp_fast_clear(&batch, &surf, format,
                       0, base_layer, layer_count,
                       0, 0, image->extent.width, image->extent.height);
      break;
   case ISL_AUX_OP_PARTIAL_RESOLVE:
      blorp_mcs_partial_resolve(&batch, &surf, format,
                                base_layer, layer_count);
      break;
   case ISL_AUX_OP_FULL_RESOLVE:
   case ISL_AUX_OP_AMBIGUATE:
   default:
      unreachable("Unsupported MCS operation");
   }

   cmd_buffer->state.pending_pipe_bits |=
      ANV_PIPE_RENDER_TARGET_CACHE_FLUSH_BIT | ANV_PIPE_CS_STALL_BIT;

   blorp_batch_finish(&batch);
}

void
anv_image_ccs_op(struct anv_cmd_buffer *cmd_buffer,
                 const struct anv_image *image,
                 enum isl_format format,
                 VkImageAspectFlagBits aspect, uint32_t level,
                 uint32_t base_layer, uint32_t layer_count,
                 enum isl_aux_op ccs_op, union isl_color_value *clear_value,
                 bool predicate)
{
   assert(image->aspects & VK_IMAGE_ASPECT_ANY_COLOR_BIT_ANV);
   assert(image->samples == 1);
   assert(level < anv_image_aux_levels(image, aspect));
   /* Multi-LOD YcBcR is not allowed */
   assert(image->n_planes == 1 || level == 0);
   assert(base_layer + layer_count <=
          anv_image_aux_layers(image, aspect, level));

   uint32_t plane = anv_image_aspect_to_plane(image->aspects, aspect);
   uint32_t width_div = image->format->planes[plane].denominator_scales[0];
   uint32_t height_div = image->format->planes[plane].denominator_scales[1];
   uint32_t level_width = anv_minify(image->extent.width, level) / width_div;
   uint32_t level_height = anv_minify(image->extent.height, level) / height_div;

   struct blorp_batch batch;
   blorp_batch_init(&cmd_buffer->device->blorp, &batch, cmd_buffer,
                    BLORP_BATCH_PREDICATE_ENABLE * predicate +
                    BLORP_BATCH_NO_UPDATE_CLEAR_COLOR * !clear_value);

   struct blorp_surf surf;
   get_blorp_surf_for_anv_image(cmd_buffer->device, image, aspect,
                                ANV_IMAGE_LAYOUT_EXPLICIT_AUX,
                                fast_clear_aux_usage(image, aspect),
                                &surf);

   /* Blorp will store the clear color for us if we provide the clear color
    * address and we are doing a fast clear. So we save the clear value into
    * the blorp surface.
    */
   if (clear_value)
      surf.clear_color = *clear_value;

   /* From the Sky Lake PRM Vol. 7, "Render Target Fast Clear":
    *
    *    "After Render target fast clear, pipe-control with color cache
    *    write-flush must be issued before sending any DRAW commands on
    *    that render target."
    *
    * This comment is a bit cryptic and doesn't really tell you what's going
    * or what's really needed.  It appears that fast clear ops are not
    * properly synchronized with other drawing.  This means that we cannot
    * have a fast clear operation in the pipe at the same time as other
    * regular drawing operations.  We need to use a PIPE_CONTROL to ensure
    * that the contents of the previous draw hit the render target before we
    * resolve and then use a second PIPE_CONTROL after the resolve to ensure
    * that it is completed before any additional drawing occurs.
    */
   cmd_buffer->state.pending_pipe_bits |=
      ANV_PIPE_RENDER_TARGET_CACHE_FLUSH_BIT | ANV_PIPE_CS_STALL_BIT;

   switch (ccs_op) {
   case ISL_AUX_OP_FAST_CLEAR:
      blorp_fast_clear(&batch, &surf, format,
                       level, base_layer, layer_count,
                       0, 0, level_width, level_height);
      break;
   case ISL_AUX_OP_FULL_RESOLVE:
   case ISL_AUX_OP_PARTIAL_RESOLVE:
      blorp_ccs_resolve(&batch, &surf, level, base_layer, layer_count,
                        format, ccs_op);
      break;
   case ISL_AUX_OP_AMBIGUATE:
      for (uint32_t a = 0; a < layer_count; a++) {
         const uint32_t layer = base_layer + a;
         blorp_ccs_ambiguate(&batch, &surf, level, layer);
      }
      break;
   default:
      unreachable("Unsupported CCS operation");
   }

   cmd_buffer->state.pending_pipe_bits |=
      ANV_PIPE_RENDER_TARGET_CACHE_FLUSH_BIT | ANV_PIPE_CS_STALL_BIT;

   blorp_batch_finish(&batch);
}
