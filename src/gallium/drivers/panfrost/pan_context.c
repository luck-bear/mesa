/*
 * Copyright (C) 2019-2020 Collabora, Ltd.
 * © Copyright 2018 Alyssa Rosenzweig
 * Copyright © 2014-2017 Broadcom
 * Copyright (C) 2017 Intel Corporation
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
 */

#include <sys/poll.h>
#include <errno.h>

#include "pan_bo.h"
#include "pan_context.h"
#include "pan_minmax_cache.h"

#include "util/macros.h"
#include "util/format/u_format.h"
#include "util/u_inlines.h"
#include "util/u_upload_mgr.h"
#include "util/u_memory.h"
#include "util/u_vbuf.h"
#include "util/half_float.h"
#include "util/u_helpers.h"
#include "util/format/u_format.h"
#include "util/u_prim.h"
#include "util/u_prim_restart.h"
#include "tgsi/tgsi_parse.h"
#include "tgsi/tgsi_from_mesa.h"
#include "nir/tgsi_to_nir.h"
#include "util/u_math.h"

#include "pan_screen.h"
#include "pan_util.h"
#include "decode.h"
#include "util/pan_lower_framebuffer.h"

static void
panfrost_clear(
        struct pipe_context *pipe,
        unsigned buffers,
        const struct pipe_scissor_state *scissor_state,
        const union pipe_color_union *color,
        double depth, unsigned stencil)
{
        struct panfrost_context *ctx = pan_context(pipe);

        if (!panfrost_render_condition_check(ctx))
                return;

        /* TODO: panfrost_get_fresh_batch_for_fbo() instantiates a new batch if
         * the existing batch targeting this FBO has draws. We could probably
         * avoid that by replacing plain clears by quad-draws with a specific
         * color/depth/stencil value, thus avoiding the generation of extra
         * fragment jobs.
         */
        struct panfrost_batch *batch = panfrost_get_fresh_batch_for_fbo(ctx, "Slow clear");
        panfrost_batch_clear(batch, buffers, color, depth, stencil);
}

bool
panfrost_writes_point_size(struct panfrost_context *ctx)
{
        assert(ctx->shader[PIPE_SHADER_VERTEX]);
        struct panfrost_shader_state *vs = panfrost_get_shader_state(ctx, PIPE_SHADER_VERTEX);

        return vs->info.vs.writes_point_size && ctx->active_prim == PIPE_PRIM_POINTS;
}

/* The entire frame is in memory -- send it off to the kernel! */

void
panfrost_flush(
        struct pipe_context *pipe,
        struct pipe_fence_handle **fence,
        unsigned flags)
{
        struct panfrost_context *ctx = pan_context(pipe);
        struct panfrost_device *dev = pan_device(pipe->screen);


        /* Submit all pending jobs */
        panfrost_flush_all_batches(ctx, NULL);

        if (fence) {
                struct pipe_fence_handle *f = panfrost_fence_create(ctx);
                pipe->screen->fence_reference(pipe->screen, fence, NULL);
                *fence = f;
        }

        if (dev->debug & PAN_DBG_TRACE)
                pandecode_next_frame();
}

static void
panfrost_texture_barrier(struct pipe_context *pipe, unsigned flags)
{
        struct panfrost_context *ctx = pan_context(pipe);
        panfrost_flush_all_batches(ctx, "Texture barrier");
}

static void
panfrost_set_frontend_noop(struct pipe_context *pipe, bool enable)
{
        struct panfrost_context *ctx = pan_context(pipe);
        panfrost_flush_all_batches(ctx, "Frontend no-op change");
        ctx->is_noop = enable;
}


static void
panfrost_generic_cso_delete(struct pipe_context *pctx, void *hwcso)
{
        free(hwcso);
}

static void
panfrost_bind_blend_state(struct pipe_context *pipe, void *cso)
{
        struct panfrost_context *ctx = pan_context(pipe);
        ctx->blend = cso;
        ctx->dirty |= PAN_DIRTY_BLEND;
}

static void
panfrost_set_blend_color(struct pipe_context *pipe,
                         const struct pipe_blend_color *blend_color)
{
        struct panfrost_context *ctx = pan_context(pipe);
        ctx->dirty |= PAN_DIRTY_BLEND;

        if (blend_color)
                ctx->blend_color = *blend_color;
}

/* Create a final blend given the context */

