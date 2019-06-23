/*****************************************************************************

YASK: Yet Another Stencil Kit
Copyright (c) 2014-2019, Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to
deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
sell copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

* The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
IN THE SOFTWARE.

*****************************************************************************/

// This file contains implementations of StencilContext methods.
// Also see setup.cpp and soln_apis.cpp.

#include "yask_stencil.hpp"
using namespace std;

namespace yask {

    ///// Top-level methods for evaluating reference and optimized stencils.

    // Eval stencil bundle(s) over var(s) using reference scalar code.
    void StencilContext::run_ref(idx_t first_step_index,
                                 idx_t last_step_index) {
        STATE_VARS(this);
        run_time.start();

        // Determine step dir from order of first/last.
        idx_t step_dir = (last_step_index >= first_step_index) ? 1 : -1;
        
        // Find begin, stride and end in step-dim.
        idx_t begin_t = first_step_index;
        idx_t stride_t = step_dir; // always +/- 1 for ref run.
        assert(stride_t);
        idx_t end_t = last_step_index + step_dir; // end is beyond last.

        // Begin & end tuples.
        // Based on rank bounding box, not extended
        // BB because we don't use wave-fronts in the ref code.
        IdxTuple begin(stencil_dims);
        begin.setVals(rank_bb.bb_begin_tuple(domain_dims), false); // 'false' because dims aren't same.
        begin[step_dim] = begin_t;
        IdxTuple end(stencil_dims);
        end.setVals(rank_bb.bb_end_tuple(domain_dims), false);
        end[step_dim] = end_t;

        TRACE_MSG("run_ref: [" << begin.makeDimValStr() << " ... " <<
                  end.makeDimValStr() << ")");

        // Force sub-sizes to whole rank size so that scratch
        // vars will be large enough. Turn off any temporal blocking.
        opts->_region_sizes.setValsSame(0);
        opts->_block_sizes.setValsSame(0);
        opts->_mini_block_sizes.setValsSame(0);
        opts->_sub_block_sizes.setValsSame(0);
        opts->adjustSettings();
        update_var_info(true);

        // Copy these settings to packs and realloc scratch vars.
        for (auto& sp : stPacks)
            sp->getLocalSettings() = *opts;
        allocScratchData();

        // Use only one set of scratch vars.
        int scratch_var_idx = 0;

        // Indices to loop through.
        // Init from begin & end tuples.
        ScanIndices rank_idxs(*dims, false, &rank_domain_offsets);
        rank_idxs.begin = begin;
        rank_idxs.end = end;

        // Set offsets in scratch vars.
        // Requires scratch vars to be allocated for whole
        // rank instead of smaller var size.
        update_scratch_var_info(scratch_var_idx, rank_idxs.begin);

        // Initial halo exchange.
        // TODO: get rid of all halo exchanges in this function,
        // and calculate overall problem in one rank.
        exchange_halos();

        // Number of iterations to get from begin_t, stopping before end_t,
        // jumping by stride_t.
        const idx_t num_t = abs(end_t - begin_t);
        for (idx_t index_t = 0; index_t < num_t; index_t++)
        {
            // This value of index_t steps from start_t to stop_t-1.
            const idx_t start_t = begin_t + (index_t * stride_t);
            const idx_t stop_t = (stride_t > 0) ?
                min(start_t + stride_t, end_t) :
                max(start_t + stride_t, end_t);

            // Set indices that will pass through generated code
            // because the step loop is coded here.
            rank_idxs.index[step_posn] = index_t;
            rank_idxs.start[step_posn] = start_t;
            rank_idxs.stop[step_posn] = stop_t;
            rank_idxs.stride[step_posn] = stride_t;

            // Loop thru bundles. We ignore bundle packs here
            // because packing bundles is an optional optimizations.
            for (auto* asg : stBundles) {

                // Scan through n-D space.
                TRACE_MSG("run_ref: step " << start_t <<
                          " in non-scratch bundle '" << asg->get_name());

                // Check step.
                if (check_step_conds && !asg->is_in_valid_step(start_t)) {
                    TRACE_MSG("run_ref: not valid for step " << start_t);
                    continue;
                }
                
                // Exchange all dirty halos.
                exchange_halos();

                // Find the bundles that need to be processed.
                // This will be the prerequisite scratch-var
                // bundles plus this non-scratch group.
                auto sg_list = asg->get_reqd_bundles();

                // Loop through all the needed bundles.
                for (auto* sg : sg_list) {

                    // Indices needed for the generated misc loops.  Will normally be a
                    // copy of rank_idxs except when updating scratch-vars.
                    ScanIndices misc_idxs = sg->adjust_span(scratch_var_idx, rank_idxs);
                    misc_idxs.stride.setFromConst(1); // ensure unit stride.

                    // Define misc-loop function.  Since stride is always 1, we
                    // ignore misc_stop.  If point is in sub-domain for this
                    // bundle, then evaluate the reference scalar code.
                    // TODO: fix domain of scratch vars.
#define MISC_FN(misc_idxs) \
                    do {                                                \
                        if (sg->is_in_valid_domain(misc_idxs.start))    \
                            sg->calc_scalar(scratch_var_idx, misc_idxs.start); \
                    } while(0)

                    // Scan through n-D space.
                    TRACE_MSG("run_ref: step " << start_t <<
                              " in bundle '" << sg->get_name() << "': [" <<
                              misc_idxs.begin.makeValStr() <<
                              " ... " << misc_idxs.end.makeValStr() << ")");
#include "yask_misc_loops.hpp"
#undef misc_fn
                } // needed bundles.

                // Mark vars that [may] have been written to.
                // Mark vars as dirty even if not actually written by this
                // rank. This is needed because neighbors will not know what
                // vars are actually dirty, and all ranks must have the same
                // information about which vars are possibly dirty.
                update_vars(nullptr, start_t, stop_t, true);

            } // all bundles.

        } // iterations.
        steps_done += abs(end_t - begin_t);

        // Final halo exchange.
        exchange_halos();

        run_time.stop();
    } // run_ref.

