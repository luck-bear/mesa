/*
 * Copyright 2018 Collabora Ltd.
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

#include "zink_context.h"
#include "zink_compiler.h"
#include "zink_program.h"
#include "zink_screen.h"
#include "nir_to_spirv/nir_to_spirv.h"

#include "pipe/p_state.h"

#include "nir.h"
#include "compiler/nir/nir_builder.h"

#include "nir/tgsi_to_nir.h"
#include "tgsi/tgsi_dump.h"
#include "tgsi/tgsi_from_mesa.h"

#include "util/u_memory.h"

#include "compiler/spirv/nir_spirv.h"
#include "vulkan/util/vk_util.h"

bool
zink_lower_cubemap_to_array(nir_shader *s, uint32_t nonseamless_cube_mask);

static void
create_vs_pushconst(nir_shader *nir)
{
   nir_variable *vs_pushconst;
   /* create compatible layout for the ntv push constant loader */
   struct glsl_struct_field *fields = rzalloc_array(nir, struct glsl_struct_field, 2);
   fields[0].type = glsl_array_type(glsl_uint_type(), 1, 0);
   fields[0].name = ralloc_asprintf(nir, "draw_mode_is_indexed");
   fields[0].offset = offsetof(struct zink_gfx_push_constant, draw_mode_is_indexed);
   fields[1].type = glsl_array_type(glsl_uint_type(), 1, 0);
   fields[1].name = ralloc_asprintf(nir, "draw_id");
   fields[1].offset = offsetof(struct zink_gfx_push_constant, draw_id);
   vs_pushconst = nir_variable_create(nir, nir_var_mem_push_const,
                                                 glsl_struct_type(fields, 2, "struct", false), "vs_pushconst");
   vs_pushconst->data.location = INT_MAX; //doesn't really matter
}

static void
create_cs_pushconst(nir_shader *nir)
{
   nir_variable *cs_pushconst;
   /* create compatible layout for the ntv push constant loader */
   struct glsl_struct_field *fields = rzalloc_size(nir, 1 * sizeof(struct glsl_struct_field));
   fields[0].type = glsl_array_type(glsl_uint_type(), 1, 0);
   fields[0].name = ralloc_asprintf(nir, "work_dim");
   fields[0].offset = 0;
   cs_pushconst = nir_variable_create(nir, nir_var_mem_push_const,
                                                 glsl_struct_type(fields, 1, "struct", false), "cs_pushconst");
   cs_pushconst->data.location = INT_MAX; //doesn't really matter
}

static bool
reads_work_dim(nir_shader *shader)
{
   return BITSET_TEST(shader->info.system_values_read, SYSTEM_VALUE_WORK_DIM);
}

static bool
lower_work_dim_instr(nir_builder *b, nir_instr *in, void *data)
{
   if (in->type != nir_instr_type_intrinsic)
      return false;
   nir_intrinsic_instr *instr = nir_instr_as_intrinsic(in);
   if (instr->intrinsic != nir_intrinsic_load_work_dim)
      return false;

   if (instr->intrinsic == nir_intrinsic_load_work_dim) {
      b->cursor = nir_after_instr(&instr->instr);
      nir_intrinsic_instr *load = nir_intrinsic_instr_create(b->shader, nir_intrinsic_load_push_constant);
      load->src[0] = nir_src_for_ssa(nir_imm_int(b, 0));
      nir_intrinsic_set_range(load, 3 * sizeof(uint32_t));
      load->num_components = 1;
      nir_ssa_dest_init(&load->instr, &load->dest, 1, 32, "work_dim");
      nir_builder_instr_insert(b, &load->instr);

      nir_ssa_def_rewrite_uses(&instr->dest.ssa, &load->dest.ssa);
   }

   return true;
}

static bool
lower_work_dim(nir_shader *shader)
{
   if (shader->info.stage != MESA_SHADER_KERNEL)
      return false;

   if (!reads_work_dim(shader))
      return false;

   return nir_shader_instructions_pass(shader, lower_work_dim_instr, nir_metadata_dominance, NULL);
}

static bool
lower_64bit_vertex_attribs_instr(nir_builder *b, nir_instr *instr, void *data)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;
   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   if (intr->intrinsic != nir_intrinsic_load_deref)
      return false;
   nir_variable *var = nir_deref_instr_get_variable(nir_instr_as_deref(intr->src[0].ssa->parent_instr));
   if (var->data.mode != nir_var_shader_in)
      return false;
   if (!glsl_type_is_64bit(var->type) || !glsl_type_is_vector(var->type) || glsl_get_vector_elements(var->type) < 3)
      return false;

   /* create second variable for the split */
   nir_variable *var2 = nir_variable_clone(var, b->shader);
   /* split new variable into second slot */
   var2->data.driver_location++;
   nir_shader_add_variable(b->shader, var2);

   unsigned total_num_components = glsl_get_vector_elements(var->type);
   /* new variable is the second half of the dvec */
   var2->type = glsl_vector_type(glsl_get_base_type(var->type), glsl_get_vector_elements(var->type) - 2);
   /* clamp original variable to a dvec2 */
   var->type = glsl_vector_type(glsl_get_base_type(var->type), 2);

   b->cursor = nir_after_instr(instr);

   /* this is the first load instruction for the first half of the dvec3/4 components */
   nir_ssa_def *load = nir_load_var(b, var);
   /* this is the second load instruction for the second half of the dvec3/4 components */
   nir_ssa_def *load2 = nir_load_var(b, var2);

   nir_ssa_def *def[4];
   /* create a new dvec3/4 comprised of all the loaded components from both variables */
   def[0] = nir_vector_extract(b, load, nir_imm_int(b, 0));
   def[1] = nir_vector_extract(b, load, nir_imm_int(b, 1));
   def[2] = nir_vector_extract(b, load2, nir_imm_int(b, 0));
   if (total_num_components == 4)
      def[3] = nir_vector_extract(b, load2, nir_imm_int(b, 1));
   nir_ssa_def *new_vec = nir_vec(b, def, total_num_components);
   /* use the assembled dvec3/4 for all other uses of the load */
   nir_ssa_def_rewrite_uses_after(&intr->dest.ssa, new_vec,
                                  new_vec->parent_instr);

   /* remove the original instr and its deref chain */
   nir_instr *parent = intr->src[0].ssa->parent_instr;
   nir_instr_remove(instr);
   nir_deref_instr_remove_if_unused(nir_instr_as_deref(parent));

   return true;
}

/* mesa/gallium always provides UINT versions of 64bit formats:
 * - rewrite loads as 32bit vec loads
 * - cast back to 64bit
 */
static bool
lower_64bit_uint_attribs_instr(nir_builder *b, nir_instr *instr, void *data)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;
   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   if (intr->intrinsic != nir_intrinsic_load_deref)
      return false;
   nir_variable *var = nir_deref_instr_get_variable(nir_instr_as_deref(intr->src[0].ssa->parent_instr));
   if (var->data.mode != nir_var_shader_in)
      return false;
   if (glsl_get_bit_size(var->type) != 64 || glsl_get_base_type(var->type) >= GLSL_TYPE_SAMPLER)
      return false;

   unsigned num_components = glsl_get_vector_elements(var->type);
   enum glsl_base_type base_type;
   switch (glsl_get_base_type(var->type)) {
   case GLSL_TYPE_UINT64:
      base_type = GLSL_TYPE_UINT;
      break;
   case GLSL_TYPE_INT64:
      base_type = GLSL_TYPE_INT;
      break;
   case GLSL_TYPE_DOUBLE:
      base_type = GLSL_TYPE_FLOAT;
      break;
   default:
      unreachable("unknown 64-bit vertex attribute format!");
   }
   var->type = glsl_vector_type(base_type, num_components * 2);

   b->cursor = nir_after_instr(instr);

   nir_ssa_def *load = nir_load_var(b, var);
   nir_ssa_def *casted[2];
   for (unsigned i = 0; i < num_components; i++)
     casted[i] = nir_pack_64_2x32(b, nir_channels(b, load, BITFIELD_RANGE(i * 2, 2)));
   nir_ssa_def_rewrite_uses(&intr->dest.ssa, nir_vec(b, casted, num_components));

   /* remove the original instr and its deref chain */
   nir_instr *parent = intr->src[0].ssa->parent_instr;
   nir_instr_remove(instr);
   nir_deref_instr_remove_if_unused(nir_instr_as_deref(parent));

   return true;
}

/* "64-bit three- and four-component vectors consume two consecutive locations."
 *  - 14.1.4. Location Assignment
 *
 * this pass splits dvec3 and dvec4 vertex inputs into a dvec2 and a double/dvec2 which
 * are assigned to consecutive locations, loaded separately, and then assembled back into a
 * composite value that's used in place of the original loaded ssa src
 */
static bool
lower_64bit_vertex_attribs(nir_shader *shader)
{
   if (shader->info.stage != MESA_SHADER_VERTEX)
      return false;

   bool progress = nir_shader_instructions_pass(shader, lower_64bit_vertex_attribs_instr, nir_metadata_dominance, NULL);
   progress |= nir_shader_instructions_pass(shader, lower_64bit_uint_attribs_instr, nir_metadata_dominance, NULL);
   return progress;
}

static bool
lower_basevertex_instr(nir_builder *b, nir_instr *in, void *data)
{
   if (in->type != nir_instr_type_intrinsic)
      return false;
   nir_intrinsic_instr *instr = nir_instr_as_intrinsic(in);
   if (instr->intrinsic != nir_intrinsic_load_base_vertex)
      return false;

   b->cursor = nir_after_instr(&instr->instr);
   nir_intrinsic_instr *load = nir_intrinsic_instr_create(b->shader, nir_intrinsic_load_push_constant);
   load->src[0] = nir_src_for_ssa(nir_imm_int(b, 0));
   nir_intrinsic_set_range(load, 4);
   load->num_components = 1;
   nir_ssa_dest_init(&load->instr, &load->dest, 1, 32, "draw_mode_is_indexed");
   nir_builder_instr_insert(b, &load->instr);

   nir_ssa_def *composite = nir_build_alu(b, nir_op_bcsel,
                                          nir_build_alu(b, nir_op_ieq, &load->dest.ssa, nir_imm_int(b, 1), NULL, NULL),
                                          &instr->dest.ssa,
                                          nir_imm_int(b, 0),
                                          NULL);

   nir_ssa_def_rewrite_uses_after(&instr->dest.ssa, composite,
                                  composite->parent_instr);
   return true;
}

static bool
lower_basevertex(nir_shader *shader)
{
   if (shader->info.stage != MESA_SHADER_VERTEX)
      return false;

   if (!BITSET_TEST(shader->info.system_values_read, SYSTEM_VALUE_BASE_VERTEX))
      return false;

   return nir_shader_instructions_pass(shader, lower_basevertex_instr, nir_metadata_dominance, NULL);
}


static bool
lower_drawid_instr(nir_builder *b, nir_instr *in, void *data)
{
   if (in->type != nir_instr_type_intrinsic)
      return false;
   nir_intrinsic_instr *instr = nir_instr_as_intrinsic(in);
   if (instr->intrinsic != nir_intrinsic_load_draw_id)
      return false;

   b->cursor = nir_before_instr(&instr->instr);
   nir_intrinsic_instr *load = nir_intrinsic_instr_create(b->shader, nir_intrinsic_load_push_constant);
   load->src[0] = nir_src_for_ssa(nir_imm_int(b, 1));
   nir_intrinsic_set_range(load, 4);
   load->num_components = 1;
   nir_ssa_dest_init(&load->instr, &load->dest, 1, 32, "draw_id");
   nir_builder_instr_insert(b, &load->instr);

   nir_ssa_def_rewrite_uses(&instr->dest.ssa, &load->dest.ssa);

   return true;
}

static bool
lower_drawid(nir_shader *shader)
{
   if (shader->info.stage != MESA_SHADER_VERTEX)
      return false;

   if (!BITSET_TEST(shader->info.system_values_read, SYSTEM_VALUE_DRAW_ID))
      return false;

   return nir_shader_instructions_pass(shader, lower_drawid_instr, nir_metadata_dominance, NULL);
}

static bool
lower_dual_blend(nir_shader *shader)
{
   bool progress = false;
   nir_variable *var = nir_find_variable_with_location(shader, nir_var_shader_out, FRAG_RESULT_DATA1);
   if (var) {
      var->data.location = FRAG_RESULT_DATA0;
      var->data.index = 1;
      progress = true;
   }
   nir_shader_preserve_all_metadata(shader);
   return progress;
}

void
zink_screen_init_compiler(struct zink_screen *screen)
{
   static const struct nir_shader_compiler_options
   default_options = {
      .lower_ffma16 = true,
      .lower_ffma32 = true,
      .lower_ffma64 = true,
      .lower_scmp = true,
      .lower_fdph = true,
      .lower_flrp32 = true,
      .lower_fpow = true,
      .lower_fsat = true,
      .lower_extract_byte = true,
      .lower_extract_word = true,
      .lower_insert_byte = true,
      .lower_insert_word = true,
      .lower_mul_high = true,
      .lower_rotate = true,
      .lower_uadd_carry = true,
      .lower_pack_64_2x32_split = true,
      .lower_unpack_64_2x32_split = true,
      .lower_pack_32_2x16_split = true,
      .lower_unpack_32_2x16_split = true,
      .lower_vector_cmp = true,
      .lower_int64_options = 0,
      .lower_doubles_options = 0,
      .lower_uniforms_to_ubo = true,
      .has_fsub = true,
      .has_isub = true,
      .has_txs = true,
      .lower_mul_2x32_64 = true,
      .support_16bit_alu = true, /* not quite what it sounds like */
   };

   screen->nir_options = default_options;

   if (!screen->info.feats.features.shaderInt64)
      screen->nir_options.lower_int64_options = ~0;

   if (!screen->info.feats.features.shaderFloat64) {
      screen->nir_options.lower_doubles_options = ~0;
      screen->nir_options.lower_flrp64 = true;
      screen->nir_options.lower_ffma64 = true;
   }

   /*
       The OpFRem and OpFMod instructions use cheap approximations of remainder,
       and the error can be large due to the discontinuity in trunc() and floor().
       This can produce mathematically unexpected results in some cases, such as
       FMod(x,x) computing x rather than 0, and can also cause the result to have
       a different sign than the infinitely precise result.

       -Table 84. Precision of core SPIR-V Instructions
       * for drivers that are known to have imprecise fmod for doubles, lower dmod
    */
   if (screen->info.driver_props.driverID == VK_DRIVER_ID_MESA_RADV ||
       screen->info.driver_props.driverID == VK_DRIVER_ID_AMD_OPEN_SOURCE ||
       screen->info.driver_props.driverID == VK_DRIVER_ID_AMD_PROPRIETARY)
      screen->nir_options.lower_doubles_options = nir_lower_dmod;
}