mali_ptr
panfrost_get_blend(struct panfrost_batch *batch, unsigned rti, struct panfrost_bo **bo, unsigned *shader_offset)
{
        struct panfrost_context *ctx = batch->ctx;
        struct panfrost_device *dev = pan_device(ctx->base.screen);
        struct panfrost_blend_state *blend = ctx->blend;
        struct pan_blend_info info = blend->info[rti];
        struct pipe_surface *surf = batch->key.cbufs[rti];
        enum pipe_format fmt = surf->format;

        /* Use fixed-function if the equation permits, the format is blendable,
         * and no more than one unique constant is accessed */
        if (info.fixed_function && panfrost_blendable_formats_v7[fmt].internal &&
                        pan_blend_is_homogenous_constant(info.constant_mask,
                                ctx->blend_color.color)) {
                return 0;
        }

        /* Otherwise, we need to grab a shader */
        struct pan_blend_state pan_blend = blend->pan;
        unsigned nr_samples = surf->nr_samples ? : surf->texture->nr_samples;

        pan_blend.rts[rti].format = fmt;
        pan_blend.rts[rti].nr_samples = nr_samples;
        memcpy(pan_blend.constants, ctx->blend_color.color,
               sizeof(pan_blend.constants));

        /* Upload the shader, sharing a BO */
        if (!(*bo)) {
                *bo = panfrost_batch_create_bo(batch, 4096, PAN_BO_EXECUTE,
                                PIPE_SHADER_FRAGMENT, "Blend shader");
        }

        struct panfrost_shader_state *ss = panfrost_get_shader_state(ctx, PIPE_SHADER_FRAGMENT);

        /* Default for Midgard */
        nir_alu_type col0_type = nir_type_float32;
        nir_alu_type col1_type = nir_type_float32;

        /* Bifrost has per-output types, respect them */
        if (dev->arch >= 6) {
                col0_type = ss->info.bifrost.blend[rti].type;
                col1_type = ss->info.bifrost.blend_src1_type;
        }

        pthread_mutex_lock(&dev->blend_shaders.lock);
        struct pan_blend_shader_variant *shader =
                pan_screen(ctx->base.screen)->vtbl.get_blend_shader(dev,
                                                                    &pan_blend,
                                                                    col0_type,
                                                                    col1_type,
                                                                    rti);

        /* Size check and upload */
        unsigned offset = *shader_offset;
        assert((offset + shader->binary.size) < 4096);
        memcpy((*bo)->ptr.cpu + offset, shader->binary.data, shader->binary.size);
        *shader_offset += shader->binary.size;
        pthread_mutex_unlock(&dev->blend_shaders.lock);

        return ((*bo)->ptr.gpu + offset) | shader->first_tag;
}

static void
panfrost_bind_rasterizer_state(
        struct pipe_context *pctx,
        void *hwcso)
{
        struct panfrost_context *ctx = pan_context(pctx);
        ctx->rasterizer = hwcso;

        /* We can assume rasterizer is always dirty, the dependencies are
         * too intricate to bother tracking in detail. However we could
         * probably diff the renderers for viewport dirty tracking, that
         * just cares about the scissor enable and the depth clips. */
        ctx->dirty |= PAN_DIRTY_SCISSOR | PAN_DIRTY_RASTERIZER;
}

static void
panfrost_set_shader_images(
        struct pipe_context *pctx,
        enum pipe_shader_type shader,
        unsigned start_slot, unsigned count, unsigned unbind_num_trailing_slots,
        const struct pipe_image_view *iviews)
{
        struct panfrost_context *ctx = pan_context(pctx);
        ctx->dirty_shader[PIPE_SHADER_FRAGMENT] |= PAN_DIRTY_STAGE_IMAGE;

        /* Unbind start_slot...start_slot+count */
        if (!iviews) {
                for (int i = start_slot; i < start_slot + count + unbind_num_trailing_slots; i++) {
                        pipe_resource_reference(&ctx->images[shader][i].resource, NULL);
                }

                ctx->image_mask[shader] &= ~(((1ull << count) - 1) << start_slot);
                return;
        }

        /* Bind start_slot...start_slot+count */
        for (int i = 0; i < count; i++) {
                const struct pipe_image_view *image = &iviews[i];
                SET_BIT(ctx->image_mask[shader], 1 << (start_slot + i), image->resource);

                if (!image->resource) {
                        util_copy_image_view(&ctx->images[shader][start_slot+i], NULL);
                        continue;
                }

                struct panfrost_resource *rsrc = pan_resource(image->resource);

                /* Images don't work with AFBC, since they require pixel-level granularity */
                if (drm_is_afbc(rsrc->image.layout.modifier)) {
                        pan_resource_modifier_convert(ctx, rsrc,
                                        DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED,
                                        "Shader image");
                }

                util_copy_image_view(&ctx->images[shader][start_slot+i], image);
        }

        /* Unbind start_slot+count...start_slot+count+unbind_num_trailing_slots */
        for (int i = 0; i < unbind_num_trailing_slots; i++) {
                SET_BIT(ctx->image_mask[shader], 1 << (start_slot + count + i), NULL);
                util_copy_image_view(&ctx->images[shader][start_slot+count+i], NULL);
        }
}

static void
panfrost_bind_vertex_elements_state(
        struct pipe_context *pctx,
        void *hwcso)
{
        struct panfrost_context *ctx = pan_context(pctx);
        ctx->vertex = hwcso;
        ctx->dirty |= PAN_DIRTY_VERTEX;
}