    // Eval stencil bundle pack(s) over var(s) using optimized code.
    void StencilContext::run_solution(idx_t first_step_index,
                                      idx_t last_step_index)
    {
        STATE_VARS(this);

        // User-provided code.
        call_2idx_hooks(_before_run_solution_hooks,
                        first_step_index, last_step_index);

        // Start timer.
        run_time.start();

        // Start vtune collection.
        VTUNE_RESUME;

        // Determine step dir from order of first/last.
        idx_t step_dir = (last_step_index >= first_step_index) ? 1 : -1;
        
        // Find begin, stride and end in step-dim.
        idx_t begin_t = first_step_index;

        // Stride-size in step-dim is number of region steps.
        // Then, it is multipled by +/-1 to get proper direction.
        idx_t stride_t = max(wf_steps, idx_t(1)) * step_dir;
        assert(stride_t);
        idx_t end_t = last_step_index + step_dir; // end is beyond last.

        // Begin, end, stride tuples.
        // Based on overall bounding box, which includes
        // any needed extensions for wave-fronts.
        IdxTuple begin(stencil_dims);
        begin.setVals(ext_bb.bb_begin_tuple(domain_dims), false);
        begin[step_posn] = begin_t;
        IdxTuple end(stencil_dims);
        end.setVals(ext_bb.bb_end_tuple(domain_dims), false);
        end[step_posn] = end_t;
        IdxTuple stride(stencil_dims);
        stride.setVals(opts->_region_sizes, false); // stride by region sizes.
        stride[step_posn] = stride_t;

        TRACE_MSG("run_solution: [" <<
                  begin.makeDimValStr() << " ... " <<
                  end.makeDimValStr() << ") by " <<
                  stride.makeDimValStr());
        if (!is_prepared())
            THROW_YASK_EXCEPTION("Error: run_solution() called without calling prepare_solution() first");
        if (ext_bb.bb_size < 1) {
            TRACE_MSG("nothing to do in solution");
            return;
        }

#ifdef MODEL_CACHE
        if (env.my_rank != env.msg_rank)
            cache_model.disable();
        if (cache_model.isEnabled())
            os << "Modeling cache...\n";
#endif

        // Adjust end points for overlapping regions due to wavefront angle.
        // For each subsequent time step in a region, the spatial location
        // of each block evaluation is shifted by the angle for each
        // bundle pack. So, the total shift in a region is the angle * num
        // packs * num timesteps. This assumes all bundle packs
        // are inter-dependent to find maximum extension. Actual required
        // size may be less, but this will just result in some calls to
        // calc_region() that do nothing.
        //
        // Conceptually (showing 2 ranks in t and x dims):
        // -----------------------------  t = rt ------------------------------
        //   \   | \     \     \|  \   |    .    |   / |  \     \     \|  \   |
        //    \  |  \     \     |   \  |    .    |  / \|   \     \     |   \  |
        //     \ |r0 \  r1 \ r2 |\ r3\ |    .    | /r0 | r1 \  r2 \ r3 |\ r4\ |
        //      \|    \     \   | \   \|         |/    |\    \     \   | \   \|
        // ------------------------------ t = 0 -------------------------------
        //       |   rank 0     |      |         |     |   rank 1      |      |
        // x = begin[x]       end[x] end[x]  begin[x] begin[x]       end[x] end[x]
        //     (rank)        (rank) (ext)     (ext)    (rank)       (rank) (adj)
        //
        //                      |XXXXXX|         |XXXXX|  <- redundant calculations.
        // XXXXXX|  <- areas outside of outer ranks not calculated ->  |XXXXXXX
        //
        if (wf_steps > 0) {
            DOMAIN_VAR_LOOP(i, j) {

                // The end should be adjusted only if an extension doesn't
                // exist.  Extentions exist between ranks, so additional
                // adjustments are only needed at the end of the right-most
                // rank in each dim.  See "(adj)" in diagram above.
                if (right_wf_exts[j] == 0)
                    end[i] += wf_shift_pts[j];
            }
        }

        // If original region covered entire rank in a dim, set
        // stride size to ensure only one stride is taken.
        DOMAIN_VAR_LOOP(i, j) {
            if (opts->_region_sizes[i] >= opts->_rank_sizes[i])
                stride[i] = end[i] - begin[i];
        }
        TRACE_MSG("run_solution: after adjustment for " << num_wf_shifts <<
                  " wave-front shift(s): [" <<
                  begin.makeDimValStr() << " ... " <<
                  end.makeDimValStr() << ") by " <<
                  stride.makeDimValStr());

        // At this point, 'begin' and 'end' should describe the *max* range
        // needed in the domain for this rank for the first time step.  At
        // any subsequent time step, this max may be shifted for temporal
        // wavefronts or blocking. Also, for each time step, the *actual*
        // range will be adjusted as needed before any actual stencil
        // calculations are made.

        // Indices needed for the 'rank' loops.
        ScanIndices rank_idxs(*dims, true, &rank_domain_offsets);
        rank_idxs.begin = begin;
        rank_idxs.end = end;
        rank_idxs.stride = stride;

        // Make sure threads are set properly for a region.
        set_region_threads();

        // Initial halo exchange.
        exchange_halos();

        // Number of iterations to get from begin_t to end_t-1,
        // jumping by stride_t.
        const idx_t num_t = CEIL_DIV(abs(end_t - begin_t), abs(stride_t));
        for (idx_t index_t = 0; index_t < num_t; index_t++)
        {
            // This value of index_t steps from start_t to stop_t-1.
            const idx_t start_t = begin_t + (index_t * stride_t);
            const idx_t stop_t = (stride_t > 0) ?
                min(start_t + stride_t, end_t) :
                max(start_t + stride_t, end_t);
            idx_t this_num_t = abs(stop_t - start_t);

            // Set indices that will pass through generated code.
            rank_idxs.index[step_posn] = index_t;
            rank_idxs.start[step_posn] = start_t;
            rank_idxs.stop[step_posn] = stop_t;
            rank_idxs.stride[step_posn] = stride_t;

            // Start timer for auto-tuner.
            _at.timer.start();
            
            // If no wave-fronts (default), loop through packs here, and do
            // only one pack at a time in calc_region(). This is similar to
            // loop in calc_rank_ref(), but with packs instead of bundles.
            if (wf_steps == 0) {

                // Loop thru packs.
                for (auto& bp : stPacks) {

                    // Check step.
                    if (check_step_conds && !bp->is_in_valid_step(start_t)) {
                        TRACE_MSG("run_solution: step " << start_t <<
                                  " not valid for pack '" <<
                                  bp->get_name() << "'");
                        continue;
                    }

                    // Do MPI-external passes?
                    if (mpi_interior.bb_valid) {
                        do_mpi_interior = false;
                        
                        // Old overlap method calculates full blocks in exterior
                        // and then in interior. Only works without WF tiling.
                        // Also, if blocks are too big, then the interior is
                        // too small. For now, keeping code for perf comparison.
#ifdef OVERLAP_WITH_BLOCKS
                        mpi_exterior_dim = -1; // indicate block method.

                        // Overlap comms and computation at a block granularity.
                        // Set both left and right exterior flags.
                        do_mpi_left = do_mpi_right = true;

                        // Include automatically-generated loop code that calls
                        // calc_region(bp) for each region.
                        TRACE_MSG("run_solution: step " << start_t <<
                                  " for pack '" << bp->get_name() << "' in MPI exterior");
#include "yask_rank_loops.hpp"

#else
                        mpi_exterior_dim = 0;

                        // Overlap comms and computation by restricting
                        // region boundaries.  Make an external pass for
                        // each side of each domain dim, e.g., 'left x',
                        // 'right x', 'left y', ...
                        DOMAIN_VAR_LOOP(i, j) {
                            for (bool is_left : { true, false }) {

                                // Skip if no halo to calculate in this
                                // section.
                                if (!does_exterior_exist(j, is_left))
                                    continue;
                        
                                // Set the proper flags to indicate what
                                // section we're working on.
                                do_mpi_left = is_left;
                                do_mpi_right = !is_left;
                                mpi_exterior_dim = j;

                                // Include automatically-generated loop
                                // code that calls calc_region(bp) for
                                // each region. The region will be trimmed
                                // to the active MPI exterior section.
                                TRACE_MSG("run_solution: step " << start_t <<
                                          " for pack '" << bp->get_name() <<
                                          "' in MPI exterior dim " << j <<
                                          " on the " << (is_left ? "left" : "right"));
#include "yask_rank_loops.hpp"
                            } // left/right.
                        } // domain dims.
#endif

                        // Mark vars that [may] have been written to by
                        // this pack. Mark vars as dirty even if not
                        // actually written by this rank, perhaps due to
                        // sub-domains or asymmetrical stencils. This is
                        // needed because neighbors will not know what vars
                        // are actually dirty, and all ranks must have the
                        // same information about which vars are possibly
                        // dirty.  TODO: make this smarter to save unneeded
                        // MPI exchanges.
                        update_vars(bp, start_t, stop_t, true);
                        
                        // Do the appropriate steps for halo exchange of exterior.
                        // TODO: exchange halo for each dim as soon as it's done.
                        do_mpi_left = do_mpi_right = true;
                        exchange_halos();
                        
                        // Do interior only in next pass.
                        do_mpi_left = do_mpi_right = false;
                        do_mpi_interior = true;
                    } // Overlapping.

                    // Include automatically-generated loop code that calls
                    // calc_region(bp) for each region. If overlapping
                    // comms, this will be just the interior.  If not, it
                    // will cover the whole rank.
                    TRACE_MSG("run_solution: step " << start_t <<
                              " for pack '" << bp->get_name() << "'");
#include "yask_rank_loops.hpp"

                    // Mark as dirty only if we did exterior.
                    bool mark_dirty = do_mpi_left || do_mpi_right;
                    update_vars(bp, start_t, stop_t, mark_dirty);

                    // Do the appropriate steps for halo exchange depending
                    // on 'do_mpi_*' flags.
                    exchange_halos();

                    // Set the overlap flags back to default.
                    do_mpi_interior = do_mpi_left = do_mpi_right = true;

                } // packs.
            } // No WF tiling.

            // If doing wave-fronts, must loop through all packs in
            // calc_region().
            else {

                // Null ptr => Eval all stencil packs each time
                // calc_region() is called.
                BundlePackPtr bp;

                // Do MPI-external passes?
                if (mpi_interior.bb_valid) {
                    do_mpi_interior = false;
                    mpi_exterior_dim = 0;

                    // Overlap comms and computation by restricting
                    // region boundaries.  Make an external pass for
                    // each side of each domain dim, e.g., 'left x',
                    // 'right x', 'left y', ...
                    DOMAIN_VAR_LOOP(i, j) {
                        for (bool is_left : { true, false }) {

                            // Skip if no halo to calculate in this
                            // section.
                            if (!does_exterior_exist(j, is_left))
                                continue;
                        
                            // Set the proper flags to indicate what
                            // section we're working on.
                            do_mpi_left = is_left;
                            do_mpi_right = !is_left;
                            mpi_exterior_dim = j;

                            // Include automatically-generated loop
                            // code that calls calc_region(bp) for
                            // each region. The region will be trimmed
                            // to the active MPI exterior section.
                            TRACE_MSG("run_solution: steps [" << start_t <<
                                      " ... " << stop_t <<
                                      ") in MPI exterior dim " << j <<
                                      " on the " << (is_left ? "left" : "right"));
#include "yask_rank_loops.hpp"
                        } // left/right.
                    } // domain dims.

                    // Mark vars dirty for all packs.
                    update_vars(bp, start_t, stop_t, true);
                    
                    // Do the appropriate steps for halo exchange of exterior.
                    // TODO: exchange halo for each dim as soon as it's done.
                    do_mpi_left = do_mpi_right = true;
                    exchange_halos();
                        
                    // Do interior only in next pass.
                    do_mpi_left = do_mpi_right = false;
                    do_mpi_interior = true;
                } // Overlapping.

                // Include automatically-generated loop code that calls
                // calc_region(bp) for each region. If overlapping
                // comms, this will be just the interior.  If not, it
                // will cover the whole rank.
                TRACE_MSG("run_solution: steps [" << start_t <<
                          " ... " << stop_t << ")");
#include "yask_rank_loops.hpp"

                // Mark as dirty only if we did exterior.
                bool mark_dirty = do_mpi_left || do_mpi_right;
                update_vars(bp, start_t, stop_t, mark_dirty);

                // Do the appropriate steps for halo exchange depending
                // on 'do_mpi_*' flags.
                exchange_halos();

                // Set the overlap flags back to default.
                do_mpi_interior = do_mpi_left = do_mpi_right = true;

            } // With WF tiling.

            // Overall steps.
            steps_done += this_num_t;

            // Count steps for each pack to properly account for
            // step conditions when using temporal tiling.
            for (auto& bp : stPacks) {
                idx_t num_pack_steps = 0;

                if (!check_step_conds)
                    num_pack_steps = this_num_t;
                else {

                    // Loop through each step.
                    assert(abs(step_dir) == 1);
                    for (idx_t t = start_t; t != stop_t; t += step_dir) {

                        // Check step cond for this t.
                        if (bp->is_in_valid_step(t))
                            num_pack_steps++;
                    }
                }

                // Count steps for this pack.
                bp->add_steps(num_pack_steps);
            }

            // Call the auto-tuner to evaluate these steps.
            eval_auto_tuner(this_num_t);

        } // step loop.

#ifdef MODEL_CACHE
        // Print cache stats, then disable.
        // Thus, cache is only modeled for first call.
        if (cache_model.isEnabled()) {
            os << "Done modeling cache...\n";
            cache_model.dumpStats();
            cache_model.disable();
        }
#endif

        // Stop vtune collection & timer.
        VTUNE_PAUSE;
        run_time.stop();

        // User-provided code.
        call_2idx_hooks(_after_run_solution_hooks,
                        first_step_index, last_step_index);

    } // run_solution().

