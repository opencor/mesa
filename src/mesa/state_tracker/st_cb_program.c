/**************************************************************************
 * 
 * Copyright 2003 VMware, Inc.
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

 /*
  * Authors:
  *   Keith Whitwell <keithw@vmware.com>
  */

#include "main/glheader.h"
#include "main/macros.h"
#include "main/enums.h"
#include "main/shaderapi.h"
#include "program/prog_instruction.h"
#include "program/program.h"

#include "cso_cache/cso_context.h"
#include "draw/draw_context.h"

#include "st_context.h"
#include "st_debug.h"
#include "st_program.h"
#include "st_mesa_to_tgsi.h"
#include "st_cb_program.h"
#include "st_glsl_to_ir.h"
#include "st_atifs_to_tgsi.h"
#include "st_util.h"


/**
 * Called via ctx->Driver.NewProgram() to allocate a new vertex or
 * fragment program.
 */
static struct gl_program *
st_new_program(struct gl_context *ctx, GLenum target, GLuint id,
               bool is_arb_asm)
{
   switch (target) {
   case GL_VERTEX_PROGRAM_ARB: {
      struct st_vertex_program *prog = rzalloc(NULL,
                                               struct st_vertex_program);
      return _mesa_init_gl_program(&prog->Base, target, id, is_arb_asm);
   }
   case GL_TESS_CONTROL_PROGRAM_NV:
   case GL_TESS_EVALUATION_PROGRAM_NV:
   case GL_GEOMETRY_PROGRAM_NV:
   case GL_FRAGMENT_PROGRAM_ARB:
   case GL_COMPUTE_PROGRAM_NV: {
      struct st_common_program *prog = rzalloc(NULL,
                                               struct st_common_program);
      return _mesa_init_gl_program(&prog->Base, target, id, is_arb_asm);
   }
   default:
      assert(0);
      return NULL;
   }
}


/**
 * Called via ctx->Driver.DeleteProgram()
 */
static void
st_delete_program(struct gl_context *ctx, struct gl_program *prog)
{
   struct st_context *st = st_context(ctx);

   switch( prog->Target ) {
   case GL_VERTEX_PROGRAM_ARB:
      {
         struct st_vertex_program *stvp = (struct st_vertex_program *) prog;
         st_release_vp_variants( st, stvp );
         
         if (stvp->glsl_to_tgsi)
            free_glsl_to_tgsi_visitor(stvp->glsl_to_tgsi);
      }
      break;
   case GL_TESS_CONTROL_PROGRAM_NV:
   case GL_TESS_EVALUATION_PROGRAM_NV:
   case GL_GEOMETRY_PROGRAM_NV:
   case GL_FRAGMENT_PROGRAM_ARB:
   case GL_COMPUTE_PROGRAM_NV:
      {
         struct st_common_program *p = st_common_program(prog);

         if (prog->Target == GL_FRAGMENT_PROGRAM_ARB)
            st_release_fp_variants(st, p);
         else
            st_release_common_variants(st, p);
         
         if (p->glsl_to_tgsi)
            free_glsl_to_tgsi_visitor(p->glsl_to_tgsi);
      }
      break;
   default:
      assert(0); /* problem */
   }

   /* delete base class */
   _mesa_delete_program( ctx, prog );
}


/**
 * Called via ctx->Driver.ProgramStringNotify()
 * Called when the program's text/code is changed.  We have to free
 * all shader variants and corresponding gallium shaders when this happens.
 */
static GLboolean
st_program_string_notify( struct gl_context *ctx,
                                           GLenum target,
                                           struct gl_program *prog )
{
   struct st_context *st = st_context(ctx);
   gl_shader_stage stage = _mesa_program_enum_to_shader_stage(target);

