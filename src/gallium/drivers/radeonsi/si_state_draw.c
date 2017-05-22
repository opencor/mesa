/*
 * Copyright 2012 Advanced Micro Devices, Inc.
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
 *
 * Authors:
 *      Christian König <christian.koenig@amd.com>
 */

#include "si_pipe.h"
#include "radeon/r600_cs.h"
#include "sid.h"
#include "gfx9d.h"

#include "util/u_index_modify.h"
#include "util/u_upload_mgr.h"
#include "util/u_prim.h"

#include "ac_debug.h"

static unsigned si_conv_pipe_prim(unsigned mode)
{
        static const unsigned prim_conv[] = {
		[PIPE_PRIM_POINTS]			= V_008958_DI_PT_POINTLIST,
		[PIPE_PRIM_LINES]			= V_008958_DI_PT_LINELIST,
		[PIPE_PRIM_LINE_LOOP]			= V_008958_DI_PT_LINELOOP,
		[PIPE_PRIM_LINE_STRIP]			= V_008958_DI_PT_LINESTRIP,
		[PIPE_PRIM_TRIANGLES]			= V_008958_DI_PT_TRILIST,
		[PIPE_PRIM_TRIANGLE_STRIP]		= V_008958_DI_PT_TRISTRIP,
		[PIPE_PRIM_TRIANGLE_FAN]		= V_008958_DI_PT_TRIFAN,
		[PIPE_PRIM_QUADS]			= V_008958_DI_PT_QUADLIST,
		[PIPE_PRIM_QUAD_STRIP]			= V_008958_DI_PT_QUADSTRIP,
		[PIPE_PRIM_POLYGON]			= V_008958_DI_PT_POLYGON,
		[PIPE_PRIM_LINES_ADJACENCY]		= V_008958_DI_PT_LINELIST_ADJ,
		[PIPE_PRIM_LINE_STRIP_ADJACENCY]	= V_008958_DI_PT_LINESTRIP_ADJ,
		[PIPE_PRIM_TRIANGLES_ADJACENCY]		= V_008958_DI_PT_TRILIST_ADJ,
		[PIPE_PRIM_TRIANGLE_STRIP_ADJACENCY]	= V_008958_DI_PT_TRISTRIP_ADJ,
		[PIPE_PRIM_PATCHES]			= V_008958_DI_PT_PATCH,
		[R600_PRIM_RECTANGLE_LIST]		= V_008958_DI_PT_RECTLIST
        };
	assert(mode < ARRAY_SIZE(prim_conv));
	return prim_conv[mode];
}

static unsigned si_conv_prim_to_gs_out(unsigned mode)
{
	static const int prim_conv[] = {
		[PIPE_PRIM_POINTS]			= V_028A6C_OUTPRIM_TYPE_POINTLIST,
		[PIPE_PRIM_LINES]			= V_028A6C_OUTPRIM_TYPE_LINESTRIP,
		[PIPE_PRIM_LINE_LOOP]			= V_028A6C_OUTPRIM_TYPE_LINESTRIP,
		[PIPE_PRIM_LINE_STRIP]			= V_028A6C_OUTPRIM_TYPE_LINESTRIP,
		[PIPE_PRIM_TRIANGLES]			= V_028A6C_OUTPRIM_TYPE_TRISTRIP,
		[PIPE_PRIM_TRIANGLE_STRIP]		= V_028A6C_OUTPRIM_TYPE_TRISTRIP,
		[PIPE_PRIM_TRIANGLE_FAN]		= V_028A6C_OUTPRIM_TYPE_TRISTRIP,
		[PIPE_PRIM_QUADS]			= V_028A6C_OUTPRIM_TYPE_TRISTRIP,
		[PIPE_PRIM_QUAD_STRIP]			= V_028A6C_OUTPRIM_TYPE_TRISTRIP,
		[PIPE_PRIM_POLYGON]			= V_028A6C_OUTPRIM_TYPE_TRISTRIP,
		[PIPE_PRIM_LINES_ADJACENCY]		= V_028A6C_OUTPRIM_TYPE_LINESTRIP,
		[PIPE_PRIM_LINE_STRIP_ADJACENCY]	= V_028A6C_OUTPRIM_TYPE_LINESTRIP,
		[PIPE_PRIM_TRIANGLES_ADJACENCY]		= V_028A6C_OUTPRIM_TYPE_TRISTRIP,
		[PIPE_PRIM_TRIANGLE_STRIP_ADJACENCY]	= V_028A6C_OUTPRIM_TYPE_TRISTRIP,
		[PIPE_PRIM_PATCHES]			= V_028A6C_OUTPRIM_TYPE_POINTLIST,
		[R600_PRIM_RECTANGLE_LIST]		= V_028A6C_OUTPRIM_TYPE_TRISTRIP
	};
	assert(mode < ARRAY_SIZE(prim_conv));

	return prim_conv[mode];
}

/**
 * This calculates the LDS size for tessellation shaders (VS, TCS, TES).
 * LS.LDS_SIZE is shared by all 3 shader stages.
 *
 * The information about LDS and other non-compile-time parameters is then
 * written to userdata SGPRs.
 */