static void *
panfrost_create_shader_state(
        struct pipe_context *pctx,
        const struct pipe_shader_state *cso)
{
        struct panfrost_shader_variants *so = CALLOC_STRUCT(panfrost_shader_variants);
        struct panfrost_device *dev = pan_device(pctx->screen);

        simple_mtx_init(&so->lock, mtx_plain);

        so->stream_output = cso->stream_output;

        if (cso->type == PIPE_SHADER_IR_TGSI)
                so->nir = tgsi_to_nir(cso->tokens, pctx->screen, false);
        else
                so->nir = cso->ir.nir;

        /* Fix linkage early */
        if (so->nir->info.stage == MESA_SHADER_VERTEX) {
                so->fixed_varying_mask =
                        (so->nir->info.outputs_written & BITFIELD_MASK(VARYING_SLOT_VAR0)) &
                        ~VARYING_BIT_POS & ~VARYING_BIT_PSIZ;
        }

        /* Precompile for shader-db if we need to */
        if (unlikely(dev->debug & PAN_DBG_PRECOMPILE)) {
                struct panfrost_context *ctx = pan_context(pctx);

                struct panfrost_shader_state state = { 0 };

                panfrost_shader_compile(pctx->screen,
                                        &ctx->shaders, &ctx->descs,
                                        so->nir, &state);
        }

        return so;
}

static void
panfrost_delete_shader_state(
        struct pipe_context *pctx,
        void *so)
{
        struct panfrost_shader_variants *cso = (struct panfrost_shader_variants *) so;

        ralloc_free(cso->nir);

        for (unsigned i = 0; i < cso->variant_count; ++i) {
                struct panfrost_shader_state *shader_state = &cso->variants[i];
                panfrost_bo_unreference(shader_state->bin.bo);
                panfrost_bo_unreference(shader_state->state.bo);
                panfrost_bo_unreference(shader_state->linkage.bo);
        }

        simple_mtx_destroy(&cso->lock);

        free(cso->variants);
        free(so);
}

static void
panfrost_bind_sampler_states(
        struct pipe_context *pctx,
        enum pipe_shader_type shader,
        unsigned start_slot, unsigned num_sampler,
        void **sampler)
{
        assert(start_slot == 0);

        struct panfrost_context *ctx = pan_context(pctx);
        ctx->dirty_shader[shader] |= PAN_DIRTY_STAGE_SAMPLER;

        ctx->sampler_count[shader] = sampler ? num_sampler : 0;
        if (sampler)
                memcpy(ctx->samplers[shader], sampler, num_sampler * sizeof (void *));
}

static void
panfrost_build_key(struct panfrost_context *ctx,
                   struct panfrost_shader_key *key,
                   nir_shader *nir)
{
        /* We don't currently have vertex shader variants */
        if (nir->info.stage != MESA_SHADER_FRAGMENT)
               return;

        struct panfrost_device *dev = pan_device(ctx->base.screen);
        struct pipe_framebuffer_state *fb = &ctx->pipe_framebuffer;
        struct pipe_rasterizer_state *rast = (void *) ctx->rasterizer;
        struct panfrost_shader_variants *vs = ctx->shader[MESA_SHADER_VERTEX];

        key->fs.nr_cbufs = fb->nr_cbufs;

        /* Point sprite lowering needed on Bifrost and newer */
        if (dev->arch >= 6 && rast && ctx->active_prim == PIPE_PRIM_POINTS) {
                key->fs.sprite_coord_enable = rast->sprite_coord_enable;
        }

        /* User clip plane lowering needed everywhere */
        if (rast) {
                key->fs.clip_plane_enable = rast->clip_plane_enable;
        }

        if (dev->arch <= 5) {
                u_foreach_bit(i, (nir->info.outputs_read >> FRAG_RESULT_DATA0)) {
                        enum pipe_format fmt = PIPE_FORMAT_R8G8B8A8_UNORM;

                        if ((fb->nr_cbufs > i) && fb->cbufs[i])
                                fmt = fb->cbufs[i]->format;

                        if (panfrost_blendable_formats_v6[fmt].internal)
                                fmt = PIPE_FORMAT_NONE;

                        key->fs.rt_formats[i] = fmt;
                }
        }

        /* Funny desktop GL varying lowering on Valhall */
        if (dev->arch >= 9) {
                assert(vs != NULL && "too early");
                key->fixed_varying_mask = vs->fixed_varying_mask;
        }
}

/**
 * Fix an uncompiled shader's stream output info, and produce a bitmask
 * of which VARYING_SLOT_* are captured for stream output.
 *
 * Core Gallium stores output->register_index as a "slot" number, where
 * slots are assigned consecutively to all outputs in info->outputs_written.
 * This naive packing of outputs doesn't work for us - we too have slots,
 * but the layout is defined by the VUE map, which we won't have until we
 * compile a specific shader variant.  So, we remap these and simply store
 * VARYING_SLOT_* in our copy's output->register_index fields.
 *
 * We then produce a bitmask of outputs which are used for SO.
 *
 * Implementation from iris.
 */

