/*
 * Copyright (C) 2018-2019 Alyssa Rosenzweig <alyssa@rosenzweig.io>
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
 */

#include "compiler.h"
#include "midgard_ops.h"
#include "util/u_memory.h"
#include "util/register_allocate.h"

/* Scheduling for Midgard is complicated, to say the least. ALU instructions
 * must be grouped into VLIW bundles according to following model:
 *
 * [VMUL] [SADD]
 * [VADD] [SMUL] [VLUT]
 *
 * A given instruction can execute on some subset of the units (or a few can
 * execute on all). Instructions can be either vector or scalar; only scalar
 * instructions can execute on SADD/SMUL units. Units on a given line execute
 * in parallel. Subsequent lines execute separately and can pass results
 * directly via pipeline registers r24/r25, bypassing the register file.
 *
 * A bundle can optionally have 128-bits of embedded constants, shared across
 * all of the instructions within a bundle.
 *
 * Instructions consuming conditionals (branches and conditional selects)
 * require their condition to be written into the conditional register (r31)
 * within the same bundle they are consumed.
 *
 * Fragment writeout requires its argument to be written in full within the
 * same bundle as the branch, with no hanging dependencies.
 *
 * Load/store instructions are also in bundles of simply two instructions, and
 * texture instructions have no bundling.
 *
 * -------------------------------------------------------------------------
 *
 */

/* We create the dependency graph with per-byte granularity */

#define BYTE_COUNT 16

static void
add_dependency(struct util_dynarray *table, unsigned index, uint16_t mask, midgard_instruction **instructions, unsigned child)
{
        for (unsigned i = 0; i < BYTE_COUNT; ++i) {
                if (!(mask & (1 << i)))
                        continue;

                struct util_dynarray *parents = &table[(BYTE_COUNT * index) + i];

                util_dynarray_foreach(parents, unsigned, parent) {
                        BITSET_WORD *dependents = instructions[*parent]->dependents;

                        /* Already have the dependency */
                        if (BITSET_TEST(dependents, child))
                                continue;

                        BITSET_SET(dependents, child);
                        instructions[child]->nr_dependencies++;
                }
        }
}

static void
mark_access(struct util_dynarray *table, unsigned index, uint16_t mask, unsigned parent)
{
        for (unsigned i = 0; i < BYTE_COUNT; ++i) {
                if (!(mask & (1 << i)))
                        continue;

                util_dynarray_append(&table[(BYTE_COUNT * index) + i], unsigned, parent);
        }
}

static void
mir_create_dependency_graph(midgard_instruction **instructions, unsigned count, unsigned node_count)
{
        size_t sz = node_count * BYTE_COUNT;

        struct util_dynarray *last_read = calloc(sizeof(struct util_dynarray), sz);
        struct util_dynarray *last_write = calloc(sizeof(struct util_dynarray), sz);

        for (unsigned i = 0; i < sz; ++i) {
                util_dynarray_init(&last_read[i], NULL);
                util_dynarray_init(&last_write[i], NULL);
        }

        /* Initialize dependency graph */
        for (unsigned i = 0; i < count; ++i) {
                instructions[i]->dependents =
                        calloc(BITSET_WORDS(count), sizeof(BITSET_WORD));

                instructions[i]->nr_dependencies = 0;
        }

        /* Populate dependency graph */
        for (signed i = count - 1; i >= 0; --i) {
                if (instructions[i]->compact_branch)
                        continue;

                unsigned dest = instructions[i]->dest;
                unsigned mask = mir_bytemask(instructions[i]);

                mir_foreach_src((*instructions), s) {
                        unsigned src = instructions[i]->src[s];

                        if (src < node_count) {
                                unsigned readmask = mir_bytemask_of_read_components(instructions[i], src);
                                add_dependency(last_write, src, readmask, instructions, i);
                        }
                }

                if (dest < node_count) {
                        add_dependency(last_read, dest, mask, instructions, i);
                        add_dependency(last_write, dest, mask, instructions, i);
                        mark_access(last_write, dest, mask, i);
                }

                mir_foreach_src((*instructions), s) {
                        unsigned src = instructions[i]->src[s];

                        if (src < node_count) {
                                unsigned readmask = mir_bytemask_of_read_components(instructions[i], src);
                                mark_access(last_read, src, readmask, i);
                        }
                }
        }

        /* If there is a branch, all instructions depend on it, as interblock
         * execution must be purely in-order */

        if (instructions[count - 1]->compact_branch) {
                BITSET_WORD *dependents = instructions[count - 1]->dependents;

                for (signed i = count - 2; i >= 0; --i) {
                        if (BITSET_TEST(dependents, i))
                                continue;

                        BITSET_SET(dependents, i);
                        instructions[i]->nr_dependencies++;
                }
        }

        /* Free the intermediate structures */
        for (unsigned i = 0; i < sz; ++i) {
                util_dynarray_fini(&last_read[i]);
                util_dynarray_fini(&last_write[i]);
        }
}

/* Does the mask cover more than a scalar? */

static bool
is_single_component_mask(unsigned mask)
{
        int components = 0;

        for (int c = 0; c < 8; ++c) {
                if (mask & (1 << c))
                        components++;
        }

        return components == 1;
}

/* Helpers for scheudling */