const void *
zink_get_compiler_options(struct pipe_screen *pscreen,
                          enum pipe_shader_ir ir,
                          enum pipe_shader_type shader)
{
   assert(ir == PIPE_SHADER_IR_NIR);
   return &zink_screen(pscreen)->nir_options;
}

struct nir_shader *
zink_tgsi_to_nir(struct pipe_screen *screen, const struct tgsi_token *tokens)
{
   if (zink_debug & ZINK_DEBUG_TGSI) {
      fprintf(stderr, "TGSI shader:\n---8<---\n");
      tgsi_dump_to_file(tokens, 0, stderr);
      fprintf(stderr, "---8<---\n\n");
   }

   return tgsi_to_nir(tokens, screen, false);
}

static void
optimize_nir(struct nir_shader *s)
{
   bool progress;
   do {
      progress = false;
      NIR_PASS_V(s, nir_lower_vars_to_ssa);
      NIR_PASS(progress, s, nir_copy_prop);
      NIR_PASS(progress, s, nir_opt_remove_phis);
      NIR_PASS(progress, s, nir_opt_dce);
      NIR_PASS(progress, s, nir_opt_dead_cf);
      NIR_PASS(progress, s, nir_lower_phis_to_scalar, false);
      NIR_PASS(progress, s, nir_opt_cse);
      NIR_PASS(progress, s, nir_opt_peephole_select, 8, true, true);
      NIR_PASS(progress, s, nir_opt_algebraic);
      NIR_PASS(progress, s, nir_opt_constant_folding);
      NIR_PASS(progress, s, nir_opt_undef);
      NIR_PASS(progress, s, zink_nir_lower_b2b);
   } while (progress);

   do {
      progress = false;
      NIR_PASS(progress, s, nir_opt_algebraic_late);
      if (progress) {
         NIR_PASS_V(s, nir_copy_prop);
         NIR_PASS_V(s, nir_opt_dce);
         NIR_PASS_V(s, nir_opt_cse);
      }
   } while (progress);
}

/* - copy the lowered fbfetch variable
 * - set the new one up as an input attachment for descriptor 0.6
 * - load it as an image
 * - overwrite the previous load
 */
static bool
lower_fbfetch_instr(nir_builder *b, nir_instr *instr, void *data)
{
   bool ms = data != NULL;
   if (instr->type != nir_instr_type_intrinsic)
      return false;
   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   if (intr->intrinsic != nir_intrinsic_load_deref)
      return false;
   nir_variable *var = nir_deref_instr_get_variable(nir_src_as_deref(intr->src[0]));
   if (!var->data.fb_fetch_output)
      return false;
   b->cursor = nir_after_instr(instr);
   nir_variable *fbfetch = nir_variable_clone(var, b->shader);
   /* If Dim is SubpassData, ... Image Format must be Unknown
    * - SPIRV OpTypeImage specification
    */
   fbfetch->data.image.format = 0;
   fbfetch->data.index = 0; /* fix this if more than 1 fbfetch target is supported */
   fbfetch->data.mode = nir_var_uniform;
   fbfetch->data.binding = ZINK_FBFETCH_BINDING;
   fbfetch->data.binding = ZINK_FBFETCH_BINDING;
   fbfetch->data.sample = ms;
   enum glsl_sampler_dim dim = ms ? GLSL_SAMPLER_DIM_SUBPASS_MS : GLSL_SAMPLER_DIM_SUBPASS;
   fbfetch->type = glsl_image_type(dim, false, GLSL_TYPE_FLOAT);
   nir_shader_add_variable(b->shader, fbfetch);
   nir_ssa_def *deref = &nir_build_deref_var(b, fbfetch)->dest.ssa;
   nir_ssa_def *sample = ms ? nir_load_sample_id(b) : nir_ssa_undef(b, 1, 32);
   nir_ssa_def *load = nir_image_deref_load(b, 4, 32, deref, nir_imm_vec4(b, 0, 0, 0, 1), sample, nir_imm_int(b, 0));
   unsigned swiz[4] = {2, 1, 0, 3};
   nir_ssa_def *swizzle = nir_swizzle(b, load, swiz, 4);
   nir_ssa_def_rewrite_uses(&intr->dest.ssa, swizzle);
   return true;
}

static bool
lower_fbfetch(nir_shader *shader, nir_variable **fbfetch, bool ms)
{
   nir_foreach_shader_out_variable(var, shader) {
      if (var->data.fb_fetch_output) {
         *fbfetch = var;
         break;
      }
   }
   assert(*fbfetch);
   if (!*fbfetch)
      return false;
   return nir_shader_instructions_pass(shader, lower_fbfetch_instr, nir_metadata_dominance, (void*)ms);
}

/* check for a genuine gl_PointSize output vs one from nir_lower_point_size_mov */
static bool
check_psiz(struct nir_shader *s)
{
   bool have_psiz = false;
   nir_foreach_shader_out_variable(var, s) {
      if (var->data.location == VARYING_SLOT_PSIZ) {
         /* genuine PSIZ outputs will have this set */
         have_psiz |= !!var->data.explicit_location;
      }
   }
   return have_psiz;
}

static nir_variable *
find_var_with_location_frac(nir_shader *nir, unsigned location, unsigned location_frac, bool have_psiz)
{
   unsigned found = 0;
   if (!location_frac && location != VARYING_SLOT_PSIZ) {
      nir_foreach_shader_out_variable(var, nir) {
         if (var->data.location == location)
            found++;
      }
   }
   if (found) {
      /* multiple variables found for this location: find the biggest one */
      nir_variable *out = NULL;
      unsigned slots = 0;
      nir_foreach_shader_out_variable(var, nir) {
         if (var->data.location == location) {
            unsigned count_slots = glsl_count_vec4_slots(var->type, false, false);
            if (count_slots > slots) {
               slots = count_slots;
               out = var;
            }
         }
      }
      return out;
   } else {
      /* only one variable found or this is location_frac */
      nir_foreach_shader_out_variable(var, nir) {
         if (var->data.location == location &&
             (var->data.location_frac == location_frac ||
              (glsl_type_is_array(var->type) ? glsl_array_size(var->type) : glsl_get_vector_elements(var->type)) >= location_frac + 1)) {
            if (location != VARYING_SLOT_PSIZ || !have_psiz || var->data.explicit_location)
               return var;
         }
      }
   }
   return NULL;
}

static bool
is_inlined(const bool *inlined, const struct pipe_stream_output *output)
{
   for (unsigned i = 0; i < output->num_components; i++)
      if (!inlined[output->start_component + i])
         return false;
   return true;
}

static void
update_psiz_location(nir_shader *nir, nir_variable *psiz)
{
   uint32_t last_output = util_last_bit64(nir->info.outputs_written);
   if (last_output < VARYING_SLOT_VAR0)
      last_output = VARYING_SLOT_VAR0;
   else
      last_output++;
   /* this should get fixed up by slot remapping */
   psiz->data.location = last_output;
}

static const struct glsl_type *
clamp_slot_type(const struct glsl_type *type, unsigned slot)
{
   /* could be dvec/dmat/mat: each member is the same */
   const struct glsl_type *plain = glsl_without_array_or_matrix(type);
   /* determine size of each member type */
   unsigned slot_count = glsl_count_vec4_slots(plain, false, false);
   /* normalize slot idx to current type's size */
   slot %= slot_count;
   unsigned slot_components = glsl_get_components(plain);
   if (glsl_base_type_is_64bit(glsl_get_base_type(plain)))
      slot_components *= 2;
   /* create a vec4 mask of the selected slot's components out of all the components */
   uint32_t mask = BITFIELD_MASK(slot_components) & BITFIELD_RANGE(slot * 4, 4);
   /* return a vecN of the selected components */
   slot_components = util_bitcount(mask);
   return glsl_vec_type(slot_components);
}

static const struct glsl_type *
unroll_struct_type(const struct glsl_type *slot_type, unsigned *slot_idx)
{
   const struct glsl_type *type = slot_type;
   unsigned slot_count = 0;
   unsigned cur_slot = 0;
   /* iterate over all the members in the struct, stopping once the slot idx is reached */
   for (unsigned i = 0; i < glsl_get_length(slot_type) && cur_slot <= *slot_idx; i++, cur_slot += slot_count) {
      /* use array type for slot counting but return array member type for unroll */
      const struct glsl_type *arraytype = glsl_get_struct_field(slot_type, i);
      type = glsl_without_array(arraytype);
      slot_count = glsl_count_vec4_slots(arraytype, false, false);
   }
   *slot_idx -= (cur_slot - slot_count);
   if (!glsl_type_is_struct_or_ifc(type))
      /* this is a fully unrolled struct: find the number of vec components to output */
      type = clamp_slot_type(type, *slot_idx);
   return type;
}

static unsigned
get_slot_components(nir_variable *var, unsigned slot, unsigned so_slot)
{
   assert(var && slot < var->data.location + glsl_count_vec4_slots(var->type, false, false));
   const struct glsl_type *orig_type = var->type;
   const struct glsl_type *type = glsl_without_array(var->type);
   unsigned slot_idx = slot - so_slot;
   if (type != orig_type)
      slot_idx %= glsl_count_vec4_slots(type, false, false);
   /* need to find the vec4 that's being exported by this slot */
   while (glsl_type_is_struct_or_ifc(type))
      type = unroll_struct_type(type, &slot_idx);

   /* arrays here are already fully unrolled from their structs, so slot handling is implicit */
   unsigned num_components = glsl_get_components(glsl_without_array(type));
   const struct glsl_type *arraytype = orig_type;
   while (glsl_type_is_array(arraytype) && !glsl_type_is_struct_or_ifc(glsl_without_array(arraytype))) {
      num_components *= glsl_array_size(arraytype);
      arraytype = glsl_get_array_element(arraytype);
   }
   assert(num_components);
   /* gallium handles xfb in terms of 32bit units */
   if (glsl_base_type_is_64bit(glsl_get_base_type(glsl_without_array(type))))
      num_components *= 2;
   return num_components;
}

static const struct pipe_stream_output *
find_packed_output(const struct pipe_stream_output_info *so_info, uint8_t *reverse_map, unsigned slot)
{
   for (unsigned i = 0; i < so_info->num_outputs; i++) {
      const struct pipe_stream_output *packed_output = &so_info->output[i];
      if (reverse_map[packed_output->register_index] == slot)
         return packed_output;
   }
   return NULL;
}