static uint64_t
update_so_info(struct pipe_stream_output_info *so_info,
               uint64_t outputs_written)
{
	uint64_t so_outputs = 0;
	uint8_t reverse_map[64] = {0};
	unsigned slot = 0;

	while (outputs_written)
		reverse_map[slot++] = u_bit_scan64(&outputs_written);

	for (unsigned i = 0; i < so_info->num_outputs; i++) {
		struct pipe_stream_output *output = &so_info->output[i];

		/* Map Gallium's condensed "slots" back to real VARYING_SLOT_* enums */
		output->register_index = reverse_map[output->register_index];

		so_outputs |= 1ull << output->register_index;
	}

	return so_outputs;
}

static unsigned
panfrost_new_variant_locked(
        struct panfrost_context *ctx,
        struct panfrost_shader_variants *variants,
        struct panfrost_shader_key *key)
{
        unsigned variant = variants->variant_count++;

        if (variants->variant_count > variants->variant_space) {
                unsigned old_space = variants->variant_space;

                variants->variant_space *= 2;
                if (variants->variant_space == 0)
                        variants->variant_space = 1;

                /* Arbitrary limit to stop runaway programs from
                 * creating an unbounded number of shader variants. */
                assert(variants->variant_space < 1024);

                unsigned msize = sizeof(struct panfrost_shader_state);
                variants->variants = realloc(variants->variants,
                                             variants->variant_space * msize);

                memset(&variants->variants[old_space], 0,
                       (variants->variant_space - old_space) * msize);
        }

        variants->variants[variant].key = *key;

        struct panfrost_shader_state *shader_state = &variants->variants[variant];

        /* We finally have a variant, so compile it */
        panfrost_shader_compile(ctx->base.screen,
                                &ctx->shaders, &ctx->descs,
                                variants->nir, shader_state);

        /* Fixup the stream out information */
        shader_state->stream_output = variants->stream_output;
        shader_state->so_mask =
                update_so_info(&shader_state->stream_output,
                               shader_state->info.outputs_written);

        return variant;
}

static void
panfrost_bind_shader_state(
        struct pipe_context *pctx,
        void *hwcso,
        enum pipe_shader_type type)
{
        struct panfrost_context *ctx = pan_context(pctx);
        ctx->shader[type] = hwcso;

        ctx->dirty |= PAN_DIRTY_TLS_SIZE;
        ctx->dirty_shader[type] |= PAN_DIRTY_STAGE_SHADER;

        if (hwcso)
                panfrost_update_shader_variant(ctx, type);
}

void
panfrost_update_shader_variant(struct panfrost_context *ctx,
                               enum pipe_shader_type type)
{
        /* No shader variants for compute */
        if (type == PIPE_SHADER_COMPUTE)
                return;

        /* We need linking information, defer this */
        if (type == PIPE_SHADER_FRAGMENT && !ctx->shader[PIPE_SHADER_VERTEX])
                return;

        /* Match the appropriate variant */
        signed variant = -1;
        struct panfrost_shader_variants *variants = ctx->shader[type];

        simple_mtx_lock(&variants->lock);

        struct panfrost_shader_key key = {
                .fixed_varying_mask = variants->fixed_varying_mask
        };

        panfrost_build_key(ctx, &key, variants->nir);

        for (unsigned i = 0; i < variants->variant_count; ++i) {
                if (memcmp(&key, &variants->variants[i].key, sizeof(key)) == 0) {
                        variant = i;
                        break;
                }
        }

        if (variant == -1)
                variant = panfrost_new_variant_locked(ctx, variants, &key);

        variants->active_variant = variant;

        /* TODO: it would be more efficient to release the lock before
         * compiling instead of after, but that can race if thread A compiles a
         * variant while thread B searches for that same variant */
        simple_mtx_unlock(&variants->lock);
}

static void
panfrost_bind_vs_state(struct pipe_context *pctx, void *hwcso)
{
        panfrost_bind_shader_state(pctx, hwcso, PIPE_SHADER_VERTEX);

        /* Fragment shaders are linked with vertex shaders */
        struct panfrost_context *ctx = pan_context(pctx);
        panfrost_update_shader_variant(ctx, PIPE_SHADER_FRAGMENT);
}

static void
panfrost_bind_fs_state(struct pipe_context *pctx, void *hwcso)
{
        panfrost_bind_shader_state(pctx, hwcso, PIPE_SHADER_FRAGMENT);
}