static bool
mir_is_scalar(midgard_instruction *ains)
{
        /* Do we try to use it as a vector op? */
        if (!is_single_component_mask(ains->mask))
                return false;

        /* Otherwise, check mode hazards */
        bool could_scalar = true;

        /* Only 16/32-bit can run on a scalar unit */
        could_scalar &= ains->alu.reg_mode != midgard_reg_mode_8;
        could_scalar &= ains->alu.reg_mode != midgard_reg_mode_64;
        could_scalar &= ains->alu.dest_override == midgard_dest_override_none;

        if (ains->alu.reg_mode == midgard_reg_mode_16) {
                /* If we're running in 16-bit mode, we
                 * can't have any 8-bit sources on the
                 * scalar unit (since the scalar unit
                 * doesn't understand 8-bit) */

                midgard_vector_alu_src s1 =
                        vector_alu_from_unsigned(ains->alu.src1);

                could_scalar &= !s1.half;

                midgard_vector_alu_src s2 =
                        vector_alu_from_unsigned(ains->alu.src2);

                could_scalar &= !s2.half;
        }

        return could_scalar;
}

/* How many bytes does this ALU instruction add to the bundle? */

static unsigned
bytes_for_instruction(midgard_instruction *ains)
{
        if (ains->unit & UNITS_ANY_VECTOR)
                return sizeof(midgard_reg_info) + sizeof(midgard_vector_alu);
        else if (ains->unit == ALU_ENAB_BRANCH)
                return sizeof(midgard_branch_extended);
        else if (ains->compact_branch)
                return sizeof(ains->br_compact);
        else
                return sizeof(midgard_reg_info) + sizeof(midgard_scalar_alu);
}

/* We would like to flatten the linked list of midgard_instructions in a bundle
 * to an array of pointers on the heap for easy indexing */

static midgard_instruction **
flatten_mir(midgard_block *block, unsigned *len)
{
        *len = list_length(&block->instructions);

        if (!(*len))
                return NULL;

        midgard_instruction **instructions =
                calloc(sizeof(midgard_instruction *), *len);

        unsigned i = 0;

        mir_foreach_instr_in_block(block, ins)
                instructions[i++] = ins;

        return instructions;
}

/* The worklist is the set of instructions that can be scheduled now; that is,
 * the set of instructions with no remaining dependencies */

static void
mir_initialize_worklist(BITSET_WORD *worklist, midgard_instruction **instructions, unsigned count)
{
        for (unsigned i = 0; i < count; ++i) {
                if (instructions[i]->nr_dependencies == 0)
                        BITSET_SET(worklist, i);
        }
}

/* Update the worklist after an instruction terminates. Remove its edges from
 * the graph and if that causes any node to have no dependencies, add it to the
 * worklist */

static void
mir_update_worklist(
                BITSET_WORD *worklist, unsigned count,
                midgard_instruction **instructions, midgard_instruction *done)
{
        /* Sanity check: if no instruction terminated, there is nothing to do.
         * If the instruction that terminated had dependencies, that makes no
         * sense and means we messed up the worklist. Finally, as the purpose
         * of this routine is to update dependents, we abort early if there are
         * no dependents defined. */

        if (!done)
                return;

        assert(done->nr_dependencies == 0);

        if (!done->dependents)
                return;

        /* We have an instruction with dependents. Iterate each dependent to
         * remove one dependency (`done`), adding dependents to the worklist
         * where possible. */

        unsigned i;
        BITSET_WORD tmp;
        BITSET_FOREACH_SET(i, tmp, done->dependents, count) {
                assert(instructions[i]->nr_dependencies);

                if (!(--instructions[i]->nr_dependencies))
                        BITSET_SET(worklist, i);
        }

        free(done->dependents);
}

/* While scheduling, we need to choose instructions satisfying certain
 * criteria. As we schedule backwards, we choose the *last* instruction in the
 * worklist to simulate in-order scheduling. Chosen instructions must satisfy a
 * given predicate. */

struct midgard_predicate {
        /* TAG or ~0 for dont-care */
        unsigned tag;

        /* True if we want to pop off the chosen instruction */
        bool destructive;

        /* For ALU, choose only this unit */
        unsigned unit;

        /* State for bundle constants. constants is the actual constants
         * for the bundle. constant_count is the number of bytes (up to
         * 16) currently in use for constants. When picking in destructive
         * mode, the constants array will be updated, and the instruction
         * will be adjusted to index into the constants array */

        uint8_t *constants;
        unsigned constant_count;
        bool blend_constant;

        /* Exclude this destination (if not ~0) */
        unsigned exclude;

        /* Don't schedule instructions consuming conditionals (since we already
         * scheduled one). Excludes conditional branches and csel */
        bool no_cond;

        /* Require a minimal mask and (if nonzero) given destination. Used for
         * writeout optimizations */

        unsigned mask;
        unsigned dest;
};

/* For an instruction that can fit, adjust it to fit and update the constants
 * array, in destructive mode. Returns whether the fitting was successful. */