static void si_emit_derived_tess_state(struct si_context *sctx,
				       const struct pipe_draw_info *info,
				       unsigned *num_patches)
{
	struct radeon_winsys_cs *cs = sctx->b.gfx.cs;
	struct si_shader_ctx_state *ls = &sctx->vs_shader;
	/* The TES pointer will only be used for sctx->last_tcs.
	 * It would be wrong to think that TCS = TES. */
	struct si_shader_selector *tcs =
		sctx->tcs_shader.cso ? sctx->tcs_shader.cso : sctx->tes_shader.cso;
	unsigned tes_sh_base = sctx->shader_userdata.sh_base[PIPE_SHADER_TESS_EVAL];
	unsigned num_tcs_input_cp = info->vertices_per_patch;
	unsigned num_tcs_output_cp, num_tcs_inputs, num_tcs_outputs;
	unsigned num_tcs_patch_outputs;
	unsigned input_vertex_size, output_vertex_size, pervertex_output_patch_size;
	unsigned input_patch_size, output_patch_size, output_patch0_offset;
	unsigned perpatch_output_offset, lds_size, ls_rsrc2;
	unsigned tcs_in_layout, tcs_out_layout, tcs_out_offsets;
	unsigned offchip_layout, hardware_lds_size, ls_hs_config;

	if (sctx->last_ls == ls->current &&
	    sctx->last_tcs == tcs &&
	    sctx->last_tes_sh_base == tes_sh_base &&
	    sctx->last_num_tcs_input_cp == num_tcs_input_cp) {
		*num_patches = sctx->last_num_patches;
		return;
	}

	sctx->last_ls = ls->current;
	sctx->last_tcs = tcs;
	sctx->last_tes_sh_base = tes_sh_base;
	sctx->last_num_tcs_input_cp = num_tcs_input_cp;

	/* This calculates how shader inputs and outputs among VS, TCS, and TES
	 * are laid out in LDS. */
	num_tcs_inputs = util_last_bit64(ls->cso->outputs_written);

	if (sctx->tcs_shader.cso) {
		num_tcs_outputs = util_last_bit64(tcs->outputs_written);
		num_tcs_output_cp = tcs->info.properties[TGSI_PROPERTY_TCS_VERTICES_OUT];
		num_tcs_patch_outputs = util_last_bit64(tcs->patch_outputs_written);
	} else {
		/* No TCS. Route varyings from LS to TES. */
		num_tcs_outputs = num_tcs_inputs;
		num_tcs_output_cp = num_tcs_input_cp;
		num_tcs_patch_outputs = 2; /* TESSINNER + TESSOUTER */
	}

	input_vertex_size = num_tcs_inputs * 16;
	output_vertex_size = num_tcs_outputs * 16;

	input_patch_size = num_tcs_input_cp * input_vertex_size;

	pervertex_output_patch_size = num_tcs_output_cp * output_vertex_size;
	output_patch_size = pervertex_output_patch_size + num_tcs_patch_outputs * 16;

	/* Ensure that we only need one wave per SIMD so we don't need to check
	 * resource usage. Also ensures that the number of tcs in and out
	 * vertices per threadgroup are at most 256.
	 */
	*num_patches = 64 / MAX2(num_tcs_input_cp, num_tcs_output_cp) * 4;

	/* Make sure that the data fits in LDS. This assumes the shaders only
	 * use LDS for the inputs and outputs.
	 */
	hardware_lds_size = sctx->b.chip_class >= CIK ? 65536 : 32768;
	*num_patches = MIN2(*num_patches, hardware_lds_size / (input_patch_size +
	                                                       output_patch_size));

	/* Make sure the output data fits in the offchip buffer */
	*num_patches = MIN2(*num_patches,
			    (sctx->screen->tess_offchip_block_dw_size * 4) /
			    output_patch_size);

	/* Not necessary for correctness, but improves performance. The
	 * specific value is taken from the proprietary driver.
	 */
	*num_patches = MIN2(*num_patches, 40);

	/* SI bug workaround - limit LS-HS threadgroups to only one wave. */
	if (sctx->b.chip_class == SI) {
		unsigned one_wave = 64 / MAX2(num_tcs_input_cp, num_tcs_output_cp);
		*num_patches = MIN2(*num_patches, one_wave);
	}

	sctx->last_num_patches = *num_patches;

	output_patch0_offset = input_patch_size * *num_patches;
	perpatch_output_offset = output_patch0_offset + pervertex_output_patch_size;

	lds_size = output_patch0_offset + output_patch_size * *num_patches;
	ls_rsrc2 = ls->current->config.rsrc2;

	if (sctx->b.chip_class >= CIK) {
		assert(lds_size <= 65536);
		lds_size = align(lds_size, 512) / 512;
	} else {
		assert(lds_size <= 32768);
		lds_size = align(lds_size, 256) / 256;
	}
	si_multiwave_lds_size_workaround(sctx->screen, &lds_size);
	ls_rsrc2 |= S_00B52C_LDS_SIZE(lds_size);

	/* Due to a hw bug, RSRC2_LS must be written twice with another
	 * LS register written in between. */
	if (sctx->b.chip_class == CIK && sctx->b.family != CHIP_HAWAII)
		radeon_set_sh_reg(cs, R_00B52C_SPI_SHADER_PGM_RSRC2_LS, ls_rsrc2);
	radeon_set_sh_reg_seq(cs, R_00B528_SPI_SHADER_PGM_RSRC1_LS, 2);
	radeon_emit(cs, ls->current->config.rsrc1);
	radeon_emit(cs, ls_rsrc2);

	/* Compute userdata SGPRs. */
	assert(((input_vertex_size / 4) & ~0xff) == 0);
	assert(((output_vertex_size / 4) & ~0xff) == 0);
	assert(((input_patch_size / 4) & ~0x1fff) == 0);
	assert(((output_patch_size / 4) & ~0x1fff) == 0);
	assert(((output_patch0_offset / 16) & ~0xffff) == 0);
	assert(((perpatch_output_offset / 16) & ~0xffff) == 0);
	assert(num_tcs_input_cp <= 32);
	assert(num_tcs_output_cp <= 32);

	tcs_in_layout = S_VS_STATE_LS_OUT_PATCH_SIZE(input_patch_size / 4) |
			S_VS_STATE_LS_OUT_VERTEX_SIZE(input_vertex_size / 4);
	tcs_out_layout = (output_patch_size / 4) |
			 ((output_vertex_size / 4) << 13);
	tcs_out_offsets = (output_patch0_offset / 16) |
			  ((perpatch_output_offset / 16) << 16);
	offchip_layout = (pervertex_output_patch_size * *num_patches << 16) |
			 (num_tcs_output_cp << 9) | *num_patches;

	/* Set them for LS. */
	sctx->current_vs_state &= C_VS_STATE_LS_OUT_PATCH_SIZE &
				  C_VS_STATE_LS_OUT_VERTEX_SIZE;
	sctx->current_vs_state |= tcs_in_layout;

	/* Set them for TCS. */
	radeon_set_sh_reg_seq(cs,
		R_00B430_SPI_SHADER_USER_DATA_HS_0 + SI_SGPR_TCS_OFFCHIP_LAYOUT * 4, 4);
	radeon_emit(cs, offchip_layout);
	radeon_emit(cs, tcs_out_offsets);
	radeon_emit(cs, tcs_out_layout | (num_tcs_input_cp << 26));
	radeon_emit(cs, tcs_in_layout);

	/* Set them for TES. */
	radeon_set_sh_reg_seq(cs, tes_sh_base + SI_SGPR_TCS_OFFCHIP_LAYOUT * 4, 1);
	radeon_emit(cs, offchip_layout);

	ls_hs_config = S_028B58_NUM_PATCHES(*num_patches) |
		       S_028B58_HS_NUM_INPUT_CP(num_tcs_input_cp) |
		       S_028B58_HS_NUM_OUTPUT_CP(num_tcs_output_cp);

	if (sctx->b.chip_class >= CIK)
		radeon_set_context_reg_idx(cs, R_028B58_VGT_LS_HS_CONFIG, 2,
					   ls_hs_config);
	else
		radeon_set_context_reg(cs, R_028B58_VGT_LS_HS_CONFIG,
				       ls_hs_config);
}

static unsigned si_num_prims_for_vertices(const struct pipe_draw_info *info)
{
	switch (info->mode) {
	case PIPE_PRIM_PATCHES:
		return info->count / info->vertices_per_patch;
	case R600_PRIM_RECTANGLE_LIST:
		return info->count / 3;
	default:
		return u_prims_for_vertices(info->mode, info->count);
	}
}

static unsigned
si_get_init_multi_vgt_param(struct si_screen *sscreen,
			    union si_vgt_param_key *key)
{
	STATIC_ASSERT(sizeof(union si_vgt_param_key) == 4);
	unsigned max_primgroup_in_wave = 2;

	/* SWITCH_ON_EOP(0) is always preferable. */
	bool wd_switch_on_eop = false;
	bool ia_switch_on_eop = false;
	bool ia_switch_on_eoi = false;
	bool partial_vs_wave = false;
	bool partial_es_wave = false;

	if (key->u.uses_tess) {
		/* SWITCH_ON_EOI must be set if PrimID is used. */
		if (key->u.tcs_tes_uses_prim_id)
			ia_switch_on_eoi = true;

		/* Bug with tessellation and GS on Bonaire and older 2 SE chips. */
		if ((sscreen->b.family == CHIP_TAHITI ||
		     sscreen->b.family == CHIP_PITCAIRN ||
		     sscreen->b.family == CHIP_BONAIRE) &&
		    key->u.uses_gs)
			partial_vs_wave = true;

		/* Needed for 028B6C_DISTRIBUTION_MODE != 0 */
		if (sscreen->has_distributed_tess) {
			if (key->u.uses_gs) {
				if (sscreen->b.chip_class <= VI)
					partial_es_wave = true;

				/* GPU hang workaround. */
				if (sscreen->b.family == CHIP_TONGA ||
				    sscreen->b.family == CHIP_FIJI ||
				    sscreen->b.family == CHIP_POLARIS10 ||
				    sscreen->b.family == CHIP_POLARIS11 ||
				    sscreen->b.family == CHIP_POLARIS12)
					partial_vs_wave = true;
			} else {
				partial_vs_wave = true;
			}
		}
	}

	/* This is a hardware requirement. */
	if (key->u.line_stipple_enabled ||
	    (sscreen->b.debug_flags & DBG_SWITCH_ON_EOP)) {
		ia_switch_on_eop = true;
		wd_switch_on_eop = true;
	}