static void
panfrost_set_vertex_buffers(
        struct pipe_context *pctx,
        unsigned start_slot,
        unsigned num_buffers,
        unsigned unbind_num_trailing_slots,
        bool take_ownership,
        const struct pipe_vertex_buffer *buffers)
{
        struct panfrost_context *ctx = pan_context(pctx);

        util_set_vertex_buffers_mask(ctx->vertex_buffers, &ctx->vb_mask, buffers,
                                     start_slot, num_buffers, unbind_num_trailing_slots,
                                     take_ownership);

        ctx->dirty |= PAN_DIRTY_VERTEX;
}

static void
panfrost_set_constant_buffer(
        struct pipe_context *pctx,
        enum pipe_shader_type shader, uint index, bool take_ownership,
        const struct pipe_constant_buffer *buf)
{
        struct panfrost_context *ctx = pan_context(pctx);
        struct panfrost_constant_buffer *pbuf = &ctx->constant_buffer[shader];

        util_copy_constant_buffer(&pbuf->cb[index], buf, take_ownership);

        unsigned mask = (1 << index);

        if (unlikely(!buf)) {
                pbuf->enabled_mask &= ~mask;
                return;
        }

        pbuf->enabled_mask |= mask;
        ctx->dirty_shader[shader] |= PAN_DIRTY_STAGE_CONST;
}

static void
panfrost_set_stencil_ref(
        struct pipe_context *pctx,
        const struct pipe_stencil_ref ref)
{
        struct panfrost_context *ctx = pan_context(pctx);
        ctx->stencil_ref = ref;
        ctx->dirty |= PAN_DIRTY_ZS;
}

static void
panfrost_set_sampler_views(
        struct pipe_context *pctx,
        enum pipe_shader_type shader,
        unsigned start_slot, unsigned num_views,
        unsigned unbind_num_trailing_slots,
        bool take_ownership,
        struct pipe_sampler_view **views)
{
        struct panfrost_context *ctx = pan_context(pctx);
        ctx->dirty_shader[shader] |= PAN_DIRTY_STAGE_TEXTURE;

        unsigned new_nr = 0;
        unsigned i;

        for (i = 0; i < num_views; ++i) {
                struct pipe_sampler_view *view = views ? views[i] : NULL;
                unsigned p = i + start_slot;

                if (view)
                        new_nr = p + 1;

                if (take_ownership) {
                        pipe_sampler_view_reference((struct pipe_sampler_view **)&ctx->sampler_views[shader][p],
                                                    NULL);
                        ctx->sampler_views[shader][i] = (struct panfrost_sampler_view *)view;
                } else {
                        pipe_sampler_view_reference((struct pipe_sampler_view **)&ctx->sampler_views[shader][p],
                                                    view);
                }
        }

        for (; i < num_views + unbind_num_trailing_slots; i++) {
                unsigned p = i + start_slot;
		pipe_sampler_view_reference((struct pipe_sampler_view **)&ctx->sampler_views[shader][p],
		                            NULL);
        }

        /* If the sampler view count is higher than the greatest sampler view
         * we touch, it can't change */
        if (ctx->sampler_view_count[shader] > start_slot + num_views + unbind_num_trailing_slots)
                return;

        /* If we haven't set any sampler views here, search lower numbers for
         * set sampler views */
        if (new_nr == 0) {
                for (i = 0; i < start_slot; ++i) {
                        if (ctx->sampler_views[shader][i])
                                new_nr = i + 1;
                }
        }

        ctx->sampler_view_count[shader] = new_nr;
}

static void
panfrost_set_shader_buffers(
        struct pipe_context *pctx,
        enum pipe_shader_type shader,
        unsigned start, unsigned count,
        const struct pipe_shader_buffer *buffers,
        unsigned writable_bitmask)
{
        struct panfrost_context *ctx = pan_context(pctx);

        util_set_shader_buffers_mask(ctx->ssbo[shader], &ctx->ssbo_mask[shader],
                        buffers, start, count);

        ctx->dirty_shader[shader] |= PAN_DIRTY_STAGE_SSBO;
}

static void
panfrost_set_framebuffer_state(struct pipe_context *pctx,
                               const struct pipe_framebuffer_state *fb)
{
        struct panfrost_context *ctx = pan_context(pctx);

        util_copy_framebuffer_state(&ctx->pipe_framebuffer, fb);
        ctx->batch = NULL;

        /* Hot draw call path needs the mask of active render targets */
        ctx->fb_rt_mask = 0;

        for (unsigned i = 0; i < ctx->pipe_framebuffer.nr_cbufs; ++i) {
                if (ctx->pipe_framebuffer.cbufs[i])
                        ctx->fb_rt_mask |= BITFIELD_BIT(i);
        }
}

static void
panfrost_bind_depth_stencil_state(struct pipe_context *pipe,
                                  void *cso)
{
        struct panfrost_context *ctx = pan_context(pipe);
        ctx->depth_stencil = cso;
        ctx->dirty |= PAN_DIRTY_ZS;
}

static void
panfrost_set_sample_mask(struct pipe_context *pipe,
                         unsigned sample_mask)
{
        struct panfrost_context *ctx = pan_context(pipe);
        ctx->sample_mask = sample_mask;
        ctx->dirty |= PAN_DIRTY_MSAA;
}