   if (target == GL_FRAGMENT_PROGRAM_ARB ||
       target == GL_FRAGMENT_SHADER_ATI) {
      struct st_common_program *stfp = (struct st_common_program *) prog;

      if (target == GL_FRAGMENT_SHADER_ATI) {
         assert(stfp->ati_fs);
         assert(stfp->ati_fs->Program == prog);

         st_init_atifs_prog(ctx, prog);
      }

      st_release_fp_variants(st, stfp);
      if (!stfp->shader_program && /* not GLSL->NIR */
          !st_translate_fragment_program(st, stfp))
         return false;

      if (st->fp == stfp)
	 st->dirty |= stfp->affected_states;
   } else if (target == GL_VERTEX_PROGRAM_ARB) {
      struct st_vertex_program *stvp = (struct st_vertex_program *) prog;

      st_release_vp_variants(st, stvp);
      if (!stvp->shader_program && /* not GLSL->NIR */
          !st_translate_vertex_program(st, stvp))
         return false;

      if (st->vp == stvp)
	 st->dirty |= ST_NEW_VERTEX_PROGRAM(st, stvp);
   } else {
      struct st_common_program *stcp = st_common_program(prog);

      st_release_common_variants(st, stcp);
      if (!stcp->shader_program && /* not GLSL->NIR */
          !st_translate_common_program(st, stcp))
         return false;

      if ((prog->info.stage == MESA_SHADER_TESS_CTRL && st->tcp == stcp) ||
          (prog->info.stage == MESA_SHADER_TESS_EVAL && st->tep == stcp) ||
          (prog->info.stage == MESA_SHADER_GEOMETRY && st->gp == stcp) ||
          (prog->info.stage == MESA_SHADER_COMPUTE && st->cp == stcp))
	 st->dirty |= stcp->affected_states;
   }

   if (ST_DEBUG & DEBUG_PRECOMPILE ||
       st->shader_has_one_variant[stage])
      st_precompile_shader_variant(st, prog);

   return GL_TRUE;
}

/**
 * Called via ctx->Driver.NewATIfs()
 * Called in glEndFragmentShaderATI()
 */
static struct gl_program *
st_new_ati_fs(struct gl_context *ctx, struct ati_fragment_shader *curProg)
{
   struct gl_program *prog = ctx->Driver.NewProgram(ctx, GL_FRAGMENT_PROGRAM_ARB,
         curProg->Id, true);
   struct st_common_program *stfp = (struct st_common_program *)prog;
   stfp->ati_fs = curProg;
   return prog;
}

static void
st_max_shader_compiler_threads(struct gl_context *ctx, unsigned count)
{
   struct pipe_screen *screen = st_context(ctx)->pipe->screen;

   if (screen->set_max_shader_compiler_threads)
      screen->set_max_shader_compiler_threads(screen, count);
}

static bool
st_get_shader_program_completion_status(struct gl_context *ctx,
                                        struct gl_shader_program *shprog)
{
   struct pipe_screen *screen = st_context(ctx)->pipe->screen;

   if (!screen->is_parallel_shader_compilation_finished)
      return true;

   for (unsigned i = 0; i < MESA_SHADER_STAGES; i++) {
      struct gl_linked_shader *linked = shprog->_LinkedShaders[i];
      void *sh = NULL;

      if (!linked || !linked->Program)
         continue;

      switch (i) {
      case MESA_SHADER_VERTEX:
         if (st_vertex_program(linked->Program)->variants)
            sh = st_vertex_program(linked->Program)->variants->driver_shader;
         break;
      case MESA_SHADER_FRAGMENT:
         if (st_common_program(linked->Program)->fp_variants)
            sh = st_common_program(linked->Program)->fp_variants->driver_shader;
         break;
      case MESA_SHADER_TESS_CTRL:
      case MESA_SHADER_TESS_EVAL:
      case MESA_SHADER_GEOMETRY:
      case MESA_SHADER_COMPUTE:
         if (st_common_program(linked->Program)->variants)
            sh = st_common_program(linked->Program)->variants->driver_shader;
         break;
      }

      unsigned type = pipe_shader_type_from_mesa(i);

      if (sh &&
          !screen->is_parallel_shader_compilation_finished(screen, sh, type))
         return false;
   }
   return true;
}

/**
 * Plug in the program and shader-related device driver functions.
 */
void
st_init_program_functions(struct dd_function_table *functions)
{
   functions->NewProgram = st_new_program;
   functions->DeleteProgram = st_delete_program;
   functions->ProgramStringNotify = st_program_string_notify;
   functions->NewATIfs = st_new_ati_fs;
   functions->LinkShader = st_link_shader;
   functions->SetMaxShaderCompilerThreads = st_max_shader_compiler_threads;
   functions->GetShaderProgramCompletionStatus =
      st_get_shader_program_completion_status;
}