static void
update_so_info(struct zink_shader *zs, const struct pipe_stream_output_info *so_info,
               uint64_t outputs_written, bool have_psiz)
{
   uint8_t reverse_map[VARYING_SLOT_MAX] = {0};
   unsigned slot = 0;
   /* semi-copied from iris */
   while (outputs_written) {
      int bit = u_bit_scan64(&outputs_written);
      /* PSIZ from nir_lower_point_size_mov breaks stream output, so always skip it */
      if (bit == VARYING_SLOT_PSIZ && !have_psiz)
         continue;
      reverse_map[slot++] = bit;
   }

   bool have_fake_psiz = false;
   nir_foreach_shader_out_variable(var, zs->nir) {
      var->data.explicit_xfb_buffer = 0;
      if (var->data.location == VARYING_SLOT_PSIZ && !var->data.explicit_location)
         have_fake_psiz = true;
   }

   bool inlined[VARYING_SLOT_MAX][4] = {0};
   uint64_t packed = 0;
   uint8_t packed_components[VARYING_SLOT_MAX] = {0};
   uint8_t packed_streams[VARYING_SLOT_MAX] = {0};
   uint8_t packed_buffers[VARYING_SLOT_MAX] = {0};
   uint16_t packed_offsets[VARYING_SLOT_MAX][4] = {0};
   nir_variable *psiz = NULL;
   for (unsigned i = 0; i < so_info->num_outputs; i++) {
      const struct pipe_stream_output *output = &so_info->output[i];
      unsigned slot = reverse_map[output->register_index];
      /* always set stride to be used during draw */
      zs->sinfo.so_info.stride[output->output_buffer] = so_info->stride[output->output_buffer];
      if (zs->nir->info.stage != MESA_SHADER_GEOMETRY || util_bitcount(zs->nir->info.gs.active_stream_mask) == 1) {
         nir_variable *var = NULL;
         unsigned so_slot;
         while (!var)
            var = find_var_with_location_frac(zs->nir, slot--, output->start_component, have_psiz);
         if (var->data.location == VARYING_SLOT_PSIZ)
            psiz = var;
         so_slot = slot + 1;
         slot = reverse_map[output->register_index];
         if (var->data.explicit_xfb_buffer) {
            /* handle dvec3 where gallium splits streamout over 2 registers */
            for (unsigned j = 0; j < output->num_components; j++)
               inlined[slot][output->start_component + j] = true;
         }
         if (is_inlined(inlined[slot], output))
            continue;
         bool is_struct = glsl_type_is_struct_or_ifc(glsl_without_array(var->type));
         unsigned num_components = get_slot_components(var, slot, so_slot);
         /* if this is the entire variable, try to blast it out during the initial declaration
          * structs must be handled later to ensure accurate analysis
          */
         if (!is_struct && (num_components == output->num_components || (num_components > output->num_components && output->num_components == 4))) {
            var->data.explicit_xfb_buffer = 1;
            var->data.xfb.buffer = output->output_buffer;
            var->data.xfb.stride = so_info->stride[output->output_buffer] * 4;
            var->data.offset = output->dst_offset * 4;
            var->data.stream = output->stream;
            for (unsigned j = 0; j < output->num_components; j++)
               inlined[slot][output->start_component + j] = true;
         } else {
            /* otherwise store some metadata for later */
            packed |= BITFIELD64_BIT(slot);
            packed_components[slot] += output->num_components;
            packed_streams[slot] |= BITFIELD_BIT(output->stream);
            packed_buffers[slot] |= BITFIELD_BIT(output->output_buffer);
            for (unsigned j = 0; j < output->num_components; j++)
               packed_offsets[output->register_index][j + output->start_component] = output->dst_offset + j;
         }
      }
   }

   /* if this was flagged as a packed output before, and if all the components are
    * being output with the same stream on the same buffer with increasing offsets, this entire variable
    * can be consolidated into a single output to conserve locations
    */
   for (unsigned i = 0; i < so_info->num_outputs; i++) {
      const struct pipe_stream_output *output = &so_info->output[i];
      unsigned slot = reverse_map[output->register_index];
      if (is_inlined(inlined[slot], output))
         continue;
      if (zs->nir->info.stage != MESA_SHADER_GEOMETRY || util_bitcount(zs->nir->info.gs.active_stream_mask) == 1) {
         nir_variable *var = NULL;
         while (!var)
            var = find_var_with_location_frac(zs->nir, slot--, output->start_component, have_psiz);

         unsigned num_slots = glsl_count_vec4_slots(var->type, false, false);
         /* for each variable, iterate over all the variable's slots and inline the outputs */
         for (unsigned j = 0; j < num_slots; j++) {
            slot = var->data.location + j;
            const struct pipe_stream_output *packed_output = find_packed_output(so_info, reverse_map, slot);
            if (!packed_output)
               goto out;

            /* if this slot wasn't packed or isn't in the same stream/buffer, skip consolidation */
            if (!(packed & BITFIELD64_BIT(slot)) ||
                util_bitcount(packed_streams[slot]) != 1 ||
                util_bitcount(packed_buffers[slot]) != 1)
               goto out;

            /* if all the components the variable exports to this slot aren't captured, skip consolidation */
            unsigned num_components = get_slot_components(var, slot, var->data.location);
            if (num_components != packed_components[slot])
               goto out;

            /* in order to pack the xfb output, all the offsets must be sequentially incrementing */
            uint32_t prev_offset = packed_offsets[packed_output->register_index][0];
            for (unsigned k = 1; k < num_components; k++) {
               /* if the offsets are not incrementing as expected, skip consolidation */
               if (packed_offsets[packed_output->register_index][k] != prev_offset + 1)
                  goto out;
               prev_offset = packed_offsets[packed_output->register_index][k + packed_output->start_component];
            }
         }
         /* this output can be consolidated: blast out all the data inlined */
         var->data.explicit_xfb_buffer = 1;
         var->data.xfb.buffer = output->output_buffer;
         var->data.xfb.stride = so_info->stride[output->output_buffer] * 4;
         var->data.offset = output->dst_offset * 4;
         var->data.stream = output->stream;
         /* GLSL specifies that interface blocks are split per-buffer in XFB */
         if (glsl_type_is_array(var->type) && glsl_array_size(var->type) > 1 && glsl_type_is_interface(glsl_without_array(var->type)))
            zs->sinfo.so_propagate |= BITFIELD_BIT(var->data.location - VARYING_SLOT_VAR0);
         /* mark all slot components inlined to skip subsequent loop iterations */
         for (unsigned j = 0; j < num_slots; j++) {
            slot = var->data.location + j;
            for (unsigned k = 0; k < packed_components[slot]; k++)
               inlined[slot][k] = true;
            packed &= ~BITFIELD64_BIT(slot);
         }
         continue;
      }
out:
      /* these are packed/explicit varyings which can't be exported with normal output */
      zs->sinfo.so_info.output[zs->sinfo.so_info.num_outputs] = *output;
      /* Map Gallium's condensed "slots" back to real VARYING_SLOT_* enums */
      zs->sinfo.so_info_slots[zs->sinfo.so_info.num_outputs++] = reverse_map[output->register_index];
   }
   zs->sinfo.have_xfb = zs->sinfo.so_info.num_outputs || zs->sinfo.so_propagate;
   /* ensure this doesn't get output in the shader by unsetting location */
   if (have_fake_psiz && psiz)
      update_psiz_location(zs->nir, psiz);
}

struct decompose_state {
  nir_variable **split;
  bool needs_w;
};

static bool
lower_attrib(nir_builder *b, nir_instr *instr, void *data)
{
   struct decompose_state *state = data;
   nir_variable **split = state->split;
   if (instr->type != nir_instr_type_intrinsic)
      return false;
   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   if (intr->intrinsic != nir_intrinsic_load_deref)
      return false;
   nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
   nir_variable *var = nir_deref_instr_get_variable(deref);
   if (var != split[0])
      return false;
   unsigned num_components = glsl_get_vector_elements(split[0]->type);
   b->cursor = nir_after_instr(instr);
   nir_ssa_def *loads[4];
   for (unsigned i = 0; i < (state->needs_w ? num_components - 1 : num_components); i++)
      loads[i] = nir_load_deref(b, nir_build_deref_var(b, split[i+1]));
   if (state->needs_w) {
      /* oob load w comopnent to get correct value for int/float */
      loads[3] = nir_channel(b, loads[0], 3);
      loads[0] = nir_channel(b, loads[0], 0);
   }
   nir_ssa_def *new_load = nir_vec(b, loads, num_components);
   nir_ssa_def_rewrite_uses(&intr->dest.ssa, new_load);
   nir_instr_remove_v(instr);
   return true;
}

static bool
decompose_attribs(nir_shader *nir, uint32_t decomposed_attrs, uint32_t decomposed_attrs_without_w)
{
   uint32_t bits = 0;
   nir_foreach_variable_with_modes(var, nir, nir_var_shader_in)
      bits |= BITFIELD_BIT(var->data.driver_location);
   bits = ~bits;
   u_foreach_bit(location, decomposed_attrs | decomposed_attrs_without_w) {
      nir_variable *split[5];
      struct decompose_state state;
      state.split = split;
      nir_variable *var = nir_find_variable_with_driver_location(nir, nir_var_shader_in, location);
      assert(var);
      split[0] = var;
      bits |= BITFIELD_BIT(var->data.driver_location);
      const struct glsl_type *new_type = glsl_type_is_scalar(var->type) ? var->type : glsl_get_array_element(var->type);
      unsigned num_components = glsl_get_vector_elements(var->type);
      state.needs_w = (decomposed_attrs_without_w & BITFIELD_BIT(location)) != 0 && num_components == 4;
      for (unsigned i = 0; i < (state.needs_w ? num_components - 1 : num_components); i++) {
         split[i+1] = nir_variable_clone(var, nir);
         split[i+1]->name = ralloc_asprintf(nir, "%s_split%u", var->name, i);
         if (decomposed_attrs_without_w & BITFIELD_BIT(location))
            split[i+1]->type = !i && num_components == 4 ? var->type : new_type;
         else
            split[i+1]->type = new_type;
         split[i+1]->data.driver_location = ffs(bits) - 1;
         bits &= ~BITFIELD_BIT(split[i+1]->data.driver_location);
         nir_shader_add_variable(nir, split[i+1]);
      }
      var->data.mode = nir_var_shader_temp;
      nir_shader_instructions_pass(nir, lower_attrib, nir_metadata_dominance, &state);
   }
   nir_fixup_deref_modes(nir);
   NIR_PASS_V(nir, nir_remove_dead_variables, nir_var_shader_temp, NULL);
   optimize_nir(nir);
   return true;
}

static bool
rewrite_bo_access_instr(nir_builder *b, nir_instr *instr, void *data)
{
   struct zink_screen *screen = data;
   const bool has_int64 = screen->info.feats.features.shaderInt64;
   if (instr->type != nir_instr_type_intrinsic)
      return false;
   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   b->cursor = nir_before_instr(instr);
   switch (intr->intrinsic) {
   case nir_intrinsic_load_ssbo:
   case nir_intrinsic_load_ubo: {
      /* ubo0 can have unaligned 64bit loads, particularly for bindless texture ids */
      bool force_2x32 = intr->intrinsic == nir_intrinsic_load_ubo &&
                        nir_src_is_const(intr->src[0]) &&
                        nir_src_as_uint(intr->src[0]) == 0 &&
                        nir_dest_bit_size(intr->dest) == 64 &&
                        nir_intrinsic_align_offset(intr) % 8 != 0;
      nir_instr_rewrite_src_ssa(instr, &intr->src[1], nir_udiv_imm(b, intr->src[1].ssa,
                                (force_2x32 ? 32 : nir_dest_bit_size(intr->dest)) / 8));
      /* if 64bit isn't supported, 64bit loads definitely aren't supported, so rewrite as 2x32 with cast and pray */
      if (force_2x32 || (nir_dest_bit_size(intr->dest) == 64 && !has_int64)) {
         /* this is always scalarized */
         assert(intr->dest.ssa.num_components == 1);
         /* rewrite as 2x32 */
         nir_ssa_def *load;
         if (intr->intrinsic == nir_intrinsic_load_ssbo)
            load = nir_load_ssbo(b, 2, 32, intr->src[0].ssa, intr->src[1].ssa, .align_mul = 4, .align_offset = 0);
         else
            load = nir_load_ubo(b, 2, 32, intr->src[0].ssa, intr->src[1].ssa, .align_mul = 4, .align_offset = 0, .range = 4);
         nir_intrinsic_set_access(nir_instr_as_intrinsic(load->parent_instr), nir_intrinsic_access(intr));
         /* cast back to 64bit */
         nir_ssa_def *casted = nir_pack_64_2x32(b, load);
         nir_ssa_def_rewrite_uses(&intr->dest.ssa, casted);
         nir_instr_remove(instr);
      }
      return true;
   }
   case nir_intrinsic_load_shared:
      b->cursor = nir_before_instr(instr);
      nir_instr_rewrite_src_ssa(instr, &intr->src[0], nir_udiv_imm(b, intr->src[0].ssa, nir_dest_bit_size(intr->dest) / 8));
      /* if 64bit isn't supported, 64bit loads definitely aren't supported, so rewrite as 2x32 with cast and pray */
      if (nir_dest_bit_size(intr->dest) == 64 && !has_int64) {
         /* this is always scalarized */
         assert(intr->dest.ssa.num_components == 1);
         /* rewrite as 2x32 */
         nir_ssa_def *load = nir_load_shared(b, 2, 32, intr->src[0].ssa, .align_mul = 4, .align_offset = 0);
         /* cast back to 64bit */
         nir_ssa_def *casted = nir_pack_64_2x32(b, load);
         nir_ssa_def_rewrite_uses(&intr->dest.ssa, casted);
         nir_instr_remove(instr);
         return true;
      }
      break;
   case nir_intrinsic_store_ssbo:
      b->cursor = nir_before_instr(instr);
      nir_instr_rewrite_src_ssa(instr, &intr->src[2], nir_udiv_imm(b, intr->src[2].ssa, nir_src_bit_size(intr->src[0]) / 8));
      /* if 64bit isn't supported, 64bit loads definitely aren't supported, so rewrite as 2x32 with cast and pray */
      if (nir_src_bit_size(intr->src[0]) == 64 && !has_int64) {
         /* this is always scalarized */
         assert(intr->src[0].ssa->num_components == 1);
         /* cast to 32bit: nir_unpack_64_2x32 not supported by ntv */
         nir_ssa_def *casted = nir_vec2(b, nir_u2u32(b, intr->src[0].ssa), nir_u2u32(b, nir_ushr_imm(b, intr->src[0].ssa, 32)));
         /* rewrite as 2x32 */
         nir_store_ssbo(b, casted, intr->src[1].ssa, intr->src[2].ssa, .align_mul = 4, .align_offset = 0);
         nir_instr_remove(instr);
      }
      return true;
   case nir_intrinsic_store_shared:
      b->cursor = nir_before_instr(instr);
      nir_instr_rewrite_src_ssa(instr, &intr->src[1], nir_udiv_imm(b, intr->src[1].ssa, nir_src_bit_size(intr->src[0]) / 8));
      /* if 64bit isn't supported, 64bit loads definitely aren't supported, so rewrite as 2x32 with cast and pray */
      if (nir_src_bit_size(intr->src[0]) == 64 && !has_int64) {
         /* this is always scalarized */
         assert(intr->src[0].ssa->num_components == 1);
         /* cast to 32bit: nir_unpack_64_2x32 not supported by ntv */
         nir_ssa_def *casted = nir_vec2(b, nir_u2u32(b, intr->src[0].ssa), nir_u2u32(b, nir_ushr_imm(b, intr->src[0].ssa, 32)));
         /* rewrite as 2x32 */
         nir_store_shared(b, casted, intr->src[1].ssa, .align_mul = 4, .align_offset = 0);
         nir_instr_remove(instr);
      }
      return true;
   default:
      break;
   }
   return false;
}