static bool
mir_adjust_constants(midgard_instruction *ins,
                struct midgard_predicate *pred,
                bool destructive)
{
        /* Blend constants dominate */
        if (ins->has_blend_constant) {
                if (pred->constant_count)
                        return false;
                else if (destructive) {
                        pred->blend_constant = true;
                        pred->constant_count = 16;
                        return true;
                }
        }

        /* No constant, nothing to adjust */
        if (!ins->has_constants)
                return true;

        if (ins->alu.reg_mode == midgard_reg_mode_16) {
                /* TODO: 16-bit constant combining */
                if (pred->constant_count)
                        return false;

                uint16_t *bundles = (uint16_t *) pred->constants;
                uint32_t *constants = (uint32_t *) ins->constants;

                /* Copy them wholesale */
                for (unsigned i = 0; i < 4; ++i)
                        bundles[i] = constants[i];

                pred->constant_count = 16;
        } else {
                /* Pack 32-bit constants */
                uint32_t *bundles = (uint32_t *) pred->constants;
                uint32_t *constants = (uint32_t *) ins->constants;
                unsigned r_constant = SSA_FIXED_REGISTER(REGISTER_CONSTANT);
                unsigned mask = mir_from_bytemask(mir_bytemask_of_read_components(ins, r_constant), midgard_reg_mode_32);

                /* First, check if it fits */
                unsigned count = DIV_ROUND_UP(pred->constant_count, sizeof(uint32_t));
                unsigned existing_count = count;

                for (unsigned i = 0; i < 4; ++i) {
                        if (!(mask & (1 << i)))
                                continue;

                        bool ok = false;

                        /* Look for existing constant */
                        for (unsigned j = 0; j < existing_count; ++j) {
                                if (bundles[j] == constants[i]) {
                                        ok = true;
                                        break;
                                }
                        }

                        if (ok)
                                continue;

                        /* If the constant is new, check ourselves */
                        for (unsigned j = 0; j < i; ++j) {
                                if (constants[j] == constants[i]) {
                                        ok = true;
                                        break;
                                }
                        }

                        if (ok)
                                continue;

                        /* Otherwise, this is a new constant */
                        count++;
                }

                /* Check if we have space */
                if (count > 4)
                        return false;

                /* If non-destructive, we're done */
                if (!destructive)
                        return true;

                /* If destructive, let's copy in the new constants and adjust
                 * swizzles to pack it in. */

                uint32_t indices[4] = { 0 };

                /* Reset count */
                count = existing_count;

                for (unsigned i = 0; i < 4; ++i) {
                        if (!(mask & (1 << i)))
                                continue;

                        uint32_t cons = constants[i];
                        bool constant_found = false;

                        /* Search for the constant */
                        for (unsigned j = 0; j < count; ++j) {
                                if (bundles[j] != cons)
                                        continue;

                                /* We found it, reuse */
                                indices[i] = j;
                                constant_found = true;
                                break;
                        }

                        if (constant_found)
                                continue;

                        /* We didn't find it, so allocate it */
                        unsigned idx = count++;

                        /* We have space, copy it in! */
                        bundles[idx] = cons;
                        indices[i] = idx;
                }

                pred->constant_count = count * sizeof(uint32_t);

                /* Cool, we have it in. So use indices as a
                 * swizzle */

                unsigned swizzle = SWIZZLE_FROM_ARRAY(indices);

                if (ins->src[0] == r_constant)
                        ins->alu.src1 = vector_alu_apply_swizzle(ins->alu.src1, swizzle);

                if (ins->src[1] == r_constant)
                        ins->alu.src2 = vector_alu_apply_swizzle(ins->alu.src2, swizzle);

        }

        return true;
}

static midgard_instruction *
mir_choose_instruction(
                midgard_instruction **instructions,
                BITSET_WORD *worklist, unsigned count,
                struct midgard_predicate *predicate)
{
        /* Parse the predicate */
        unsigned tag = predicate->tag;
        bool alu = tag == TAG_ALU_4;
        unsigned unit = predicate->unit;
        bool branch = alu && (unit == ALU_ENAB_BR_COMPACT);
        bool scalar = (unit != ~0) && (unit & UNITS_SCALAR);
        bool no_cond = predicate->no_cond;

        unsigned mask = predicate->mask;
        unsigned dest = predicate->dest;
        bool needs_dest = mask & 0xF;

        /* Iterate to find the best instruction satisfying the predicate */
        unsigned i;
        BITSET_WORD tmp;

        signed best_index = -1;
        bool best_conditional = false;

        /* Enforce a simple metric limiting distance to keep down register
         * pressure. TOOD: replace with liveness tracking for much better
         * results */

        unsigned max_active = 0;
        unsigned max_distance = 6;

        BITSET_FOREACH_SET(i, tmp, worklist, count) {
                max_active = MAX2(max_active, i);
        }

        BITSET_FOREACH_SET(i, tmp, worklist, count) {
                if ((max_active - i) >= max_distance)
                        continue;

                if (tag != ~0 && instructions[i]->type != tag)
                        continue;

                if (predicate->exclude != ~0 && instructions[i]->dest == predicate->exclude)
                        continue;

                if (alu && !branch && !(alu_opcode_props[instructions[i]->alu.op].props & unit))
                        continue;

                if (branch && !instructions[i]->compact_branch)
                        continue;

                if (alu && scalar && !mir_is_scalar(instructions[i]))
                        continue;

                if (alu && !mir_adjust_constants(instructions[i], predicate, false))
                        continue;

                if (needs_dest && instructions[i]->dest != dest)
                        continue;

                if (mask && ((~instructions[i]->mask) & mask))
                        continue;

                bool conditional = alu && !branch && OP_IS_CSEL(instructions[i]->alu.op);
                conditional |= (branch && !instructions[i]->prepacked_branch && instructions[i]->branch.conditional);

                if (conditional && no_cond)
                        continue;

                /* Simulate in-order scheduling */
                if ((signed) i < best_index)
                        continue;

                best_index = i;
                best_conditional = conditional;
        }