    // Calculate results within a region.  Each region is typically computed
    // in a separate OpenMP 'for' region.  In this function, we loop over
    // the time steps and bundle packs and evaluate a pack in each of
    // the blocks in the region.  If 'sel_bp' is null, eval all packs; else
    // eval only the one pointed to.
    void StencilContext::calc_region(BundlePackPtr& sel_bp,
                                     const ScanIndices& rank_idxs) {
        STATE_VARS(this);
        TRACE_MSG("calc_region: region [" <<
                  rank_idxs.start.makeValStr() << " ... " <<
                  rank_idxs.stop.makeValStr() << ") within rank [" <<
                  rank_idxs.begin.makeValStr() << " ... " <<
                  rank_idxs.end.makeValStr() << ")" );

        // Track time (use "else" to avoid double-counting).
        if (!do_mpi_interior && (do_mpi_left || do_mpi_right))
            ext_time.start();
        else
            int_time.start();

        // Init region begin & end from rank start & stop indices.
        ScanIndices region_idxs(*dims, true, &rank_domain_offsets);
        region_idxs.initFromOuter(rank_idxs);

        // Time range.
        // When doing WF rank tiling, this loop will stride through
        // several time-steps in each region.
        // When also doing TB, it will stride by the block strides.
        idx_t begin_t = region_idxs.begin[step_posn];
        idx_t end_t = region_idxs.end[step_posn];
        idx_t step_dir = (end_t >= begin_t) ? 1 : -1;
        idx_t stride_t = max(tb_steps, idx_t(1)) * step_dir;
        assert(stride_t);
        const idx_t num_t = CEIL_DIV(abs(end_t - begin_t), abs(stride_t));

        // Time loop.
        idx_t region_shift_num = 0;
        for (idx_t index_t = 0; index_t < num_t; index_t++) {

            // This value of index_t steps from start_t to stop_t-1.
            const idx_t start_t = begin_t + (index_t * stride_t);
            const idx_t stop_t = (stride_t > 0) ?
                min(start_t + stride_t, end_t) :
                max(start_t + stride_t, end_t);

            // Set step indices that will pass through generated code.
            region_idxs.index[step_posn] = index_t;
            region_idxs.start[step_posn] = start_t;
            region_idxs.stop[step_posn] = stop_t;

            // If no temporal blocking (default), loop through packs here,
            // and do only one pack at a time in calc_block(). If there is
            // no WF blocking either, the pack loop body will only execute
            // with one active pack, and 'region_shift_num' will never be > 0.
            if (tb_steps == 0) {

                // Stencil bundle packs to evaluate at this time step.
                for (auto& bp : stPacks) {

                    // Not a selected bundle pack?
                    if (sel_bp && sel_bp != bp)
                        continue;

                    TRACE_MSG("calc_region: no TB; pack '" <<
                              bp->get_name() << "' in step(s) [" <<
                              start_t << " ... " << stop_t << ")");

                    // Check step.
                    if (check_step_conds && !bp->is_in_valid_step(start_t)) {
                        TRACE_MSG("calc_region: step " << start_t <<
                                  " not valid for pack '" << bp->get_name() << "'");
                        continue;
                    }

                    // Strides within a region are based on pack block sizes.
                    auto& settings = bp->getActiveSettings();
                    region_idxs.stride = settings._block_sizes;
                    region_idxs.stride[step_posn] = stride_t;

                    // Groups in region loops are based on block-group sizes.
                    region_idxs.group_size = settings._block_group_sizes;

                    // Set region_idxs begin & end based on shifted rank
                    // start & stop (original region begin & end), rank
                    // boundaries, and pack BB. This will be the base of the
                    // region loops.
                    bool ok = shift_region(rank_idxs.start, rank_idxs.stop,
                                           region_shift_num, bp,
                                           region_idxs);

                    DOMAIN_VAR_LOOP(i, j) {
                        
                        // If there is only one blk in a region, make sure
                        // this blk fills this whole region.
                        if (settings._block_sizes[i] >= settings._region_sizes[i])
                            region_idxs.stride[i] = region_idxs.end[i] - region_idxs.begin[i];
                    }
                
                    // Only need to loop through the span of the region if it is
                    // at least partly inside the extended BB. For overlapping
                    // regions, they may start outside the domain but enter the
                    // domain as time progresses and their boundaries shift. So,
                    // we don't want to return if this condition isn't met.
                    if (ok) {
                        idx_t nphases = 1; // Only 1 phase w/o TB.
                        idx_t phase = 0;

                        // Include automatically-generated loop code that
                        // calls calc_block() for each block in this region.
                        // Loops through x from begin_rx to end_rx-1;
                        // similar for y and z.  This code typically
                        // contains the outer OpenMP loop(s).
#include "yask_region_loops.hpp"
                    }

                    // Need to shift for next pack and/or time.
                    region_shift_num++;
                    
                } // stencil bundle packs.
            } // no temporal blocking.

            // If using TB, iterate thru steps in a WF and packs in calc_block().
            else {

                TRACE_MSG("calc_region: w/TB in step(s) [" <<
                          start_t << " ... " << stop_t << ")");

                // Null ptr => Eval all stencil packs each time
                // calc_block() is called.
                BundlePackPtr bp;

                // Strides within a region are based on rank block sizes.
                auto& settings = *opts;
                region_idxs.stride = settings._block_sizes;
                region_idxs.stride[step_posn] = stride_t;

                // Groups in region loops are based on block-group sizes.
                region_idxs.group_size = settings._block_group_sizes;

                // Set region_idxs begin & end based on shifted start & stop
                // and rank boundaries.  This will be the base of the region
                // loops. The bounds in region_idxs may be outside the
                // actual rank because we're starting with the expanded rank.
                bool ok = shift_region(rank_idxs.start, rank_idxs.stop,
                                       region_shift_num, bp,
                                       region_idxs);

                // Should always be valid because we just shifted (no trim).
                // Trimming will be done at the mini-block level.
                assert(ok);

                DOMAIN_VAR_LOOP(i, j) {

                    // If original blk covered entire region, reset stride.
                    if (settings._block_sizes[i] >= settings._region_sizes[i])
                        region_idxs.stride[i] = region_idxs.end[i] - region_idxs.begin[i];
                }
                
                // To tesselate n-D domain space, we use n+1 distinct
                // "phases".  For example, 1-D TB uses "upward" triangles
                // and "downward" triangles. Region threads sync after every
                // phase. Thus, the phase loop is here around the generated
                // OMP loops.  TODO: schedule phases and their shapes via task
                // dependencies.
                idx_t nphases = nddims + 1; 
                for (idx_t phase = 0; phase < nphases; phase++) {
                    
                    // Call calc_block() on every block concurrently.  Only
                    // the shapes corresponding to the current 'phase' will
                    // be calculated.
#include "yask_region_loops.hpp"
                }
            
                // Loop thru stencil bundle packs that were evaluated in
                // these 'tb_steps' to increment shift for next region
                // "layer", if any. This is needed when there are more WF
                // steps than TB steps.  TODO: consider moving this inside
                // calc_block().
                for (idx_t t = start_t; t != stop_t; t += step_dir) {
                    for (auto& bp : stPacks) {

                        // Check step.
                        if (check_step_conds && !bp->is_in_valid_step(t))
                            continue;

                        // One shift for each pack in each TB step.
                        region_shift_num++;
                    }
                }
            } // with temporal blocking.
        } // time.

        if (!do_mpi_interior && (do_mpi_left || do_mpi_right)) {
            double ext_delta = ext_time.stop();
            TRACE_MSG("secs spent in this region for rank-exterior blocks: " << makeNumStr(ext_delta));
        }
        else {
            double int_delta = int_time.stop();
            TRACE_MSG("secs spent in this region for rank-interior blocks: " << makeNumStr(int_delta));
        }

    } // calc_region.
    