static void
panfrost_set_min_samples(struct pipe_context *pipe,
                         unsigned min_samples)
{
        struct panfrost_context *ctx = pan_context(pipe);
        ctx->min_samples = min_samples;
        ctx->dirty |= PAN_DIRTY_MSAA;
}

static void
panfrost_set_clip_state(struct pipe_context *pipe,
                        const struct pipe_clip_state *clip)
{
        //struct panfrost_context *panfrost = pan_context(pipe);
}

static void
panfrost_set_viewport_states(struct pipe_context *pipe,
                             unsigned start_slot,
                             unsigned num_viewports,
                             const struct pipe_viewport_state *viewports)
{
        struct panfrost_context *ctx = pan_context(pipe);

        assert(start_slot == 0);
        assert(num_viewports == 1);

        ctx->pipe_viewport = *viewports;
        ctx->dirty |= PAN_DIRTY_VIEWPORT;
}

static void
panfrost_set_scissor_states(struct pipe_context *pipe,
                            unsigned start_slot,
                            unsigned num_scissors,
                            const struct pipe_scissor_state *scissors)
{
        struct panfrost_context *ctx = pan_context(pipe);

        assert(start_slot == 0);
        assert(num_scissors == 1);

        ctx->scissor = *scissors;
        ctx->dirty |= PAN_DIRTY_SCISSOR;
}

static void
panfrost_set_polygon_stipple(struct pipe_context *pipe,
                             const struct pipe_poly_stipple *stipple)
{
        //struct panfrost_context *panfrost = pan_context(pipe);
}

static void
panfrost_set_active_query_state(struct pipe_context *pipe,
                                bool enable)
{
        struct panfrost_context *ctx = pan_context(pipe);
        ctx->active_queries = enable;
        ctx->dirty |= PAN_DIRTY_OQ;
}

static void
panfrost_render_condition(struct pipe_context *pipe,
                          struct pipe_query *query,
                          bool condition,
                          enum pipe_render_cond_flag mode)
{
        struct panfrost_context *ctx = pan_context(pipe);

        ctx->cond_query = (struct panfrost_query *)query;
        ctx->cond_cond = condition;
        ctx->cond_mode = mode;
}

static void
panfrost_destroy(struct pipe_context *pipe)
{
        struct panfrost_context *panfrost = pan_context(pipe);

        _mesa_hash_table_destroy(panfrost->writers, NULL);

        if (panfrost->blitter)
                util_blitter_destroy(panfrost->blitter);

        util_unreference_framebuffer_state(&panfrost->pipe_framebuffer);
        u_upload_destroy(pipe->stream_uploader);

        panfrost_pool_cleanup(&panfrost->descs);
        panfrost_pool_cleanup(&panfrost->shaders);

        ralloc_free(pipe);
}

static struct pipe_query *
panfrost_create_query(struct pipe_context *pipe,
                      unsigned type,
                      unsigned index)
{
        struct panfrost_query *q = rzalloc(pipe, struct panfrost_query);

        q->type = type;
        q->index = index;

        return (struct pipe_query *) q;
}

static void
panfrost_destroy_query(struct pipe_context *pipe, struct pipe_query *q)
{
        struct panfrost_query *query = (struct panfrost_query *) q;

        if (query->rsrc)
                pipe_resource_reference(&query->rsrc, NULL);

        ralloc_free(q);
}

static bool
panfrost_begin_query(struct pipe_context *pipe, struct pipe_query *q)
{
        struct panfrost_context *ctx = pan_context(pipe);
        struct panfrost_device *dev = pan_device(ctx->base.screen);
        struct panfrost_query *query = (struct panfrost_query *) q;

        switch (query->type) {
        case PIPE_QUERY_OCCLUSION_COUNTER:
        case PIPE_QUERY_OCCLUSION_PREDICATE:
        case PIPE_QUERY_OCCLUSION_PREDICATE_CONSERVATIVE: {
                unsigned size = sizeof(uint64_t) * dev->core_count;

                /* Allocate a resource for the query results to be stored */
                if (!query->rsrc) {
                        query->rsrc = pipe_buffer_create(ctx->base.screen,
                                        PIPE_BIND_QUERY_BUFFER, 0, size);
                }

                /* Default to 0 if nothing at all drawn. */
                uint8_t *zeroes = alloca(size);
                memset(zeroes, 0, size);
                pipe_buffer_write(pipe, query->rsrc, 0, size, zeroes);

                query->msaa = (ctx->pipe_framebuffer.samples > 1);
                ctx->occlusion_query = query;
                ctx->dirty |= PAN_DIRTY_OQ;
                break;
        }

        /* Geometry statistics are computed in the driver. XXX: geom/tess
         * shaders.. */

        case PIPE_QUERY_PRIMITIVES_GENERATED:
                query->start = ctx->prims_generated;
                break;
        case PIPE_QUERY_PRIMITIVES_EMITTED:
                query->start = ctx->tf_prims_generated;
                break;

        default:
                /* TODO: timestamp queries, etc? */
                break;
        }

        return true;
}