        /* Did we find anything?  */

        if (best_index < 0)
                return NULL;

        /* If we found something, remove it from the worklist */
        assert(best_index < count);

        if (predicate->destructive) {
                BITSET_CLEAR(worklist, best_index);

                if (alu)
                        mir_adjust_constants(instructions[best_index], predicate, true);

                /* Once we schedule a conditional, we can't again */
                predicate->no_cond |= best_conditional;
        }

        return instructions[best_index];
}

/* Still, we don't choose instructions in a vacuum. We need a way to choose the
 * best bundle type (ALU, load/store, texture). Nondestructive. */

static unsigned
mir_choose_bundle(
                midgard_instruction **instructions,
                BITSET_WORD *worklist, unsigned count)
{
        /* At the moment, our algorithm is very simple - use the bundle of the
         * best instruction, regardless of what else could be scheduled
         * alongside it. This is not optimal but it works okay for in-order */

        struct midgard_predicate predicate = {
                .tag = ~0,
                .destructive = false,
                .exclude = ~0
        };

        midgard_instruction *chosen = mir_choose_instruction(instructions, worklist, count, &predicate);

        if (chosen)
                return chosen->type;
        else
                return ~0;
}

/* We want to choose an ALU instruction filling a given unit */
static void
mir_choose_alu(midgard_instruction **slot,
                midgard_instruction **instructions,
                BITSET_WORD *worklist, unsigned len,
                struct midgard_predicate *predicate,
                unsigned unit)
{
        /* Did we already schedule to this slot? */
        if ((*slot) != NULL)
                return;

        /* Try to schedule something, if not */
        predicate->unit = unit;
        *slot = mir_choose_instruction(instructions, worklist, len, predicate);

        /* Store unit upon scheduling */
        if (*slot && !((*slot)->compact_branch))
                (*slot)->unit = unit;
}

/* When we are scheduling a branch/csel, we need the consumed condition in the
 * same block as a pipeline register. There are two options to enable this:
 *
 *  - Move the conditional into the bundle. Preferred, but only works if the
 *    conditional is used only once and is from this block.
 *  - Copy the conditional.
 *
 * We search for the conditional. If it's in this block, single-use, and
 * without embedded constants, we schedule it immediately. Otherwise, we
 * schedule a move for it.
 *
 * mir_comparison_mobile is a helper to find the moveable condition.
 */

static unsigned
mir_comparison_mobile(
                compiler_context *ctx,
                midgard_instruction **instructions,
                struct midgard_predicate *predicate,
                unsigned count,
                unsigned cond)
{
        if (!mir_single_use(ctx, cond))
                return ~0;

        unsigned ret = ~0;

        for (unsigned i = 0; i < count; ++i) {
                if (instructions[i]->dest != cond)
                        continue;

                /* Must fit in an ALU bundle */
                if (instructions[i]->type != TAG_ALU_4)
                        return ~0;

                /* We'll need to rewrite to .w but that doesn't work for vector
                 * ops that don't replicate (ball/bany), so bail there */

                if (GET_CHANNEL_COUNT(alu_opcode_props[instructions[i]->alu.op].props))
                        return ~0;

                /* Ensure it will fit with constants */

                if (!mir_adjust_constants(instructions[i], predicate, false))
                        return ~0;

                /* Ensure it is written only once */

                if (ret != ~0)
                        return ~0;
                else
                        ret = i;
        }

        /* Inject constants now that we are sure we want to */
        if (ret != ~0)
                mir_adjust_constants(instructions[ret], predicate, true);

        return ret;
}

/* Using the information about the moveable conditional itself, we either pop
 * that condition off the worklist for use now, or create a move to
 * artificially schedule instead as a fallback */

static midgard_instruction *
mir_schedule_comparison(
                compiler_context *ctx,
                midgard_instruction **instructions,
                struct midgard_predicate *predicate,
                BITSET_WORD *worklist, unsigned count,
                unsigned cond, bool vector, unsigned swizzle,
                midgard_instruction *user)
{
        /* TODO: swizzle when scheduling */
        unsigned comp_i =
                (!vector && (swizzle == 0)) ?
                mir_comparison_mobile(ctx, instructions, predicate, count, cond) : ~0;

        /* If we can, schedule the condition immediately */
        if ((comp_i != ~0) && BITSET_TEST(worklist, comp_i)) {
                assert(comp_i < count);
                BITSET_CLEAR(worklist, comp_i);
                return instructions[comp_i];
        }

        /* Otherwise, we insert a move */
        midgard_vector_alu_src csel = {
                .swizzle = swizzle
        };

        midgard_instruction mov = v_mov(cond, csel, cond);
        mov.mask = vector ? 0xF : 0x1;

        return mir_insert_instruction_before(ctx, user, mov);
}

/* Most generally, we need instructions writing to r31 in the appropriate
 * components */

