/*
 * Copyright 2010 Jerome Glisse <glisse@freedesktop.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "r600_pipe.h"
#include "r600_public.h"
#include "r600_isa.h"
#include "evergreen_compute.h"
#include "r600d.h"

#include "sb/sb_public.h"

#include <errno.h>
#include "pipe/p_shader_tokens.h"
#include "util/u_debug.h"
#include "util/u_memory.h"
#include "util/u_simple_shaders.h"
#include "util/u_upload_mgr.h"
#include "util/u_math.h"
#include "vl/vl_decoder.h"
#include "vl/vl_video_buffer.h"
#include "radeon_video.h"
#include "radeon_uvd.h"
#include "util/os_time.h"

static const struct debug_named_value r600_debug_options[] = {
	/* features */
	{ "nocpdma", DBG_NO_CP_DMA, "Disable CP DMA" },

	/* shader backend */
	{ "nosb", DBG_NO_SB, "Disable sb backend for graphics shaders" },
	{ "sbcl", DBG_SB_CS, "Enable sb backend for compute shaders" },
	{ "sbdry", DBG_SB_DRY_RUN, "Don't use optimized bytecode (just print the dumps)" },
	{ "sbstat", DBG_SB_STAT, "Print optimization statistics for shaders" },
	{ "sbdump", DBG_SB_DUMP, "Print IR dumps after some optimization passes" },
	{ "sbnofallback", DBG_SB_NO_FALLBACK, "Abort on errors instead of fallback" },
	{ "sbdisasm", DBG_SB_DISASM, "Use sb disassembler for shader dumps" },
	{ "sbsafemath", DBG_SB_SAFEMATH, "Disable unsafe math optimizations" },

	DEBUG_NAMED_VALUE_END /* must be last */
};

/*
 * pipe_context
 */

static void r600_destroy_context(struct pipe_context *context)
{
	struct r600_context *rctx = (struct r600_context *)context;
	unsigned sh, i;

	r600_isa_destroy(rctx->isa);

	r600_sb_context_destroy(rctx->sb_context);

	for (sh = 0; sh < (rctx->b.chip_class < EVERGREEN ? R600_NUM_HW_STAGES : EG_NUM_HW_STAGES); sh++) {
		r600_resource_reference(&rctx->scratch_buffers[sh].buffer, NULL);
	}
	r600_resource_reference(&rctx->dummy_cmask, NULL);
	r600_resource_reference(&rctx->dummy_fmask, NULL);

	if (rctx->append_fence)
		pipe_resource_reference((struct pipe_resource**)&rctx->append_fence, NULL);
	for (sh = 0; sh < PIPE_SHADER_TYPES; sh++) {
		rctx->b.b.set_constant_buffer(&rctx->b.b, sh, R600_BUFFER_INFO_CONST_BUFFER, NULL);
		free(rctx->driver_consts[sh].constants);
	}

	if (rctx->fixed_func_tcs_shader)
		rctx->b.b.delete_tcs_state(&rctx->b.b, rctx->fixed_func_tcs_shader);

	if (rctx->dummy_pixel_shader) {
		rctx->b.b.delete_fs_state(&rctx->b.b, rctx->dummy_pixel_shader);
	}
	if (rctx->custom_dsa_flush) {
		rctx->b.b.delete_depth_stencil_alpha_state(&rctx->b.b, rctx->custom_dsa_flush);
	}
	if (rctx->custom_blend_resolve) {
		rctx->b.b.delete_blend_state(&rctx->b.b, rctx->custom_blend_resolve);
	}
	if (rctx->custom_blend_decompress) {
		rctx->b.b.delete_blend_state(&rctx->b.b, rctx->custom_blend_decompress);
	}
	if (rctx->custom_blend_fastclear) {
		rctx->b.b.delete_blend_state(&rctx->b.b, rctx->custom_blend_fastclear);
	}
	util_unreference_framebuffer_state(&rctx->framebuffer.state);

	for (sh = 0; sh < PIPE_SHADER_TYPES; ++sh)
		for (i = 0; i < PIPE_MAX_CONSTANT_BUFFERS; ++i)
			rctx->b.b.set_constant_buffer(context, sh, i, NULL);

	if (rctx->blitter) {
		util_blitter_destroy(rctx->blitter);
	}
	if (rctx->allocator_fetch_shader) {
		u_suballocator_destroy(rctx->allocator_fetch_shader);
	}

	r600_release_command_buffer(&rctx->start_cs_cmd);

	FREE(rctx->start_compute_cs_cmd.buf);

	r600_common_context_cleanup(&rctx->b);

	r600_resource_reference(&rctx->trace_buf, NULL);
	r600_resource_reference(&rctx->last_trace_buf, NULL);
	radeon_clear_saved_cs(&rctx->last_gfx);

	FREE(rctx);
}

