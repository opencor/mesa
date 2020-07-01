/*
 * Copyright © 2010 Intel Corporation
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

#ifndef BRW_SHADER_H
#define BRW_SHADER_H

#include <stdint.h>
#include "brw_cfg.h"
#include "brw_compiler.h"
#include "compiler/nir/nir.h"

#ifdef __cplusplus
#include "brw_ir_analysis.h"
#include "brw_ir_allocator.h"

enum instruction_scheduler_mode {
   SCHEDULE_PRE,
   SCHEDULE_PRE_NON_LIFO,
   SCHEDULE_PRE_LIFO,
   SCHEDULE_POST,
};

struct backend_shader {
protected:

   backend_shader(const struct brw_compiler *compiler,
                  void *log_data,
                  void *mem_ctx,
                  const nir_shader *shader,
                  struct brw_stage_prog_data *stage_prog_data);

public:
   virtual ~backend_shader();

   const struct brw_compiler *compiler;
   void *log_data; /* Passed to compiler->*_log functions */

   const struct gen_device_info * const devinfo;
   const nir_shader *nir;
   struct brw_stage_prog_data * const stage_prog_data;

   /** ralloc context for temporary data used during compile */
   void *mem_ctx;

   /**
    * List of either fs_inst or vec4_instruction (inheriting from
    * backend_instruction)
    */
   exec_list instructions;

   cfg_t *cfg;
   BRW_ANALYSIS(idom_analysis, brw::idom_tree,
                const backend_shader *) idom_analysis;

   gl_shader_stage stage;
   bool debug_enabled;
   const char *stage_name;
   const char *stage_abbrev;

   brw::simple_allocator alloc;

   virtual void dump_instruction(const backend_instruction *inst) const = 0;
   virtual void dump_instruction(const backend_instruction *inst, FILE *file) const = 0;
   virtual void dump_instructions() const;
   virtual void dump_instructions(const char *name) const;

   void calculate_cfg();

   virtual void invalidate_analysis(brw::analysis_dependency_class c);
};

bool brw_texture_offset(const nir_tex_instr *tex, unsigned src,
                        uint32_t *offset_bits);

#else
struct backend_shader;
#endif /* __cplusplus */

enum brw_reg_type brw_type_for_base_type(const struct glsl_type *type);
enum brw_conditional_mod brw_conditional_for_comparison(unsigned int op);
uint32_t brw_math_function(enum opcode op);
const char *brw_instruction_name(const struct gen_device_info *devinfo,
                                 enum opcode op);
bool brw_saturate_immediate(enum brw_reg_type type, struct brw_reg *reg);
bool brw_negate_immediate(enum brw_reg_type type, struct brw_reg *reg);
bool brw_abs_immediate(enum brw_reg_type type, struct brw_reg *reg);

bool opt_predicated_break(struct backend_shader *s);

#ifdef __cplusplus
extern "C" {
#endif

/* brw_fs_reg_allocate.cpp */
void brw_fs_alloc_reg_sets(struct brw_compiler *compiler);

/* brw_vec4_reg_allocate.cpp */
void brw_vec4_alloc_reg_set(struct brw_compiler *compiler);

/* brw_disasm.c */
extern const char *const conditional_modifier[16];
extern const char *const pred_ctrl_align16[16];

/* Per-thread scratch space is a power-of-two multiple of 1KB. */
static inline int
brw_get_scratch_size(int size)
{
   return MAX2(1024, util_next_power_of_two(size));
}

/**
 * Scratch data used when compiling a GLSL geometry shader.
 */
struct brw_gs_compile
{
   struct brw_gs_prog_key key;
   struct brw_vue_map input_vue_map;

   unsigned control_data_bits_per_vertex;
   unsigned control_data_header_size_bits;
};

#ifdef __cplusplus
}
#endif

#endif /* BRW_SHADER_H */