    // Calculate results within a block. This function calls
    // 'calc_mini_block()' for the specified pack or all packs if 'sel_bp'
    // is null.  When using TB, only the shape(s) needed for the tesselation
    // 'phase' are computed.  Typically called by a top-level OMP thread
    // from calc_region().
    void StencilContext::calc_block(BundlePackPtr& sel_bp,
                                    idx_t region_shift_num,
                                    idx_t nphases, idx_t phase,
                                    const ScanIndices& rank_idxs,
                                    const ScanIndices& region_idxs) {

        STATE_VARS(this);
        auto* bp = sel_bp.get();
        int region_thread_idx = omp_get_thread_num();
        TRACE_MSG("calc_block: phase " << phase << ", block [" <<
                  region_idxs.start.makeValStr() << " ... " <<
                  region_idxs.stop.makeValStr() << 
                  ") within region [" <<
                  region_idxs.begin.makeValStr() << " ... " <<
                  region_idxs.end.makeValStr() << 
                  ") by region thread " << region_thread_idx);

#ifdef OVERLAP_WITH_BLOCKS
        // If we are not calculating some of the blocks, determine
        // whether this block is *completely* inside the interior.
        // A block even partially in the exterior is not considered
        // "inside".
        if (is_overlap_active()) {

            // Starting point and ending point must be in BB.
            bool inside = true;
            DOMAIN_VAR_LOOP(i, j) {

                // Starting before beginning of interior?
                if (region_idxs.start[i] < mpi_interior.bb_begin[j])
                    inside = false;

                // Stopping after ending of interior?
                if (region_idxs.stop[i] > mpi_interior.bb_end[j])
                    inside = false;
            }
            if (do_mpi_interior) {
                if (inside)
                    TRACE_MSG(" calculating because block is interior");
                else {
                    TRACE_MSG(" *not* calculating because block is exterior");
                    return;
                }
            }
            if (do_mpi_left || do_mpi_right) {
                if (!inside)
                    TRACE_MSG(" calculating because block is exterior");
                else {
                    TRACE_MSG(" *not* calculating because block is interior");
                    return;
                }
            }
        }
#endif
        
        // Init block begin & end from region start & stop indices.
        ScanIndices block_idxs(*dims, true);
        block_idxs.initFromOuter(region_idxs);

        // Time range.
        // When not doing TB, there is only one step.
        // When doing TB, we will only do one iteration here
        // that covers all steps,
        // and calc_mini_block() will loop over all steps.
        idx_t begin_t = block_idxs.begin[step_posn];
        idx_t end_t = block_idxs.end[step_posn];
        idx_t step_dir = (end_t >= begin_t) ? 1 : -1;
        idx_t stride_t = max(tb_steps, idx_t(1)) * step_dir;
        assert(stride_t);
        const idx_t num_t = CEIL_DIV(abs(end_t - begin_t), abs(stride_t));

        // If TB is not being used, just process the given pack.
        // No need for a time loop.
        // No need to check bounds, because they were checked in
        // calc_region() when not using TB.
        if (tb_steps == 0) {
            assert(bp);
            assert(abs(stride_t) == 1);
            assert(abs(end_t - begin_t) == 1);
            assert(num_t == 1);
        
            // Set step indices that will pass through generated code.
            block_idxs.index[step_posn] = 0;
            block_idxs.start[step_posn] = begin_t;
            block_idxs.stop[step_posn] = end_t;

            // Strides within a block are based on pack mini-block sizes.
            auto& settings = bp->getActiveSettings();
            block_idxs.stride = settings._mini_block_sizes;
            block_idxs.stride[step_posn] = stride_t;
        
            // Groups in block loops are based on mini-block-group sizes.
            block_idxs.group_size = settings._mini_block_group_sizes;

            // Default settings for no TB.
            BundlePackPtr bp = sel_bp;
            assert(phase == 0);
            idx_t nshapes = 1;
            idx_t shape = 0;
            idx_t shift_num = 0;
            BridgeMask bridge_mask;
            ScanIndices adj_block_idxs = block_idxs;

            // Include automatically-generated loop code that
            // calls calc_mini_block() for each mini-block in this block.
#include "yask_block_loops.hpp"
        } // no TB.

        // If TB is active, loop thru each required shape.
        else {
            assert(phase >= 0);
            assert(phase < nphases); // E.g., phase = 0..2 for 2D.
            
            // Determine number of shapes for this 'phase'. First and last
            // phase need one shape. Other (bridge) phases need one shape
            // for each combination of domain dims. E.g., need 'x' and
            // 'y' bridges for 2D problem in phase 1.
            idx_t nshapes = choose(nddims, phase);
            int dims_to_bridge[phase];
            BridgeMask bridge_mask(nddims, false);

            // Set temporal indices to full range.
            block_idxs.index[step_posn] = 0; // only one index.
            block_idxs.start[step_posn] = begin_t;
            block_idxs.stop[step_posn] = end_t;

            // Strides within a block are based on rank mini-block sizes.
            auto& settings = *opts;
            block_idxs.stride = settings._mini_block_sizes;
            block_idxs.stride[step_posn] = step_dir;

            // Groups in block loops are based on mini-block-group sizes.
            block_idxs.group_size = settings._mini_block_group_sizes;

            // Increase range of block to cover all phases and
            // shapes.
            ScanIndices adj_block_idxs = block_idxs;
            DOMAIN_VAR_LOOP(i, j) {
                    
                // TB shapes can extend to the right only.  They can
                // cover a range as big as this block's base plus the
                // next block in all dims, so we add the width of the
                // current block to the end.  This makes the adjusted
                // blocks overlap, but the size of each mini-block is
                // trimmed at each step to the proper active size.
                // TODO: find a way to make this more efficient to avoid
                // calling calc_mini_block() many times with nothing to
                // do.
                auto width = region_idxs.stop[i] - region_idxs.start[i];
                adj_block_idxs.end[i] += width;

                // If original MB covers a whole block, reset stride.
                if (settings._mini_block_sizes[i] >= settings._block_sizes[i])
                    adj_block_idxs.stride[i] = adj_block_idxs.end[i] - adj_block_idxs.begin[i];
            }
            TRACE_MSG("calc_block: phase " << phase <<
                      ", adjusted block [" <<
                      adj_block_idxs.begin.makeValStr() << " ... " <<
                      adj_block_idxs.end.makeValStr() << 
                      ") with mini-block stride " <<
                      adj_block_idxs.stride.makeValStr());
                    
            // Loop thru shapes.
            for (idx_t shape = 0; shape < nshapes; shape++) {

                // Get 'shape'th combo of 'phase' things from 'nddims'.
                // These will be used to create bridge shapes.
                combination(dims_to_bridge, nddims, phase, shape + 1);

                // Set bits for selected dims.
                DOMAIN_VAR_LOOP(i, j)
                    bridge_mask.at(j) = false;
                for (int i = 0; i < phase; i++) {
                    auto dim = dims_to_bridge[i] - 1;
                    bridge_mask.at(dim) = true;
                }
                
                // Can only be one time iteration here when doing TB
                // because mini-block temporal size is always same
                // as block temporal size.
                assert(num_t == 1);
                    
                // Include automatically-generated loop code that calls
                // calc_mini_block() for each mini-block in this block.
                BundlePackPtr bp; // null.
#include "yask_block_loops.hpp"

            } // shape loop.
        } // TB.
    } // calc_block().