static struct pipe_context *r600_create_context(struct pipe_screen *screen,
                                                void *priv, unsigned flags)
{
	struct r600_context *rctx = CALLOC_STRUCT(r600_context);
	struct r600_screen* rscreen = (struct r600_screen *)screen;
	struct radeon_winsys *ws = rscreen->b.ws;

	if (!rctx)
		return NULL;

	rctx->b.b.screen = screen;
	assert(!priv);
	rctx->b.b.priv = NULL; /* for threaded_context_unwrap_sync */
	rctx->b.b.destroy = r600_destroy_context;
	rctx->b.set_atom_dirty = (void *)r600_set_atom_dirty;

	if (!r600_common_context_init(&rctx->b, &rscreen->b, flags))
		goto fail;

	rctx->screen = rscreen;
	LIST_INITHEAD(&rctx->texture_buffers);

	r600_init_blit_functions(rctx);

	if (rscreen->b.info.has_hw_decode) {
		rctx->b.b.create_video_codec = r600_uvd_create_decoder;
		rctx->b.b.create_video_buffer = r600_video_buffer_create;
	} else {
		rctx->b.b.create_video_codec = vl_create_decoder;
		rctx->b.b.create_video_buffer = vl_video_buffer_create;
	}

	if (getenv("R600_TRACE"))
		rctx->is_debug = true;
	r600_init_common_state_functions(rctx);

	switch (rctx->b.chip_class) {
	case R600:
	case R700:
		r600_init_state_functions(rctx);
		r600_init_atom_start_cs(rctx);
		rctx->custom_dsa_flush = r600_create_db_flush_dsa(rctx);
		rctx->custom_blend_resolve = rctx->b.chip_class == R700 ? r700_create_resolve_blend(rctx)
								      : r600_create_resolve_blend(rctx);
		rctx->custom_blend_decompress = r600_create_decompress_blend(rctx);
		rctx->has_vertex_cache = !(rctx->b.family == CHIP_RV610 ||
					   rctx->b.family == CHIP_RV620 ||
					   rctx->b.family == CHIP_RS780 ||
					   rctx->b.family == CHIP_RS880 ||
					   rctx->b.family == CHIP_RV710);
		break;
	case EVERGREEN:
	case CAYMAN:
		evergreen_init_state_functions(rctx);
		evergreen_init_atom_start_cs(rctx);
		evergreen_init_atom_start_compute_cs(rctx);
		rctx->custom_dsa_flush = evergreen_create_db_flush_dsa(rctx);
		rctx->custom_blend_resolve = evergreen_create_resolve_blend(rctx);
		rctx->custom_blend_decompress = evergreen_create_decompress_blend(rctx);
		rctx->custom_blend_fastclear = evergreen_create_fastclear_blend(rctx);
		rctx->has_vertex_cache = !(rctx->b.family == CHIP_CEDAR ||
					   rctx->b.family == CHIP_PALM ||
					   rctx->b.family == CHIP_SUMO ||
					   rctx->b.family == CHIP_SUMO2 ||
					   rctx->b.family == CHIP_CAICOS ||
					   rctx->b.family == CHIP_CAYMAN ||
					   rctx->b.family == CHIP_ARUBA);

		rctx->append_fence = pipe_buffer_create(rctx->b.b.screen, PIPE_BIND_CUSTOM,
							 PIPE_USAGE_DEFAULT, 32);
		break;
	default:
		R600_ERR("Unsupported chip class %d.\n", rctx->b.chip_class);
		goto fail;
	}

	rctx->b.gfx.cs = ws->cs_create(rctx->b.ctx, RING_GFX,
				       r600_context_gfx_flush, rctx);
	rctx->b.gfx.flush = r600_context_gfx_flush;

	rctx->allocator_fetch_shader =
		u_suballocator_create(&rctx->b.b, 64 * 1024,
				      0, PIPE_USAGE_DEFAULT, 0, FALSE);
	if (!rctx->allocator_fetch_shader)
		goto fail;

	rctx->isa = calloc(1, sizeof(struct r600_isa));
	if (!rctx->isa || r600_isa_init(rctx, rctx->isa))
		goto fail;

	if (rscreen->b.debug_flags & DBG_FORCE_DMA)
		rctx->b.b.resource_copy_region = rctx->b.dma_copy;

	rctx->blitter = util_blitter_create(&rctx->b.b);
	if (rctx->blitter == NULL)
		goto fail;
	util_blitter_set_texture_multisample(rctx->blitter, rscreen->has_msaa);
	rctx->blitter->draw_rectangle = r600_draw_rectangle;

	r600_begin_new_cs(rctx);

	rctx->dummy_pixel_shader =
		util_make_fragment_cloneinput_shader(&rctx->b.b, 0,
						     TGSI_SEMANTIC_GENERIC,
						     TGSI_INTERPOLATE_CONSTANT);
	rctx->b.b.bind_fs_state(&rctx->b.b, rctx->dummy_pixel_shader);

	return &rctx->b.b;

fail:
	r600_destroy_context(&rctx->b.b);
	return NULL;
}

/*
 * pipe_screen
 */