static bool
panfrost_end_query(struct pipe_context *pipe, struct pipe_query *q)
{
        struct panfrost_context *ctx = pan_context(pipe);
        struct panfrost_query *query = (struct panfrost_query *) q;

        switch (query->type) {
        case PIPE_QUERY_OCCLUSION_COUNTER:
        case PIPE_QUERY_OCCLUSION_PREDICATE:
        case PIPE_QUERY_OCCLUSION_PREDICATE_CONSERVATIVE:
                ctx->occlusion_query = NULL;
                ctx->dirty |= PAN_DIRTY_OQ;
                break;
        case PIPE_QUERY_PRIMITIVES_GENERATED:
                query->end = ctx->prims_generated;
                break;
        case PIPE_QUERY_PRIMITIVES_EMITTED:
                query->end = ctx->tf_prims_generated;
                break;
        }

        return true;
}

static bool
panfrost_get_query_result(struct pipe_context *pipe,
                          struct pipe_query *q,
                          bool wait,
                          union pipe_query_result *vresult)
{
        struct panfrost_query *query = (struct panfrost_query *) q;
        struct panfrost_context *ctx = pan_context(pipe);
        struct panfrost_device *dev = pan_device(ctx->base.screen);
        struct panfrost_resource *rsrc = pan_resource(query->rsrc);

        switch (query->type) {
        case PIPE_QUERY_OCCLUSION_COUNTER:
        case PIPE_QUERY_OCCLUSION_PREDICATE:
        case PIPE_QUERY_OCCLUSION_PREDICATE_CONSERVATIVE:
                panfrost_flush_writer(ctx, rsrc, "Occlusion query");
                panfrost_bo_wait(rsrc->image.data.bo, INT64_MAX, false);

                /* Read back the query results */
                uint64_t *result = (uint64_t *) rsrc->image.data.bo->ptr.cpu;

                if (query->type == PIPE_QUERY_OCCLUSION_COUNTER) {
                        uint64_t passed = 0;
                        for (int i = 0; i < dev->core_count; ++i)
                                passed += result[i];

                        if (dev->arch <= 5 && !query->msaa)
                                passed /= 4;

                        vresult->u64 = passed;
                } else {
                        vresult->b = !!result[0];
                }

                break;

        case PIPE_QUERY_PRIMITIVES_GENERATED:
        case PIPE_QUERY_PRIMITIVES_EMITTED:
                panfrost_flush_all_batches(ctx, "Primitive count query");
                vresult->u64 = query->end - query->start;
                break;

        default:
                /* TODO: more queries */
                break;
        }

        return true;
}

bool
panfrost_render_condition_check(struct panfrost_context *ctx)
{
	if (!ctx->cond_query)
		return true;

        perf_debug_ctx(ctx, "Implementing conditional rendering on the CPU");

	union pipe_query_result res = { 0 };
	bool wait =
		ctx->cond_mode != PIPE_RENDER_COND_NO_WAIT &&
		ctx->cond_mode != PIPE_RENDER_COND_BY_REGION_NO_WAIT;

        struct pipe_query *pq = (struct pipe_query *)ctx->cond_query;

        if (panfrost_get_query_result(&ctx->base, pq, wait, &res))
                return res.u64 != ctx->cond_cond;

	return true;
}

static struct pipe_stream_output_target *
panfrost_create_stream_output_target(struct pipe_context *pctx,
                                     struct pipe_resource *prsc,
                                     unsigned buffer_offset,
                                     unsigned buffer_size)
{
        struct pipe_stream_output_target *target;

        target = &rzalloc(pctx, struct panfrost_streamout_target)->base;

        if (!target)
                return NULL;

        pipe_reference_init(&target->reference, 1);
        pipe_resource_reference(&target->buffer, prsc);

        target->context = pctx;
        target->buffer_offset = buffer_offset;
        target->buffer_size = buffer_size;

        return target;
}

static void
panfrost_stream_output_target_destroy(struct pipe_context *pctx,
                                      struct pipe_stream_output_target *target)
{
        pipe_resource_reference(&target->buffer, NULL);
        ralloc_free(target);
}

static void
panfrost_set_stream_output_targets(struct pipe_context *pctx,
                                   unsigned num_targets,
                                   struct pipe_stream_output_target **targets,
                                   const unsigned *offsets)
{
        struct panfrost_context *ctx = pan_context(pctx);
        struct panfrost_streamout *so = &ctx->streamout;

        assert(num_targets <= ARRAY_SIZE(so->targets));

        for (unsigned i = 0; i < num_targets; i++) {
                if (offsets[i] != -1)
                        pan_so_target(targets[i])->offset = offsets[i];

                pipe_so_target_reference(&so->targets[i], targets[i]);
        }

        for (unsigned i = 0; i < so->num_targets; i++)
                pipe_so_target_reference(&so->targets[i], NULL);

        so->num_targets = num_targets;
}