static midgard_instruction *
mir_schedule_condition(compiler_context *ctx,
                struct midgard_predicate *predicate,
                BITSET_WORD *worklist, unsigned count,
                midgard_instruction **instructions,
                midgard_instruction *last)
{
        /* For a branch, the condition is the only argument; for csel, third */
        bool branch = last->compact_branch;
        unsigned condition_index = branch ? 0 : 2;

        /* csel_v is vector; otherwise, conditions are scalar */
        bool vector = !branch && OP_IS_CSEL_V(last->alu.op);

        /* Grab the conditional instruction */

        midgard_instruction *cond = mir_schedule_comparison(
                        ctx, instructions, predicate, worklist, count, last->src[condition_index],
                        vector, last->cond_swizzle, last);

        /* We have exclusive reign over this (possibly move) conditional
         * instruction. We can rewrite into a pipeline conditional register */

        predicate->exclude = cond->dest;
        cond->dest = SSA_FIXED_REGISTER(31);

        if (!vector) {
                cond->mask = (1 << COMPONENT_W);

                mir_foreach_src(cond, s) {
                        if (cond->src[s] == ~0)
                                continue;

                        mir_set_swizzle(cond, s, (mir_get_swizzle(cond, s) << (2*3)) & 0xFF);
                }
        }

        /* Schedule the unit: csel is always in the latter pipeline, so a csel
         * condition must be in the former pipeline stage (vmul/sadd),
         * depending on scalar/vector of the instruction itself. A branch must
         * be written from the latter pipeline stage and a branch condition is
         * always scalar, so it is always in smul (exception: ball/bany, which
         * will be vadd) */

        if (branch)
                cond->unit = UNIT_SMUL;
        else
                cond->unit = vector ? UNIT_VMUL : UNIT_SADD;

        return cond;
}

/* Schedules a single bundle of the given type */

static midgard_bundle
mir_schedule_texture(
                midgard_instruction **instructions,
                BITSET_WORD *worklist, unsigned len)
{
        struct midgard_predicate predicate = {
                .tag = TAG_TEXTURE_4,
                .destructive = true,
                .exclude = ~0
        };

        midgard_instruction *ins =
                mir_choose_instruction(instructions, worklist, len, &predicate);

        mir_update_worklist(worklist, len, instructions, ins);

        struct midgard_bundle out = {
                .tag = TAG_TEXTURE_4,
                .instruction_count = 1,
                .instructions = { ins }
        };

        return out;
}

static midgard_bundle
mir_schedule_ldst(
                midgard_instruction **instructions,
                BITSET_WORD *worklist, unsigned len)
{
        struct midgard_predicate predicate = {
                .tag = TAG_LOAD_STORE_4,
                .destructive = true,
                .exclude = ~0
        };

        /* Try to pick two load/store ops. Second not gauranteed to exist */

        midgard_instruction *ins =
                mir_choose_instruction(instructions, worklist, len, &predicate);

        midgard_instruction *pair =
                mir_choose_instruction(instructions, worklist, len, &predicate);

        struct midgard_bundle out = {
                .tag = TAG_LOAD_STORE_4,
                .instruction_count = pair ? 2 : 1,
                .instructions = { ins, pair }
        };

        /* We have to update the worklist atomically, since the two
         * instructions run concurrently (TODO: verify it's not pipelined) */

        mir_update_worklist(worklist, len, instructions, ins);
        mir_update_worklist(worklist, len, instructions, pair);

        return out;
}