	if (sscreen->b.chip_class >= CIK) {
		/* WD_SWITCH_ON_EOP has no effect on GPUs with less than
		 * 4 shader engines. Set 1 to pass the assertion below.
		 * The other cases are hardware requirements.
		 *
		 * Polaris supports primitive restart with WD_SWITCH_ON_EOP=0
		 * for points, line strips, and tri strips.
		 */
		if (sscreen->b.info.max_se < 4 ||
		    key->u.prim == PIPE_PRIM_POLYGON ||
		    key->u.prim == PIPE_PRIM_LINE_LOOP ||
		    key->u.prim == PIPE_PRIM_TRIANGLE_FAN ||
		    key->u.prim == PIPE_PRIM_TRIANGLE_STRIP_ADJACENCY ||
		    (key->u.primitive_restart &&
		     (sscreen->b.family < CHIP_POLARIS10 ||
		      (key->u.prim != PIPE_PRIM_POINTS &&
		       key->u.prim != PIPE_PRIM_LINE_STRIP &&
		       key->u.prim != PIPE_PRIM_TRIANGLE_STRIP))) ||
		    key->u.count_from_stream_output)
			wd_switch_on_eop = true;

		/* Hawaii hangs if instancing is enabled and WD_SWITCH_ON_EOP is 0.
		 * We don't know that for indirect drawing, so treat it as
		 * always problematic. */
		if (sscreen->b.family == CHIP_HAWAII &&
		    key->u.uses_instancing)
			wd_switch_on_eop = true;

		/* Performance recommendation for 4 SE Gfx7-8 parts if
		 * instances are smaller than a primgroup.
		 * Assume indirect draws always use small instances.
		 * This is needed for good VS wave utilization.
		 */
		if (sscreen->b.chip_class <= VI &&
		    sscreen->b.info.max_se == 4 &&
		    key->u.multi_instances_smaller_than_primgroup)
			wd_switch_on_eop = true;

		/* Required on CIK and later. */
		if (sscreen->b.info.max_se > 2 && !wd_switch_on_eop)
			ia_switch_on_eoi = true;

		/* Required by Hawaii and, for some special cases, by VI. */
		if (ia_switch_on_eoi &&
		    (sscreen->b.family == CHIP_HAWAII ||
		     (sscreen->b.chip_class == VI &&
		      (key->u.uses_gs || max_primgroup_in_wave != 2))))
			partial_vs_wave = true;

		/* Instancing bug on Bonaire. */
		if (sscreen->b.family == CHIP_BONAIRE && ia_switch_on_eoi &&
		    key->u.uses_instancing)
			partial_vs_wave = true;

		/* If the WD switch is false, the IA switch must be false too. */
		assert(wd_switch_on_eop || !ia_switch_on_eop);
	}

	/* If SWITCH_ON_EOI is set, PARTIAL_ES_WAVE must be set too. */
	if (sscreen->b.chip_class <= VI && ia_switch_on_eoi)
		partial_es_wave = true;

	return S_028AA8_SWITCH_ON_EOP(ia_switch_on_eop) |
		S_028AA8_SWITCH_ON_EOI(ia_switch_on_eoi) |
		S_028AA8_PARTIAL_VS_WAVE_ON(partial_vs_wave) |
		S_028AA8_PARTIAL_ES_WAVE_ON(partial_es_wave) |
		S_028AA8_WD_SWITCH_ON_EOP(sscreen->b.chip_class >= CIK ? wd_switch_on_eop : 0) |
		/* The following field was moved to VGT_SHADER_STAGES_EN in GFX9. */
		S_028AA8_MAX_PRIMGRP_IN_WAVE(sscreen->b.chip_class == VI ?
					     max_primgroup_in_wave : 0) |
		S_030960_EN_INST_OPT_BASIC(sscreen->b.chip_class >= GFX9) |
		S_030960_EN_INST_OPT_ADV(sscreen->b.chip_class >= GFX9);
}

void si_init_ia_multi_vgt_param_table(struct si_context *sctx)
{
	for (int prim = 0; prim <= R600_PRIM_RECTANGLE_LIST; prim++)
	for (int uses_instancing = 0; uses_instancing < 2; uses_instancing++)
	for (int multi_instances = 0; multi_instances < 2; multi_instances++)
	for (int primitive_restart = 0; primitive_restart < 2; primitive_restart++)
	for (int count_from_so = 0; count_from_so < 2; count_from_so++)
	for (int line_stipple = 0; line_stipple < 2; line_stipple++)
	for (int uses_tess = 0; uses_tess < 2; uses_tess++)
	for (int tess_uses_primid = 0; tess_uses_primid < 2; tess_uses_primid++)
	for (int uses_gs = 0; uses_gs < 2; uses_gs++) {
		union si_vgt_param_key key;

		key.index = 0;
		key.u.prim = prim;
		key.u.uses_instancing = uses_instancing;
		key.u.multi_instances_smaller_than_primgroup = multi_instances;
		key.u.primitive_restart = primitive_restart;
		key.u.count_from_stream_output = count_from_so;
		key.u.line_stipple_enabled = line_stipple;
		key.u.uses_tess = uses_tess;
		key.u.tcs_tes_uses_prim_id = tess_uses_primid;
		key.u.uses_gs = uses_gs;

		sctx->ia_multi_vgt_param[key.index] =
			si_get_init_multi_vgt_param(sctx->screen, &key);
	}
}

static unsigned si_get_ia_multi_vgt_param(struct si_context *sctx,
					  const struct pipe_draw_info *info,
					  unsigned num_patches)
{
	union si_vgt_param_key key = sctx->ia_multi_vgt_param_key;
	unsigned primgroup_size;
	unsigned ia_multi_vgt_param;

	if (sctx->tes_shader.cso) {
		primgroup_size = num_patches; /* must be a multiple of NUM_PATCHES */
	} else if (sctx->gs_shader.cso) {
		primgroup_size = 64; /* recommended with a GS */
	} else {
		primgroup_size = 128; /* recommended without a GS and tess */
	}

	key.u.prim = info->mode;
	key.u.uses_instancing = info->indirect || info->instance_count > 1;
	key.u.multi_instances_smaller_than_primgroup =
		info->indirect ||
		(info->instance_count > 1 &&
		 (info->count_from_stream_output ||
		  si_num_prims_for_vertices(info) < primgroup_size));
	key.u.primitive_restart = info->primitive_restart;
	key.u.count_from_stream_output = info->count_from_stream_output != NULL;

	ia_multi_vgt_param = sctx->ia_multi_vgt_param[key.index] |
			     S_028AA8_PRIMGROUP_SIZE(primgroup_size - 1);

	if (sctx->gs_shader.cso) {
		/* GS requirement. */
		if (SI_GS_PER_ES / primgroup_size >= sctx->screen->gs_table_depth - 3)
			ia_multi_vgt_param |= S_028AA8_PARTIAL_ES_WAVE_ON(1);

		/* GS hw bug with single-primitive instances and SWITCH_ON_EOI.
		 * The hw doc says all multi-SE chips are affected, but Vulkan
		 * only applies it to Hawaii. Do what Vulkan does.
		 */
		if (sctx->b.family == CHIP_HAWAII &&
		    G_028AA8_SWITCH_ON_EOI(ia_multi_vgt_param) &&
		    (info->indirect ||
		     (info->instance_count > 1 &&
		      (info->count_from_stream_output ||
		       si_num_prims_for_vertices(info) <= 1))))
			sctx->b.flags |= SI_CONTEXT_VGT_FLUSH;
	}

	return ia_multi_vgt_param;
}

