/*
 * Copyright (C) 2020 Collabora, Ltd.
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
#include "util/u_memory.h"
#include "util/list.h"
#include "util/set.h"

/* Liveness analysis is a backwards-may dataflow analysis pass. Within a block,
 * we compute live_out from live_in. The intrablock pass is linear-time. It
 * returns whether progress was made. */

void
bi_liveness_ins_update(uint8_t *live, bi_instr *ins, unsigned max)
{
        /* live_in[s] = GEN[s] + (live_out[s] - KILL[s]) */

        bi_foreach_dest(ins, d) {
                unsigned node = bi_get_node(ins->dest[d]);

                if (node < max)
                        live[node] &= ~bi_writemask(ins, d);
        }

        bi_foreach_src(ins, src) {
                unsigned count = bi_count_read_registers(ins, src);
                unsigned rmask = BITFIELD_MASK(count);
                uint8_t mask = (rmask << ins->src[src].offset);

                unsigned node = bi_get_node(ins->src[src]);
                if (node < max)
                        live[node] |= mask;
        }
}

static bool
liveness_block_update(bi_block *blk, unsigned temp_count)
{
        bool progress = false;

        /* live_out[s] = sum { p in succ[s] } ( live_in[p] ) */
        bi_foreach_successor(blk, succ) {
                for (unsigned i = 0; i < temp_count; ++i)
                        blk->live_out[i] |= succ->live_in[i];
        }

        uint8_t *live = ralloc_array(blk, uint8_t, temp_count);
        memcpy(live, blk->live_out, temp_count);

        bi_foreach_instr_in_block_rev(blk, ins)
                bi_liveness_ins_update(live, ins, temp_count);

        /* To figure out progress, diff live_in */

        for (unsigned i = 0; (i < temp_count) && !progress; ++i)
                progress |= (blk->live_in[i] != live[i]);

        ralloc_free(blk->live_in);
        blk->live_in = live;

        return progress;
}

/* Globally, liveness analysis uses a fixed-point algorithm based on a
 * worklist. We initialize a work list with the exit block. We iterate the work
 * list to compute live_in from live_out for each block on the work list,
 * adding the predecessors of the block to the work list if we made progress.
 */

void
bi_compute_liveness(bi_context *ctx)
{
        if (ctx->has_liveness)
                return;

        unsigned temp_count = bi_max_temp(ctx);

        u_worklist worklist;
        bi_worklist_init(ctx, &worklist);

        bi_foreach_block(ctx, block) {
                if (block->live_in)
                        ralloc_free(block->live_in);

                if (block->live_out)
                        ralloc_free(block->live_out);

                block->live_in = rzalloc_array(block, uint8_t, temp_count);
                block->live_out = rzalloc_array(block, uint8_t, temp_count);

                bi_worklist_push_tail(&worklist, block);
        }

        while (!u_worklist_is_empty(&worklist)) {
                /* Pop off in reverse order since liveness is backwards */
                bi_block *blk = bi_worklist_pop_tail(&worklist);

                /* Update liveness information. If we made progress, we need to
                 * reprocess the predecessors
                 */
                if (liveness_block_update(blk, temp_count)) {
                        bi_foreach_predecessor(blk, pred)
                                bi_worklist_push_head(&worklist, *pred);
                }
        }

        u_worklist_fini(&worklist);

        ctx->has_liveness = true;
}

/* Once liveness data is no longer valid, call this */

void
bi_invalidate_liveness(bi_context *ctx)
{
        ctx->has_liveness = false;
}