static bool
rewrite_bo_access(nir_shader *shader, struct zink_screen *screen)
{
   return nir_shader_instructions_pass(shader, rewrite_bo_access_instr, nir_metadata_dominance, screen);
}

struct bo_vars {
   nir_variable *ubo[2][5];
   nir_variable *ssbo[5];
   uint32_t first_ubo;
   uint32_t first_ssbo;
};

static nir_variable *
get_bo_var(nir_shader *shader, struct bo_vars *bo, bool ssbo, nir_src *src, unsigned bit_size)
{
   nir_variable *var, **ptr;
   nir_variable **arr = (nir_variable**)bo->ubo;
   unsigned idx = ssbo || (nir_src_is_const(*src) && !nir_src_as_uint(*src)) ? 0 : 1;

   if (ssbo)
      ptr = &bo->ssbo[bit_size >> 4];
   else
      ptr = &arr[idx * 5 + (bit_size >> 4)];
   var = *ptr;
   if (!var) {
      if (ssbo)
         var = bo->ssbo[32 >> 4];
      else
         var = arr[idx * 5 + (32 >> 4)];
      var = nir_variable_clone(var, shader);
      *ptr = var;
      nir_shader_add_variable(shader, var);

      struct glsl_struct_field *fields = rzalloc_array(shader, struct glsl_struct_field, 2);
      fields[0].name = ralloc_strdup(shader, "base");
      fields[1].name = ralloc_strdup(shader, "unsized");
      unsigned array_size = glsl_get_length(var->type);
      const struct glsl_type *bare_type = glsl_without_array(var->type);
      const struct glsl_type *array_type = glsl_get_struct_field(bare_type, 0);
      unsigned length = glsl_get_length(array_type);
      const struct glsl_type *type;
      const struct glsl_type *unsized = glsl_array_type(glsl_uintN_t_type(bit_size), 0, bit_size / 8);
      if (bit_size > 32) {
         assert(bit_size == 64);
         type = glsl_array_type(glsl_uintN_t_type(bit_size), length / 2, bit_size / 8);
      } else {
         type = glsl_array_type(glsl_uintN_t_type(bit_size), length * (32 / bit_size), bit_size / 8);
      }
      fields[0].type = type;
      fields[1].type = unsized;
      var->type = glsl_array_type(glsl_struct_type(fields, glsl_get_length(bare_type), "struct", false), array_size, 0);
      var->data.driver_location = idx;
   }
   return var;
}

static bool
remove_bo_access_instr(nir_builder *b, nir_instr *instr, void *data)
{
   struct bo_vars *bo = data;
   if (instr->type != nir_instr_type_intrinsic)
      return false;
   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   nir_variable *var = NULL;
   nir_ssa_def *offset = NULL;
   bool is_load = true;
   b->cursor = nir_before_instr(instr);
   nir_src *src;
   bool ssbo = true;
   switch (intr->intrinsic) {
   /* TODO: these should all be rewritten to use deref intrinsics */
   case nir_intrinsic_ssbo_atomic_add:
   case nir_intrinsic_ssbo_atomic_umin:
   case nir_intrinsic_ssbo_atomic_imin:
   case nir_intrinsic_ssbo_atomic_umax:
   case nir_intrinsic_ssbo_atomic_imax:
   case nir_intrinsic_ssbo_atomic_and:
   case nir_intrinsic_ssbo_atomic_or:
   case nir_intrinsic_ssbo_atomic_xor:
   case nir_intrinsic_ssbo_atomic_exchange:
   case nir_intrinsic_ssbo_atomic_comp_swap:
      nir_instr_rewrite_src_ssa(instr, &intr->src[0], nir_iadd_imm(b, intr->src[0].ssa, -bo->first_ssbo));
      return true;
   case nir_intrinsic_store_ssbo:
      src = &intr->src[1];
      var = get_bo_var(b->shader, bo, true, src, nir_src_bit_size(intr->src[0]));
      offset = intr->src[2].ssa;
      is_load = false;
      break;
   case nir_intrinsic_load_ssbo:
      src = &intr->src[0];
      var = get_bo_var(b->shader, bo, true, src, nir_dest_bit_size(intr->dest));
      offset = intr->src[1].ssa;
      break;
   case nir_intrinsic_load_ubo:
      src = &intr->src[0];
      var = get_bo_var(b->shader, bo, false, src, nir_dest_bit_size(intr->dest));
      offset = intr->src[1].ssa;
      ssbo = false;
      break;
   default:
      return false;
   }
   assert(var);
   assert(offset);
   nir_deref_instr *deref_var = nir_build_deref_var(b, var);
   nir_ssa_def *idx = !ssbo && var->data.driver_location ? nir_iadd_imm(b, src->ssa, -1) : src->ssa;
   if (!ssbo && bo->first_ubo && var->data.driver_location)
      idx = nir_iadd_imm(b, idx, -bo->first_ubo);
   else if (ssbo && bo->first_ssbo)
      idx = nir_iadd_imm(b, idx, -bo->first_ssbo);
   nir_deref_instr *deref_array = nir_build_deref_array(b, deref_var, idx);
   nir_deref_instr *deref_struct = nir_build_deref_struct(b, deref_array, 0);
   assert(intr->num_components <= 2);
   if (is_load) {
      nir_ssa_def *result[2];
      for (unsigned i = 0; i < intr->num_components; i++) {
         nir_deref_instr *deref_arr = nir_build_deref_array(b, deref_struct, offset);
         result[i] = nir_load_deref(b, deref_arr);
         if (intr->intrinsic == nir_intrinsic_load_ssbo)
            nir_intrinsic_set_access(nir_instr_as_intrinsic(result[i]->parent_instr), nir_intrinsic_access(intr));
         offset = nir_iadd_imm(b, offset, 1);
      }
      nir_ssa_def *load = nir_vec(b, result, intr->num_components);
      nir_ssa_def_rewrite_uses(&intr->dest.ssa, load);
   } else {
      nir_deref_instr *deref_arr = nir_build_deref_array(b, deref_struct, offset);
      nir_build_store_deref(b, &deref_arr->dest.ssa, intr->src[0].ssa, BITFIELD_MASK(intr->num_components), nir_intrinsic_access(intr));
   }
   nir_instr_remove(instr);
   return true;
}

static bool
remove_bo_access(nir_shader *shader, struct zink_shader *zs)
{
   struct bo_vars bo;
   memset(&bo, 0, sizeof(bo));
   if (zs->ubos_used)
      bo.first_ubo = ffs(zs->ubos_used & ~BITFIELD_BIT(0)) - 2;
   assert(bo.first_ssbo < PIPE_MAX_CONSTANT_BUFFERS);
   if (zs->ssbos_used)
      bo.first_ssbo = ffs(zs->ssbos_used) - 1;
   assert(bo.first_ssbo < PIPE_MAX_SHADER_BUFFERS);
   nir_foreach_variable_with_modes(var, shader, nir_var_mem_ssbo | nir_var_mem_ubo) {
      if (var->data.mode == nir_var_mem_ssbo) {
         assert(!bo.ssbo[32 >> 4]);
         bo.ssbo[32 >> 4] = var;
      } else {
         assert(!bo.ubo[!!var->data.driver_location][32 >> 4]);
         bo.ubo[!!var->data.driver_location][32 >> 4] = var;
      }
   }
   return nir_shader_instructions_pass(shader, remove_bo_access_instr, nir_metadata_dominance, &bo);
}

static void
assign_producer_var_io(gl_shader_stage stage, nir_variable *var, unsigned *reserved, unsigned char *slot_map)
{
   unsigned slot = var->data.location;
   switch (slot) {
   case -1:
   case VARYING_SLOT_POS:
   case VARYING_SLOT_PNTC:
   case VARYING_SLOT_PSIZ:
   case VARYING_SLOT_LAYER:
   case VARYING_SLOT_PRIMITIVE_ID:
   case VARYING_SLOT_CLIP_DIST0:
   case VARYING_SLOT_CULL_DIST0:
   case VARYING_SLOT_VIEWPORT:
   case VARYING_SLOT_FACE:
   case VARYING_SLOT_TESS_LEVEL_OUTER:
   case VARYING_SLOT_TESS_LEVEL_INNER:
      /* use a sentinel value to avoid counting later */
      var->data.driver_location = UINT_MAX;
      break;

   default:
      if (var->data.patch) {
         assert(slot >= VARYING_SLOT_PATCH0);
         slot -= VARYING_SLOT_PATCH0;
      }
      if (slot_map[slot] == 0xff) {
         assert(*reserved < MAX_VARYING);
         slot_map[slot] = *reserved;
         if (stage == MESA_SHADER_TESS_EVAL && var->data.mode == nir_var_shader_in && !var->data.patch)
            *reserved += glsl_count_vec4_slots(glsl_get_array_element(var->type), false, false);
         else
            *reserved += glsl_count_vec4_slots(var->type, false, false);
      }
      slot = slot_map[slot];
      assert(slot < MAX_VARYING);
      var->data.driver_location = slot;
   }
}

ALWAYS_INLINE static bool
is_texcoord(gl_shader_stage stage, const nir_variable *var)
{
   if (stage != MESA_SHADER_FRAGMENT)
      return false;
   return var->data.location >= VARYING_SLOT_TEX0 && 
          var->data.location <= VARYING_SLOT_TEX7;
}

static bool
assign_consumer_var_io(gl_shader_stage stage, nir_variable *var, unsigned *reserved, unsigned char *slot_map)
{
   unsigned slot = var->data.location;
   switch (slot) {
   case VARYING_SLOT_POS:
   case VARYING_SLOT_PNTC:
   case VARYING_SLOT_PSIZ:
   case VARYING_SLOT_LAYER:
   case VARYING_SLOT_PRIMITIVE_ID:
   case VARYING_SLOT_CLIP_DIST0:
   case VARYING_SLOT_CULL_DIST0:
   case VARYING_SLOT_VIEWPORT:
   case VARYING_SLOT_FACE:
   case VARYING_SLOT_TESS_LEVEL_OUTER:
   case VARYING_SLOT_TESS_LEVEL_INNER:
      /* use a sentinel value to avoid counting later */
      var->data.driver_location = UINT_MAX;
      break;
   default:
      if (var->data.patch) {
         assert(slot >= VARYING_SLOT_PATCH0);
         slot -= VARYING_SLOT_PATCH0;
      }
      if (slot_map[slot] == (unsigned char)-1) {
         if (stage != MESA_SHADER_TESS_CTRL && !is_texcoord(stage, var))
            /* dead io */
            return false;
         /* - texcoords can't be eliminated in fs due to GL_COORD_REPLACE
          * - patch variables may be read in the workgroup
          */
         slot_map[slot] = (*reserved)++;
      }
      var->data.driver_location = slot_map[slot];
   }
   return true;
}


static bool
rewrite_and_discard_read(nir_builder *b, nir_instr *instr, void *data)
{
   nir_variable *var = data;
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   if (intr->intrinsic != nir_intrinsic_load_deref)
      return false;
   nir_variable *deref_var = nir_intrinsic_get_var(intr, 0);
   if (deref_var != var)
      return false;
   nir_ssa_def *undef = nir_ssa_undef(b, nir_dest_num_components(intr->dest), nir_dest_bit_size(intr->dest));
   nir_ssa_def_rewrite_uses(&intr->dest.ssa, undef);
   return true;
}

void
zink_compiler_assign_io(nir_shader *producer, nir_shader *consumer)
{
   unsigned reserved = 0;
   unsigned char slot_map[VARYING_SLOT_MAX];
   memset(slot_map, -1, sizeof(slot_map));
   bool do_fixup = false;
   nir_shader *nir = producer->info.stage == MESA_SHADER_TESS_CTRL ? producer : consumer;
   if (consumer->info.stage != MESA_SHADER_FRAGMENT) {
      /* remove injected pointsize from all but the last vertex stage */
      nir_variable *var = nir_find_variable_with_location(producer, nir_var_shader_out, VARYING_SLOT_PSIZ);
      if (var && !var->data.explicit_location) {
         var->data.mode = nir_var_shader_temp;
         nir_fixup_deref_modes(producer);
         NIR_PASS_V(producer, nir_remove_dead_variables, nir_var_shader_temp, NULL);
         optimize_nir(producer);
      }
   }
   if (producer->info.stage == MESA_SHADER_TESS_CTRL) {
      /* never assign from tcs -> tes, always invert */
      nir_foreach_variable_with_modes(var, consumer, nir_var_shader_in)
         assign_producer_var_io(consumer->info.stage, var, &reserved, slot_map);
      nir_foreach_variable_with_modes_safe(var, producer, nir_var_shader_out) {
         if (!assign_consumer_var_io(producer->info.stage, var, &reserved, slot_map))
            /* this is an output, nothing more needs to be done for it to be dropped */
            do_fixup = true;
      }
   } else {
      nir_foreach_variable_with_modes(var, producer, nir_var_shader_out)
         assign_producer_var_io(producer->info.stage, var, &reserved, slot_map);
      nir_foreach_variable_with_modes_safe(var, consumer, nir_var_shader_in) {
         if (!assign_consumer_var_io(consumer->info.stage, var, &reserved, slot_map)) {
            do_fixup = true;
            /* input needs to be rewritten as an undef to ensure the entire deref chain is deleted */
            nir_shader_instructions_pass(consumer, rewrite_and_discard_read, nir_metadata_dominance, var);
         }
      }
   }
   if (!do_fixup)
      return;
   nir_fixup_deref_modes(nir);
   NIR_PASS_V(nir, nir_remove_dead_variables, nir_var_shader_temp, NULL);
   optimize_nir(nir);
}