static int r600_get_param(struct pipe_screen* pscreen, enum pipe_cap param)
{
	struct r600_screen *rscreen = (struct r600_screen *)pscreen;
	enum radeon_family family = rscreen->b.family;

	switch (param) {
	/* Supported features (boolean caps). */
	case PIPE_CAP_NPOT_TEXTURES:
	case PIPE_CAP_MIXED_FRAMEBUFFER_SIZES:
	case PIPE_CAP_MIXED_COLOR_DEPTH_BITS:
	case PIPE_CAP_ANISOTROPIC_FILTER:
	case PIPE_CAP_POINT_SPRITE:
	case PIPE_CAP_OCCLUSION_QUERY:
	case PIPE_CAP_TEXTURE_MIRROR_CLAMP:
	case PIPE_CAP_BLEND_EQUATION_SEPARATE:
	case PIPE_CAP_TEXTURE_SWIZZLE:
	case PIPE_CAP_DEPTH_CLIP_DISABLE:
	case PIPE_CAP_SHADER_STENCIL_EXPORT:
	case PIPE_CAP_VERTEX_ELEMENT_INSTANCE_DIVISOR:
	case PIPE_CAP_MIXED_COLORBUFFER_FORMATS:
	case PIPE_CAP_TGSI_FS_COORD_ORIGIN_UPPER_LEFT:
	case PIPE_CAP_TGSI_FS_COORD_PIXEL_CENTER_HALF_INTEGER:
	case PIPE_CAP_SM3:
	case PIPE_CAP_SEAMLESS_CUBE_MAP:
	case PIPE_CAP_PRIMITIVE_RESTART:
	case PIPE_CAP_CONDITIONAL_RENDER:
	case PIPE_CAP_TEXTURE_BARRIER:
	case PIPE_CAP_VERTEX_COLOR_UNCLAMPED:
	case PIPE_CAP_QUADS_FOLLOW_PROVOKING_VERTEX_CONVENTION:
	case PIPE_CAP_TGSI_INSTANCEID:
	case PIPE_CAP_VERTEX_BUFFER_OFFSET_4BYTE_ALIGNED_ONLY:
	case PIPE_CAP_VERTEX_BUFFER_STRIDE_4BYTE_ALIGNED_ONLY:
	case PIPE_CAP_VERTEX_ELEMENT_SRC_OFFSET_4BYTE_ALIGNED_ONLY:
	case PIPE_CAP_START_INSTANCE:
	case PIPE_CAP_MAX_DUAL_SOURCE_RENDER_TARGETS:
	case PIPE_CAP_TEXTURE_BUFFER_OBJECTS:
	case PIPE_CAP_PREFER_BLIT_BASED_TEXTURE_TRANSFER:
	case PIPE_CAP_QUERY_PIPELINE_STATISTICS:
	case PIPE_CAP_TEXTURE_MULTISAMPLE:
	case PIPE_CAP_BUFFER_MAP_PERSISTENT_COHERENT:
	case PIPE_CAP_TGSI_VS_WINDOW_SPACE_POSITION:
	case PIPE_CAP_TGSI_VS_LAYER_VIEWPORT:
	case PIPE_CAP_SAMPLE_SHADING:
	case PIPE_CAP_CLIP_HALFZ:
	case PIPE_CAP_POLYGON_OFFSET_CLAMP:
	case PIPE_CAP_CONDITIONAL_RENDER_INVERTED:
	case PIPE_CAP_TEXTURE_FLOAT_LINEAR:
	case PIPE_CAP_TEXTURE_HALF_FLOAT_LINEAR:
	case PIPE_CAP_TGSI_TXQS:
	case PIPE_CAP_COPY_BETWEEN_COMPRESSED_AND_PLAIN_FORMATS:
	case PIPE_CAP_INVALIDATE_BUFFER:
	case PIPE_CAP_SURFACE_REINTERPRET_BLOCKS:
	case PIPE_CAP_QUERY_MEMORY_INFO:
	case PIPE_CAP_FRAMEBUFFER_NO_ATTACHMENT:
	case PIPE_CAP_POLYGON_OFFSET_UNITS_UNSCALED:
	case PIPE_CAP_CLEAR_TEXTURE:
	case PIPE_CAP_TGSI_MUL_ZERO_WINS:
	case PIPE_CAP_CAN_BIND_CONST_BUFFER_AS_VERTEX:
	case PIPE_CAP_ALLOW_MAPPED_BUFFERS_DURING_EXECUTION:
	case PIPE_CAP_ROBUST_BUFFER_ACCESS_BEHAVIOR:
		return 1;

	case PIPE_CAP_DEVICE_RESET_STATUS_QUERY:
		return rscreen->b.info.drm_major == 2 && rscreen->b.info.drm_minor >= 43;

	case PIPE_CAP_RESOURCE_FROM_USER_MEMORY:
		return !R600_BIG_ENDIAN && rscreen->b.info.has_userptr;

	case PIPE_CAP_COMPUTE:
		return rscreen->b.chip_class > R700;

	case PIPE_CAP_TGSI_TEXCOORD:
		return 0;

	case PIPE_CAP_FAKE_SW_MSAA:
		return 0;

	case PIPE_CAP_MAX_TEXTURE_BUFFER_SIZE:
		return MIN2(rscreen->b.info.max_alloc_size, INT_MAX);

        case PIPE_CAP_MIN_MAP_BUFFER_ALIGNMENT:
                return R600_MAP_BUFFER_ALIGNMENT;

	case PIPE_CAP_CONSTANT_BUFFER_OFFSET_ALIGNMENT:
		return 256;

	case PIPE_CAP_TEXTURE_BUFFER_OFFSET_ALIGNMENT:
		return 1;

	case PIPE_CAP_GLSL_FEATURE_LEVEL:
		if (family >= CHIP_CEDAR)
		   return 430;
		/* pre-evergreen geom shaders need newer kernel */
		if (rscreen->b.info.drm_minor >= 37)
		   return 330;
		return 140;

	case PIPE_CAP_GLSL_FEATURE_LEVEL_COMPATIBILITY:
		return 140;

	/* Supported except the original R600. */
	case PIPE_CAP_INDEP_BLEND_ENABLE:
	case PIPE_CAP_INDEP_BLEND_FUNC:
		/* R600 doesn't support per-MRT blends */
		return family == CHIP_R600 ? 0 : 1;

	/* Supported on Evergreen. */
	case PIPE_CAP_SEAMLESS_CUBE_MAP_PER_TEXTURE:
	case PIPE_CAP_CUBE_MAP_ARRAY:
	case PIPE_CAP_TEXTURE_GATHER_SM5:
	case PIPE_CAP_TEXTURE_QUERY_LOD:
	case PIPE_CAP_TGSI_FS_FINE_DERIVATIVE:
	case PIPE_CAP_SAMPLER_VIEW_TARGET:
	case PIPE_CAP_TGSI_PACK_HALF_FLOAT:
	case PIPE_CAP_TGSI_CLOCK:
	case PIPE_CAP_TGSI_ARRAY_COMPONENTS:
	case PIPE_CAP_QUERY_BUFFER_OBJECT:
		return family >= CHIP_CEDAR ? 1 : 0;
	case PIPE_CAP_MAX_TEXTURE_GATHER_COMPONENTS:
		return family >= CHIP_CEDAR ? 4 : 0;
	case PIPE_CAP_DRAW_INDIRECT:
		/* kernel command checker support is also required */
		return family >= CHIP_CEDAR && rscreen->b.info.drm_minor >= 41;

	case PIPE_CAP_BUFFER_SAMPLER_VIEW_RGBA_ONLY:
		return family >= CHIP_CEDAR ? 0 : 1;

	case PIPE_CAP_MAX_COMBINED_SHADER_OUTPUT_RESOURCES:
		return 8;

	/* Unsupported features. */
	case PIPE_CAP_TGSI_FS_COORD_ORIGIN_LOWER_LEFT:
	case PIPE_CAP_TGSI_FS_COORD_PIXEL_CENTER_INTEGER:
	case PIPE_CAP_TGSI_CAN_COMPACT_CONSTANTS:
	case PIPE_CAP_FRAGMENT_COLOR_CLAMPED:
	case PIPE_CAP_VERTEX_COLOR_CLAMPED:
	case PIPE_CAP_USER_VERTEX_BUFFERS:
	case PIPE_CAP_TEXTURE_GATHER_OFFSETS:
	case PIPE_CAP_VERTEXID_NOBASE:
	case PIPE_CAP_DEPTH_BOUNDS_TEST:
	case PIPE_CAP_FORCE_PERSAMPLE_INTERP:
	case PIPE_CAP_SHAREABLE_SHADERS:
	case PIPE_CAP_DRAW_PARAMETERS:
	case PIPE_CAP_MULTI_DRAW_INDIRECT:
	case PIPE_CAP_MULTI_DRAW_INDIRECT_PARAMS:
	case PIPE_CAP_TGSI_FS_POSITION_IS_SYSVAL:
	case PIPE_CAP_TGSI_FS_FACE_IS_INTEGER_SYSVAL:
	case PIPE_CAP_GENERATE_MIPMAP:
	case PIPE_CAP_STRING_MARKER:
	case PIPE_CAP_PRIMITIVE_RESTART_FOR_PATCHES:
	case PIPE_CAP_TGSI_VOTE:
	case PIPE_CAP_MAX_WINDOW_RECTANGLES:
	case PIPE_CAP_TGSI_CAN_READ_OUTPUTS:
	case PIPE_CAP_NATIVE_FENCE_FD:
	case PIPE_CAP_GLSL_OPTIMIZE_CONSERVATIVELY:
	case PIPE_CAP_TGSI_FS_FBFETCH:
	case PIPE_CAP_INT64:
	case PIPE_CAP_INT64_DIVMOD:
	case PIPE_CAP_TGSI_TEX_TXF_LZ:
	case PIPE_CAP_POLYGON_MODE_FILL_RECTANGLE:
	case PIPE_CAP_SPARSE_BUFFER_PAGE_SIZE:
	case PIPE_CAP_TGSI_BALLOT:
	case PIPE_CAP_TGSI_TES_LAYER_VIEWPORT:
	case PIPE_CAP_POST_DEPTH_COVERAGE:
	case PIPE_CAP_BINDLESS_TEXTURE:
	case PIPE_CAP_NIR_SAMPLERS_AS_DEREF:
	case PIPE_CAP_QUERY_SO_OVERFLOW:
	case PIPE_CAP_MEMOBJ:
	case PIPE_CAP_LOAD_CONSTBUF:
	case PIPE_CAP_TGSI_ANY_REG_AS_ADDRESS:
	case PIPE_CAP_TILE_RASTER_ORDER:
	case PIPE_CAP_SIGNED_VERTEX_BUFFER_OFFSET:
	case PIPE_CAP_CONTEXT_PRIORITY_MASK:
	case PIPE_CAP_FENCE_SIGNAL:
	case PIPE_CAP_CONSTBUF0_FLAGS:
	case PIPE_CAP_PACKED_UNIFORMS:
	case PIPE_CAP_FRAMEBUFFER_MSAA_CONSTRAINTS:
	case PIPE_CAP_CONSERVATIVE_RASTER_POST_SNAP_TRIANGLES:
	case PIPE_CAP_CONSERVATIVE_RASTER_POST_SNAP_POINTS_LINES:
	case PIPE_CAP_CONSERVATIVE_RASTER_PRE_SNAP_TRIANGLES:
	case PIPE_CAP_CONSERVATIVE_RASTER_PRE_SNAP_POINTS_LINES:
	case PIPE_CAP_CONSERVATIVE_RASTER_POST_DEPTH_COVERAGE:
	case PIPE_CAP_MAX_CONSERVATIVE_RASTER_SUBPIXEL_PRECISION_BIAS:
	case PIPE_CAP_PROGRAMMABLE_SAMPLE_LOCATIONS:
		return 0;

	case PIPE_CAP_DOUBLES:
		if (rscreen->b.family == CHIP_ARUBA ||
		    rscreen->b.family == CHIP_CAYMAN ||
		    rscreen->b.family == CHIP_CYPRESS ||
		    rscreen->b.family == CHIP_HEMLOCK)
			return 1;
		return 0;
	case PIPE_CAP_CULL_DISTANCE:
		return 1;

	case PIPE_CAP_SHADER_BUFFER_OFFSET_ALIGNMENT:
		if (family >= CHIP_CEDAR)
			return 256;
		return 0;

	case PIPE_CAP_MAX_SHADER_PATCH_VARYINGS:
		if (family >= CHIP_CEDAR)
			return 30;
		else
			return 0;
	/* Stream output. */
	case PIPE_CAP_MAX_STREAM_OUTPUT_BUFFERS:
		return rscreen->b.has_streamout ? 4 : 0;
	case PIPE_CAP_STREAM_OUTPUT_PAUSE_RESUME:
	case PIPE_CAP_STREAM_OUTPUT_INTERLEAVE_BUFFERS:
		return rscreen->b.has_streamout ? 1 : 0;
	case PIPE_CAP_MAX_STREAM_OUTPUT_SEPARATE_COMPONENTS:
	case PIPE_CAP_MAX_STREAM_OUTPUT_INTERLEAVED_COMPONENTS:
		return 32*4;

	/* Geometry shader output. */
	case PIPE_CAP_MAX_GEOMETRY_OUTPUT_VERTICES:
		return 1024;
	case PIPE_CAP_MAX_GEOMETRY_TOTAL_OUTPUT_COMPONENTS:
		return 16384;
	case PIPE_CAP_MAX_VERTEX_STREAMS:
		return family >= CHIP_CEDAR ? 4 : 1;

	case PIPE_CAP_MAX_VERTEX_ATTRIB_STRIDE:
		/* Should be 2047, but 2048 is a requirement for GL 4.4 */
		return 2048;

	/* Texturing. */
	case PIPE_CAP_MAX_TEXTURE_2D_LEVELS:
	case PIPE_CAP_MAX_TEXTURE_CUBE_LEVELS:
		if (family >= CHIP_CEDAR)
			return 15;
		else
			return 14;
	case PIPE_CAP_MAX_TEXTURE_3D_LEVELS:
		/* textures support 8192, but layered rendering supports 2048 */
		return 12;
	case PIPE_CAP_MAX_TEXTURE_ARRAY_LAYERS:
		/* textures support 8192, but layered rendering supports 2048 */
		return 2048;

	/* Render targets. */
	case PIPE_CAP_MAX_RENDER_TARGETS:
		/* XXX some r6xx are buggy and can only do 4 */
		return 8;

	case PIPE_CAP_MAX_VIEWPORTS:
		return R600_MAX_VIEWPORTS;
	case PIPE_CAP_VIEWPORT_SUBPIXEL_BITS:
		return 8;

	/* Timer queries, present when the clock frequency is non zero. */
	case PIPE_CAP_QUERY_TIME_ELAPSED:
		return rscreen->b.info.clock_crystal_freq != 0;
	case PIPE_CAP_QUERY_TIMESTAMP:
		return rscreen->b.info.drm_minor >= 20 &&
		       rscreen->b.info.clock_crystal_freq != 0;

	case PIPE_CAP_MIN_TEXTURE_GATHER_OFFSET:
	case PIPE_CAP_MIN_TEXEL_OFFSET:
		return -8;

	case PIPE_CAP_MAX_TEXTURE_GATHER_OFFSET:
	case PIPE_CAP_MAX_TEXEL_OFFSET:
		return 7;

	case PIPE_CAP_TEXTURE_BORDER_COLOR_QUIRK:
		return PIPE_QUIRK_TEXTURE_BORDER_COLOR_SWIZZLE_R600;
	case PIPE_CAP_ENDIANNESS:
		return PIPE_ENDIAN_LITTLE;

	case PIPE_CAP_VENDOR_ID:
		return ATI_VENDOR_ID;
	case PIPE_CAP_DEVICE_ID:
		return rscreen->b.info.pci_id;
	case PIPE_CAP_ACCELERATED:
		return 1;
	case PIPE_CAP_VIDEO_MEMORY:
		return rscreen->b.info.vram_size >> 20;
	case PIPE_CAP_UMA:
		return 0;
	case PIPE_CAP_MULTISAMPLE_Z_RESOLVE:
		return rscreen->b.chip_class >= R700;
	case PIPE_CAP_PCI_GROUP:
		return rscreen->b.info.pci_domain;
	case PIPE_CAP_PCI_BUS:
		return rscreen->b.info.pci_bus;
	case PIPE_CAP_PCI_DEVICE:
		return rscreen->b.info.pci_dev;
	case PIPE_CAP_PCI_FUNCTION:
		return rscreen->b.info.pci_func;
	}
	return 0;
}