    // Calculate results within a mini-block.
    // This function calls 'StencilBundleBase::calc_mini_block()'
    // for each bundle in the specified pack or all packs if 'sel_bp' is
    // null. When using TB, only the 'shape' needed for the tesselation
    // 'phase' are computed. The starting 'shift_num' is relative
    // to the bottom of the current region and block.
    void StencilContext::calc_mini_block(int region_thread_idx,
                                         BundlePackPtr& sel_bp,
                                         idx_t region_shift_num,
                                         idx_t nphases, idx_t phase,
                                         idx_t nshapes, idx_t shape,
                                         const BridgeMask& bridge_mask,
                                         const ScanIndices& rank_idxs,
                                         const ScanIndices& base_region_idxs,
                                         const ScanIndices& base_block_idxs,
                                         const ScanIndices& adj_block_idxs) {

        STATE_VARS(this);
        TRACE_MSG("calc_mini_block: phase " << phase <<
                  ", shape " << shape <<
                  ", mini-block [" <<
                  adj_block_idxs.start.makeValStr() << " ... " <<
                  adj_block_idxs.stop.makeValStr() << ") within base-block [" <<
                  base_block_idxs.begin.makeValStr() << " ... " <<
                  base_block_idxs.end.makeValStr() << ") within base-region [" <<
                  base_region_idxs.begin.makeValStr() << " ... " <<
                  base_region_idxs.end.makeValStr() <<
                  ") by region thread " << region_thread_idx);

        // Promote forward progress in MPI when calc'ing interior
        // only. Call from one thread only.
        // Let all other threads continue.
        if (is_overlap_active() && do_mpi_interior) {
            if (region_thread_idx == 0)
                poke_halo_exchange();
        }

        // Init mini-block begin & end from blk start & stop indices.
        ScanIndices mini_block_idxs(*dims, true);
        mini_block_idxs.initFromOuter(adj_block_idxs);

        // Time range.
        // No more temporal blocks below mini-blocks, so we always stride
        // by +/- 1.
        idx_t begin_t = mini_block_idxs.begin[step_posn];
        idx_t end_t = mini_block_idxs.end[step_posn];
        idx_t step_dir = (end_t >= begin_t) ? 1 : -1;
        idx_t stride_t = 1 * step_dir;        // +/- 1.
        assert(stride_t);
        const idx_t num_t = CEIL_DIV(abs(end_t - begin_t), abs(stride_t));

        // Time loop.
        idx_t shift_num = 0;
        for (idx_t index_t = 0; index_t < num_t; index_t++) {

            // This value of index_t steps from start_t to stop_t-1.
            const idx_t start_t = begin_t + (index_t * stride_t);
            const idx_t stop_t = (stride_t > 0) ?
                min(start_t + stride_t, end_t) :
                max(start_t + stride_t, end_t);
            TRACE_MSG("calc_mini_block: phase " << phase <<
                      ", shape " << shape <<
                      ", in step " << start_t);
            assert(abs(stop_t - start_t) == 1); // no more TB.
            
            // Set step indices that will pass through generated code.
            mini_block_idxs.index[step_posn] = index_t;
            mini_block_idxs.begin[step_posn] = start_t;
            mini_block_idxs.end[step_posn] = stop_t;
            mini_block_idxs.start[step_posn] = start_t;
            mini_block_idxs.stop[step_posn] = stop_t;

            // Stencil bundle packs to evaluate at this time step.
            for (auto& bp : stPacks) {
            
                // Not a selected bundle pack?
                if (sel_bp && sel_bp != bp)
                    continue;
            
                // Check step.
                if (check_step_conds && !bp->is_in_valid_step(start_t)) {
                    TRACE_MSG("calc_mini_block: step " << start_t <<
                              " not valid for pack '" <<
                              bp->get_name() << "'");
                    continue;
                }
                TRACE_MSG("calc_mini_block: phase " << phase <<
                          ", shape " << shape <<
                          ", step " << start_t <<
                          ", pack '" << bp->get_name() <<
                          "', shift-num " << shift_num);

                // Start timers for this pack.  Tracking only on thread
                // 0. TODO: track all threads and report cross-thread stats.
                if (region_thread_idx == 0)
                    bp->start_timers();
                
                // Strides within a mini-blk are based on sub-blk sizes.
                // This will get overridden later if thread binding is enabled.
                auto& settings = bp->getActiveSettings();
                mini_block_idxs.stride = settings._sub_block_sizes;
                mini_block_idxs.stride[step_posn] = stride_t;

                // Groups in mini-blk loops are based on sub-block-group sizes.
                mini_block_idxs.group_size = settings._sub_block_group_sizes;

                // Set mini_block_idxs begin & end based on shifted rank
                // start & stop (original region begin & end), rank
                // boundaries, and pack BB. There may be several TB layers
                // within a region WF, so we need to add the region and
                // local mini-block shift counts.
                bool ok = shift_region(rank_idxs.start, rank_idxs.stop,
                                       region_shift_num + shift_num, bp,
                                       mini_block_idxs);

                // Set mini_block_idxs begin & end based on shifted begin &
                // end of block for given phase & shape.  This will be the
                // base for the mini-block loops, which have no temporal
                // tiling.
                if (ok)
                    ok = shift_mini_block(adj_block_idxs.start, adj_block_idxs.stop,
                                          adj_block_idxs.begin, adj_block_idxs.end,
                                          base_block_idxs.begin, base_block_idxs.end,
                                          base_region_idxs.begin, base_region_idxs.end,
                                          shift_num,
                                          nphases, phase,
                                          nshapes, shape,
                                          bridge_mask,
                                          mini_block_idxs);

                if (ok) {

                    // Update offsets of scratch vars based on the current
                    // mini-block location.
                    if (scratchVecs.size())
                        update_scratch_var_info(region_thread_idx, mini_block_idxs.begin);

                    // Call calc_mini_block() for each non-scratch bundle.
                    for (auto* sb : *bp)
                        if (sb->getBB().bb_num_points)
                            sb->calc_mini_block(region_thread_idx, settings, mini_block_idxs);

                    // Make sure streaming stores are visible for later loads.
                    make_stores_visible();
                }

                // Need to shift for next pack and/or time-step.
                shift_num++;

                // Stop timers for this pack.
                if (region_thread_idx == 0)
                    bp->stop_timers();

            } // packs.
        } // time-steps.

    } // calc_mini_block().

    // Find boundaries within region with 'base_start' to 'base_stop'
    // shifted 'shift_num' times, which should start at 0 and increment for
    // each pack in each time-step.  Trim to ext-BB and MPI section if 'bp' if
    // not null.  Write results into 'begin' and 'end' in 'idxs'.  Return
    // 'true' if resulting area is non-empty, 'false' if empty.
    bool StencilContext::shift_region(const Indices& base_start, const Indices& base_stop,
                                      idx_t shift_num,
                                      BundlePackPtr& bp,
                                      ScanIndices& idxs) {
        STATE_VARS(this);

        // For wavefront adjustments, see conceptual diagram in
        // run_solution().  At each pack and time-step, the parallelogram
        // may be trimmed based on the BB and WF extensions outside of the
        // rank-BB.

        // Actual region boundaries must stay within [extended] pack BB.
        // We have to calculate the posn in the extended rank at each
        // value of 'shift_num' because it is being shifted spatially.
        bool ok = true;
        DOMAIN_VAR_LOOP(i, j) {
            auto angle = wf_angles[j];
            idx_t shift_amt = angle * shift_num;

            // Shift initial spatial region boundaries for this iteration of
            // temporal wavefront.  Regions only shift left, so region loops
            // must strictly increment. They may do so in any order.  Shift
            // by pts in one WF step.  Always shift left in WFs.
            idx_t rstart = base_start[i] - shift_amt;
            idx_t rstop = base_stop[i] - shift_amt;

            // Trim only if pack is specified.
            if (bp.get()) {

                // Trim to extended BB of pack. This will also trim
                // to the extended BB of the rank.
                auto& pbb = bp.get()->getBB();
                rstart = max(rstart, pbb.bb_begin[j]);
                rstop = min(rstop, pbb.bb_end[j]);

                // Find non-extended domain. We'll use this to determine if
                // we're in an extension, where special rules apply.
                idx_t dbegin = rank_bb.bb_begin[j];
                idx_t dend = rank_bb.bb_end[j];

                // In left ext, add 'angle' points for every shift to get
                // region boundary in ext.
                if (rstart < dbegin && left_wf_exts[j])
                    rstart = max(rstart, dbegin - left_wf_exts[j] + shift_amt);

                // In right ext, subtract 'angle' points for every shift.
                if (rstop > dend && right_wf_exts[j])
                    rstop = min(rstop, dend + right_wf_exts[j] - shift_amt);

                // Trim region based on current MPI section if
                // using overlapping but not whole-block method.
                if (is_overlap_active() && mpi_exterior_dim >= 0) {

                    // Interior boundaries.
                    idx_t int_begin = mpi_interior.bb_begin[j];
                    idx_t int_end = mpi_interior.bb_end[j];
                    
                    if (wf_steps > 0) {

                        // If doing WF tiling, each exterior shape is a
                        // trapezoid with its height in the time dim.  Each
                        // shift reduces the width of the trapezoid until it is
                        // the minimum width at the top.  Thus, the interior is
                        // an inverted trapezoid between the exterior ones.
                        
                        //       +----+---------------+----+ 
                        // t    / ext  \  interior   / ext  \    .
                        // ^   /  left  \           /  right \   .
                        // |  +----------+---------+----------+  
                        // +--->x        ^          ^
                        //               |          |
                        //             int_begin  int_end
                        
                        // Modify interior if there is an external MPI
                        // section on either side.  Reduce interior by
                        // 'wf_shift_pts' to get size at base of region,
                        // then expand by current shift amount to get size
                        // at current shift number.
                        if (does_exterior_exist(j, true)) { // left.
                            int_begin += wf_shift_pts[j];
                            int_begin -= shift_amt;
                        }
                        if (does_exterior_exist(j, false)) { // right.
                            int_end -= wf_shift_pts[j];
                            int_end += shift_amt;
                        }
                    }

                    // In interior.
                    if (do_mpi_interior) {
                        rstart = max(rstart, int_begin);
                        rstop = min(rstop, int_end);
                    }

                    // In one of the exterior sections.
                    else {

                        // Should be doing either left or right, not both.
                        assert(do_mpi_left != do_mpi_right);

                        // Nothing to do if specified exterior section
                        // doesn't exist.
                        if (!does_exterior_exist(mpi_exterior_dim, do_mpi_left)) {
                            ok = false;
                            break;
                        }

                        // Example in 2D:
                        // +------+------------+------+
                        // |      | ext left y |      |
                        // |      |            |      |
                        // | ext  +------------+ ext  | <-- mpi_interior.bb_begin[y]
                        // | left |  interior  | right|
                        // | x    |            | x    |
                        // |      +------------+      |
                        // |      | ext right y|      | <-- mpi_interior.bb_end[y]
                        // |      |            |      |
                        // +------+------------+------+
                        //        ^             ^
                        //        |             |
                        //        |           mpi_interior.bb_end[x]
                        //      mpi_interior.bb_begin[x]

                        // Trim left or right for current dim.
                        if (j == mpi_exterior_dim) {
                            if (do_mpi_left)
                                rstop = min(rstop, int_begin);

                            else {
                                rstart = max(rstart, int_end);

                                // For right, also need to trim to avoid
                                // overlap with left. This could happen
                                // when the width of the rank is less
                                // than twice the amount of temporal
                                // shifting. This implies left always
                                // needs to be done before right.
                                rstart = max(rstart, int_begin);
                            }
                        }

                        // Trim across all dims up to current one, e.g.,
                        // trim overlap between 'x' and 'y' from 'y'.
                        // See above diagram. This implies dims need
                        // to be done in ascending numerical order.
                        if (j < mpi_exterior_dim) {
                            rstart = max(rstart, int_begin);
                            rstop = min(rstop, int_end);
                        }
                    } // exterior.
                } // overlapping.

                // Anything to do in the adjusted region?
                if (rstop <= rstart) {
                    ok = false;
                    break;
                }
            } // Trimming.
            
            // Copy result into idxs.
            idxs.begin[i] = rstart;
            idxs.end[i] = rstop;
        }
        TRACE_MSG("shift_region: updated span: [" <<
                  idxs.begin.makeValStr() << " ... " <<
                  idxs.end.makeValStr() << ") within region base [" <<
                  base_start.makeValStr() << " ... " <<
                  base_stop.makeValStr() << ") shifted " <<
                  shift_num << " time(s) is " <<
                  (ok ? "not " : "") << "empty");
        return ok;
    }
    