/* rast_prim is the primitive type after GS. */
static void si_emit_rasterizer_prim_state(struct si_context *sctx)
{
	struct radeon_winsys_cs *cs = sctx->b.gfx.cs;
	enum pipe_prim_type rast_prim = sctx->current_rast_prim;
	struct si_state_rasterizer *rs = sctx->emitted.named.rasterizer;

	/* Skip this if not rendering lines. */
	if (rast_prim != PIPE_PRIM_LINES &&
	    rast_prim != PIPE_PRIM_LINE_LOOP &&
	    rast_prim != PIPE_PRIM_LINE_STRIP &&
	    rast_prim != PIPE_PRIM_LINES_ADJACENCY &&
	    rast_prim != PIPE_PRIM_LINE_STRIP_ADJACENCY)
		return;

	if (rast_prim == sctx->last_rast_prim &&
	    rs->pa_sc_line_stipple == sctx->last_sc_line_stipple)
		return;

	/* For lines, reset the stipple pattern at each primitive. Otherwise,
	 * reset the stipple pattern at each packet (line strips, line loops).
	 */
	radeon_set_context_reg(cs, R_028A0C_PA_SC_LINE_STIPPLE,
		rs->pa_sc_line_stipple |
		S_028A0C_AUTO_RESET_CNTL(rast_prim == PIPE_PRIM_LINES ? 1 : 2));

	sctx->last_rast_prim = rast_prim;
	sctx->last_sc_line_stipple = rs->pa_sc_line_stipple;
}

static void si_emit_vs_state(struct si_context *sctx,
			     const struct pipe_draw_info *info)
{
	sctx->current_vs_state &= C_VS_STATE_INDEXED;
	sctx->current_vs_state |= S_VS_STATE_INDEXED(!!info->indexed);

	if (sctx->current_vs_state != sctx->last_vs_state) {
		struct radeon_winsys_cs *cs = sctx->b.gfx.cs;

		radeon_set_sh_reg(cs,
			sctx->shader_userdata.sh_base[PIPE_SHADER_VERTEX] +
			SI_SGPR_VS_STATE_BITS * 4,
			sctx->current_vs_state);

		sctx->last_vs_state = sctx->current_vs_state;
	}
}

static void si_emit_draw_registers(struct si_context *sctx,
				   const struct pipe_draw_info *info,
				   unsigned num_patches)
{
	struct radeon_winsys_cs *cs = sctx->b.gfx.cs;
	unsigned prim = si_conv_pipe_prim(info->mode);
	unsigned gs_out_prim = si_conv_prim_to_gs_out(sctx->current_rast_prim);
	unsigned ia_multi_vgt_param;

	ia_multi_vgt_param = si_get_ia_multi_vgt_param(sctx, info, num_patches);

	/* Draw state. */
	if (ia_multi_vgt_param != sctx->last_multi_vgt_param) {
		if (sctx->b.chip_class >= GFX9)
			radeon_set_uconfig_reg_idx(cs, R_030960_IA_MULTI_VGT_PARAM, 4, ia_multi_vgt_param);
		else if (sctx->b.chip_class >= CIK)
			radeon_set_context_reg_idx(cs, R_028AA8_IA_MULTI_VGT_PARAM, 1, ia_multi_vgt_param);
		else
			radeon_set_context_reg(cs, R_028AA8_IA_MULTI_VGT_PARAM, ia_multi_vgt_param);

		sctx->last_multi_vgt_param = ia_multi_vgt_param;
	}
	if (prim != sctx->last_prim) {
		if (sctx->b.chip_class >= CIK)
			radeon_set_uconfig_reg_idx(cs, R_030908_VGT_PRIMITIVE_TYPE, 1, prim);
		else
			radeon_set_config_reg(cs, R_008958_VGT_PRIMITIVE_TYPE, prim);

		sctx->last_prim = prim;
	}

	if (gs_out_prim != sctx->last_gs_out_prim) {
		radeon_set_context_reg(cs, R_028A6C_VGT_GS_OUT_PRIM_TYPE, gs_out_prim);
		sctx->last_gs_out_prim = gs_out_prim;
	}

	/* Primitive restart. */
	if (info->primitive_restart != sctx->last_primitive_restart_en) {
		if (sctx->b.chip_class >= GFX9)
			radeon_set_uconfig_reg(cs, R_03092C_VGT_MULTI_PRIM_IB_RESET_EN,
					       info->primitive_restart);
		else
			radeon_set_context_reg(cs, R_028A94_VGT_MULTI_PRIM_IB_RESET_EN,
					       info->primitive_restart);

		sctx->last_primitive_restart_en = info->primitive_restart;

	}
	if (info->primitive_restart &&
	    (info->restart_index != sctx->last_restart_index ||
	     sctx->last_restart_index == SI_RESTART_INDEX_UNKNOWN)) {
		radeon_set_context_reg(cs, R_02840C_VGT_MULTI_PRIM_IB_RESET_INDX,
				       info->restart_index);
		sctx->last_restart_index = info->restart_index;
	}
}