static int r600_get_shader_param(struct pipe_screen* pscreen,
				 enum pipe_shader_type shader,
				 enum pipe_shader_cap param)
{
	struct r600_screen *rscreen = (struct r600_screen *)pscreen;

	switch(shader)
	{
	case PIPE_SHADER_FRAGMENT:
	case PIPE_SHADER_VERTEX:
	case PIPE_SHADER_COMPUTE:
		break;
	case PIPE_SHADER_GEOMETRY:
		if (rscreen->b.family >= CHIP_CEDAR)
			break;
		/* pre-evergreen geom shaders need newer kernel */
		if (rscreen->b.info.drm_minor >= 37)
			break;
		return 0;
	case PIPE_SHADER_TESS_CTRL:
	case PIPE_SHADER_TESS_EVAL:
		if (rscreen->b.family >= CHIP_CEDAR)
			break;
	default:
		return 0;
	}

	switch (param) {
	case PIPE_SHADER_CAP_MAX_INSTRUCTIONS:
	case PIPE_SHADER_CAP_MAX_ALU_INSTRUCTIONS:
	case PIPE_SHADER_CAP_MAX_TEX_INSTRUCTIONS:
	case PIPE_SHADER_CAP_MAX_TEX_INDIRECTIONS:
		return 16384;
	case PIPE_SHADER_CAP_MAX_CONTROL_FLOW_DEPTH:
		return 32;
	case PIPE_SHADER_CAP_MAX_INPUTS:
		return shader == PIPE_SHADER_VERTEX ? 16 : 32;
	case PIPE_SHADER_CAP_MAX_OUTPUTS:
		return shader == PIPE_SHADER_FRAGMENT ? 8 : 32;
	case PIPE_SHADER_CAP_MAX_TEMPS:
		return 256; /* Max native temporaries. */
	case PIPE_SHADER_CAP_MAX_CONST_BUFFER_SIZE:
		if (shader == PIPE_SHADER_COMPUTE) {
			uint64_t max_const_buffer_size;
			pscreen->get_compute_param(pscreen, PIPE_SHADER_IR_TGSI,
				PIPE_COMPUTE_CAP_MAX_MEM_ALLOC_SIZE,
				&max_const_buffer_size);
			return MIN2(max_const_buffer_size, INT_MAX);

		} else {
			return R600_MAX_CONST_BUFFER_SIZE;
		}
	case PIPE_SHADER_CAP_MAX_CONST_BUFFERS:
		return R600_MAX_USER_CONST_BUFFERS;
	case PIPE_SHADER_CAP_TGSI_CONT_SUPPORTED:
		return 1;
	case PIPE_SHADER_CAP_TGSI_SQRT_SUPPORTED:
		return 1;
	case PIPE_SHADER_CAP_INDIRECT_INPUT_ADDR:
	case PIPE_SHADER_CAP_INDIRECT_OUTPUT_ADDR:
	case PIPE_SHADER_CAP_INDIRECT_TEMP_ADDR:
	case PIPE_SHADER_CAP_INDIRECT_CONST_ADDR:
		return 1;
	case PIPE_SHADER_CAP_SUBROUTINES:
	case PIPE_SHADER_CAP_INT64_ATOMICS:
	case PIPE_SHADER_CAP_FP16:
		return 0;
	case PIPE_SHADER_CAP_INTEGERS:
	case PIPE_SHADER_CAP_TGSI_ANY_INOUT_DECL_RANGE:
		return 1;
	case PIPE_SHADER_CAP_MAX_TEXTURE_SAMPLERS:
	case PIPE_SHADER_CAP_MAX_SAMPLER_VIEWS:
		return 16;
        case PIPE_SHADER_CAP_PREFERRED_IR:
		return PIPE_SHADER_IR_TGSI;
	case PIPE_SHADER_CAP_SUPPORTED_IRS: {
		int ir = 0;
		if (shader == PIPE_SHADER_COMPUTE)
			ir = 1 << PIPE_SHADER_IR_NATIVE;
		if (rscreen->b.family >= CHIP_CEDAR)
			ir |= 1 << PIPE_SHADER_IR_TGSI;
		return ir;
	}
	case PIPE_SHADER_CAP_TGSI_FMA_SUPPORTED:
		if (rscreen->b.family == CHIP_ARUBA ||
		    rscreen->b.family == CHIP_CAYMAN ||
		    rscreen->b.family == CHIP_CYPRESS ||
		    rscreen->b.family == CHIP_HEMLOCK)
			return 1;
		return 0;
	case PIPE_SHADER_CAP_TGSI_DROUND_SUPPORTED:
	case PIPE_SHADER_CAP_TGSI_DFRACEXP_DLDEXP_SUPPORTED:
	case PIPE_SHADER_CAP_TGSI_LDEXP_SUPPORTED:
	case PIPE_SHADER_CAP_LOWER_IF_THRESHOLD:
	case PIPE_SHADER_CAP_TGSI_SKIP_MERGE_REGISTERS:
		return 0;
	case PIPE_SHADER_CAP_MAX_SHADER_BUFFERS:
	case PIPE_SHADER_CAP_MAX_SHADER_IMAGES:
		if (rscreen->b.family >= CHIP_CEDAR &&
		    (shader == PIPE_SHADER_FRAGMENT || shader == PIPE_SHADER_COMPUTE))
		    return 8;
		return 0;
	case PIPE_SHADER_CAP_MAX_HW_ATOMIC_COUNTERS:
		if (rscreen->b.family >= CHIP_CEDAR && rscreen->has_atomics)
			return 8;
		return 0;
	case PIPE_SHADER_CAP_MAX_HW_ATOMIC_COUNTER_BUFFERS:
		/* having to allocate the atomics out amongst shaders stages is messy,
		   so give compute 8 buffers and all the others one */
		if (rscreen->b.family >= CHIP_CEDAR && rscreen->has_atomics) {
			return EG_MAX_ATOMIC_BUFFERS;
		}
		return 0;
	case PIPE_SHADER_CAP_SCALAR_ISA:
		return 0;
	case PIPE_SHADER_CAP_MAX_UNROLL_ITERATIONS_HINT:
		/* due to a bug in the shader compiler, some loops hang
		 * if they are not unrolled, see:
		 *    https://bugs.freedesktop.org/show_bug.cgi?id=86720
		 */
		return 255;
	}
	return 0;
}