    // For given 'phase' and 'shape', find boundaries within mini-block at
    // 'mb_base_start' to 'mb_base_stop' shifted by 'mb_shift_num', which
    // should start at 0 and increment for each pack in each time-step.
    // 'mb_base' is subset of 'adj_block_base'.  Also trim to block at
    // 'block_base_start' to 'block_base_stop' shifted by 'mb_shift_num'.
    // Input 'begin' and 'end' of 'idxs' should be trimmed to region.  Writes
    // results back into 'begin' and 'end' of 'idxs'.  Returns 'true' if
    // resulting area is non-empty, 'false' if empty.
    bool StencilContext::shift_mini_block(const Indices& mb_base_start,
                                          const Indices& mb_base_stop,
                                          const Indices& adj_block_base_start,
                                          const Indices& adj_block_base_stop,
                                          const Indices& block_base_start,
                                          const Indices& block_base_stop,
                                          const Indices& region_base_start,
                                          const Indices& region_base_stop,
                                          idx_t mb_shift_num,
                                          idx_t nphases, idx_t phase,
                                          idx_t nshapes, idx_t shape,
                                          const BridgeMask& bridge_mask,
                                          ScanIndices& idxs) {
        STATE_VARS(this);
        auto npacks = stPacks.size();
        bool ok = true;
        
        // Loop thru dims, breaking out if any dim has no work.
        DOMAIN_VAR_LOOP(i, j) {

            // Determine range of this block for current phase, shape, and
            // shift. For each dim, we'll first compute the L & R sides of
            // the base block and the L side of the next block.
            auto tb_angle = tb_angles[j];

            // Is this block first and/or last in region?
            bool is_first_blk = block_base_start[i] <= region_base_start[i];
            bool is_last_blk = block_base_stop[i] >= region_base_stop[i];

            // Is there only one blk in the region in this dim?
            bool is_one_blk = is_first_blk && is_last_blk;

            // Initial start and stop point of phase-0 block.
            idx_t blk_start = block_base_start[i];
            idx_t blk_stop = block_base_stop[i];

            // If more than one blk, adjust for base of phase-0 trapezoid.
            if (nphases > 1 && !is_one_blk)
                blk_stop = min(blk_start + tb_widths[j], block_base_stop[i]);

            // Starting point of the *next* block.  This is used to create
            // bridge shapes between blocks.  Initially, the beginning of
            // the next block is the end of this block.
            // TODO: split these parts more evenly when not full triangles.
            idx_t next_blk_start = block_base_stop[i];

            // Adjust these based on current shift.  Adjust by pts in one TB
            // step, reducing size on R & L sides.  But if block is first
            // and/or last, clamp to region.  TODO: have different R & L
            // angles. TODO: have different shifts for each pack.

            // Shift start to right unless first.  First block will be a
            // parallelogram or trapezoid clamped to beginning of region.
            blk_start += tb_angle * mb_shift_num;
            if (is_first_blk)
                blk_start = idxs.begin[i];

            // Shift stop to left. If there will be no bridges, clamp
            // last block to end of region.
            blk_stop -= tb_angle * mb_shift_num;
            if ((nphases == 1 || is_one_blk) && is_last_blk)
                blk_stop = idxs.end[i];

            // Shift start of next block. Last bridge will be
            // clamped to end of region.
            next_blk_start += tb_angle * mb_shift_num;
            if (is_last_blk)
                next_blk_start = idxs.end[i];

            // Use these 3 values to determine the beginning and end
            // of the current shape for the current phase.
            // For phase 0, limits are simply the base start and stop.
            idx_t shape_start = blk_start;
            idx_t shape_stop = blk_stop;

            // Depending on the phase and shape, create a bridge between
            // from RHS of base block to the LHS of the next block
            // until all dims are bridged at last phase.
            // Use list of dims to bridge for this shape
            // computed earlier.
            if (phase > 0 && bridge_mask[j]) {
                TRACE_MSG("shift_mini_block: phase " << phase <<
                          ", shape " << shape <<
                          ": bridging dim " << j);
                
                // Start at end of base block, but not
                // before start of block.
                shape_start = max(blk_stop, blk_start);
                
                // Stop at beginning of next block.
                shape_stop = next_blk_start;
            }
            
            // We now have bounds of this shape in shape_{start,stop}
            // for given phase and shift.
            if (shape_stop <= shape_start)
                ok = false;
            else {

                // Is this mini-block first and/or last in block?
                bool is_first_mb = mb_base_start[i] <= adj_block_base_start[i];
                bool is_last_mb = mb_base_stop[i] >= adj_block_base_stop[i];

                // Is there only one MB?
                bool is_one_mb = is_first_mb && is_last_mb;

                // Beginning and end of min-block.
                idx_t mb_start = mb_base_start[i];
                idx_t mb_stop = mb_base_stop[i];

                // Shift mini-block by MB angles unless there is only one.
                // MB is a wave-front, so only shift left.
                if (!is_one_mb) {
                    auto mb_angle = mb_angles[j];
                    mb_start -= mb_angle * mb_shift_num;
                    mb_stop -= mb_angle * mb_shift_num;
                }

                // Clamp first & last MB to shape boundaries.
                if (is_first_mb)
                    mb_start = shape_start;
                if (is_last_mb)
                    mb_stop = shape_stop;

                // Trim mini-block to fit in region.
                mb_start = max(mb_start, idxs.begin[i]);
                mb_stop = min(mb_stop, idxs.end[i]);
            
                // Trim mini-block range to fit in shape.
                mb_start = max(mb_start, shape_start);
                mb_stop = min(mb_stop, shape_stop);

                // Update 'idxs'.
                idxs.begin[i] = mb_start;
                idxs.end[i] = mb_stop;

                // No work to do?
                if (mb_stop <= mb_start)
                    ok = false;
            }
            if (!ok)
                break;

        } // dims.

        TRACE_MSG("shift_mini_block: phase " << phase << "/" << nphases <<
                  ", shape " << shape << "/" << nshapes <<
                  ", updated span: [" <<
                  idxs.begin.makeValStr() << " ... " <<
                  idxs.end.makeValStr() << ") from original mini-block [" <<
                  mb_base_start.makeValStr() << " ... " <<
                  mb_base_stop.makeValStr() << ") shifted " <<
                  mb_shift_num << " time(s) within adj-block base [" <<
                  adj_block_base_start.makeValStr() << " ... " <<
                  adj_block_base_stop.makeValStr() << ") and actual block base [" <<
                  block_base_start.makeValStr() << " ... " <<
                  block_base_stop.makeValStr() << ") and region base [" <<
                  region_base_start.makeValStr() << " ... " <<
                  region_base_stop.makeValStr() << ") is " <<
                  (ok ? "not " : "") << "empty");
        return ok;
    }
    
    // Adjust offsets of scratch vars based on thread number 'thread_idx'
    // and beginning point of mini-block 'idxs'.  Each scratch-var is
    // assigned to a thread, so it must "move around" as the thread is
    // assigned to each mini-block.  This move is accomplished by changing
    // the vars' local offsets.
    void StencilContext::update_scratch_var_info(int thread_idx,
                                                  const Indices& idxs) {
        STATE_VARS(this);

        // Loop thru vecs of scratch vars.
        for (auto* sv : scratchVecs) {
            assert(sv);

            // Get ptr to the scratch var for this thread.
            auto& gp = sv->at(thread_idx);
            assert(gp);
            auto& gb = gp->gb();
            assert(gb.is_scratch());

            // i: index for stencil dims, j: index for domain dims.
            DOMAIN_VAR_LOOP(i, j) {

                auto& dim = stencil_dims.getDim(i);
                auto& dname = dim.getName();

                // Is this dim used in this var?
                int posn = gb.get_dim_posn(dname);
                if (posn >= 0) {

                    // Set rank offset of var based on starting point of rank.
                    // Thus, it it not necessarily a vec mult.
                    auto rofs = rank_domain_offsets[j];
                    gp->_set_rank_offset(posn, rofs);

                    // Must use the vector len in this var, which may
                    // not be the same as vec_lens[posn] because var
                    // may not be vectorized.
                    auto vlen = gp->_get_vec_len(posn);
                    
                    // See diagram in yk_var defn.  Local offset is the
                    // offset of this var relative to the beginning of the
                    // current rank.  Set local offset to diff between
                    // global offset and rank offset.  Round down to make
                    // sure it's vec-aligned.
                    auto lofs = round_down_flr(idxs[i] - rofs, vlen);
                    gp->_set_local_offset(posn, lofs);
                }
            }
        }
    }