static void si_emit_draw_packets(struct si_context *sctx,
				 const struct pipe_draw_info *info,
				 const struct pipe_index_buffer *ib)
{
	struct radeon_winsys_cs *cs = sctx->b.gfx.cs;
	unsigned sh_base_reg = sctx->shader_userdata.sh_base[PIPE_SHADER_VERTEX];
	bool render_cond_bit = sctx->b.render_cond && !sctx->b.render_cond_force_off;
	uint32_t index_max_size = 0;
	uint64_t index_va = 0;

	if (info->count_from_stream_output) {
		struct r600_so_target *t =
			(struct r600_so_target*)info->count_from_stream_output;
		uint64_t va = t->buf_filled_size->gpu_address +
			      t->buf_filled_size_offset;

		radeon_set_context_reg(cs, R_028B30_VGT_STRMOUT_DRAW_OPAQUE_VERTEX_STRIDE,
				       t->stride_in_dw);

		radeon_emit(cs, PKT3(PKT3_COPY_DATA, 4, 0));
		radeon_emit(cs, COPY_DATA_SRC_SEL(COPY_DATA_MEM) |
			    COPY_DATA_DST_SEL(COPY_DATA_REG) |
			    COPY_DATA_WR_CONFIRM);
		radeon_emit(cs, va);     /* src address lo */
		radeon_emit(cs, va >> 32); /* src address hi */
		radeon_emit(cs, R_028B2C_VGT_STRMOUT_DRAW_OPAQUE_BUFFER_FILLED_SIZE >> 2);
		radeon_emit(cs, 0); /* unused */

		radeon_add_to_buffer_list(&sctx->b, &sctx->b.gfx,
				      t->buf_filled_size, RADEON_USAGE_READ,
				      RADEON_PRIO_SO_FILLED_SIZE);
	}

	/* draw packet */
	if (info->indexed) {
		if (ib->index_size != sctx->last_index_size) {
			unsigned index_type;

			/* index type */
			switch (ib->index_size) {
			case 1:
				index_type = V_028A7C_VGT_INDEX_8;
				break;
			case 2:
				index_type = V_028A7C_VGT_INDEX_16 |
					     (SI_BIG_ENDIAN && sctx->b.chip_class <= CIK ?
						      V_028A7C_VGT_DMA_SWAP_16_BIT : 0);
				break;
			case 4:
				index_type = V_028A7C_VGT_INDEX_32 |
					     (SI_BIG_ENDIAN && sctx->b.chip_class <= CIK ?
						      V_028A7C_VGT_DMA_SWAP_32_BIT : 0);
				break;
			default:
				assert(!"unreachable");
				return;
			}

			if (sctx->b.chip_class >= GFX9) {
				radeon_set_uconfig_reg_idx(cs, R_03090C_VGT_INDEX_TYPE,
							   2, index_type);
			} else {
				radeon_emit(cs, PKT3(PKT3_INDEX_TYPE, 0, 0));
				radeon_emit(cs, index_type);
			}

			sctx->last_index_size = ib->index_size;
		}

		index_max_size = (ib->buffer->width0 - ib->offset) /
				  ib->index_size;
		index_va = r600_resource(ib->buffer)->gpu_address + ib->offset;

		radeon_add_to_buffer_list(&sctx->b, &sctx->b.gfx,
				      (struct r600_resource *)ib->buffer,
				      RADEON_USAGE_READ, RADEON_PRIO_INDEX_BUFFER);
	} else {
		/* On CI and later, non-indexed draws overwrite VGT_INDEX_TYPE,
		 * so the state must be re-emitted before the next indexed draw.
		 */
		if (sctx->b.chip_class >= CIK)
			sctx->last_index_size = -1;
	}

	if (info->indirect) {
		uint64_t indirect_va = r600_resource(info->indirect)->gpu_address;

		assert(indirect_va % 8 == 0);

		si_invalidate_draw_sh_constants(sctx);

		radeon_emit(cs, PKT3(PKT3_SET_BASE, 2, 0));
		radeon_emit(cs, 1);
		radeon_emit(cs, indirect_va);
		radeon_emit(cs, indirect_va >> 32);

		radeon_add_to_buffer_list(&sctx->b, &sctx->b.gfx,
				      (struct r600_resource *)info->indirect,
				      RADEON_USAGE_READ, RADEON_PRIO_DRAW_INDIRECT);

		unsigned di_src_sel = info->indexed ? V_0287F0_DI_SRC_SEL_DMA
						    : V_0287F0_DI_SRC_SEL_AUTO_INDEX;

		assert(info->indirect_offset % 4 == 0);

		if (info->indexed) {
			radeon_emit(cs, PKT3(PKT3_INDEX_BASE, 1, 0));
			radeon_emit(cs, index_va);
			radeon_emit(cs, index_va >> 32);

			radeon_emit(cs, PKT3(PKT3_INDEX_BUFFER_SIZE, 0, 0));
			radeon_emit(cs, index_max_size);
		}

		if (!sctx->screen->has_draw_indirect_multi) {
			radeon_emit(cs, PKT3(info->indexed ? PKT3_DRAW_INDEX_INDIRECT
							   : PKT3_DRAW_INDIRECT,
					     3, render_cond_bit));
			radeon_emit(cs, info->indirect_offset);
			radeon_emit(cs, (sh_base_reg + SI_SGPR_BASE_VERTEX * 4 - SI_SH_REG_OFFSET) >> 2);
			radeon_emit(cs, (sh_base_reg + SI_SGPR_START_INSTANCE * 4 - SI_SH_REG_OFFSET) >> 2);
			radeon_emit(cs, di_src_sel);
		} else {
			uint64_t count_va = 0;

			if (info->indirect_params) {
				struct r600_resource *params_buf =
					(struct r600_resource *)info->indirect_params;

				radeon_add_to_buffer_list(
					&sctx->b, &sctx->b.gfx, params_buf,
					RADEON_USAGE_READ, RADEON_PRIO_DRAW_INDIRECT);

				count_va = params_buf->gpu_address + info->indirect_params_offset;
			}

			radeon_emit(cs, PKT3(info->indexed ? PKT3_DRAW_INDEX_INDIRECT_MULTI :
							     PKT3_DRAW_INDIRECT_MULTI,
					     8, render_cond_bit));
			radeon_emit(cs, info->indirect_offset);
			radeon_emit(cs, (sh_base_reg + SI_SGPR_BASE_VERTEX * 4 - SI_SH_REG_OFFSET) >> 2);
			radeon_emit(cs, (sh_base_reg + SI_SGPR_START_INSTANCE * 4 - SI_SH_REG_OFFSET) >> 2);
			radeon_emit(cs, ((sh_base_reg + SI_SGPR_DRAWID * 4 - SI_SH_REG_OFFSET) >> 2) |
					S_2C3_DRAW_INDEX_ENABLE(1) |
					S_2C3_COUNT_INDIRECT_ENABLE(!!info->indirect_params));
			radeon_emit(cs, info->indirect_count);
			radeon_emit(cs, count_va);
			radeon_emit(cs, count_va >> 32);
			radeon_emit(cs, info->indirect_stride);
			radeon_emit(cs, di_src_sel);
		}
	} else {
		int base_vertex;

		radeon_emit(cs, PKT3(PKT3_NUM_INSTANCES, 0, 0));
		radeon_emit(cs, info->instance_count);

		/* Base vertex and start instance. */
		base_vertex = info->indexed ? info->index_bias : info->start;

		if (base_vertex != sctx->last_base_vertex ||
		    sctx->last_base_vertex == SI_BASE_VERTEX_UNKNOWN ||
		    info->start_instance != sctx->last_start_instance ||
		    info->drawid != sctx->last_drawid ||
		    sh_base_reg != sctx->last_sh_base_reg) {
			radeon_set_sh_reg_seq(cs, sh_base_reg + SI_SGPR_BASE_VERTEX * 4, 3);
			radeon_emit(cs, base_vertex);
			radeon_emit(cs, info->start_instance);
			radeon_emit(cs, info->drawid);

			sctx->last_base_vertex = base_vertex;
			sctx->last_start_instance = info->start_instance;
			sctx->last_drawid = info->drawid;
			sctx->last_sh_base_reg = sh_base_reg;
		}

		if (info->indexed) {
			index_va += info->start * ib->index_size;

			radeon_emit(cs, PKT3(PKT3_DRAW_INDEX_2, 4, render_cond_bit));
			radeon_emit(cs, index_max_size);
			radeon_emit(cs, index_va);
			radeon_emit(cs, index_va >> 32);
			radeon_emit(cs, info->count);
			radeon_emit(cs, V_0287F0_DI_SRC_SEL_DMA);
		} else {
			radeon_emit(cs, PKT3(PKT3_DRAW_INDEX_AUTO, 1, render_cond_bit));
			radeon_emit(cs, info->count);
			radeon_emit(cs, V_0287F0_DI_SRC_SEL_AUTO_INDEX |
				        S_0287F0_USE_OPAQUE(!!info->count_from_stream_output));
		}
	}
}

static void si_emit_surface_sync(struct r600_common_context *rctx,
				 unsigned cp_coher_cntl)
{
	struct radeon_winsys_cs *cs = rctx->gfx.cs;

	if (rctx->chip_class >= GFX9) {
		/* Flush caches and wait for the caches to assert idle. */
		radeon_emit(cs, PKT3(PKT3_ACQUIRE_MEM, 5, 0));
		radeon_emit(cs, cp_coher_cntl);	/* CP_COHER_CNTL */
		radeon_emit(cs, 0xffffffff);	/* CP_COHER_SIZE */
		radeon_emit(cs, 0xffffff);	/* CP_COHER_SIZE_HI */
		radeon_emit(cs, 0);		/* CP_COHER_BASE */
		radeon_emit(cs, 0);		/* CP_COHER_BASE_HI */
		radeon_emit(cs, 0x0000000A);	/* POLL_INTERVAL */
	} else {
		/* ACQUIRE_MEM is only required on a compute ring. */
		radeon_emit(cs, PKT3(PKT3_SURFACE_SYNC, 3, 0));
		radeon_emit(cs, cp_coher_cntl);   /* CP_COHER_CNTL */
		radeon_emit(cs, 0xffffffff);      /* CP_COHER_SIZE */
		radeon_emit(cs, 0);               /* CP_COHER_BASE */
		radeon_emit(cs, 0x0000000A);      /* POLL_INTERVAL */
	}
}