static midgard_bundle
mir_schedule_alu(
                compiler_context *ctx,
                midgard_instruction **instructions,
                BITSET_WORD *worklist, unsigned len)
{
        struct midgard_bundle bundle = {};

        unsigned bytes_emitted = sizeof(bundle.control);

        struct midgard_predicate predicate = {
                .tag = TAG_ALU_4,
                .destructive = true,
                .exclude = ~0,
                .constants = (uint8_t *) bundle.constants
        };

        midgard_instruction *vmul = NULL;
        midgard_instruction *vadd = NULL;
        midgard_instruction *vlut = NULL;
        midgard_instruction *smul = NULL;
        midgard_instruction *sadd = NULL;
        midgard_instruction *branch = NULL;

        mir_choose_alu(&branch, instructions, worklist, len, &predicate, ALU_ENAB_BR_COMPACT);
        mir_update_worklist(worklist, len, instructions, branch);
        bool writeout = branch && branch->writeout;

        if (branch && !branch->prepacked_branch && branch->branch.conditional) {
                midgard_instruction *cond = mir_schedule_condition(ctx, &predicate, worklist, len, instructions, branch);

                if (cond->unit == UNIT_VADD)
                        vadd = cond;
                else if (cond->unit == UNIT_SMUL)
                        smul = cond;
                else
                        unreachable("Bad condition");
        }

        mir_choose_alu(&smul, instructions, worklist, len, &predicate, UNIT_SMUL);

        if (!writeout)
                mir_choose_alu(&vlut, instructions, worklist, len, &predicate, UNIT_VLUT);

        mir_choose_alu(&vadd, instructions, worklist, len, &predicate, UNIT_VADD);
        
        mir_update_worklist(worklist, len, instructions, vlut);
        mir_update_worklist(worklist, len, instructions, vadd);
        mir_update_worklist(worklist, len, instructions, smul);

        bool vadd_csel = vadd && OP_IS_CSEL(vadd->alu.op);
        bool smul_csel = smul && OP_IS_CSEL(smul->alu.op);

        if (vadd_csel || smul_csel) {
                midgard_instruction *ins = vadd_csel ? vadd : smul;
                midgard_instruction *cond = mir_schedule_condition(ctx, &predicate, worklist, len, instructions, ins);

                if (cond->unit == UNIT_VMUL)
                        vmul = cond;
                else if (cond->unit == UNIT_SADD)
                        sadd = cond;
                else
                        unreachable("Bad condition");
        }

        /* Stage 2, let's schedule sadd before vmul for writeout */
        mir_choose_alu(&sadd, instructions, worklist, len, &predicate, UNIT_SADD);

        /* Check if writeout reads its own register */
        bool bad_writeout = false;

        if (branch && branch->writeout) {
                midgard_instruction *stages[] = { sadd, vadd, smul };
                unsigned src = (branch->src[0] == ~0) ? SSA_FIXED_REGISTER(0) : branch->src[0];
                unsigned writeout_mask = 0x0;

                for (unsigned i = 0; i < ARRAY_SIZE(stages); ++i) {
                        if (!stages[i])
                                continue;

                        if (stages[i]->dest != src)
                                continue;

                        writeout_mask |= stages[i]->mask;
                        bad_writeout |= mir_has_arg(stages[i], branch->src[0]);
                }

                /* It's possible we'll be able to schedule something into vmul
                 * to fill r0. Let's peak into the future, trying to schedule
                 * vmul specially that way. */

                if (!bad_writeout && writeout_mask != 0xF) {
                        predicate.unit = UNIT_VMUL;
                        predicate.dest = src;
                        predicate.mask = writeout_mask ^ 0xF;

                        struct midgard_instruction *peaked =
                                mir_choose_instruction(instructions, worklist, len, &predicate);

                        if (peaked) {
                                vmul = peaked;
                                vmul->unit = UNIT_VMUL;
                                writeout_mask |= predicate.mask;
                                assert(writeout_mask == 0xF);
                        }

                        /* Cleanup */
                        predicate.dest = predicate.mask = 0;
                }

                /* Finally, add a move if necessary */
                if (bad_writeout || writeout_mask != 0xF) {
                        unsigned temp = (branch->src[0] == ~0) ? SSA_FIXED_REGISTER(0) : make_compiler_temp(ctx);
                        midgard_instruction mov = v_mov(src, blank_alu_src, temp);
                        vmul = mem_dup(&mov, sizeof(midgard_instruction));
                        vmul->unit = UNIT_VMUL;
                        vmul->mask = 0xF ^ writeout_mask;
                        /* TODO: Don't leak */

                        /* Rewrite to use our temp */

                        for (unsigned i = 0; i < ARRAY_SIZE(stages); ++i) {
                                if (stages[i])
                                        mir_rewrite_index_dst_single(stages[i], src, temp);
                        }

                        mir_rewrite_index_src_single(branch, src, temp);
                }
        }

        mir_choose_alu(&vmul, instructions, worklist, len, &predicate, UNIT_VMUL);

        mir_update_worklist(worklist, len, instructions, vmul);
        mir_update_worklist(worklist, len, instructions, sadd);

        bundle.has_blend_constant = predicate.blend_constant;
        bundle.has_embedded_constants = predicate.constant_count > 0;

        unsigned padding = 0;

        /* Now that we have finished scheduling, build up the bundle */
        midgard_instruction *stages[] = { vmul, sadd, vadd, smul, vlut, branch };

        for (unsigned i = 0; i < ARRAY_SIZE(stages); ++i) {
                if (stages[i]) {
                        bundle.control |= stages[i]->unit;
                        bytes_emitted += bytes_for_instruction(stages[i]);
                        bundle.instructions[bundle.instruction_count++] = stages[i];
                }
        }

        /* Pad ALU op to nearest word */

        if (bytes_emitted & 15) {
                padding = 16 - (bytes_emitted & 15);
                bytes_emitted += padding;
        }

        /* Constants must always be quadwords */
        if (bundle.has_embedded_constants)
                bytes_emitted += 16;

        /* Size ALU instruction for tag */
        bundle.tag = (TAG_ALU_4) + (bytes_emitted / 16) - 1;
        bundle.padding = padding;
        bundle.control |= bundle.tag;

        return bundle;
}

/* Schedule a single block by iterating its instruction to create bundles.
 * While we go, tally about the bundle sizes to compute the block size. */


static void
schedule_block(compiler_context *ctx, midgard_block *block)
{
        /* Copy list to dynamic array */
        unsigned len = 0;
        midgard_instruction **instructions = flatten_mir(block, &len);

        if (!len)
                return;

        /* Calculate dependencies and initial worklist */
        unsigned node_count = ctx->temp_count + 1;
        mir_create_dependency_graph(instructions, len, node_count);

        /* Allocate the worklist */
        size_t sz = BITSET_WORDS(len) * sizeof(BITSET_WORD);
        BITSET_WORD *worklist = calloc(sz, 1);
        mir_initialize_worklist(worklist, instructions, len);

        struct util_dynarray bundles;
        util_dynarray_init(&bundles, NULL);

        block->quadword_count = 0;
        unsigned blend_offset = 0;

        for (;;) {
                unsigned tag = mir_choose_bundle(instructions, worklist, len);
                midgard_bundle bundle;

                if (tag == TAG_TEXTURE_4)
                        bundle = mir_schedule_texture(instructions, worklist, len);
                else if (tag == TAG_LOAD_STORE_4)
                        bundle = mir_schedule_ldst(instructions, worklist, len);
                else if (tag == TAG_ALU_4)
                        bundle = mir_schedule_alu(ctx, instructions, worklist, len);
                else
                        break;

                util_dynarray_append(&bundles, midgard_bundle, bundle);

                if (bundle.has_blend_constant)
                        blend_offset = block->quadword_count;

                block->quadword_count += quadword_size(bundle.tag);
        }

        /* We emitted bundles backwards; copy into the block in reverse-order */

        util_dynarray_init(&block->bundles, NULL);
        util_dynarray_foreach_reverse(&bundles, midgard_bundle, bundle) {
                util_dynarray_append(&block->bundles, midgard_bundle, *bundle);
        }

        /* Blend constant was backwards as well. blend_offset if set is
         * strictly positive, as an offset of zero would imply constants before
         * any instructions which is invalid in Midgard */

        if (blend_offset)
                ctx->blend_constant_offset = ((ctx->quadword_count + block->quadword_count) - blend_offset - 1) * 0x10;

        block->is_scheduled = true;
        ctx->quadword_count += block->quadword_count;

        /* Reorder instructions to match bundled. First remove existing
         * instructions and then recreate the list */

        mir_foreach_instr_in_block_safe(block, ins) {
                list_del(&ins->link);
        }

        mir_foreach_instr_in_block_scheduled_rev(block, ins) {
                list_add(&ins->link, &block->instructions);
        }
}