    // Compare vars in contexts.
    // Return number of mis-compares.
    idx_t StencilContext::compareData(const StencilContext& ref) const {
        STATE_VARS_CONST(this);

        DEBUG_MSG("Comparing var(s) in '" << name << "' to '" << ref.name << "'...");
        if (varPtrs.size() != ref.varPtrs.size()) {
            TRACE_MSG("** number of vars not equal");
            return 1;
        }
        idx_t errs = 0;
        for (size_t gi = 0; gi < varPtrs.size(); gi++) {
            TRACE_MSG("Var '" << ref.varPtrs[gi]->get_name() << "'...");
            auto& gb = varPtrs[gi]->gb();
            auto* rgbp = ref.varPtrs[gi]->gbp();
            errs += gb.compare(rgbp);
        }

        return errs;
    }

    // Call MPI_Test() on all unfinished requests to promote MPI progress.
    // TODO: replace with more direct and less intrusive techniques.
    void StencilContext::poke_halo_exchange() {
        STATE_VARS(this);

#ifdef USE_MPI
        if (!enable_halo_exchange || env->num_ranks < 2)
            return;

        test_time.start();
        TRACE_MSG("poke_halo_exchange");

        // Loop thru MPI data.
        int num_tests = 0;
        for (auto& mdi : mpiData) {
            auto& gname = mdi.first;
            auto& var_mpi_data = mdi.second;
            MPI_Request* var_recv_reqs = var_mpi_data.recv_reqs.data();
            MPI_Request* var_send_reqs = var_mpi_data.send_reqs.data();

            int flag;
#if 1
            int indices[max(var_mpi_data.recv_reqs.size(), var_mpi_data.send_reqs.size())];
            MPI_Testsome(int(var_mpi_data.recv_reqs.size()), var_recv_reqs, &flag, indices, MPI_STATUS_IGNORE);
            MPI_Testsome(int(var_mpi_data.send_reqs.size()), var_send_reqs, &flag, indices, MPI_STATUS_IGNORE);
#elif 0
            int index;
            MPI_Testany(int(var_mpi_data.recv_reqs.size()), var_recv_reqs, &index, &flag, MPI_STATUS_IGNORE);
            MPI_Testany(int(var_mpi_data.send_reqs.size()), var_send_reqs, &index, &flag, MPI_STATUS_IGNORE);
#else
            for (size_t i = 0; i < var_mpi_data.recv_reqs.size(); i++) {
                auto& r = var_recv_reqs[i];
                if (r != MPI_REQUEST_NULL) {
                    //TRACE_MSG(gname << " recv test &MPI_Request = " << &r);
                    MPI_Test(&r, &flag, MPI_STATUS_IGNORE);
                    num_tests++;
                    if (flag)
                        r = MPI_REQUEST_NULL;
                }
            }
            for (size_t i = 0; i < var_mpi_data.send_reqs.size(); i++) {
                auto& r = var_send_reqs[i];
                if (r != MPI_REQUEST_NULL) {
                    //TRACE_MSG(gname << " send test &MPI_Request = " << &r);
                    MPI_Test(&r, &flag, MPI_STATUS_IGNORE);
                    num_tests++;
                    if (flag)
                        r = MPI_REQUEST_NULL;
                }
            }
#endif
        }
        auto ttime = test_time.stop();
        TRACE_MSG("poke_halo_exchange: secs spent in " << num_tests <<
                  " MPI test(s): " << makeNumStr(ttime));
#endif
    }