void si_emit_cache_flush(struct si_context *sctx)
{
	struct r600_common_context *rctx = &sctx->b;
	struct radeon_winsys_cs *cs = rctx->gfx.cs;
	uint32_t cp_coher_cntl = 0;
	uint32_t flush_cb_db = rctx->flags & (SI_CONTEXT_FLUSH_AND_INV_CB |
					      SI_CONTEXT_FLUSH_AND_INV_DB);

	if (rctx->flags & (SI_CONTEXT_FLUSH_AND_INV_CB |
			   SI_CONTEXT_FLUSH_AND_INV_DB))
		sctx->b.num_fb_cache_flushes++;

	/* SI has a bug that it always flushes ICACHE and KCACHE if either
	 * bit is set. An alternative way is to write SQC_CACHES, but that
	 * doesn't seem to work reliably. Since the bug doesn't affect
	 * correctness (it only does more work than necessary) and
	 * the performance impact is likely negligible, there is no plan
	 * to add a workaround for it.
	 */

	if (rctx->flags & SI_CONTEXT_INV_ICACHE)
		cp_coher_cntl |= S_0085F0_SH_ICACHE_ACTION_ENA(1);
	if (rctx->flags & SI_CONTEXT_INV_SMEM_L1)
		cp_coher_cntl |= S_0085F0_SH_KCACHE_ACTION_ENA(1);

	if (rctx->chip_class <= VI) {
		if (rctx->flags & SI_CONTEXT_FLUSH_AND_INV_CB) {
			cp_coher_cntl |= S_0085F0_CB_ACTION_ENA(1) |
					 S_0085F0_CB0_DEST_BASE_ENA(1) |
					 S_0085F0_CB1_DEST_BASE_ENA(1) |
					 S_0085F0_CB2_DEST_BASE_ENA(1) |
					 S_0085F0_CB3_DEST_BASE_ENA(1) |
					 S_0085F0_CB4_DEST_BASE_ENA(1) |
					 S_0085F0_CB5_DEST_BASE_ENA(1) |
					 S_0085F0_CB6_DEST_BASE_ENA(1) |
					 S_0085F0_CB7_DEST_BASE_ENA(1);

			/* Necessary for DCC */
			if (rctx->chip_class == VI)
				r600_gfx_write_event_eop(rctx, V_028A90_FLUSH_AND_INV_CB_DATA_TS,
							 0, 0, NULL, 0, 0, 0);
		}
		if (rctx->flags & SI_CONTEXT_FLUSH_AND_INV_DB)
			cp_coher_cntl |= S_0085F0_DB_ACTION_ENA(1) |
					 S_0085F0_DB_DEST_BASE_ENA(1);
	}

	if (rctx->flags & SI_CONTEXT_FLUSH_AND_INV_CB) {
		/* Flush CMASK/FMASK/DCC. SURFACE_SYNC will wait for idle. */
		radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
		radeon_emit(cs, EVENT_TYPE(V_028A90_FLUSH_AND_INV_CB_META) | EVENT_INDEX(0));
	}
	if (rctx->flags & SI_CONTEXT_FLUSH_AND_INV_DB) {
		/* Flush HTILE. SURFACE_SYNC will wait for idle. */
		radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
		radeon_emit(cs, EVENT_TYPE(V_028A90_FLUSH_AND_INV_DB_META) | EVENT_INDEX(0));
	}

	/* Wait for shader engines to go idle.
	 * VS and PS waits are unnecessary if SURFACE_SYNC is going to wait
	 * for everything including CB/DB cache flushes.
	 */
	if (!flush_cb_db) {
		if (rctx->flags & SI_CONTEXT_PS_PARTIAL_FLUSH) {
			radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
			radeon_emit(cs, EVENT_TYPE(V_028A90_PS_PARTIAL_FLUSH) | EVENT_INDEX(4));
			/* Only count explicit shader flushes, not implicit ones
			 * done by SURFACE_SYNC.
			 */
			rctx->num_vs_flushes++;
			rctx->num_ps_flushes++;
		} else if (rctx->flags & SI_CONTEXT_VS_PARTIAL_FLUSH) {
			radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
			radeon_emit(cs, EVENT_TYPE(V_028A90_VS_PARTIAL_FLUSH) | EVENT_INDEX(4));
			rctx->num_vs_flushes++;
		}
	}

	if (rctx->flags & SI_CONTEXT_CS_PARTIAL_FLUSH &&
	    sctx->compute_is_busy) {
		radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
		radeon_emit(cs, EVENT_TYPE(V_028A90_CS_PARTIAL_FLUSH | EVENT_INDEX(4)));
		rctx->num_cs_flushes++;
		sctx->compute_is_busy = false;
	}

	/* VGT state synchronization. */
	if (rctx->flags & SI_CONTEXT_VGT_FLUSH) {
		radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
		radeon_emit(cs, EVENT_TYPE(V_028A90_VGT_FLUSH) | EVENT_INDEX(0));
	}
	if (rctx->flags & SI_CONTEXT_VGT_STREAMOUT_SYNC) {
		radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
		radeon_emit(cs, EVENT_TYPE(V_028A90_VGT_STREAMOUT_SYNC) | EVENT_INDEX(0));
	}

	/* GFX9: Wait for idle if we're flushing CB or DB. ACQUIRE_MEM doesn't
	 * wait for idle on GFX9. We have to use a TS event.
	 */
	if (sctx->b.chip_class >= GFX9 && flush_cb_db) {
		struct r600_resource *rbuf = NULL;
		uint64_t va;
		unsigned offset = 0, tc_flags, cb_db_event;

		/* Set the CB/DB flush event. */
		switch (flush_cb_db) {
		case SI_CONTEXT_FLUSH_AND_INV_CB:
			cb_db_event = V_028A90_FLUSH_AND_INV_CB_DATA_TS;
			break;
		case SI_CONTEXT_FLUSH_AND_INV_DB:
			cb_db_event = V_028A90_FLUSH_AND_INV_DB_DATA_TS;
			break;
		default:
			/* both CB & DB */
			cb_db_event = V_028A90_CACHE_FLUSH_AND_INV_TS_EVENT;
		}

		/* TC    | TC_WB         = invalidate L2 data
		 * TC_MD | TC_WB         = invalidate L2 metadata
		 * TC    | TC_WB | TC_MD = invalidate L2 data & metadata
		 *
		 * The metadata cache must always be invalidated for coherency
		 * between CB/DB and shaders. (metadata = HTILE, CMASK, DCC)
		 *
		 * TC must be invalidated on GFX9 only if the CB/DB surface is
		 * not pipe-aligned. If the surface is RB-aligned, it might not
		 * strictly be pipe-aligned since RB alignment takes precendence.
		 */
		tc_flags = EVENT_TC_WB_ACTION_ENA |
			   EVENT_TC_MD_ACTION_ENA;

		/* Ideally flush TC together with CB/DB. */
		if (rctx->flags & SI_CONTEXT_INV_GLOBAL_L2) {
			tc_flags |= EVENT_TC_ACTION_ENA |
				    EVENT_TCL1_ACTION_ENA;

			/* Clear the flags. */
			rctx->flags &= ~(SI_CONTEXT_INV_GLOBAL_L2 |
					 SI_CONTEXT_WRITEBACK_GLOBAL_L2 |
					 SI_CONTEXT_INV_VMEM_L1);
		}

		/* Allocate memory for the fence. */
		u_suballocator_alloc(rctx->allocator_zeroed_memory, 4, 4,
				     &offset, (struct pipe_resource**)&rbuf);
		va = rbuf->gpu_address + offset;

		r600_gfx_write_event_eop(rctx, cb_db_event, tc_flags, 1,
					 rbuf, va, 0, 1);
		r600_gfx_wait_fence(rctx, va, 1, 0xffffffff);
	}

	/* Make sure ME is idle (it executes most packets) before continuing.
	 * This prevents read-after-write hazards between PFP and ME.
	 */
	if (cp_coher_cntl ||
	    (rctx->flags & (SI_CONTEXT_CS_PARTIAL_FLUSH |
			    SI_CONTEXT_INV_VMEM_L1 |
			    SI_CONTEXT_INV_GLOBAL_L2 |
			    SI_CONTEXT_WRITEBACK_GLOBAL_L2))) {
		radeon_emit(cs, PKT3(PKT3_PFP_SYNC_ME, 0, 0));
		radeon_emit(cs, 0);
	}

	/* SI-CI-VI only:
	 *   When one of the CP_COHER_CNTL.DEST_BASE flags is set, SURFACE_SYNC
	 *   waits for idle, so it should be last. SURFACE_SYNC is done in PFP.
	 *
	 * cp_coher_cntl should contain all necessary flags except TC flags
	 * at this point.
	 *
	 * SI-CIK don't support L2 write-back.
	 */
	if (rctx->flags & SI_CONTEXT_INV_GLOBAL_L2 ||
	    (rctx->chip_class <= CIK &&
	     (rctx->flags & SI_CONTEXT_WRITEBACK_GLOBAL_L2))) {
		/* Invalidate L1 & L2. (L1 is always invalidated on SI)
		 * WB must be set on VI+ when TC_ACTION is set.
		 */
		si_emit_surface_sync(rctx, cp_coher_cntl |
				     S_0085F0_TC_ACTION_ENA(1) |
				     S_0085F0_TCL1_ACTION_ENA(1) |
				     S_0301F0_TC_WB_ACTION_ENA(rctx->chip_class >= VI));
		cp_coher_cntl = 0;
		sctx->b.num_L2_invalidates++;
	} else {
		/* L1 invalidation and L2 writeback must be done separately,
		 * because both operations can't be done together.
		 */
		if (rctx->flags & SI_CONTEXT_WRITEBACK_GLOBAL_L2) {
			/* WB = write-back
			 * NC = apply to non-coherent MTYPEs
			 *      (i.e. MTYPE <= 1, which is what we use everywhere)
			 *
			 * WB doesn't work without NC.
			 */
			si_emit_surface_sync(rctx, cp_coher_cntl |
					     S_0301F0_TC_WB_ACTION_ENA(1) |
					     S_0301F0_TC_NC_ACTION_ENA(1));
			cp_coher_cntl = 0;
			sctx->b.num_L2_writebacks++;
		}
		if (rctx->flags & SI_CONTEXT_INV_VMEM_L1) {
			/* Invalidate per-CU VMEM L1. */
			si_emit_surface_sync(rctx, cp_coher_cntl |
					     S_0085F0_TCL1_ACTION_ENA(1));
			cp_coher_cntl = 0;
		}
	}

	/* If TC flushes haven't cleared this... */
	if (cp_coher_cntl)
		si_emit_surface_sync(rctx, cp_coher_cntl);

	if (rctx->flags & R600_CONTEXT_START_PIPELINE_STATS) {
		radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
		radeon_emit(cs, EVENT_TYPE(V_028A90_PIPELINESTAT_START) |
			        EVENT_INDEX(0));
	} else if (rctx->flags & R600_CONTEXT_STOP_PIPELINE_STATS) {
		radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
		radeon_emit(cs, EVENT_TYPE(V_028A90_PIPELINESTAT_STOP) |
			        EVENT_INDEX(0));
	}

	rctx->flags = 0;
}