static void
zink_shader_dump(void *words, size_t size, const char *file)
{
   FILE *fp = fopen(file, "wb");
   if (fp) {
      fwrite(words, 1, size, fp);
      fclose(fp);
      fprintf(stderr, "wrote '%s'...\n", file);
   }
}

VkShaderModule
zink_shader_spirv_compile(struct zink_screen *screen, struct zink_shader *zs, struct spirv_shader *spirv)
{
   VkShaderModule mod;
   VkShaderModuleCreateInfo smci = {0};

   if (!spirv)
      spirv = zs->spirv;

   if (zink_debug & ZINK_DEBUG_SPIRV) {
      char buf[256];
      static int i;
      snprintf(buf, sizeof(buf), "dump%02d.spv", i++);
      zink_shader_dump(spirv->words, spirv->num_words * sizeof(uint32_t), buf);
   }

   smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
   smci.codeSize = spirv->num_words * sizeof(uint32_t);
   smci.pCode = spirv->words;

#ifndef NDEBUG
   static const struct spirv_to_nir_options spirv_options = {
      .environment = NIR_SPIRV_VULKAN,
      .caps = {
         .float64 = true,
         .int16 = true,
         .int64 = true,
         .tessellation = true,
         .float_controls = true,
         .image_ms_array = true,
         .image_read_without_format = true,
         .image_write_without_format = true,
         .storage_image_ms = true,
         .geometry_streams = true,
         .storage_8bit = true,
         .storage_16bit = true,
         .variable_pointers = true,
         .stencil_export = true,
         .post_depth_coverage = true,
         .transform_feedback = true,
         .device_group = true,
         .draw_parameters = true,
         .shader_viewport_index_layer = true,
         .multiview = true,
         .physical_storage_buffer_address = true,
         .int64_atomics = true,
         .subgroup_arithmetic = true,
         .subgroup_basic = true,
         .subgroup_ballot = true,
         .subgroup_quad = true,
         .subgroup_shuffle = true,
         .subgroup_vote = true,
         .vk_memory_model = true,
         .vk_memory_model_device_scope = true,
         .int8 = true,
         .float16 = true,
         .demote_to_helper_invocation = true,
         .sparse_residency = true,
         .min_lod = true,
      },
      .ubo_addr_format = nir_address_format_32bit_index_offset,
      .ssbo_addr_format = nir_address_format_32bit_index_offset,
      .phys_ssbo_addr_format = nir_address_format_64bit_global,
      .push_const_addr_format = nir_address_format_logical,
      .shared_addr_format = nir_address_format_32bit_offset,
   };
   uint32_t num_spec_entries = 0;
   struct nir_spirv_specialization *spec_entries = NULL;
   VkSpecializationInfo sinfo = {0};
   VkSpecializationMapEntry me[3];
   uint32_t size[3] = {1,1,1};
   if (!zs->nir->info.workgroup_size[0]) {
      sinfo.mapEntryCount = 3;
      sinfo.pMapEntries = &me[0];
      sinfo.dataSize = sizeof(uint32_t) * 3;
      sinfo.pData = size;
      uint32_t ids[] = {ZINK_WORKGROUP_SIZE_X, ZINK_WORKGROUP_SIZE_Y, ZINK_WORKGROUP_SIZE_Z};
      for (int i = 0; i < 3; i++) {
         me[i].size = sizeof(uint32_t);
         me[i].constantID = ids[i];
         me[i].offset = i * sizeof(uint32_t);
      }
      spec_entries = vk_spec_info_to_nir_spirv(&sinfo, &num_spec_entries);
   }
   nir_shader *nir = spirv_to_nir(spirv->words, spirv->num_words,
                      spec_entries, num_spec_entries,
                      zs->nir->info.stage, "main", &spirv_options, &screen->nir_options);
   assert(nir);
   ralloc_free(nir);
   free(spec_entries);
#endif

   VkResult ret = VKSCR(CreateShaderModule)(screen->dev, &smci, NULL, &mod);
   bool success = zink_screen_handle_vkresult(screen, ret);
   assert(success);
   return success ? mod : VK_NULL_HANDLE;
}

static bool
find_var_deref(nir_shader *nir, nir_variable *var)
{
   nir_foreach_function(function, nir) {
      if (!function->impl)
         continue;

      nir_foreach_block(block, function->impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_deref)
               continue;
            nir_deref_instr *deref = nir_instr_as_deref(instr);
            if (deref->deref_type == nir_deref_type_var && deref->var == var)
               return true;
         }
      }
   }
   return false;
}

static void
prune_io(nir_shader *nir)
{
   nir_foreach_shader_in_variable_safe(var, nir) {
      if (!find_var_deref(nir, var))
         var->data.mode = nir_var_shader_temp;
   }
   nir_foreach_shader_out_variable_safe(var, nir) {
      if (!find_var_deref(nir, var))
         var->data.mode = nir_var_shader_temp;
   }
}

VkShaderModule
zink_shader_compile(struct zink_screen *screen, struct zink_shader *zs, nir_shader *base_nir, const struct zink_shader_key *key)
{
   VkShaderModule mod = VK_NULL_HANDLE;
   struct zink_shader_info *sinfo = &zs->sinfo;
   nir_shader *nir = nir_shader_clone(NULL, base_nir);
   bool need_optimize = false;
   bool inlined_uniforms = false;

   if (key) {
      if (key->inline_uniforms) {
         NIR_PASS_V(nir, nir_inline_uniforms,
                    nir->info.num_inlinable_uniforms,
                    key->base.inlined_uniform_values,
                    nir->info.inlinable_uniform_dw_offsets);

         inlined_uniforms = true;
      }

      /* TODO: use a separate mem ctx here for ralloc */
      switch (zs->nir->info.stage) {
      case MESA_SHADER_VERTEX: {
         uint32_t decomposed_attrs = 0, decomposed_attrs_without_w = 0;
         const struct zink_vs_key *vs_key = zink_vs_key(key);
         switch (vs_key->size) {
         case 4:
            decomposed_attrs = vs_key->u32.decomposed_attrs;
            decomposed_attrs_without_w = vs_key->u32.decomposed_attrs_without_w;
            break;
         case 2:
            decomposed_attrs = vs_key->u16.decomposed_attrs;
            decomposed_attrs_without_w = vs_key->u16.decomposed_attrs_without_w;
            break;
         case 1:
            decomposed_attrs = vs_key->u8.decomposed_attrs;
            decomposed_attrs_without_w = vs_key->u8.decomposed_attrs_without_w;
            break;
         default: break;
         }
         if (decomposed_attrs || decomposed_attrs_without_w)
            NIR_PASS_V(nir, decompose_attribs, decomposed_attrs, decomposed_attrs_without_w);
         FALLTHROUGH;
      }
      case MESA_SHADER_TESS_EVAL:
      case MESA_SHADER_GEOMETRY:
         if (zink_vs_key_base(key)->last_vertex_stage) {
            if (zs->sinfo.have_xfb)
               sinfo->last_vertex = true;

            if (!zink_vs_key_base(key)->clip_halfz && screen->driver_workarounds.depth_clip_control_missing) {
               NIR_PASS_V(nir, nir_lower_clip_halfz);
            }
            if (zink_vs_key_base(key)->push_drawid) {
               NIR_PASS_V(nir, lower_drawid);
            }
         }
         break;
      case MESA_SHADER_FRAGMENT:
         if (!zink_fs_key(key)->samples &&
            nir->info.outputs_written & BITFIELD64_BIT(FRAG_RESULT_SAMPLE_MASK)) {
            /* VK will always use gl_SampleMask[] values even if sample count is 0,
            * so we need to skip this write here to mimic GL's behavior of ignoring it
            */
            nir_foreach_shader_out_variable(var, nir) {
               if (var->data.location == FRAG_RESULT_SAMPLE_MASK)
                  var->data.mode = nir_var_shader_temp;
            }
            nir_fixup_deref_modes(nir);
            NIR_PASS_V(nir, nir_remove_dead_variables, nir_var_shader_temp, NULL);
            need_optimize = true;
         }
         if (zink_fs_key(key)->force_dual_color_blend && nir->info.outputs_written & BITFIELD64_BIT(FRAG_RESULT_DATA1)) {
            NIR_PASS_V(nir, lower_dual_blend);
         }
         if (zink_fs_key(key)->coord_replace_bits) {
            NIR_PASS_V(nir, nir_lower_texcoord_replace, zink_fs_key(key)->coord_replace_bits,
                     false, zink_fs_key(key)->coord_replace_yinvert);
         }
         if (zink_fs_key(key)->force_persample_interp || zink_fs_key(key)->fbfetch_ms) {
            nir_foreach_shader_in_variable(var, nir)
               var->data.sample = true;
            nir->info.fs.uses_sample_qualifier = true;
            nir->info.fs.uses_sample_shading = true;
         }
         if (nir->info.fs.uses_fbfetch_output) {
            nir_variable *fbfetch = NULL;
            NIR_PASS_V(nir, lower_fbfetch, &fbfetch, zink_fs_key(key)->fbfetch_ms);
            /* old variable must be deleted to avoid spirv errors */
            fbfetch->data.mode = nir_var_shader_temp;
            nir_fixup_deref_modes(nir);
            NIR_PASS_V(nir, nir_remove_dead_variables, nir_var_shader_temp, NULL);
            need_optimize = true;
         }
         break;
      default: break;
      }
      if (key->base.nonseamless_cube_mask) {
         NIR_PASS_V(nir, zink_lower_cubemap_to_array, key->base.nonseamless_cube_mask);
         need_optimize = true;
      }
   }
   if (screen->driconf.inline_uniforms) {
      NIR_PASS_V(nir, nir_lower_io_to_scalar, nir_var_mem_ubo | nir_var_mem_ssbo | nir_var_mem_shared);
      NIR_PASS_V(nir, rewrite_bo_access, screen);
      NIR_PASS_V(nir, remove_bo_access, zs);
   }
   if (inlined_uniforms) {
      optimize_nir(nir);

      /* This must be done again. */
      NIR_PASS_V(nir, nir_io_add_const_offset_to_base, nir_var_shader_in |
                                                       nir_var_shader_out);
   } else if (need_optimize)
      optimize_nir(nir);
   prune_io(nir);

   NIR_PASS_V(nir, nir_convert_from_ssa, true);

   struct spirv_shader *spirv = nir_to_spirv(nir, sinfo, screen->spirv_version);
   if (spirv)
      mod = zink_shader_spirv_compile(screen, zs, spirv);

   ralloc_free(nir);

   /* TODO: determine if there's any reason to cache spirv output? */
   if (zs->is_generated)
      zs->spirv = spirv;
   else
      ralloc_free(spirv);
   return mod;
}

static bool
lower_baseinstance_instr(nir_builder *b, nir_instr *instr, void *data)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;
   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   if (intr->intrinsic != nir_intrinsic_load_instance_id)
      return false;
   b->cursor = nir_after_instr(instr);
   nir_ssa_def *def = nir_isub(b, &intr->dest.ssa, nir_load_base_instance(b));
   nir_ssa_def_rewrite_uses_after(&intr->dest.ssa, def, def->parent_instr);
   return true;
}

static bool
lower_baseinstance(nir_shader *shader)
{
   if (shader->info.stage != MESA_SHADER_VERTEX)
      return false;
   return nir_shader_instructions_pass(shader, lower_baseinstance_instr, nir_metadata_dominance, NULL);
}

/* gl_nir_lower_buffers makes variables unusable for all UBO/SSBO access
 * so instead we delete all those broken variables and just make new ones
 */
static bool
unbreak_bos(nir_shader *shader, struct zink_shader *zs, bool needs_size)
{
   uint64_t max_ssbo_size = 0;
   uint64_t max_ubo_size = 0;

   if (!shader->info.num_ssbos && !shader->info.num_ubos)
      return false;

   nir_foreach_variable_with_modes(var, shader, nir_var_mem_ssbo | nir_var_mem_ubo) {
      const struct glsl_type *type = glsl_without_array(var->type);
      if (type_is_counter(type))
         continue;
      unsigned size = glsl_count_attribute_slots(glsl_type_is_array(var->type) ? var->type : type, false);
      if (var->data.mode == nir_var_mem_ubo)
         max_ubo_size = MAX2(max_ubo_size, size);
      else
         max_ssbo_size = MAX2(max_ssbo_size, size);
      var->data.mode = nir_var_shader_temp;
   }
   nir_fixup_deref_modes(shader);
   NIR_PASS_V(shader, nir_remove_dead_variables, nir_var_shader_temp, NULL);
   optimize_nir(shader);

   struct glsl_struct_field *fields = rzalloc_array(shader, struct glsl_struct_field, 2);
   fields[0].name = ralloc_strdup(shader, "base");
   fields[1].name = ralloc_strdup(shader, "unsized");
   if (shader->info.num_ubos) {
      const struct glsl_type *ubo_type = glsl_array_type(glsl_uint_type(), max_ubo_size * 4, 4);
      fields[0].type = ubo_type;
      if (shader->num_uniforms && zs->ubos_used & BITFIELD_BIT(0)) {
         nir_variable *var = nir_variable_create(shader, nir_var_mem_ubo,
                                                 glsl_array_type(glsl_struct_type(fields, 1, "struct", false), 1, 0),
                                                 "uniform_0");
         var->interface_type = var->type;
         var->data.mode = nir_var_mem_ubo;
         var->data.driver_location = 0;
      }

      unsigned num_ubos = shader->info.num_ubos - !!shader->info.first_ubo_is_default_ubo;
      uint32_t ubos_used = zs->ubos_used & ~BITFIELD_BIT(0);
      if (num_ubos && ubos_used) {
         /* shrink array as much as possible */
         unsigned first_ubo = ffs(ubos_used) - 2;
         assert(first_ubo < PIPE_MAX_CONSTANT_BUFFERS);
         num_ubos -= first_ubo;
         assert(num_ubos);
         nir_variable *var = nir_variable_create(shader, nir_var_mem_ubo,
                                   glsl_array_type(glsl_struct_type(fields, 1, "struct", false), num_ubos, 0),
                                   "ubos");
         var->interface_type = var->type;
         var->data.mode = nir_var_mem_ubo;
         var->data.driver_location = first_ubo + !!shader->info.first_ubo_is_default_ubo;
      }
   }
   if (shader->info.num_ssbos && zs->ssbos_used) {
      /* shrink array as much as possible */
      unsigned first_ssbo = ffs(zs->ssbos_used) - 1;
      assert(first_ssbo < PIPE_MAX_SHADER_BUFFERS);
      unsigned num_ssbos = shader->info.num_ssbos - first_ssbo;
      assert(num_ssbos);
      const struct glsl_type *ssbo_type = glsl_array_type(glsl_uint_type(), max_ssbo_size * 4, 4);
      const struct glsl_type *unsized = glsl_array_type(glsl_uint_type(), 0, 4);
      fields[0].type = ssbo_type;
      fields[1].type = max_ssbo_size ? unsized : NULL;
      unsigned field_count = max_ssbo_size && needs_size ? 2 : 1;
      nir_variable *var = nir_variable_create(shader, nir_var_mem_ssbo,
                                              glsl_array_type(glsl_struct_type(fields, field_count, "struct", false), num_ssbos, 0),
                                              "ssbos");
      var->interface_type = var->type;
      var->data.mode = nir_var_mem_ssbo;
      var->data.driver_location = first_ssbo;
   }
   return true;
}

