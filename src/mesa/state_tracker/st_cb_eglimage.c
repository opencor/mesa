/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 2010 LunarG Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Chia-I Wu <olv@lunarg.com>
 */

#include "main/errors.h"
#include "main/texobj.h"
#include "main/teximage.h"
#include "util/u_inlines.h"
#include "util/u_format.h"
#include "st_cb_eglimage.h"
#include "st_cb_fbo.h"
#include "st_context.h"
#include "st_texture.h"
#include "st_format.h"
#include "st_manager.h"
#include "st_sampler_view.h"
#include "util/u_surface.h"


/**
 * Return the surface of an EGLImage.
 * FIXME: I think this should operate on resources, not surfaces
 */
static struct pipe_surface *
st_egl_image_get_surface(struct gl_context *ctx, GLeglImageOES image_handle,
                         unsigned usage, const char *error)
{
   struct st_context *st = st_context(ctx);
   struct pipe_screen *screen = st->pipe->screen;
   struct st_manager *smapi =
      (struct st_manager *) st->iface.st_context_private;
   struct st_egl_image stimg;
   struct pipe_surface *ps, surf_tmpl;

   if (!smapi || !smapi->get_egl_image)
      return NULL;

   memset(&stimg, 0, sizeof(stimg));
   if (!smapi->get_egl_image(smapi, (void *) image_handle, &stimg)) {
      /* image_handle does not refer to a valid EGL image object */
      _mesa_error(ctx, GL_INVALID_VALUE, "%s(image handle not found)", error);
      return NULL;
   }

   if (!screen->is_format_supported(screen, stimg.format, PIPE_TEXTURE_2D,
                                    stimg.texture->nr_samples, usage)) {
      /* unable to specify a texture object using the specified EGL image */
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(format not supported)", error);
      return NULL;
   }

   u_surface_default_template(&surf_tmpl, stimg.texture);
   surf_tmpl.format = stimg.format;
   surf_tmpl.u.tex.level = stimg.level;
   surf_tmpl.u.tex.first_layer = stimg.layer;
   surf_tmpl.u.tex.last_layer = stimg.layer;
   ps = st->pipe->create_surface(st->pipe, stimg.texture, &surf_tmpl);
   pipe_resource_reference(&stimg.texture, NULL);

   return ps;
}

/**
 * Return the base format just like _mesa_base_fbo_format does.
 */
static GLenum
st_pipe_format_to_base_format(enum pipe_format format)
{
   GLenum base_format;

   if (util_format_is_depth_or_stencil(format)) {
      if (util_format_is_depth_and_stencil(format)) {
         base_format = GL_DEPTH_STENCIL;
      }
      else {
         if (format == PIPE_FORMAT_S8_UINT)
            base_format = GL_STENCIL_INDEX;
         else
            base_format = GL_DEPTH_COMPONENT;
      }
   }
   else {
      /* is this enough? */
      if (util_format_has_alpha(format))
         base_format = GL_RGBA;
      else
         base_format = GL_RGB;
   }

   return base_format;
}

static void
st_egl_image_target_renderbuffer_storage(struct gl_context *ctx,
					 struct gl_renderbuffer *rb,
					 GLeglImageOES image_handle)
{
   struct st_renderbuffer *strb = st_renderbuffer(rb);
   struct pipe_surface *ps;

   ps = st_egl_image_get_surface(ctx, image_handle, PIPE_BIND_RENDER_TARGET,
				 "glEGLImageTargetRenderbufferStorage");
   if (ps) {
      strb->Base.Width = ps->width;
      strb->Base.Height = ps->height;
      strb->Base.Format = st_pipe_format_to_mesa_format(ps->format);
      strb->Base._BaseFormat = st_pipe_format_to_base_format(ps->format);
      strb->Base.InternalFormat = strb->Base._BaseFormat;

      pipe_surface_reference(&strb->surface, ps);
      pipe_resource_reference(&strb->texture, ps->texture);

      pipe_surface_reference(&ps, NULL);
   }
}

static void
st_bind_surface(struct gl_context *ctx, GLenum target,
                struct gl_texture_object *texObj,
                struct gl_texture_image *texImage,
                struct pipe_surface *ps)
{
   struct st_context *st = st_context(ctx);
   struct st_texture_object *stObj;
   struct st_texture_image *stImage;
   GLenum internalFormat;
   mesa_format texFormat;

   /* map pipe format to base format */
   if (util_format_get_component_bits(ps->format, UTIL_FORMAT_COLORSPACE_RGB, 3) > 0)
      internalFormat = GL_RGBA;
   else
      internalFormat = GL_RGB;

   stObj = st_texture_object(texObj);
   stImage = st_texture_image(texImage);

   /* switch to surface based */
   if (!stObj->surface_based) {
      _mesa_clear_texture_object(ctx, texObj);
      stObj->surface_based = GL_TRUE;
   }

   texFormat = st_pipe_format_to_mesa_format(ps->format);

   /* TODO RequiredTextureImageUnits should probably be reset back
    * to 1 somewhere if different texture is bound??
    */
   if (texFormat == MESA_FORMAT_NONE) {
      switch (ps->format) {
      case PIPE_FORMAT_NV12:
         texFormat = MESA_FORMAT_R_UNORM8;
         texObj->RequiredTextureImageUnits = 2;
         break;
      case PIPE_FORMAT_IYUV:
         texFormat = MESA_FORMAT_R_UNORM8;
         texObj->RequiredTextureImageUnits = 3;
         break;
      default:
         unreachable("bad YUV format!");
      }
   }

   _mesa_init_teximage_fields(ctx, texImage,
                              ps->width, ps->height, 1, 0, internalFormat,
                              texFormat);

   /* FIXME create a non-default sampler view from the pipe_surface? */
   pipe_resource_reference(&stObj->pt, ps->texture);
   st_texture_release_all_sampler_views(st, stObj);
   pipe_resource_reference(&stImage->pt, stObj->pt);

   stObj->surface_format = ps->format;

   _mesa_dirty_texobj(ctx, texObj);
}

static void
st_egl_image_target_texture_2d(struct gl_context *ctx, GLenum target,
			       struct gl_texture_object *texObj,
			       struct gl_texture_image *texImage,
			       GLeglImageOES image_handle)
{
   struct pipe_surface *ps;

   ps = st_egl_image_get_surface(ctx, image_handle, PIPE_BIND_SAMPLER_VIEW,
				 "glEGLImageTargetTexture2D");
   if (ps) {
      st_bind_surface(ctx, target, texObj, texImage, ps);
      pipe_surface_reference(&ps, NULL);
   }
}

void
st_init_eglimage_functions(struct dd_function_table *functions)
{
   functions->EGLImageTargetTexture2D = st_egl_image_target_texture_2d;
   functions->EGLImageTargetRenderbufferStorage = st_egl_image_target_renderbuffer_storage;
}