static void r600_destroy_screen(struct pipe_screen* pscreen)
{
	struct r600_screen *rscreen = (struct r600_screen *)pscreen;

	if (!rscreen)
		return;

	if (!rscreen->b.ws->unref(rscreen->b.ws))
		return;

	if (rscreen->global_pool) {
		compute_memory_pool_delete(rscreen->global_pool);
	}

	r600_destroy_common_screen(&rscreen->b);
}

static struct pipe_resource *r600_resource_create(struct pipe_screen *screen,
						  const struct pipe_resource *templ)
{
	if (templ->target == PIPE_BUFFER &&
	    (templ->bind & PIPE_BIND_GLOBAL))
		return r600_compute_global_buffer_create(screen, templ);

	return r600_resource_create_common(screen, templ);
}

struct pipe_screen *r600_screen_create(struct radeon_winsys *ws,
				       const struct pipe_screen_config *config)
{
	struct r600_screen *rscreen = CALLOC_STRUCT(r600_screen);

	if (!rscreen) {
		return NULL;
	}

	/* Set functions first. */
	rscreen->b.b.context_create = r600_create_context;
	rscreen->b.b.destroy = r600_destroy_screen;
	rscreen->b.b.get_param = r600_get_param;
	rscreen->b.b.get_shader_param = r600_get_shader_param;
	rscreen->b.b.resource_create = r600_resource_create;

	if (!r600_common_screen_init(&rscreen->b, ws)) {
		FREE(rscreen);
		return NULL;
	}

	if (rscreen->b.info.chip_class >= EVERGREEN) {
		rscreen->b.b.is_format_supported = evergreen_is_format_supported;
	} else {
		rscreen->b.b.is_format_supported = r600_is_format_supported;
	}

	rscreen->b.debug_flags |= debug_get_flags_option("R600_DEBUG", r600_debug_options, 0);
	if (debug_get_bool_option("R600_DEBUG_COMPUTE", FALSE))
		rscreen->b.debug_flags |= DBG_COMPUTE;
	if (debug_get_bool_option("R600_DUMP_SHADERS", FALSE))
		rscreen->b.debug_flags |= DBG_ALL_SHADERS | DBG_FS;
	if (!debug_get_bool_option("R600_HYPERZ", TRUE))
		rscreen->b.debug_flags |= DBG_NO_HYPERZ;

	if (rscreen->b.family == CHIP_UNKNOWN) {
		fprintf(stderr, "r600: Unknown chipset 0x%04X\n", rscreen->b.info.pci_id);
		FREE(rscreen);
		return NULL;
	}

	/* Figure out streamout kernel support. */
	switch (rscreen->b.chip_class) {
	case R600:
		if (rscreen->b.family < CHIP_RS780) {
			rscreen->b.has_streamout = rscreen->b.info.drm_minor >= 14;
		} else {
			rscreen->b.has_streamout = rscreen->b.info.drm_minor >= 23;
		}
		break;
	case R700:
		rscreen->b.has_streamout = rscreen->b.info.drm_minor >= 17;
		break;
	case EVERGREEN:
	case CAYMAN:
		rscreen->b.has_streamout = rscreen->b.info.drm_minor >= 14;
		break;
	default:
		rscreen->b.has_streamout = FALSE;
		break;
	}

	/* MSAA support. */
	switch (rscreen->b.chip_class) {
	case R600:
	case R700:
		rscreen->has_msaa = rscreen->b.info.drm_minor >= 22;
		rscreen->has_compressed_msaa_texturing = false;
		break;
	case EVERGREEN:
		rscreen->has_msaa = rscreen->b.info.drm_minor >= 19;
		rscreen->has_compressed_msaa_texturing = rscreen->b.info.drm_minor >= 24;
		break;
	case CAYMAN:
		rscreen->has_msaa = rscreen->b.info.drm_minor >= 19;
		rscreen->has_compressed_msaa_texturing = true;
		break;
	default:
		rscreen->has_msaa = FALSE;
		rscreen->has_compressed_msaa_texturing = false;
	}

	rscreen->b.has_cp_dma = rscreen->b.info.drm_minor >= 27 &&
			      !(rscreen->b.debug_flags & DBG_NO_CP_DMA);

	rscreen->b.barrier_flags.cp_to_L2 =
		R600_CONTEXT_INV_VERTEX_CACHE |
		R600_CONTEXT_INV_TEX_CACHE |
		R600_CONTEXT_INV_CONST_CACHE;
	rscreen->b.barrier_flags.compute_to_L2 = R600_CONTEXT_CS_PARTIAL_FLUSH | R600_CONTEXT_FLUSH_AND_INV;

	rscreen->global_pool = compute_memory_pool_new(rscreen);

	/* Create the auxiliary context. This must be done last. */
	rscreen->b.aux_context = rscreen->b.b.context_create(&rscreen->b.b, NULL, 0);

	rscreen->has_atomics = rscreen->b.info.drm_minor >= 44;
#if 0 /* This is for testing whether aux_context and buffer clearing work correctly. */
	struct pipe_resource templ = {};

	templ.width0 = 4;
	templ.height0 = 2048;
	templ.depth0 = 1;
	templ.array_size = 1;
	templ.target = PIPE_TEXTURE_2D;
	templ.format = PIPE_FORMAT_R8G8B8A8_UNORM;
	templ.usage = PIPE_USAGE_DEFAULT;

	struct r600_resource *res = r600_resource(rscreen->screen.resource_create(&rscreen->screen, &templ));
	unsigned char *map = ws->buffer_map(res->buf, NULL, PIPE_TRANSFER_WRITE);

	memset(map, 0, 256);

	r600_screen_clear_buffer(rscreen, &res->b.b, 4, 4, 0xCC);
	r600_screen_clear_buffer(rscreen, &res->b.b, 8, 4, 0xDD);
	r600_screen_clear_buffer(rscreen, &res->b.b, 12, 4, 0xEE);
	r600_screen_clear_buffer(rscreen, &res->b.b, 20, 4, 0xFF);
	r600_screen_clear_buffer(rscreen, &res->b.b, 32, 20, 0x87);

	ws->buffer_wait(res->buf, RADEON_USAGE_WRITE);

	int i;
	for (i = 0; i < 256; i++) {
		printf("%02X", map[i]);
		if (i % 16 == 15)
			printf("\n");
	}
#endif

	if (rscreen->b.debug_flags & DBG_TEST_DMA)
		r600_test_dma(&rscreen->b);

	r600_query_fix_enabled_rb_mask(&rscreen->b);
	return &rscreen->b.b;
}