/* When we're 'squeezing down' the values in the IR, we maintain a hash
 * as such */

static unsigned
find_or_allocate_temp(compiler_context *ctx, unsigned hash)
{
        if (hash >= SSA_FIXED_MINIMUM)
                return hash;

        unsigned temp = (uintptr_t) _mesa_hash_table_u64_search(
                                ctx->hash_to_temp, hash + 1);

        if (temp)
                return temp - 1;

        /* If no temp is find, allocate one */
        temp = ctx->temp_count++;
        ctx->max_hash = MAX2(ctx->max_hash, hash);

        _mesa_hash_table_u64_insert(ctx->hash_to_temp,
                                    hash + 1, (void *) ((uintptr_t) temp + 1));

        return temp;
}

/* Reassigns numbering to get rid of gaps in the indices */

static void
mir_squeeze_index(compiler_context *ctx)
{
        /* Reset */
        ctx->temp_count = 0;
        /* TODO don't leak old hash_to_temp */
        ctx->hash_to_temp = _mesa_hash_table_u64_create(NULL);

        mir_foreach_instr_global(ctx, ins) {
                ins->dest = find_or_allocate_temp(ctx, ins->dest);

                for (unsigned i = 0; i < ARRAY_SIZE(ins->src); ++i)
                        ins->src[i] = find_or_allocate_temp(ctx, ins->src[i]);
        }
}

static midgard_instruction
v_load_store_scratch(
                unsigned srcdest,
                unsigned index,
                bool is_store,
                unsigned mask)
{
        /* We index by 32-bit vec4s */
        unsigned byte = (index * 4 * 4);

        midgard_instruction ins = {
                .type = TAG_LOAD_STORE_4,
                .mask = mask,
                .dest = ~0,
                .src = { ~0, ~0, ~0 },
                .load_store = {
                        .op = is_store ? midgard_op_st_int4 : midgard_op_ld_int4,
                        .swizzle = SWIZZLE_XYZW,

                        /* For register spilling - to thread local storage */
                        .arg_1 = 0xEA,
                        .arg_2 = 0x1E,

                        /* Splattered across, TODO combine logically */
                        .varying_parameters = (byte & 0x1FF) << 1,
                        .address = (byte >> 9)
                },

                /* If we spill an unspill, RA goes into an infinite loop */
                .no_spill = true
        };

       if (is_store) {
                /* r0 = r26, r1 = r27 */
                assert(srcdest == SSA_FIXED_REGISTER(26) || srcdest == SSA_FIXED_REGISTER(27));
                ins.src[0] = srcdest;
        } else {
                ins.dest = srcdest;
        }

        return ins;
}

/* If register allocation fails, find the best spill node and spill it to fix
 * whatever the issue was. This spill node could be a work register (spilling
 * to thread local storage), but it could also simply be a special register
 * that needs to spill to become a work register. */