static void si_get_draw_start_count(struct si_context *sctx,
				    const struct pipe_draw_info *info,
				    unsigned *start, unsigned *count)
{
	if (info->indirect) {
		unsigned indirect_count;
		struct pipe_transfer *transfer;
		unsigned begin, end;
		unsigned map_size;
		unsigned *data;

		if (info->indirect_params) {
			data = pipe_buffer_map_range(&sctx->b.b,
					info->indirect_params,
					info->indirect_params_offset,
					sizeof(unsigned),
					PIPE_TRANSFER_READ, &transfer);

			indirect_count = *data;

			pipe_buffer_unmap(&sctx->b.b, transfer);
		} else {
			indirect_count = info->indirect_count;
		}

		if (!indirect_count) {
			*start = *count = 0;
			return;
		}

		map_size = (indirect_count - 1) * info->indirect_stride + 3 * sizeof(unsigned);
		data = pipe_buffer_map_range(&sctx->b.b, info->indirect,
					     info->indirect_offset, map_size,
					     PIPE_TRANSFER_READ, &transfer);

		begin = UINT_MAX;
		end = 0;

		for (unsigned i = 0; i < indirect_count; ++i) {
			unsigned count = data[0];
			unsigned start = data[2];

			if (count > 0) {
				begin = MIN2(begin, start);
				end = MAX2(end, start + count);
			}

			data += info->indirect_stride / sizeof(unsigned);
		}

		pipe_buffer_unmap(&sctx->b.b, transfer);

		if (begin < end) {
			*start = begin;
			*count = end - begin;
		} else {
			*start = *count = 0;
		}
	} else {
		*start = info->start;
		*count = info->count;
	}
}

void si_ce_pre_draw_synchronization(struct si_context *sctx)
{
	if (sctx->ce_need_synchronization) {
		radeon_emit(sctx->ce_ib, PKT3(PKT3_INCREMENT_CE_COUNTER, 0, 0));
		radeon_emit(sctx->ce_ib, 1);

		radeon_emit(sctx->b.gfx.cs, PKT3(PKT3_WAIT_ON_CE_COUNTER, 0, 0));
		radeon_emit(sctx->b.gfx.cs, 1);
	}
}

void si_ce_post_draw_synchronization(struct si_context *sctx)
{
	if (sctx->ce_need_synchronization) {
		radeon_emit(sctx->b.gfx.cs, PKT3(PKT3_INCREMENT_DE_COUNTER, 0, 0));
		radeon_emit(sctx->b.gfx.cs, 0);

		sctx->ce_need_synchronization = false;
	}
}