    // Exchange dirty halo data for all vars and all steps.
    void StencilContext::exchange_halos() {
        
#ifdef USE_MPI
        STATE_VARS(this);
        if (!enable_halo_exchange || env->num_ranks < 2)
            return;

        halo_time.start();
        double wait_delta = 0.;
        TRACE_MSG("exchange_halos");
        if (is_overlap_active()) {
            if (do_mpi_left)
                TRACE_MSG(" following calc of MPI left exterior");
            if (do_mpi_right)
                TRACE_MSG(" following calc of MPI right exterior");
            if (do_mpi_interior)
                TRACE_MSG(" following calc of MPI interior");
        }
        
        // Vars for list of vars that need to be swapped and their step
        // indices.  Use an ordered map by *name* to make sure vars are
        // swapped in same order on all ranks. (If we order vars by
        // pointer, pointer values will not generally be the same on each
        // rank.)
        VarPtrMap varsToSwap;
        map<YkVarPtr, idx_t> firstStepsToSwap;
        map<YkVarPtr, idx_t> lastStepsToSwap;

        // Loop thru all vars.
        for (auto& gp : varPtrs) {
            auto& gb = gp->gb();

            // Don't swap scratch vars.
            if (gb.is_scratch())
                continue;

            // Only need to swap vars that have any MPI buffers.
            auto& gname = gp->get_name();
            if (mpiData.count(gname) == 0)
                continue;

            // Check all allocated step indices.
            // Use '0' for vars that don't use the step dim.
            idx_t start_t = 0, stop_t = 1;
            if (gp->is_dim_used(step_dim)) {
                start_t = gp->get_first_valid_step_index();
                stop_t = gp->get_last_valid_step_index() + 1;
            }
            for (idx_t t = start_t; t < stop_t; t++) {
                            
                // Only need to swap vars whose halos are not up-to-date
                // for this step.
                if (!gb.is_dirty(t))
                    continue;

                // Swap this var.
                varsToSwap[gname] = gp;

                // Update first step.
                if (firstStepsToSwap.count(gp) == 0 || t < firstStepsToSwap[gp])
                    firstStepsToSwap[gp] = t;

                // Update last step.
                if (lastStepsToSwap.count(gp) == 0 || t > lastStepsToSwap[gp])
                    lastStepsToSwap[gp] = t;

            } // steps.
        } // vars.
        TRACE_MSG("exchange_halos: need to exchange halos for " <<
                  varsToSwap.size() << " var(s)");
        assert(varsToSwap.size() == firstStepsToSwap.size());
        assert(varsToSwap.size() == lastStepsToSwap.size());

        // Sequence of things to do for each neighbor.
        enum halo_steps { halo_irecv, halo_pack_isend, halo_unpack, halo_final };
        vector<halo_steps> steps_to_do;

        // Flags indicate what part of vars were most recently calc'd.
        // These determine what exchange steps need to be done now.
        if (varsToSwap.size()) {
            if (do_mpi_left || do_mpi_right) {
                steps_to_do.push_back(halo_irecv);
                steps_to_do.push_back(halo_pack_isend);
            }
            if (do_mpi_interior) {
                steps_to_do.push_back(halo_unpack);
                steps_to_do.push_back(halo_final);
            }
        }

        int num_send_reqs = 0;
        int num_recv_reqs = 0;
        for (auto halo_step : steps_to_do) {

            if (halo_step == halo_irecv)
                TRACE_MSG("exchange_halos: requesting data phase");
            else if (halo_step == halo_pack_isend)
                TRACE_MSG("exchange_halos: packing and sending data phase");
            else if (halo_step == halo_unpack)
                TRACE_MSG("exchange_halos: waiting for and unpacking data phase");
            else if (halo_step == halo_final)
                TRACE_MSG("exchange_halos: waiting for send to finish phase");
            else
                THROW_YASK_EXCEPTION("internal error: unknown halo-exchange step");

            // Loop thru all vars to swap.
            // Use 'gi' as an MPI tag.
            int gi = 0;
            for (auto gtsi : varsToSwap) {
                gi++;
                auto& gname = gtsi.first;
                auto& gp = gtsi.second;
                auto& gb = gp->gb();
                auto& var_mpi_data = mpiData.at(gname);
                MPI_Request* var_recv_reqs = var_mpi_data.recv_reqs.data();
                MPI_Request* var_send_reqs = var_mpi_data.send_reqs.data();

                // Loop thru all this rank's neighbors.
                var_mpi_data.visitNeighbors
                    ([&](const IdxTuple& offsets, // NeighborOffset.
                         int neighbor_rank,
                         int ni, // unique neighbor index.
                         MPIBufs& bufs) {
                        auto& sendBuf = bufs.bufs[MPIBufs::bufSend];
                        auto& recvBuf = bufs.bufs[MPIBufs::bufRecv];
                        TRACE_MSG("exchange_halos:   with rank " << neighbor_rank << " at relative position " <<
                                  offsets.subElements(1).makeDimValOffsetStr());

                        // Are we using MPI shm w/this neighbor?
                        bool using_shm = opts->use_shm && mpiInfo->shm_ranks.at(ni) != MPI_PROC_NULL;

                        // Submit async request to receive data from neighbor.
                        if (halo_step == halo_irecv) {
                            auto nbytes = recvBuf.get_bytes();
                            if (nbytes) {
                                if (using_shm)
                                    TRACE_MSG("exchange_halos:    no receive req due to shm");
                                else {
                                    void* buf = (void*)recvBuf._elems;
                                    TRACE_MSG("exchange_halos:    requesting up to " << makeByteStr(nbytes));
                                    auto& r = var_recv_reqs[ni];
                                    MPI_Irecv(buf, nbytes, MPI_BYTE,
                                              neighbor_rank, int(gi),
                                              env->comm, &r);
                                    num_recv_reqs++;
                                }
                            }
                            else
                                TRACE_MSG("exchange_halos:    0B to request");
                        }

                        // Pack data into send buffer, then send to neighbor.
                        else if (halo_step == halo_pack_isend) {
                            auto nbytes = sendBuf.get_bytes();
                            if (nbytes) {

                                // Vec ok?
                                // Domain sizes must be ok, and buffer size must be ok
                                // as calculated when buffers were created.
                                bool send_vec_ok = allow_vec_exchange && sendBuf.vec_copy_ok;

                                // Get first and last ranges.
                                IdxTuple first = sendBuf.begin_pt;
                                IdxTuple last = sendBuf.last_pt;

                                // The code in allocMpiData() pre-calculated the first and
                                // last points of each buffer, except in the step dim, where
                                // the max range was set. Update actual range now.
                                if (gp->is_dim_used(step_dim)) {
                                    first.setVal(step_dim, firstStepsToSwap[gp]);
                                    last.setVal(step_dim, lastStepsToSwap[gp]);
                                }

                                // Wait until buffer is avail.
                                if (using_shm) {
                                    TRACE_MSG("exchange_halos:    waiting to write to shm buffer");
                                    wait_time.start();
                                    sendBuf.wait_for_ok_to_write();
                                    wait_delta += wait_time.stop();
                                }
                                
                                // Copy (pack) data from var to buffer.
                                void* buf = (void*)sendBuf._elems;
                                idx_t nelems = 0;
                                TRACE_MSG("exchange_halos:    packing [" << first.makeDimValStr() <<
                                          " ... " << last.makeDimValStr() << "] " <<
                                          (send_vec_ok ? "with" : "without") <<
                                          " vector copy into " << buf);
                                if (send_vec_ok)
                                    nelems = gp->get_vecs_in_slice(buf, first, last);
                                else
                                    nelems = gp->get_elements_in_slice(buf, first, last);
                                idx_t nbytes = nelems * get_element_bytes();

                                if (using_shm) {
                                    TRACE_MSG("exchange_halos:    no send req due to shm");
                                    sendBuf.mark_write_done();
                                }
                                else {

                                    // Send packed buffer to neighbor.
                                    assert(nbytes <= sendBuf.get_bytes());
                                    TRACE_MSG("exchange_halos:    sending " << makeByteStr(nbytes));
                                    auto& r = var_send_reqs[ni];
                                    MPI_Isend(buf, nbytes, MPI_BYTE,
                                              neighbor_rank, int(gi), env->comm, &r);
                                    num_send_reqs++;
                                }
                            }
                            else
                                TRACE_MSG("   0B to send");
                        }

                        // Wait for data from neighbor, then unpack it.
                        else if (halo_step == halo_unpack) {
                            auto nbytes = recvBuf.get_bytes();
                            if (nbytes) {

                                // Wait until buffer is avail.
                                if (using_shm) {
                                    TRACE_MSG("exchange_halos:    waiting to read from shm buffer");
                                    wait_time.start();
                                    recvBuf.wait_for_ok_to_read();
                                    wait_delta += wait_time.stop();
                                }
                                else {

                                    // Wait for data from neighbor before unpacking it.
                                    auto& r = var_recv_reqs[ni];
                                    if (r != MPI_REQUEST_NULL) {
                                        TRACE_MSG("   waiting for receipt of " << makeByteStr(nbytes));
                                        wait_time.start();
                                        MPI_Wait(&r, MPI_STATUS_IGNORE);
                                        wait_delta += wait_time.stop();
                                    }
                                    r = MPI_REQUEST_NULL;
                                }

                                // Vec ok?
                                bool recv_vec_ok = allow_vec_exchange && recvBuf.vec_copy_ok;

                                // Get first and last ranges.
                                IdxTuple first = recvBuf.begin_pt;
                                IdxTuple last = recvBuf.last_pt;

                                // Set step val as above.
                                if (gp->is_dim_used(step_dim)) {
                                    first.setVal(step_dim, firstStepsToSwap[gp]);
                                    last.setVal(step_dim, lastStepsToSwap[gp]);
                                }

                                // Copy data from buffer to var.
                                void* buf = (void*)recvBuf._elems;
                                idx_t nelems = 0;
                                TRACE_MSG("exchange_halos:    got data; unpacking into [" << first.makeDimValStr() <<
                                          " ... " << last.makeDimValStr() << "] " <<
                                          (recv_vec_ok ? "with" : "without") <<
                                          " vector copy from " << buf);
                                if (recv_vec_ok)
                                    nelems = gp->set_vecs_in_slice(buf, first, last);
                                else
                                    nelems = gp->set_elements_in_slice(buf, first, last);
                                assert(nelems <= recvBuf.get_size());

                                if (using_shm)
                                    recvBuf.mark_read_done();
                            }
                            else
                                TRACE_MSG("exchange_halos:    0B to wait for");
                        }

                        // Final steps.
                        else if (halo_step == halo_final) {
                            auto nbytes = sendBuf.get_bytes();
                            if (nbytes) {

                                if (using_shm)
                                    TRACE_MSG("exchange_halos:    no send wait due to shm");
                                else {

                                    // Wait for send to finish.
                                    // TODO: consider using MPI_WaitAll.
                                    // TODO: strictly, we don't have to wait on the
                                    // send to finish until we want to reuse this buffer,
                                    // so we could wait on the *previous* send right before
                                    // doing another one.
                                    auto& r = var_send_reqs[ni];
                                    if (r != MPI_REQUEST_NULL) {
                                        TRACE_MSG("   waiting to finish send of " << makeByteStr(nbytes));
                                        wait_time.start();
                                        MPI_Wait(&var_send_reqs[ni], MPI_STATUS_IGNORE);
                                        wait_delta += wait_time.stop();
                                    }
                                    r = MPI_REQUEST_NULL;
                                }
                            }

                            // Mark vars as up-to-date when done.
                            for (idx_t si = firstStepsToSwap[gp]; si <= lastStepsToSwap[gp]; si++) {
                                if (gb.is_dirty(si)) {
                                    gb.set_dirty(false, si);
                                    TRACE_MSG("exchange_halos: var '" << gname <<
                                              "' marked as clean at step-index " << si);
                                }
                            }
                        }
                            
                    }); // visit neighbors.

            } // vars.

        } // exchange sequence.

        TRACE_MSG("exchange_halos: " << num_recv_reqs << " MPI receive request(s) issued");
        TRACE_MSG("exchange_halos: " << num_send_reqs << " MPI send request(s) issued");

        auto mpi_call_time = halo_time.stop();
        TRACE_MSG("exchange_halos: secs spent in MPI waits: " << makeNumStr(wait_delta));
        TRACE_MSG("exchange_halos: secs spent in this call: " << makeNumStr(mpi_call_time));
#endif
    }

    // Update data in vars that have been written to by bundle pack 'sel_bp'.
    void StencilContext::update_vars(const BundlePackPtr& sel_bp,
                                      idx_t start, idx_t stop,
                                      bool mark_dirty) {
        STATE_VARS(this);
        idx_t stride = (start > stop) ? -1 : 1;
        map<YkVarPtr, set<idx_t>> vars_done;

        // Stencil bundle packs.
        for (auto& bp : stPacks) {

            // Not a selected bundle pack?
            if (sel_bp && sel_bp != bp)
                continue;

            // Each input step.
            for (idx_t t = start; t != stop; t += stride) {

                // Each bundle in this pack.
                for (auto* sb : *bp) {

                    // Get output step for this bundle, if any.
                    // For many stencils, this will be t+1 or
                    // t-1 if striding backward.
                    idx_t t_out = 0;
                    if (!sb->get_output_step_index(t, t_out))
                        continue;

                    // Output vars for this bundle.  NB: don't need to mark
                    // scratch vars as dirty because they are never exchanged.
                    for (auto gp : sb->outputVarPtrs) {
                        auto& gb = gp->gb();

                        // Update if not already done.
                        if (vars_done[gp].count(t_out) == 0) {
                            gb.update_valid_step(t_out);
                            if (mark_dirty)
                                gb.set_dirty(true, t_out);
                            TRACE_MSG("var '" << gp->get_name() <<
                                      "' updated at step " << t_out);
                            vars_done[gp].insert(t_out);
                        }
                    }
                } // bundles.
            } // steps.
        } // packs.
    } // update_vars().

    // Reset any locks, etc.
    void StencilContext::reset_locks() {

        // MPI buffer locks.
        for (auto& mdi : mpiData) {
            auto& md = mdi.second;
            md.reset_locks();
        }
    }

} // namespace yask.