static void mir_spill_register(
                compiler_context *ctx,
                struct ra_graph *g,
                unsigned *spill_count)
{
        unsigned spill_index = ctx->temp_count;

        /* Our first step is to calculate spill cost to figure out the best
         * spill node. All nodes are equal in spill cost, but we can't spill
         * nodes written to from an unspill */

        for (unsigned i = 0; i < ctx->temp_count; ++i) {
                ra_set_node_spill_cost(g, i, 1.0);
        }

        /* We can't spill any bundles that contain unspills. This could be
         * optimized to allow use of r27 to spill twice per bundle, but if
         * you're at the point of optimizing spilling, it's too late.
         *
         * We also can't double-spill. */

        mir_foreach_block(ctx, block) {
                mir_foreach_bundle_in_block(block, bun) {
                        bool no_spill = false;

                        for (unsigned i = 0; i < bun->instruction_count; ++i) {
                                no_spill |= bun->instructions[i]->no_spill;

                                if (bun->instructions[i]->no_spill) {
                                        mir_foreach_src(bun->instructions[i], s) {
                                                unsigned src = bun->instructions[i]->src[s];

                                                if (src < ctx->temp_count)
                                                        ra_set_node_spill_cost(g, src, -1.0);
                                        }
                                }
                        }

                        if (!no_spill)
                                continue;

                        for (unsigned i = 0; i < bun->instruction_count; ++i) {
                                unsigned dest = bun->instructions[i]->dest;
                                if (dest < ctx->temp_count)
                                        ra_set_node_spill_cost(g, dest, -1.0);
                        }
                }
        }

        int spill_node = ra_get_best_spill_node(g);

        if (spill_node < 0) {
                mir_print_shader(ctx);
                assert(0);
        }

        /* We have a spill node, so check the class. Work registers
         * legitimately spill to TLS, but special registers just spill to work
         * registers */

        unsigned class = ra_get_node_class(g, spill_node);
        bool is_special = (class >> 2) != REG_CLASS_WORK;
        bool is_special_w = (class >> 2) == REG_CLASS_TEXW;

        /* Allocate TLS slot (maybe) */
        unsigned spill_slot = !is_special ? (*spill_count)++ : 0;

        /* For TLS, replace all stores to the spilled node. For
         * special reads, just keep as-is; the class will be demoted
         * implicitly. For special writes, spill to a work register */

        if (!is_special || is_special_w) {
                if (is_special_w)
                        spill_slot = spill_index++;

                mir_foreach_block(ctx, block) {
                mir_foreach_instr_in_block_safe(block, ins) {
                        if (ins->dest != spill_node) continue;

                        midgard_instruction st;

                        if (is_special_w) {
                                st = v_mov(spill_node, blank_alu_src, spill_slot);
                                st.no_spill = true;
                        } else {
                                ins->dest = SSA_FIXED_REGISTER(26);
                                ins->no_spill = true;
                                st = v_load_store_scratch(ins->dest, spill_slot, true, ins->mask);
                        }

                        /* Hint: don't rewrite this node */
                        st.hint = true;

                        mir_insert_instruction_after_scheduled(ctx, block, ins, st);

                        if (!is_special)
                                ctx->spills++;
                }
                }
        }

        /* For special reads, figure out how many bytes we need */
        unsigned read_bytemask = 0;

        mir_foreach_instr_global_safe(ctx, ins) {
                read_bytemask |= mir_bytemask_of_read_components(ins, spill_node);
        }

        /* Insert a load from TLS before the first consecutive
         * use of the node, rewriting to use spilled indices to
         * break up the live range. Or, for special, insert a
         * move. Ironically the latter *increases* register
         * pressure, but the two uses of the spilling mechanism
         * are somewhat orthogonal. (special spilling is to use
         * work registers to back special registers; TLS
         * spilling is to use memory to back work registers) */

        mir_foreach_block(ctx, block) {
                bool consecutive_skip = false;
                unsigned consecutive_index = 0;

                mir_foreach_instr_in_block(block, ins) {
                        /* We can't rewrite the moves used to spill in the
                         * first place. These moves are hinted. */
                        if (ins->hint) continue;

                        if (!mir_has_arg(ins, spill_node)) {
                                consecutive_skip = false;
                                continue;
                        }

                        if (consecutive_skip) {
                                /* Rewrite */
                                mir_rewrite_index_src_single(ins, spill_node, consecutive_index);
                                continue;
                        }

                        if (!is_special_w) {
                                consecutive_index = ++spill_index;

                                midgard_instruction *before = ins;

                                /* For a csel, go back one more not to break up the bundle */
                                if (ins->type == TAG_ALU_4 && OP_IS_CSEL(ins->alu.op))
                                        before = mir_prev_op(before);

                                midgard_instruction st;

                                if (is_special) {
                                        /* Move */
                                        st = v_mov(spill_node, blank_alu_src, consecutive_index);
                                        st.no_spill = true;
                                } else {
                                        /* TLS load */
                                        st = v_load_store_scratch(consecutive_index, spill_slot, false, 0xF);
                                }

                                /* Mask the load based on the component count
                                 * actually needed to prvent RA loops */

                                st.mask = mir_from_bytemask(read_bytemask, midgard_reg_mode_32);

                                mir_insert_instruction_before_scheduled(ctx, block, before, st);
                               // consecutive_skip = true;
                        } else {
                                /* Special writes already have their move spilled in */
                                consecutive_index = spill_slot;
                        }


                        /* Rewrite to use */
                        mir_rewrite_index_src_single(ins, spill_node, consecutive_index);

                        if (!is_special)
                                ctx->fills++;
                }
        }

        /* Reset hints */

        mir_foreach_instr_global(ctx, ins) {
                ins->hint = false;
        }
}

void
schedule_program(compiler_context *ctx)
{
        struct ra_graph *g = NULL;
        bool spilled = false;
        int iter_count = 1000; /* max iterations */

        /* Number of 128-bit slots in memory we've spilled into */
        unsigned spill_count = 0;

        midgard_promote_uniforms(ctx, 16);

        /* Must be lowered right before RA */
        mir_squeeze_index(ctx);
        mir_lower_special_reads(ctx);
        mir_squeeze_index(ctx);

        /* Lowering can introduce some dead moves */

        mir_foreach_block(ctx, block) {
                midgard_opt_dead_move_eliminate(ctx, block);
                schedule_block(ctx, block);
        }

        mir_create_pipeline_registers(ctx);

        do {
                if (spilled) 
                        mir_spill_register(ctx, g, &spill_count);

                mir_squeeze_index(ctx);
                mir_invalidate_liveness(ctx);

                g = NULL;
                g = allocate_registers(ctx, &spilled);
        } while(spilled && ((iter_count--) > 0));

        if (iter_count <= 0) {
                fprintf(stderr, "panfrost: Gave up allocating registers, rendering will be incomplete\n");
                assert(0);
        }

        /* Report spilling information. spill_count is in 128-bit slots (vec4 x
         * fp32), but tls_size is in bytes, so multiply by 16 */

        ctx->tls_size = spill_count * 16;

        install_registers(ctx, g);
}