void si_draw_vbo(struct pipe_context *ctx, const struct pipe_draw_info *info)
{
	struct si_context *sctx = (struct si_context *)ctx;
	struct si_state_rasterizer *rs = sctx->queued.named.rasterizer;
	const struct pipe_index_buffer *ib = &sctx->index_buffer;
	struct pipe_index_buffer ib_tmp; /* for index buffer uploads only */
	unsigned mask, dirty_tex_counter;
	enum pipe_prim_type rast_prim;
	unsigned num_patches = 0;

	if (likely(!info->indirect)) {
		/* SI-CI treat instance_count==0 as instance_count==1. There is
		 * no workaround for indirect draws, but we can at least skip
		 * direct draws.
		 */
		if (unlikely(!info->instance_count))
			return;

		/* Handle count == 0. */
		if (unlikely(!info->count &&
			     (info->indexed || !info->count_from_stream_output)))
			return;
	}

	if (unlikely(!sctx->vs_shader.cso)) {
		assert(0);
		return;
	}
	if (unlikely(!sctx->ps_shader.cso && (!rs || !rs->rasterizer_discard))) {
		assert(0);
		return;
	}
	if (unlikely(!!sctx->tes_shader.cso != (info->mode == PIPE_PRIM_PATCHES))) {
		assert(0);
		return;
	}

	/* Recompute and re-emit the texture resource states if needed. */
	dirty_tex_counter = p_atomic_read(&sctx->b.screen->dirty_tex_counter);
	if (unlikely(dirty_tex_counter != sctx->b.last_dirty_tex_counter)) {
		sctx->b.last_dirty_tex_counter = dirty_tex_counter;
		sctx->framebuffer.dirty_cbufs |=
			((1 << sctx->framebuffer.state.nr_cbufs) - 1);
		sctx->framebuffer.dirty_zsbuf = true;
		sctx->framebuffer.do_update_surf_dirtiness = true;
		si_mark_atom_dirty(sctx, &sctx->framebuffer.atom);
		si_update_all_texture_descriptors(sctx);
	}

	si_decompress_graphics_textures(sctx);

	/* Set the rasterization primitive type.
	 *
	 * This must be done after si_decompress_textures, which can call
	 * draw_vbo recursively, and before si_update_shaders, which uses
	 * current_rast_prim for this draw_vbo call. */
	if (sctx->gs_shader.cso)
		rast_prim = sctx->gs_shader.cso->gs_output_prim;
	else if (sctx->tes_shader.cso)
		rast_prim = sctx->tes_shader.cso->info.properties[TGSI_PROPERTY_TES_PRIM_MODE];
	else
		rast_prim = info->mode;

	if (rast_prim != sctx->current_rast_prim) {
		sctx->current_rast_prim = rast_prim;
		sctx->do_update_shaders = true;
	}

	if (sctx->gs_shader.cso) {
		/* Determine whether the GS triangle strip adjacency fix should
		 * be applied. Rotate every other triangle if
		 * - triangle strips with adjacency are fed to the GS and
		 * - primitive restart is disabled (the rotation doesn't help
		 *   when the restart occurs after an odd number of triangles).
		 */
		bool gs_tri_strip_adj_fix =
			!sctx->tes_shader.cso &&
			info->mode == PIPE_PRIM_TRIANGLE_STRIP_ADJACENCY &&
			!info->primitive_restart;

		if (gs_tri_strip_adj_fix != sctx->gs_tri_strip_adj_fix) {
			sctx->gs_tri_strip_adj_fix = gs_tri_strip_adj_fix;
			sctx->do_update_shaders = true;
		}
	}

	if (sctx->do_update_shaders && !si_update_shaders(sctx))
		return;

	if (!si_upload_graphics_shader_descriptors(sctx))
		return;

	ib_tmp.buffer = NULL;

	if (info->indexed) {
		/* Translate or upload, if needed. */
		/* 8-bit indices are supported on VI. */
		if (sctx->b.chip_class <= CIK && ib->index_size == 1) {
			unsigned start, count, start_offset, size;
			void *ptr;

			si_get_draw_start_count(sctx, info, &start, &count);
			start_offset = start * 2;
			size = count * 2;

			u_upload_alloc(ctx->stream_uploader, start_offset,
				       size,
				       si_optimal_tcc_alignment(sctx, size),
				       &ib_tmp.offset, &ib_tmp.buffer, &ptr);
			if (!ib_tmp.buffer)
				return;

			util_shorten_ubyte_elts_to_userptr(&sctx->b.b, ib, 0, 0,
							   ib->offset + start,
							   count, ptr);

			/* info->start will be added by the drawing code */
			ib_tmp.offset -= start_offset;
			ib_tmp.index_size = 2;
			ib = &ib_tmp;
		} else if (ib->user_buffer && !ib->buffer) {
			unsigned start_offset;

			assert(!info->indirect);
			start_offset = info->start * ib->index_size;

			u_upload_data(ctx->stream_uploader, start_offset,
				      info->count * ib->index_size,
				      sctx->screen->b.info.tcc_cache_line_size,
				      (char*)ib->user_buffer + start_offset,
				      &ib_tmp.offset, &ib_tmp.buffer);
			if (!ib_tmp.buffer)
				return;

			/* info->start will be added by the drawing code */
			ib_tmp.offset -= start_offset;
			ib_tmp.index_size = ib->index_size;
			ib = &ib_tmp;
		} else if (sctx->b.chip_class <= CIK &&
			   r600_resource(ib->buffer)->TC_L2_dirty) {
			/* VI reads index buffers through TC L2, so it doesn't
			 * need this. */
			sctx->b.flags |= SI_CONTEXT_WRITEBACK_GLOBAL_L2;
			r600_resource(ib->buffer)->TC_L2_dirty = false;
		}
	}

	if (info->indirect) {
		/* Add the buffer size for memory checking in need_cs_space. */
		r600_context_add_resource_size(ctx, info->indirect);

		if (r600_resource(info->indirect)->TC_L2_dirty) {
			sctx->b.flags |= SI_CONTEXT_WRITEBACK_GLOBAL_L2;
			r600_resource(info->indirect)->TC_L2_dirty = false;
		}

		if (info->indirect_params &&
		    r600_resource(info->indirect_params)->TC_L2_dirty) {
			sctx->b.flags |= SI_CONTEXT_WRITEBACK_GLOBAL_L2;
			r600_resource(info->indirect_params)->TC_L2_dirty = false;
		}
	}

	si_need_cs_space(sctx);

	/* Since we've called r600_context_add_resource_size for vertex buffers,
	 * this must be called after si_need_cs_space, because we must let
	 * need_cs_space flush before we add buffers to the buffer list.
	 */
	if (!si_upload_vertex_buffer_descriptors(sctx))
		return;

	/* GFX9 scissor bug workaround. There is also a more efficient but
	 * more involved alternative workaround. */
	if (sctx->b.chip_class == GFX9 &&
	    si_is_atom_dirty(sctx, &sctx->b.scissors.atom))
		sctx->b.flags |= SI_CONTEXT_PS_PARTIAL_FLUSH;

	/* Flush caches before the first state atom, which does L2 prefetches. */
	if (sctx->b.flags)
		si_emit_cache_flush(sctx);

	/* Emit state atoms. */
	mask = sctx->dirty_atoms;
	while (mask) {
		struct r600_atom *atom = sctx->atoms.array[u_bit_scan(&mask)];

		atom->emit(&sctx->b, atom);
	}
	sctx->dirty_atoms = 0;

	/* Emit states. */
	mask = sctx->dirty_states;
	while (mask) {
		unsigned i = u_bit_scan(&mask);
		struct si_pm4_state *state = sctx->queued.array[i];

		if (!state || sctx->emitted.array[i] == state)
			continue;

		si_pm4_emit(sctx, state);
		sctx->emitted.array[i] = state;
	}
	sctx->dirty_states = 0;

	si_emit_rasterizer_prim_state(sctx);
	if (sctx->tes_shader.cso)
		si_emit_derived_tess_state(sctx, info, &num_patches);
	si_emit_vs_state(sctx, info);
	si_emit_draw_registers(sctx, info, num_patches);

	si_ce_pre_draw_synchronization(sctx);
	si_emit_draw_packets(sctx, info, ib);
	si_ce_post_draw_synchronization(sctx);

	if (sctx->trace_buf)
		si_trace_emit(sctx);

	/* Workaround for a VGT hang when streamout is enabled.
	 * It must be done after drawing. */
	if ((sctx->b.family == CHIP_HAWAII ||
	     sctx->b.family == CHIP_TONGA ||
	     sctx->b.family == CHIP_FIJI) &&
	    r600_get_strmout_en(&sctx->b)) {
		sctx->b.flags |= SI_CONTEXT_VGT_STREAMOUT_SYNC;
	}

	if (sctx->framebuffer.do_update_surf_dirtiness) {
		/* Set the depth buffer as dirty. */
		if (sctx->framebuffer.state.zsbuf) {
			struct pipe_surface *surf = sctx->framebuffer.state.zsbuf;
			struct r600_texture *rtex = (struct r600_texture *)surf->texture;

			if (!rtex->tc_compatible_htile)
				rtex->dirty_level_mask |= 1 << surf->u.tex.level;

			if (rtex->surface.flags & RADEON_SURF_SBUFFER)
				rtex->stencil_dirty_level_mask |= 1 << surf->u.tex.level;
		}
		if (sctx->framebuffer.compressed_cb_mask) {
			struct pipe_surface *surf;
			struct r600_texture *rtex;
			unsigned mask = sctx->framebuffer.compressed_cb_mask;

			do {
				unsigned i = u_bit_scan(&mask);
				surf = sctx->framebuffer.state.cbufs[i];
				rtex = (struct r600_texture*)surf->texture;

				if (rtex->fmask.size)
					rtex->dirty_level_mask |= 1 << surf->u.tex.level;
				if (rtex->dcc_gather_statistics)
					rtex->separate_dcc_dirty = true;
			} while (mask);
		}
		sctx->framebuffer.do_update_surf_dirtiness = false;
	}

	pipe_resource_reference(&ib_tmp.buffer, NULL);
	sctx->b.num_draw_calls++;
	if (G_0286E8_WAVESIZE(sctx->spi_tmpring_size))
		sctx->b.num_spill_draw_calls++;
}

void si_trace_emit(struct si_context *sctx)
{
	struct radeon_winsys_cs *cs = sctx->b.gfx.cs;

	sctx->trace_id++;
	radeon_add_to_buffer_list(&sctx->b, &sctx->b.gfx, sctx->trace_buf,
			      RADEON_USAGE_READWRITE, RADEON_PRIO_TRACE);
	radeon_emit(cs, PKT3(PKT3_WRITE_DATA, 3, 0));
	radeon_emit(cs, S_370_DST_SEL(V_370_MEMORY_SYNC) |
		    S_370_WR_CONFIRM(1) |
		    S_370_ENGINE_SEL(V_370_ME));
	radeon_emit(cs, sctx->trace_buf->gpu_address);
	radeon_emit(cs, sctx->trace_buf->gpu_address >> 32);
	radeon_emit(cs, sctx->trace_id);
	radeon_emit(cs, PKT3(PKT3_NOP, 0, 0));
	radeon_emit(cs, AC_ENCODE_TRACE_POINT(sctx->trace_id));
}