struct pipe_context *
panfrost_create_context(struct pipe_screen *screen, void *priv, unsigned flags)
{
        struct panfrost_context *ctx = rzalloc(screen, struct panfrost_context);
        struct pipe_context *gallium = (struct pipe_context *) ctx;
        struct panfrost_device *dev = pan_device(screen);

        gallium->screen = screen;

        gallium->destroy = panfrost_destroy;

        gallium->set_framebuffer_state = panfrost_set_framebuffer_state;

        gallium->flush = panfrost_flush;
        gallium->clear = panfrost_clear;
        gallium->texture_barrier = panfrost_texture_barrier;
        gallium->set_frontend_noop = panfrost_set_frontend_noop;

        gallium->set_vertex_buffers = panfrost_set_vertex_buffers;
        gallium->set_constant_buffer = panfrost_set_constant_buffer;
        gallium->set_shader_buffers = panfrost_set_shader_buffers;
        gallium->set_shader_images = panfrost_set_shader_images;

        gallium->set_stencil_ref = panfrost_set_stencil_ref;

        gallium->set_sampler_views = panfrost_set_sampler_views;

        gallium->bind_rasterizer_state = panfrost_bind_rasterizer_state;
        gallium->delete_rasterizer_state = panfrost_generic_cso_delete;

        gallium->bind_vertex_elements_state = panfrost_bind_vertex_elements_state;
        gallium->delete_vertex_elements_state = panfrost_generic_cso_delete;

        gallium->create_fs_state = panfrost_create_shader_state;
        gallium->delete_fs_state = panfrost_delete_shader_state;
        gallium->bind_fs_state = panfrost_bind_fs_state;

        gallium->create_vs_state = panfrost_create_shader_state;
        gallium->delete_vs_state = panfrost_delete_shader_state;
        gallium->bind_vs_state = panfrost_bind_vs_state;

        gallium->delete_sampler_state = panfrost_generic_cso_delete;
        gallium->bind_sampler_states = panfrost_bind_sampler_states;

        gallium->bind_depth_stencil_alpha_state   = panfrost_bind_depth_stencil_state;
        gallium->delete_depth_stencil_alpha_state = panfrost_generic_cso_delete;

        gallium->set_sample_mask = panfrost_set_sample_mask;
        gallium->set_min_samples = panfrost_set_min_samples;

        gallium->set_clip_state = panfrost_set_clip_state;
        gallium->set_viewport_states = panfrost_set_viewport_states;
        gallium->set_scissor_states = panfrost_set_scissor_states;
        gallium->set_polygon_stipple = panfrost_set_polygon_stipple;
        gallium->set_active_query_state = panfrost_set_active_query_state;
        gallium->render_condition = panfrost_render_condition;

        gallium->create_query = panfrost_create_query;
        gallium->destroy_query = panfrost_destroy_query;
        gallium->begin_query = panfrost_begin_query;
        gallium->end_query = panfrost_end_query;
        gallium->get_query_result = panfrost_get_query_result;

        gallium->create_stream_output_target = panfrost_create_stream_output_target;
        gallium->stream_output_target_destroy = panfrost_stream_output_target_destroy;
        gallium->set_stream_output_targets = panfrost_set_stream_output_targets;

        gallium->bind_blend_state   = panfrost_bind_blend_state;
        gallium->delete_blend_state = panfrost_generic_cso_delete;

        gallium->set_blend_color = panfrost_set_blend_color;

        pan_screen(screen)->vtbl.context_init(gallium);

        panfrost_resource_context_init(gallium);
        panfrost_compute_context_init(gallium);

        gallium->stream_uploader = u_upload_create_default(gallium);
        gallium->const_uploader = gallium->stream_uploader;

        panfrost_pool_init(&ctx->descs, ctx, dev,
                        0, 4096, "Descriptors", true, false);

        panfrost_pool_init(&ctx->shaders, ctx, dev,
                        PAN_BO_EXECUTE, 4096, "Shaders", true, false);

        ctx->blitter = util_blitter_create(gallium);

        ctx->writers = _mesa_hash_table_create(gallium, _mesa_hash_pointer,
                                                        _mesa_key_pointer_equal);

        assert(ctx->blitter);

        /* Prepare for render! */

        /* By default mask everything on */
        ctx->sample_mask = ~0;
        ctx->active_queries = true;

        int ASSERTED ret;

        /* Create a syncobj in a signaled state. Will be updated to point to the
         * last queued job out_sync every time we submit a new job.
         */
        ret = drmSyncobjCreate(dev->fd, DRM_SYNCOBJ_CREATE_SIGNALED, &ctx->syncobj);
        assert(!ret && ctx->syncobj);

        return gallium;
}