static uint32_t
get_src_mask(unsigned total, nir_src src)
{
   if (nir_src_is_const(src))
      return BITFIELD_BIT(nir_src_as_uint(src));
   return BITFIELD_MASK(total);
}

static bool
analyze_io(struct zink_shader *zs, nir_shader *shader)
{
   bool ret = false;
   nir_function_impl *impl = nir_shader_get_entrypoint(shader);
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
 
         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
         switch (intrin->intrinsic) {
         case nir_intrinsic_store_ssbo:
            zs->ssbos_used |= get_src_mask(shader->info.num_ssbos, intrin->src[1]);
            break;
 
         case nir_intrinsic_get_ssbo_size: {
            zs->ssbos_used |= get_src_mask(shader->info.num_ssbos, intrin->src[0]);
            ret = true;
            break;
         }
         case nir_intrinsic_ssbo_atomic_add:
         case nir_intrinsic_ssbo_atomic_imin:
         case nir_intrinsic_ssbo_atomic_umin:
         case nir_intrinsic_ssbo_atomic_imax:
         case nir_intrinsic_ssbo_atomic_umax:
         case nir_intrinsic_ssbo_atomic_and:
         case nir_intrinsic_ssbo_atomic_or:
         case nir_intrinsic_ssbo_atomic_xor:
         case nir_intrinsic_ssbo_atomic_exchange:
         case nir_intrinsic_ssbo_atomic_comp_swap:
         case nir_intrinsic_ssbo_atomic_fmin:
         case nir_intrinsic_ssbo_atomic_fmax:
         case nir_intrinsic_ssbo_atomic_fcomp_swap:
         case nir_intrinsic_load_ssbo:
            zs->ssbos_used |= get_src_mask(shader->info.num_ssbos, intrin->src[0]);
            break;
         case nir_intrinsic_load_ubo:
         case nir_intrinsic_load_ubo_vec4:
            zs->ubos_used |= get_src_mask(shader->info.num_ubos, intrin->src[0]);
            break;
         default:
            break;
         }
      }
   }
   return ret;
}

/* this is a "default" bindless texture used if the shader has no texture variables */
static nir_variable *
create_bindless_texture(nir_shader *nir, nir_tex_instr *tex)
{
   unsigned binding = tex->sampler_dim == GLSL_SAMPLER_DIM_BUF ? 1 : 0;
   nir_variable *var;

   const struct glsl_type *sampler_type = glsl_sampler_type(tex->sampler_dim, tex->is_shadow, tex->is_array, GLSL_TYPE_FLOAT);
   var = nir_variable_create(nir, nir_var_uniform, glsl_array_type(sampler_type, ZINK_MAX_BINDLESS_HANDLES, 0), "bindless_texture");
   var->data.descriptor_set = ZINK_DESCRIPTOR_BINDLESS;
   var->data.driver_location = var->data.binding = binding;
   return var;
}

/* this is a "default" bindless image used if the shader has no image variables */
static nir_variable *
create_bindless_image(nir_shader *nir, enum glsl_sampler_dim dim)
{
   unsigned binding = dim == GLSL_SAMPLER_DIM_BUF ? 3 : 2;
   nir_variable *var;

   const struct glsl_type *image_type = glsl_image_type(dim, false, GLSL_TYPE_FLOAT);
   var = nir_variable_create(nir, nir_var_image, glsl_array_type(image_type, ZINK_MAX_BINDLESS_HANDLES, 0), "bindless_image");
   var->data.descriptor_set = ZINK_DESCRIPTOR_BINDLESS;
   var->data.driver_location = var->data.binding = binding;
   var->data.image.format = PIPE_FORMAT_R8G8B8A8_UNORM;
   return var;
}

/* rewrite bindless instructions as array deref instructions */
static bool
lower_bindless_instr(nir_builder *b, nir_instr *in, void *data)
{
   nir_variable **bindless = data;

   if (in->type == nir_instr_type_tex) {
      nir_tex_instr *tex = nir_instr_as_tex(in);
      int idx = nir_tex_instr_src_index(tex, nir_tex_src_texture_handle);
      if (idx == -1)
         return false;

      nir_variable *var = tex->sampler_dim == GLSL_SAMPLER_DIM_BUF ? bindless[1] : bindless[0];
      if (!var)
         var = create_bindless_texture(b->shader, tex);
      b->cursor = nir_before_instr(in);
      nir_deref_instr *deref = nir_build_deref_var(b, var);
      if (glsl_type_is_array(var->type))
         deref = nir_build_deref_array(b, deref, nir_u2uN(b, tex->src[idx].src.ssa, 32));
      nir_instr_rewrite_src_ssa(in, &tex->src[idx].src, &deref->dest.ssa);

      /* bindless sampling uses the variable type directly, which means the tex instr has to exactly
       * match up with it in contrast to normal sampler ops where things are a bit more flexible;
       * this results in cases where a shader is passed with sampler2DArray but the tex instr only has
       * 2 components, which explodes spirv compilation even though it doesn't trigger validation errors
       *
       * to fix this, pad the coord src here and fix the tex instr so that ntv will do the "right" thing
       * - Warhammer 40k: Dawn of War III
       */
      unsigned needed_components = glsl_get_sampler_coordinate_components(glsl_without_array(var->type));
      unsigned c = nir_tex_instr_src_index(tex, nir_tex_src_coord);
      unsigned coord_components = nir_src_num_components(tex->src[c].src);
      if (coord_components < needed_components) {
         nir_ssa_def *def = nir_pad_vector(b, tex->src[c].src.ssa, needed_components);
         nir_instr_rewrite_src_ssa(in, &tex->src[c].src, def);
         tex->coord_components = needed_components;
      }
      return true;
   }
   if (in->type != nir_instr_type_intrinsic)
      return false;
   nir_intrinsic_instr *instr = nir_instr_as_intrinsic(in);

   nir_intrinsic_op op;
#define OP_SWAP(OP) \
   case nir_intrinsic_bindless_image_##OP: \
      op = nir_intrinsic_image_deref_##OP; \
      break;


   /* convert bindless intrinsics to deref intrinsics */
   switch (instr->intrinsic) {
   OP_SWAP(atomic_add)
   OP_SWAP(atomic_and)
   OP_SWAP(atomic_comp_swap)
   OP_SWAP(atomic_dec_wrap)
   OP_SWAP(atomic_exchange)
   OP_SWAP(atomic_fadd)
   OP_SWAP(atomic_fmax)
   OP_SWAP(atomic_fmin)
   OP_SWAP(atomic_imax)
   OP_SWAP(atomic_imin)
   OP_SWAP(atomic_inc_wrap)
   OP_SWAP(atomic_or)
   OP_SWAP(atomic_umax)
   OP_SWAP(atomic_umin)
   OP_SWAP(atomic_xor)
   OP_SWAP(format)
   OP_SWAP(load)
   OP_SWAP(order)
   OP_SWAP(samples)
   OP_SWAP(size)
   OP_SWAP(store)
   default:
      return false;
   }

   enum glsl_sampler_dim dim = nir_intrinsic_image_dim(instr);
   nir_variable *var = dim == GLSL_SAMPLER_DIM_BUF ? bindless[3] : bindless[2];
   if (!var)
      var = create_bindless_image(b->shader, dim);
   instr->intrinsic = op;
   b->cursor = nir_before_instr(in);
   nir_deref_instr *deref = nir_build_deref_var(b, var);
   if (glsl_type_is_array(var->type))
      deref = nir_build_deref_array(b, deref, nir_u2uN(b, instr->src[0].ssa, 32));
   nir_instr_rewrite_src_ssa(in, &instr->src[0], &deref->dest.ssa);
   return true;
}

static bool
lower_bindless(nir_shader *shader, nir_variable **bindless)
{
   if (!nir_shader_instructions_pass(shader, lower_bindless_instr, nir_metadata_dominance, bindless))
      return false;
   nir_fixup_deref_modes(shader);
   NIR_PASS_V(shader, nir_remove_dead_variables, nir_var_shader_temp, NULL);
   optimize_nir(shader);
   return true;
}

/* convert shader image/texture io variables to int64 handles for bindless indexing */
static bool
lower_bindless_io_instr(nir_builder *b, nir_instr *in, void *data)
{
   if (in->type != nir_instr_type_intrinsic)
      return false;
   nir_intrinsic_instr *instr = nir_instr_as_intrinsic(in);
   if (instr->intrinsic != nir_intrinsic_load_deref &&
       instr->intrinsic != nir_intrinsic_store_deref)
      return false;

   nir_deref_instr *src_deref = nir_src_as_deref(instr->src[0]);
   nir_variable *var = nir_deref_instr_get_variable(src_deref);
   if (var->data.bindless)
      return false;
   if (var->data.mode != nir_var_shader_in && var->data.mode != nir_var_shader_out)
      return false;
   if (!glsl_type_is_image(var->type) && !glsl_type_is_sampler(var->type))
      return false;

   var->type = glsl_int64_t_type();
   var->data.bindless = 1;
   b->cursor = nir_before_instr(in);
   nir_deref_instr *deref = nir_build_deref_var(b, var);
   if (instr->intrinsic == nir_intrinsic_load_deref) {
       nir_ssa_def *def = nir_load_deref(b, deref);
       nir_instr_rewrite_src_ssa(in, &instr->src[0], def);
       nir_ssa_def_rewrite_uses(&instr->dest.ssa, def);
   } else {
      nir_store_deref(b, deref, instr->src[1].ssa, nir_intrinsic_write_mask(instr));
   }
   nir_instr_remove(in);
   nir_instr_remove(&src_deref->instr);
   return true;
}

static bool
lower_bindless_io(nir_shader *shader)
{
   return nir_shader_instructions_pass(shader, lower_bindless_io_instr, nir_metadata_dominance, NULL);
}

static uint32_t
zink_binding(gl_shader_stage stage, VkDescriptorType type, int index)
{
   if (stage == MESA_SHADER_NONE) {
      unreachable("not supported");
   } else {
      switch (type) {
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
         return stage * 2 + !!index;

      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
         assert(index < PIPE_MAX_SAMPLERS);
         return (stage * PIPE_MAX_SAMPLERS) + index;

      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
         return stage;

      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
         assert(index < ZINK_MAX_SHADER_IMAGES);
         return (stage * ZINK_MAX_SHADER_IMAGES) + index;

      default:
         unreachable("unexpected type");
      }
   }
}

static void
handle_bindless_var(nir_shader *nir, nir_variable *var, const struct glsl_type *type, nir_variable **bindless)
{
   if (glsl_type_is_struct(type)) {
      for (unsigned i = 0; i < glsl_get_length(type); i++)
         handle_bindless_var(nir, var, glsl_get_struct_field(type, i), bindless);
      return;
   }

   /* just a random scalar in a struct */
   if (!glsl_type_is_image(type) && !glsl_type_is_sampler(type))
      return;

   VkDescriptorType vktype = glsl_type_is_image(type) ? zink_image_type(type) : zink_sampler_type(type);
   unsigned binding;
   switch (vktype) {
      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
         binding = 0;
         break;
      case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
         binding = 1;
         break;
      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
         binding = 2;
         break;
      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
         binding = 3;
         break;
      default:
         unreachable("unknown");
   }
   if (!bindless[binding]) {
      bindless[binding] = nir_variable_clone(var, nir);
      bindless[binding]->data.bindless = 0;
      bindless[binding]->data.descriptor_set = ZINK_DESCRIPTOR_BINDLESS;
      bindless[binding]->type = glsl_array_type(type, ZINK_MAX_BINDLESS_HANDLES, 0);
      bindless[binding]->data.driver_location = bindless[binding]->data.binding = binding;
      if (!bindless[binding]->data.image.format)
         bindless[binding]->data.image.format = PIPE_FORMAT_R8G8B8A8_UNORM;
      nir_shader_add_variable(nir, bindless[binding]);
   } else {
      assert(glsl_get_sampler_dim(glsl_without_array(bindless[binding]->type)) == glsl_get_sampler_dim(glsl_without_array(var->type)));
   }
   var->data.mode = nir_var_shader_temp;
}

static enum pipe_prim_type
prim_to_pipe(enum shader_prim primitive_type)
{
   switch (primitive_type) {
   case SHADER_PRIM_POINTS:
      return PIPE_PRIM_POINTS;
   case SHADER_PRIM_LINES:
   case SHADER_PRIM_LINE_LOOP:
   case SHADER_PRIM_LINE_STRIP:
   case SHADER_PRIM_LINES_ADJACENCY:
   case SHADER_PRIM_LINE_STRIP_ADJACENCY:
      return PIPE_PRIM_LINES;
   default:
      return PIPE_PRIM_TRIANGLES;
   }
}

static enum pipe_prim_type
tess_prim_to_pipe(enum tess_primitive_mode prim_mode)
{
   switch (prim_mode) {
   case TESS_PRIMITIVE_ISOLINES:
      return PIPE_PRIM_LINES;
   default:
      return PIPE_PRIM_TRIANGLES;
   }
}

static enum pipe_prim_type
get_shader_base_prim_type(struct nir_shader *nir)
{
   switch (nir->info.stage) {
   case MESA_SHADER_GEOMETRY:
      return prim_to_pipe(nir->info.gs.output_primitive);
   case MESA_SHADER_TESS_EVAL:
      return nir->info.tess.point_mode ? PIPE_PRIM_POINTS : tess_prim_to_pipe(nir->info.tess._primitive_mode);
   default:
      break;
   }
   return PIPE_PRIM_MAX;
}

static bool
convert_1d_shadow_tex(nir_builder *b, nir_instr *instr, void *data)
{
   struct zink_screen *screen = data;
   if (instr->type != nir_instr_type_tex)
      return false;
   nir_tex_instr *tex = nir_instr_as_tex(instr);
   if (tex->sampler_dim != GLSL_SAMPLER_DIM_1D || !tex->is_shadow)
      return false;
   if (tex->is_sparse && screen->need_2D_sparse) {
      /* no known case of this exists: only nvidia can hit it, and nothing uses it */
      mesa_loge("unhandled/unsupported 1D sparse texture!");
      abort();
   }
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   b->cursor = nir_before_instr(instr);
   tex->coord_components++;
   unsigned srcs[] = {
      nir_tex_src_coord,
      nir_tex_src_offset,
      nir_tex_src_ddx,
      nir_tex_src_ddy,
   };
   for (unsigned i = 0; i < ARRAY_SIZE(srcs); i++) {
      unsigned c = nir_tex_instr_src_index(tex, srcs[i]);
      if (c == -1)
         continue;
      if (tex->src[c].src.ssa->num_components == tex->coord_components)
         continue;
      nir_ssa_def *def;
      nir_ssa_def *zero = nir_imm_zero(b, 1, tex->src[c].src.ssa->bit_size);
      if (tex->src[c].src.ssa->num_components == 1)
         def = nir_vec2(b, tex->src[c].src.ssa, zero);
      else
         def = nir_vec3(b, nir_channel(b, tex->src[c].src.ssa, 0), zero, nir_channel(b, tex->src[c].src.ssa, 1));
      nir_instr_rewrite_src_ssa(instr, &tex->src[c].src, def);
   }
   b->cursor = nir_after_instr(instr);
   unsigned needed_components = nir_tex_instr_dest_size(tex);
   unsigned num_components = tex->dest.ssa.num_components;
   if (needed_components > num_components) {
      tex->dest.ssa.num_components = needed_components;
      assert(num_components < 3);
      /* take either xz or just x since this is promoted to 2D from 1D */
      uint32_t mask = num_components == 2 ? (1|4) : 1;
      nir_ssa_def *dst = nir_channels(b, &tex->dest.ssa, mask);
      nir_ssa_def_rewrite_uses_after(&tex->dest.ssa, dst, dst->parent_instr);
   }
   return true;
}

static bool
lower_1d_shadow(nir_shader *shader, struct zink_screen *screen)
{
   bool found = false;
   nir_foreach_variable_with_modes(var, shader, nir_var_uniform | nir_var_image) {
      const struct glsl_type *type = glsl_without_array(var->type);
      unsigned length = glsl_get_length(var->type);
      if (!glsl_type_is_sampler(type) || !glsl_sampler_type_is_shadow(type) || glsl_get_sampler_dim(type) != GLSL_SAMPLER_DIM_1D)
         continue;
      const struct glsl_type *sampler = glsl_sampler_type(GLSL_SAMPLER_DIM_2D, true, glsl_sampler_type_is_array(type), glsl_get_sampler_result_type(type));
      var->type = type != var->type ? glsl_array_type(sampler, length, glsl_get_explicit_stride(var->type)) : sampler;

      found = true;
   }
   if (found)
      nir_shader_instructions_pass(shader, convert_1d_shadow_tex, nir_metadata_dominance, screen);
   return found;
}

static void
scan_nir(struct zink_screen *screen, nir_shader *shader, struct zink_shader *zs)
{
   nir_foreach_function(function, shader) {
      if (!function->impl)
         continue;
      nir_foreach_block_safe(block, function->impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type == nir_instr_type_tex) {
               nir_tex_instr *tex = nir_instr_as_tex(instr);
               zs->sinfo.have_sparse |= tex->is_sparse;
            }
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic == nir_intrinsic_image_deref_load ||
                intr->intrinsic == nir_intrinsic_image_deref_sparse_load ||
                intr->intrinsic == nir_intrinsic_image_deref_store ||
                intr->intrinsic == nir_intrinsic_image_deref_atomic_add ||
                intr->intrinsic == nir_intrinsic_image_deref_atomic_imin ||
                intr->intrinsic == nir_intrinsic_image_deref_atomic_umin ||
                intr->intrinsic == nir_intrinsic_image_deref_atomic_imax ||
                intr->intrinsic == nir_intrinsic_image_deref_atomic_umax ||
                intr->intrinsic == nir_intrinsic_image_deref_atomic_and ||
                intr->intrinsic == nir_intrinsic_image_deref_atomic_or ||
                intr->intrinsic == nir_intrinsic_image_deref_atomic_xor ||
                intr->intrinsic == nir_intrinsic_image_deref_atomic_exchange ||
                intr->intrinsic == nir_intrinsic_image_deref_atomic_comp_swap ||
                intr->intrinsic == nir_intrinsic_image_deref_atomic_fadd ||
                intr->intrinsic == nir_intrinsic_image_deref_size ||
                intr->intrinsic == nir_intrinsic_image_deref_samples ||
                intr->intrinsic == nir_intrinsic_image_deref_format ||
                intr->intrinsic == nir_intrinsic_image_deref_order) {

                nir_variable *var =
                   nir_deref_instr_get_variable(nir_src_as_deref(intr->src[0]));

                /* Structs have been lowered already, so get_aoa_size is sufficient. */
                const unsigned size =
                   glsl_type_is_array(var->type) ? glsl_get_aoa_size(var->type) : 1;
                BITSET_SET_RANGE(shader->info.images_used, var->data.binding,
                                 var->data.binding + (MAX2(size, 1) - 1));
            }
            if (intr->intrinsic == nir_intrinsic_is_sparse_texels_resident ||
                intr->intrinsic == nir_intrinsic_image_deref_sparse_load)
               zs->sinfo.have_sparse = true;

            static bool warned = false;
            if (!screen->info.have_EXT_shader_atomic_float && !screen->is_cpu && !warned) {
               switch (intr->intrinsic) {
               case nir_intrinsic_image_deref_atomic_add:
               case nir_intrinsic_image_deref_atomic_exchange: {
                  nir_variable *var = nir_intrinsic_get_var(intr, 0);
                  if (util_format_is_float(var->data.image.format))
                     fprintf(stderr, "zink: Vulkan driver missing VK_EXT_shader_atomic_float but attempting to do atomic ops!\n");
                  break;
               }
               default:
                  break;
               }
            }
         }
      }
   }
}

static bool
lower_sparse_instr(nir_builder *b, nir_instr *in, void *data)
{
   if (in->type != nir_instr_type_intrinsic)
      return false;
   nir_intrinsic_instr *instr = nir_instr_as_intrinsic(in);
   if (instr->intrinsic != nir_intrinsic_is_sparse_texels_resident)
      return false;

   /* vulkan vec can only be a vec4, but this is (maybe) vec5,
    * so just rewrite as the first component since ntv is going to use a different
    * method for storing the residency value anyway
    */
   b->cursor = nir_before_instr(&instr->instr);
   nir_instr *parent = instr->src[0].ssa->parent_instr;
   assert(parent->type == nir_instr_type_alu);
   nir_alu_instr *alu = nir_instr_as_alu(parent);
   nir_ssa_def_rewrite_uses_after(instr->src[0].ssa, nir_channel(b, alu->src[0].src.ssa, 0), parent);
   nir_instr_remove(parent);
   return true;
}

static bool
lower_sparse(nir_shader *shader)
{
   return nir_shader_instructions_pass(shader, lower_sparse_instr, nir_metadata_dominance, NULL);
}

struct zink_shader *
zink_shader_create(struct zink_screen *screen, struct nir_shader *nir,
                   const struct pipe_stream_output_info *so_info)
{
   struct zink_shader *ret = CALLOC_STRUCT(zink_shader);
   bool have_psiz = false;

   ret->sinfo.have_vulkan_memory_model = screen->info.have_KHR_vulkan_memory_model;

   ret->hash = _mesa_hash_pointer(ret);
   ret->reduced_prim = get_shader_base_prim_type(nir);

   ret->programs = _mesa_pointer_set_create(NULL);
   simple_mtx_init(&ret->lock, mtx_plain);

   nir_variable_mode indirect_derefs_modes = nir_var_function_temp;
   if (nir->info.stage == MESA_SHADER_TESS_CTRL ||
       nir->info.stage == MESA_SHADER_TESS_EVAL)
      indirect_derefs_modes |= nir_var_shader_in | nir_var_shader_out;

   NIR_PASS_V(nir, nir_lower_indirect_derefs, indirect_derefs_modes,
              UINT32_MAX);

   if (nir->info.stage == MESA_SHADER_VERTEX)
      create_vs_pushconst(nir);
   else if (nir->info.stage == MESA_SHADER_TESS_CTRL ||
            nir->info.stage == MESA_SHADER_TESS_EVAL)
      NIR_PASS_V(nir, nir_lower_io_arrays_to_elements_no_indirects, false);
   else if (nir->info.stage == MESA_SHADER_KERNEL)
      create_cs_pushconst(nir);

   if (nir->info.stage < MESA_SHADER_FRAGMENT)
      have_psiz = check_psiz(nir);
   NIR_PASS_V(nir, lower_basevertex);
   NIR_PASS_V(nir, lower_work_dim);
   NIR_PASS_V(nir, nir_lower_regs_to_ssa);
   NIR_PASS_V(nir, lower_baseinstance);
   NIR_PASS_V(nir, lower_sparse);

   if (screen->need_2D_zs)
      NIR_PASS_V(nir, lower_1d_shadow, screen);

   {
      nir_lower_subgroups_options subgroup_options = {0};
      subgroup_options.lower_to_scalar = true;
      subgroup_options.subgroup_size = screen->info.props11.subgroupSize;
      subgroup_options.ballot_bit_size = 32;
      subgroup_options.ballot_components = 4;
      subgroup_options.lower_subgroup_masks = true;
      NIR_PASS_V(nir, nir_lower_subgroups, &subgroup_options);
   }

   optimize_nir(nir);
   NIR_PASS_V(nir, nir_remove_dead_variables, nir_var_function_temp, NULL);
   NIR_PASS_V(nir, nir_lower_discard_if);
   NIR_PASS_V(nir, nir_lower_fragcolor,
         nir->info.fs.color_is_dual_source ? 1 : 8);
   NIR_PASS_V(nir, lower_64bit_vertex_attribs);
   bool needs_size = analyze_io(ret, nir);
   NIR_PASS_V(nir, unbreak_bos, ret, needs_size);
   /* run in compile if there could be inlined uniforms */
   if (!screen->driconf.inline_uniforms) {
      NIR_PASS_V(nir, nir_lower_io_to_scalar, nir_var_mem_ubo | nir_var_mem_ssbo | nir_var_mem_shared);
      NIR_PASS_V(nir, rewrite_bo_access, screen);
      NIR_PASS_V(nir, remove_bo_access, ret);
   }

   if (zink_debug & ZINK_DEBUG_NIR) {
      fprintf(stderr, "NIR shader:\n---8<---\n");
      nir_print_shader(nir, stderr);
      fprintf(stderr, "---8<---\n");
   }

   nir_variable *bindless[4] = {0};
   bool has_bindless_io = false;
   nir_foreach_variable_with_modes(var, nir, nir_var_shader_in | nir_var_shader_out) {
      if (glsl_type_is_image(var->type) || glsl_type_is_sampler(var->type)) {
         has_bindless_io = true;
         break;
      }
   }
   if (has_bindless_io)
      NIR_PASS_V(nir, lower_bindless_io);

   optimize_nir(nir);
   prune_io(nir);

   scan_nir(screen, nir, ret);

   foreach_list_typed_reverse_safe(nir_variable, var, node, &nir->variables) {
      if (_nir_shader_variable_has_mode(var, nir_var_uniform |
                                        nir_var_image |
                                        nir_var_mem_ubo |
                                        nir_var_mem_ssbo)) {
         enum zink_descriptor_type ztype;
         const struct glsl_type *type = glsl_without_array(var->type);
         if (var->data.mode == nir_var_mem_ubo) {
            ztype = ZINK_DESCRIPTOR_TYPE_UBO;
            /* buffer 0 is a push descriptor */
            var->data.descriptor_set = !!var->data.driver_location;
            var->data.binding = !var->data.driver_location ? nir->info.stage :
                                zink_binding(nir->info.stage,
                                             VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                             var->data.driver_location);
            assert(var->data.driver_location || var->data.binding < 10);
            VkDescriptorType vktype = !var->data.driver_location ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            int binding = var->data.binding;

            ret->bindings[ztype][ret->num_bindings[ztype]].index = var->data.driver_location;
            ret->bindings[ztype][ret->num_bindings[ztype]].binding = binding;
            ret->bindings[ztype][ret->num_bindings[ztype]].type = vktype;
            ret->bindings[ztype][ret->num_bindings[ztype]].size = glsl_get_length(var->type);
            assert(ret->bindings[ztype][ret->num_bindings[ztype]].size);
            ret->num_bindings[ztype]++;
         } else if (var->data.mode == nir_var_mem_ssbo) {
            ztype = ZINK_DESCRIPTOR_TYPE_SSBO;
            var->data.descriptor_set = ztype + 1;
            var->data.binding = zink_binding(nir->info.stage,
                                             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                             var->data.driver_location);
            ret->bindings[ztype][ret->num_bindings[ztype]].index = var->data.driver_location;
            ret->bindings[ztype][ret->num_bindings[ztype]].binding = var->data.binding;
            ret->bindings[ztype][ret->num_bindings[ztype]].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            ret->bindings[ztype][ret->num_bindings[ztype]].size = glsl_get_length(var->type);
            assert(ret->bindings[ztype][ret->num_bindings[ztype]].size);
            ret->num_bindings[ztype]++;
         } else {
            assert(var->data.mode == nir_var_uniform ||
                   var->data.mode == nir_var_image);
            if (var->data.bindless) {
               ret->bindless = true;
               handle_bindless_var(nir, var, type, bindless);
            } else if (glsl_type_is_sampler(type) || glsl_type_is_image(type)) {
               VkDescriptorType vktype = glsl_type_is_image(type) ? zink_image_type(type) : zink_sampler_type(type);
               ztype = zink_desc_type_from_vktype(vktype);
               if (vktype == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER)
                  ret->num_texel_buffers++;
               var->data.driver_location = var->data.binding;
               var->data.descriptor_set = ztype + 1;
               var->data.binding = zink_binding(nir->info.stage, vktype, var->data.driver_location);
               ret->bindings[ztype][ret->num_bindings[ztype]].index = var->data.driver_location;
               ret->bindings[ztype][ret->num_bindings[ztype]].binding = var->data.binding;
               ret->bindings[ztype][ret->num_bindings[ztype]].type = vktype;
               if (glsl_type_is_array(var->type))
                  ret->bindings[ztype][ret->num_bindings[ztype]].size = glsl_get_aoa_size(var->type);
               else
                  ret->bindings[ztype][ret->num_bindings[ztype]].size = 1;
               ret->num_bindings[ztype]++;
            }
         }
      }
   }
   bool bindless_lowered = false;
   NIR_PASS(bindless_lowered, nir, lower_bindless, bindless);
   ret->bindless |= bindless_lowered;

   ret->nir = nir;
   if (so_info && nir->info.outputs_written && nir->info.has_transform_feedback_varyings)
      update_so_info(ret, so_info, nir->info.outputs_written, have_psiz);
   else if (have_psiz) {
      bool have_fake_psiz = false;
      nir_variable *psiz = NULL;
      nir_foreach_shader_out_variable(var, nir) {
         if (var->data.location == VARYING_SLOT_PSIZ) {
            if (!var->data.explicit_location)
               have_fake_psiz = true;
            else
               psiz = var;
         }
      }
      if (have_fake_psiz && psiz) {
         psiz->data.mode = nir_var_shader_temp;
         nir_fixup_deref_modes(nir);
         NIR_PASS_V(nir, nir_remove_dead_variables, nir_var_shader_temp, NULL);
      }
   }

   return ret;
}

char *
zink_shader_finalize(struct pipe_screen *pscreen, void *nirptr)
{
   struct zink_screen *screen = zink_screen(pscreen);
   nir_shader *nir = nirptr;

   nir_lower_tex_options tex_opts = {
      .lower_invalid_implicit_lod = true,
   };
   /*
      Sampled Image must be an object whose type is OpTypeSampledImage.
      The Dim operand of the underlying OpTypeImage must be 1D, 2D, 3D,
      or Rect, and the Arrayed and MS operands must be 0.
      - SPIRV, OpImageSampleProj* opcodes
    */
   tex_opts.lower_txp = BITFIELD_BIT(GLSL_SAMPLER_DIM_CUBE) |
                        BITFIELD_BIT(GLSL_SAMPLER_DIM_MS);
   tex_opts.lower_txp_array = true;
   if (!screen->info.feats.features.shaderImageGatherExtended)
      tex_opts.lower_tg4_offsets = true;
   NIR_PASS_V(nir, nir_lower_tex, &tex_opts);
   NIR_PASS_V(nir, nir_lower_uniforms_to_ubo, true, false);
   if (nir->info.stage == MESA_SHADER_GEOMETRY)
      NIR_PASS_V(nir, nir_lower_gs_intrinsics, nir_lower_gs_intrinsics_per_stream);
   optimize_nir(nir);
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
   if (screen->driconf.inline_uniforms)
      nir_find_inlinable_uniforms(nir);

   return NULL;
}

void
zink_shader_free(struct zink_context *ctx, struct zink_shader *shader)
{
   set_foreach(shader->programs, entry) {
      if (shader->nir->info.stage == MESA_SHADER_COMPUTE) {
         struct zink_compute_program *comp = (void*)entry->key;
         if (!comp->base.removed) {
            _mesa_hash_table_remove_key(&ctx->compute_program_cache, comp->shader);
            comp->base.removed = true;
         }
         comp->shader = NULL;
         zink_compute_program_reference(ctx, &comp, NULL);
      } else {
         struct zink_gfx_program *prog = (void*)entry->key;
         enum pipe_shader_type pstage = pipe_shader_type_from_mesa(shader->nir->info.stage);
         assert(pstage < ZINK_SHADER_COUNT);
         if (!prog->base.removed && (shader->nir->info.stage != MESA_SHADER_TESS_CTRL || !shader->is_generated)) {
            _mesa_hash_table_remove_key(&ctx->program_cache[prog->stages_present >> 2], prog->shaders);
            prog->base.removed = true;
         }
         prog->shaders[pstage] = NULL;
         if (shader->nir->info.stage == MESA_SHADER_TESS_EVAL && shader->generated)
            /* automatically destroy generated tcs shaders when tes is destroyed */
            zink_shader_free(ctx, shader->generated);
         zink_gfx_program_reference(ctx, &prog, NULL);
      }
   }
   _mesa_set_destroy(shader->programs, NULL);
   ralloc_free(shader->nir);
   ralloc_free(shader->spirv);
   FREE(shader);
}


VkShaderModule
zink_shader_tcs_compile(struct zink_screen *screen, struct zink_shader *zs, unsigned patch_vertices)
{
   assert(zs->nir->info.stage == MESA_SHADER_TESS_CTRL);
   /* shortcut all the nir passes since we just have to change this one word */
   zs->spirv->words[zs->spirv->tcs_vertices_out_word] = patch_vertices;
   return zink_shader_spirv_compile(screen, zs, NULL);
}

/* creating a passthrough tcs shader that's roughly:

#version 150
#extension GL_ARB_tessellation_shader : require

in vec4 some_var[gl_MaxPatchVertices];
out vec4 some_var_out;

layout(push_constant) uniform tcsPushConstants {
    layout(offset = 0) float TessLevelInner[2];
    layout(offset = 8) float TessLevelOuter[4];
} u_tcsPushConstants;
layout(vertices = $vertices_per_patch) out;
void main()
{
  gl_TessLevelInner = u_tcsPushConstants.TessLevelInner;
  gl_TessLevelOuter = u_tcsPushConstants.TessLevelOuter;
  some_var_out = some_var[gl_InvocationID];
}

*/
struct zink_shader *
zink_shader_tcs_create(struct zink_screen *screen, struct zink_shader *vs, unsigned vertices_per_patch)
{
   struct zink_shader *ret = CALLOC_STRUCT(zink_shader);
   ret->hash = _mesa_hash_pointer(ret);
   ret->programs = _mesa_pointer_set_create(NULL);
   simple_mtx_init(&ret->lock, mtx_plain);

   nir_shader *nir = nir_shader_create(NULL, MESA_SHADER_TESS_CTRL, &screen->nir_options, NULL);
   nir_function *fn = nir_function_create(nir, "main");
   fn->is_entrypoint = true;
   nir_function_impl *impl = nir_function_impl_create(fn);

   nir_builder b;
   nir_builder_init(&b, impl);
   b.cursor = nir_before_block(nir_start_block(impl));

   nir_ssa_def *invocation_id = nir_load_invocation_id(&b);

   nir_foreach_shader_out_variable(var, vs->nir) {
      const struct glsl_type *type = var->type;
      const struct glsl_type *in_type = var->type;
      const struct glsl_type *out_type = var->type;
      char buf[1024];
      snprintf(buf, sizeof(buf), "%s_out", var->name);
      in_type = glsl_array_type(type, 32 /* MAX_PATCH_VERTICES */, 0);
      out_type = glsl_array_type(type, vertices_per_patch, 0);

      nir_variable *in = nir_variable_create(nir, nir_var_shader_in, in_type, var->name);
      nir_variable *out = nir_variable_create(nir, nir_var_shader_out, out_type, buf);
      out->data.location = in->data.location = var->data.location;
      out->data.location_frac = in->data.location_frac = var->data.location_frac;

      /* gl_in[] receives values from equivalent built-in output
         variables written by the vertex shader (section 2.14.7).  Each array
         element of gl_in[] is a structure holding values for a specific vertex of
         the input patch.  The length of gl_in[] is equal to the
         implementation-dependent maximum patch size (gl_MaxPatchVertices).
         - ARB_tessellation_shader
       */
      /* we need to load the invocation-specific value of the vertex output and then store it to the per-patch output */
      nir_deref_instr *in_array_var = nir_build_deref_array(&b, nir_build_deref_var(&b, in), invocation_id);
      nir_ssa_def *load = nir_load_deref(&b, in_array_var);
      nir_deref_instr *out_array_var = nir_build_deref_array(&b, nir_build_deref_var(&b, out), invocation_id);
      nir_store_deref(&b, out_array_var, load, 0xff);
   }
   nir_variable *gl_TessLevelInner = nir_variable_create(nir, nir_var_shader_out, glsl_array_type(glsl_float_type(), 2, 0), "gl_TessLevelInner");
   gl_TessLevelInner->data.location = VARYING_SLOT_TESS_LEVEL_INNER;
   gl_TessLevelInner->data.patch = 1;
   nir_variable *gl_TessLevelOuter = nir_variable_create(nir, nir_var_shader_out, glsl_array_type(glsl_float_type(), 4, 0), "gl_TessLevelOuter");
   gl_TessLevelOuter->data.location = VARYING_SLOT_TESS_LEVEL_OUTER;
   gl_TessLevelOuter->data.patch = 1;

   /* hacks so we can size these right for now */
   struct glsl_struct_field *fields = rzalloc_array(nir, struct glsl_struct_field, 3);
   /* just use a single blob for padding here because it's easier */
   fields[0].type = glsl_array_type(glsl_uint_type(), offsetof(struct zink_gfx_push_constant, default_inner_level) / 4, 0);
   fields[0].name = ralloc_asprintf(nir, "padding");
   fields[0].offset = 0;
   fields[1].type = glsl_array_type(glsl_uint_type(), 2, 0);
   fields[1].name = ralloc_asprintf(nir, "gl_TessLevelInner");
   fields[1].offset = offsetof(struct zink_gfx_push_constant, default_inner_level);
   fields[2].type = glsl_array_type(glsl_uint_type(), 4, 0);
   fields[2].name = ralloc_asprintf(nir, "gl_TessLevelOuter");
   fields[2].offset = offsetof(struct zink_gfx_push_constant, default_outer_level);
   nir_variable *pushconst = nir_variable_create(nir, nir_var_mem_push_const,
                                                 glsl_struct_type(fields, 3, "struct", false), "pushconst");
   pushconst->data.location = VARYING_SLOT_VAR0;

   nir_ssa_def *load_inner = nir_load_push_constant(&b, 2, 32, nir_imm_int(&b, 1), .base = 1, .range = 8);
   nir_ssa_def *load_outer = nir_load_push_constant(&b, 4, 32, nir_imm_int(&b, 2), .base = 2, .range = 16);

   for (unsigned i = 0; i < 2; i++) {
      nir_deref_instr *store_idx = nir_build_deref_array_imm(&b, nir_build_deref_var(&b, gl_TessLevelInner), i);
      nir_store_deref(&b, store_idx, nir_channel(&b, load_inner, i), 0xff);
   }
   for (unsigned i = 0; i < 4; i++) {
      nir_deref_instr *store_idx = nir_build_deref_array_imm(&b, nir_build_deref_var(&b, gl_TessLevelOuter), i);
      nir_store_deref(&b, store_idx, nir_channel(&b, load_outer, i), 0xff);
   }

   nir->info.tess.tcs_vertices_out = vertices_per_patch;
   nir_validate_shader(nir, "created");

   NIR_PASS_V(nir, nir_lower_regs_to_ssa);
   optimize_nir(nir);
   NIR_PASS_V(nir, nir_remove_dead_variables, nir_var_function_temp, NULL);
   NIR_PASS_V(nir, nir_convert_from_ssa, true);

   ret->nir = nir;
   ret->is_generated = true;
   return ret;
}
