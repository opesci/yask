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

// This file contains implementations of bundle and pack methods.
// Also see context_setup.cpp.

#include "yask_stencil.hpp"
using namespace std;

namespace yask {

    // Calculate results within a mini-block defined by 'mini_block_idxs'.
    // This is called by StencilContext::calc_mini_block() for each bundle.
    // It is here that any required scratch-var stencils are evaluated
    // first and then the non-scratch stencils in the stencil bundle.
    // It is also here that the boundaries of the bounding-box(es) of the bundle
    // are respected. There must not be any temporal blocking at this point.
    void StencilBundleBase::calc_mini_block(int region_thread_idx,
                                            KernelSettings& settings,
                                            const ScanIndices& mini_block_idxs) {
        STATE_VARS(this);
        TRACE_MSG("calc_mini_block('" << get_name() << "'): [" <<
                   mini_block_idxs.begin.makeValStr() << " ... " <<
                   mini_block_idxs.end.makeValStr() << ") by " <<
                   mini_block_idxs.stride.makeValStr() <<
                   " by region thread " << region_thread_idx);
        assert(!is_scratch());

        // No TB allowed here.
#ifdef CHECK
        idx_t begin_t = mini_block_idxs.begin[step_posn];
        idx_t end_t = mini_block_idxs.end[step_posn];
        assert(abs(end_t - begin_t) == 1);
#endif

        // Nothing to do if outer BB is empty.
        if (_bundle_bb.bb_num_points == 0) {
            TRACE_MSG("calc_mini_block: empty BB");
            return;
        }
        
        // TODO: if >1 BB, check limits of outer one first to save time.

        // Lookup some thread-binding info.
        int bind_posn = settings._bind_posn;
        string bind_dim;
        idx_t bind_slab_pts = 1;
        if (settings.bind_block_threads) {
            bind_dim = stencil_dims.getDimName(bind_posn);
            bind_slab_pts = settings._sub_block_sizes[bind_posn];
        }
        
        // Loop through each solid BB for this bundle.
        // For each BB, calc intersection between it and 'mini_block_idxs'.
        // If this is non-empty, apply the bundle to all its required sub-blocks.
        TRACE_MSG("calc_mini_block('" << get_name() << "'): checking " <<
                   _bb_list.size() << " BB(s)");
        int bbn = 0;
  	for (auto& bb : _bb_list) {
            bbn++;
            bool bb_ok = true;
            if (bb.bb_num_points == 0)
                bb_ok = false;

            // Trim the mini-block indices based on the bounding box(es)
            // for this bundle.
            ScanIndices mb_idxs(mini_block_idxs);
            DOMAIN_VAR_LOOP(i, j) {

                // Begin point.
                auto bbegin = max(mini_block_idxs.begin[i], bb.bb_begin[j]);
                mb_idxs.begin[i] = bbegin;

                // End point.
                auto bend = min(mini_block_idxs.end[i], bb.bb_end[j]);
                mb_idxs.end[i] = bend;
		
                // Anything to do?
                if (bend <= bbegin) {
                    bb_ok = false;
                    break;
                }
            }

            // nothing to do?
            if (!bb_ok) {
                TRACE_MSG("calc_mini_block for bundle '" << get_name() <<
                           "': no overlap between bundle " << bbn << " and current block");
                continue; // to next BB.
            }
            
            TRACE_MSG("calc_mini_block('" << get_name() <<
                       "'): after trimming for BB " << bbn << ": [" <<
                       mb_idxs.begin.makeValStr() <<
                       " ... " << mb_idxs.end.makeValStr() << ")");

            // Get the bundles that need to be processed in
            // this block. This will be any prerequisite scratch-var
            // bundles plus this non-scratch bundle.
            auto sg_list = get_reqd_bundles();

            // Loop through all the needed bundles.
            for (auto* sg : sg_list) {

                // Start threads within a block.  Each of these threads will
                // eventually work on a separate sub-block.  This is nested within
                // an OMP region thread.  If there is only one block per thread,
                // nested OMP is disabled, and this OMP pragma does nothing.
                int nbt = _context->set_block_threads();
                bool bind_threads = nbt > 1 && settings.bind_block_threads;
                _Pragma("omp parallel proc_bind(spread)") {
                    int block_thread_idx = 0;
                    if (nbt > 1) {
                        assert(omp_get_level() == 2);
                        assert(omp_get_num_threads() == nbt);
                        block_thread_idx = omp_get_thread_num();
                    }
                    
                    // Indices needed for the generated loops.  Will normally be a
                    // copy of 'mb_idxs' except when updating scratch-vars.
                    ScanIndices adj_mb_idxs = sg->adjust_span(region_thread_idx, mb_idxs);

                    // Tweak settings for adjusted indices.
                    DOMAIN_VAR_LOOP(i, j) {

                        // If binding threads to sub-blocks and this is the
                        // binding dim, set stride size and alignment
                        // granularity to the slab width. Setting the
                        // alignment keeps slabs aligned between packs.
                        if (bind_threads && i == bind_posn) {
                            adj_mb_idxs.stride[i] = bind_slab_pts;
                            adj_mb_idxs.align[i] = bind_slab_pts;
                        }

                        // If original [or auto-tuned] sub-block covers
                        // entire mini-block, set stride size to full width.
                        // Also do this when binding and this is not the
                        // binding dim.
                        else if ((settings._sub_block_sizes[i] >= settings._mini_block_sizes[i]) ||
                                 bind_threads)
                            adj_mb_idxs.stride[i] = adj_mb_idxs.end[i] - adj_mb_idxs.begin[i];
                    }

                    TRACE_MSG("calc_mini_block('" << get_name() << "'): " <<
                               " for reqd bundle '" << sg->get_name() << "': [" <<
                               adj_mb_idxs.begin.makeValStr() << " ... " <<
                               adj_mb_idxs.end.makeValStr() << ") by " <<
                               adj_mb_idxs.stride.makeValStr() <<
                               " by region thread " << region_thread_idx <<
                               " and block thread " << block_thread_idx);

                    // If binding threads to data, run the mini-block
                    // loops on all block threads and call calc_sub_block()
                    // only by the designated thread for the given slab
                    // index in the binding dim.
                    if (bind_threads) {
                        const idx_t idx_ofs = 0x1000; // to help keep pattern when idx is neg.

                        // Disable the OpenMP construct in the mini-block loop.
#define OMP_PRAGMA
#define CALC_SUB_BLOCK(mb_idxs)                                        \
                        auto bind_elem_idx = mb_idxs.start[bind_posn];  \
                        auto bind_slab_idx = idiv_flr(bind_elem_idx + idx_ofs, bind_slab_pts); \
                        auto bind_thr = imod_flr<idx_t>(bind_slab_idx, nbt); \
                        if (block_thread_idx == bind_thr)               \
                            sg->calc_sub_block(region_thread_idx, block_thread_idx, settings, mb_idxs)
#include "yask_mini_block_loops.hpp"
#undef CALC_SUB_BLOCK
#undef OMP_PRAGMA
                    }

                    // If not binding threads to data, call calc_sub_block()
                    // with a different thread for each sub-block using
                    // standard OpenMP scheduling.
                    else {
#define CALC_SUB_BLOCK(mb_idxs)                                         \
                        sg->calc_sub_block(region_thread_idx, block_thread_idx, settings, mb_idxs)
#include "yask_mini_block_loops.hpp"
#undef CALC_SUB_BLOCK
                    }

                } // OMP parallel.
            } // bundles.
        } // BB list.
    }

    // Calculate results for one sub-block using pure scalar code.
    // This is for debug.
    void StencilBundleBase::calc_sub_block_scalar(int region_thread_idx,
                                                  int block_thread_idx,
                                                  KernelSettings& settings,
                                                  const ScanIndices& mini_block_idxs) {
        STATE_VARS(this);
        TRACE_MSG("calc_sub_block_scalar for bundle '" << get_name() << "': [" <<
                   mini_block_idxs.start.makeValStr() <<
                   " ... " << mini_block_idxs.stop.makeValStr() <<
                   ") by region thread " << region_thread_idx <<
                   " and block thread " << block_thread_idx);

        // Init sub-block begin & end from block start & stop indices.
        // Use the 'misc' loops. Indices for these loops will be scalar and
        // global rather than normalized as in the cluster and vector loops.
        ScanIndices misc_idxs(*dims, true);
        misc_idxs.initFromOuter(mini_block_idxs);
        
        // Stride sizes and alignment are one element.
        misc_idxs.stride.setFromConst(1);
        misc_idxs.align.setFromConst(1);

        // Define misc-loop function.
        // Since stride is always 1, we ignore misc_idxs.stop.
#define MISC_FN(pt_idxs)  do {                                          \
            calc_scalar(region_thread_idx, pt_idxs.start);              \
        } while(0)

        // Scan through n-D space.
        // The OMP in the misc loops will be ignored if we're already in
        // the max allowed nested OMP region.
#include "yask_misc_loops.hpp"
#undef MISC_FN
    }

    // Calculate results for one sub-block.
    // The index ranges in 'mini_block_idxs' are sub-divided
    // into full vector-clusters, full vectors, and sub-vectors
    // and finally evaluated by the YASK-compiler-generated loops.
    void StencilBundleBase::calc_sub_block_vec(int region_thread_idx,
                                               int block_thread_idx,
                                               KernelSettings& settings,
                                               const ScanIndices& mini_block_idxs) {
        STATE_VARS(this);
        TRACE_MSG("calc_sub_block_vec for bundle '" << get_name() << "': [" <<
                   mini_block_idxs.start.makeValStr() <<
                   " ... " << mini_block_idxs.stop.makeValStr() <<
                   ") by region thread " << region_thread_idx <<
                   " and block thread " << block_thread_idx);

        /*
          Indices in each domain dim:

          sub_block_eidxs.begin                        rem_masks used here
          | peel_masks used here                       | sub_block_eidxs.end
          | |                                          | |
          v v                                          v v
          |---+-------+---------------------------+---+---|   "+" => vec boundaries.
          ^   ^       ^                            ^   ^   ^
          |   |       |                            |   |   |
          |   |       sub_block_fcidxs.begin       |   |   sub_block_vidxs.end
          |   sub_block_fvidxs.begin               |   sub_block_fvidxs.end
          sub_block_vidxs.begin                    sub_block_fcidxs.end
        */

        // Init sub-block begin & end from block start & stop indices.
        // These indices are in element units and global (NOT rank-relative).
        ScanIndices sub_block_idxs(*dims, true);
        sub_block_idxs.initFromOuter(mini_block_idxs);

        // Sub block indices in element units and rank-relative.
        ScanIndices sub_block_eidxs(sub_block_idxs);

        // Subset of sub-block that is full clusters.
        // These indices are in element units and rank-relative.
        ScanIndices sub_block_fcidxs(sub_block_idxs);

        // Subset of sub-block that is full vectors.
        // These indices are in element units and rank-relative.
        ScanIndices sub_block_fvidxs(sub_block_idxs);

        // Superset of sub-block that is full or partial (masked) vectors.
        // These indices are in element units and rank-relative.
        ScanIndices sub_block_vidxs(sub_block_idxs);

        // These will be set to rank-relative, so set ofs to zero.
        sub_block_eidxs.align_ofs.setFromConst(0);
        sub_block_fcidxs.align_ofs.setFromConst(0);
        sub_block_fvidxs.align_ofs.setFromConst(0);
        sub_block_vidxs.align_ofs.setFromConst(0);

        // Masks for computing partial vectors in each dim.
        // Init to all-ones (no masking).
        Indices peel_masks(nsdims), rem_masks(nsdims);
        peel_masks.setFromConst(-1);
        rem_masks.setFromConst(-1);

        // Flags that indicate what type of processing needs to be done.
        bool do_clusters = true; // any clusters to do?
        bool do_vectors = false; // any vectors to do?
        bool do_scalars = false; // any scalars to do?

        // Adjust indices to be rank-relative.
        // Determine the subset of this sub-block that is
        // clusters, vectors, and partial vectors.
        DOMAIN_VAR_LOOP(i, j) {

            // Rank offset.
            auto rofs = _context->rank_domain_offsets[j];

            // Begin/end of rank-relative scalar elements in this dim.
            auto ebgn = sub_block_idxs.begin[i] - rofs;
            auto eend = sub_block_idxs.end[i] - rofs;
            sub_block_eidxs.begin[i] = ebgn;
            sub_block_eidxs.end[i] = eend;

            // Find range of full clusters.
            // Note that fcend <= eend because we round
            // down to get whole clusters only.
            // Similarly, fcbgn >= ebgn.
            auto cpts = dims->_cluster_pts[j];
            auto fcbgn = round_up_flr(ebgn, cpts);
            auto fcend = round_down_flr(eend, cpts);
            sub_block_fcidxs.begin[i] = fcbgn;
            sub_block_fcidxs.end[i] = fcend;

            // Any clusters to do?
            if (fcend <= fcbgn)
                do_clusters = false;

            // If anything before or after clusters, continue with
            // setting vector indices and peel/rem masks.
            if (fcbgn > ebgn || fcend < eend) {

                // Find range of full and/or partial vectors.
                // Note that fvend <= eend because we round
                // down to get whole vectors only.
                // Note that vend >= eend because we round
                // up to include partial vectors.
                // Similar but opposite for begin vars.
                // We make a vector mask to pick the
                // right elements.
                auto vpts = fold_pts[j];
                auto fvbgn = round_up_flr(ebgn, vpts);
                auto fvend = round_down_flr(eend, vpts);
                auto vbgn = round_down_flr(ebgn, vpts);
                auto vend = round_up_flr(eend, vpts);
                if (i == inner_posn) {

                    // Don't do any full and/or partial vectors in plane of
                    // inner domain dim.  We'll do these with scalars.  This
                    // should be unusual because vector folding is normally
                    // done in a plane perpendicular to the inner dim for >=
                    // 2D domains.
                    fvbgn = vbgn = fcbgn;
                    fvend = vend = fcend;
                }
                sub_block_fvidxs.begin[i] = fvbgn;
                sub_block_fvidxs.end[i] = fvend;
                sub_block_vidxs.begin[i] = vbgn;
                sub_block_vidxs.end[i] = vend;

                // Any vectors to do (full and/or partial)?
                if (vbgn < fcbgn || vend > fcend)
                    do_vectors = true;

                // Calculate masks in this dim for partial vectors.
                // All such masks will be ANDed together to form the
                // final masks over all domain dims.
                // Example: assume folding is x=4*y=4.
                // Possible 'x' peel mask to exclude 1st 2 cols:
                //   0 0 1 1
                //   0 0 1 1
                //   0 0 1 1
                //   0 0 1 1
                // Possible 'y' peel mask to exclude 1st row:
                //   0 0 0 0
                //   1 1 1 1
                //   1 1 1 1
                //   1 1 1 1
                // Along 'x' face, the 'x' peel mask is used.
                // Along 'y' face, the 'y' peel mask is used.
                // Along an 'x-y' edge, they are ANDed to make this mask:
                //   0 0 0 0
                //   0 0 1 1
                //   0 0 1 1
                //   0 0 1 1
                // so that the 6 corner elements are updated.

                if (vbgn < fvbgn || vend > fvend) {
                    idx_t pmask = 0, rmask = 0;

                    // Need to set upper bit.
                    idx_t mbit = 0x1 << (dims->_fold_pts.product() - 1);

                    // Visit points in a vec-fold to set bits for this dim's
                    // masks per the diagram above.  TODO: make this more
                    // efficient.
                    dims->_fold_pts.visitAllPoints
                        ([&](const IdxTuple& pt, size_t idx) {

                            // Shift masks to next posn.
                            pmask >>= 1;
                            rmask >>= 1;

                            // If the peel point is within the sub-block,
                            // set the next bit in the mask.
                            idx_t pi = vbgn + pt[j];
                            if (pi >= ebgn)
                                pmask |= mbit;

                            // If the rem point is within the sub-block,
                            // put a 1 in the mask.
                            pi = fvend + pt[j];
                            if (pi < eend)
                                rmask |= mbit;

                            // Keep visiting.
                            return true;
                        });

                    // Save masks in this dim.
                    peel_masks[i] = pmask;
                    rem_masks[i] = rmask;
                }

                // Anything not covered?
                // This will only be needed in inner dim because we
                // will do partial vectors in other dims.
                if (i == inner_posn && (ebgn < vbgn || eend > vend))
                    do_scalars = true;
            }

            // If no peel or rem, just set vec indices to same as
            // full cluster.
            else {
                sub_block_fvidxs.begin[i] = fcbgn;
                sub_block_fvidxs.end[i] = fcend;
                sub_block_vidxs.begin[i] = fcbgn;
                sub_block_vidxs.end[i] = fcend;
            }
        }
            
        // Normalized indices needed for sub-block loop.
        ScanIndices norm_sub_block_idxs(sub_block_eidxs);

        // Normalize the cluster indices.
        // These will be the bounds of the sub-block loops.
        // Set both begin/end and start/stop to ensure start/stop
        // vars get passed through to calc_loop_of_clusters()
        // for the inner loop.
        normalize_indices(sub_block_fcidxs.begin, norm_sub_block_idxs.begin);
        norm_sub_block_idxs.start = norm_sub_block_idxs.begin;
        normalize_indices(sub_block_fcidxs.end, norm_sub_block_idxs.end);
        norm_sub_block_idxs.stop = norm_sub_block_idxs.end;
        norm_sub_block_idxs.align.setFromConst(1); // one vector.

        // Full rectilinear polytope of aligned clusters: use optimized code.
        if (do_clusters) {
            TRACE_MSG("calc_sub_block_vec:  using cluster code for [" <<
                       sub_block_fcidxs.begin.makeValStr() <<
                       " ... " << sub_block_fcidxs.end.makeValStr() <<
                       ") by region thread " << region_thread_idx <<
                       " and block thread " << block_thread_idx);

            // Stride sizes are based on cluster lengths (in vector units).
            // The stride in the inner loop is hard-coded in the generated code.
            DOMAIN_VAR_LOOP(i, j) {
                norm_sub_block_idxs.stride[i] = dims->_cluster_mults[j]; // N vecs.
            }

            // Define the function called from the generated loops to simply
            // call the loop-of-clusters functions.
#define CALC_INNER_LOOP(loop_idxs) \
            calc_loop_of_clusters(region_thread_idx, block_thread_idx, loop_idxs)

            // Include automatically-generated loop code that calls
            // calc_inner_loop().
#include "yask_sub_block_loops.hpp"
#undef CALC_INNER_LOOP

        } // whole clusters.

        // Full and partial peel/remainder vectors in all dims except
        // the inner one.
        if (do_vectors) {
            TRACE_MSG("calc_sub_block_vec:  using vector code for [" <<
                       sub_block_vidxs.begin.makeValStr() <<
                       " ... " << sub_block_vidxs.end.makeValStr() <<
                       ") *not* within full vector-clusters at [" <<
                       sub_block_fcidxs.begin.makeValStr() <<
                       " ... " << sub_block_fcidxs.end.makeValStr() <<
                       ") by region thread " << region_thread_idx <<
                       " and block thread " << block_thread_idx);

            // Keep a copy of the normalized cluster indices
            // that were calculated above.
            // The full clusters were already done above, so
            // we only need to do vectors before or after the
            // clusters in each dim.
            // We'll exclude them below.
            ScanIndices norm_sub_block_fcidxs(norm_sub_block_idxs);

            // Normalize the vector indices.
            // These will be the bounds of the sub-block loops.
            // Set both begin/end and start/stop to ensure start/stop
            // vars get passed through to calc_loop_of_clusters()
            // for the inner loop.
            normalize_indices(sub_block_vidxs.begin, norm_sub_block_idxs.begin);
            norm_sub_block_idxs.start = norm_sub_block_idxs.begin;
            normalize_indices(sub_block_vidxs.end, norm_sub_block_idxs.end);
            norm_sub_block_idxs.stop = norm_sub_block_idxs.end;

            // Stride sizes are one vector.
            // The stride in the inner loop is hard-coded in the generated code.
            norm_sub_block_idxs.stride.setFromConst(1);

            // Also normalize the *full* vector indices to determine if
            // we need a mask at each vector index.
            // We just need begin and end indices for this.
            ScanIndices norm_sub_block_fvidxs(sub_block_eidxs);
            normalize_indices(sub_block_fvidxs.begin, norm_sub_block_fvidxs.begin);
            normalize_indices(sub_block_fvidxs.end, norm_sub_block_fvidxs.end);
            norm_sub_block_fvidxs.align.setFromConst(1); // one vector.

            // Define the function called from the generated loops to
            // determine whether a loop of vectors is within the peel range
            // (before the cluster) and/or remainder range (after the
            // clusters)--setting the 'ok' flag. In other words, the vectors
            // should be used only around the outside of the inner block of
            // clusters. Then, call the loop-of-vectors function
            // w/appropriate mask.  See the mask diagrams above that show
            // how the masks are ANDed together.  Since stride is always 1, we
            // ignore loop_idxs.stop.
#define CALC_INNER_LOOP(loop_idxs) \
            bool ok = false;                                            \
            idx_t mask = idx_t(-1);                                     \
            DOMAIN_VAR_LOOP(i, j) {                                     \
                if (i != inner_posn &&                                 \
                    (loop_idxs.start[i] < norm_sub_block_fcidxs.begin[i] || \
                     loop_idxs.start[i] >= norm_sub_block_fcidxs.end[i])) { \
                    ok = true;                                          \
                    if (loop_idxs.start[i] < norm_sub_block_fvidxs.begin[i]) \
                        mask &= peel_masks[i];                          \
                    if (loop_idxs.start[i] >= norm_sub_block_fvidxs.end[i]) \
                        mask &= rem_masks[i];                           \
                }                                                       \
            }                                                           \
            if (ok) calc_loop_of_vectors(region_thread_idx, block_thread_idx, loop_idxs, mask);

            // Include automatically-generated loop code that calls
            // calc_inner_loop().
#include "yask_sub_block_loops.hpp"
#undef CALC_INNER_LOOP
        }

        // Use scalar code for anything not done above.  This should only be
        // called if vectorizing on the inner loop and sub-block size in
        // that dim is not a multiple of the inner-dim vector len, so that
        // situation should be avoided.
        if (do_scalars) {

            // Use the 'misc' loops. Indices for these loops will be scalar and
            // global rather than normalized as in the cluster and vector loops.
            ScanIndices misc_idxs(sub_block_idxs);
            
            // Stride sizes and alignment are one element.
            misc_idxs.stride.setFromConst(1);
            misc_idxs.align.setFromConst(1);

            TRACE_MSG("calc_sub_block_vec:  using scalar code for [" <<
                       misc_idxs.begin.makeValStr() << " ... " <<
                       misc_idxs.end.makeValStr() <<
                       ") *not* within vectors at [" <<
                       sub_block_vidxs.begin.makeValStr() << " ... " <<
                       sub_block_vidxs.end.makeValStr() << 
                       ") by region thread " << region_thread_idx <<
                       " and block thread " << block_thread_idx);

            // Define misc-loop function.  This is called at each point in
            // the sub-block.  Since stride is always 1, we ignore
            // misc_idxs.stop.  TODO: handle more efficiently: do one slab
            // for inner-peel and one for outer-peel, calculate masks, and
            // call vector code.
#define MISC_FN(pt_idxs)  do {                                          \
                bool ok = false;                                        \
                DOMAIN_VAR_LOOP(i, j) {                                 \
                    auto rofs = _context->rank_domain_offsets[j];       \
                    if (pt_idxs.start[i] < rofs + sub_block_vidxs.begin[i] || \
                        pt_idxs.start[i] >= rofs + sub_block_vidxs.end[i]) { \
                        ok = true; break; }                             \
                }                                                       \
                if (ok) {                                               \
                    calc_scalar(region_thread_idx, pt_idxs.start);      \
                }                                                       \
            } while(0)

            // Scan through n-D space.
            // The OMP in the misc loops will be ignored if we're already in
            // the max allowed nested OMP region.
#include "yask_misc_loops.hpp"
#undef MISC_FN
        }
        
    } // calc_sub_block_vec.

    // Calculate a series of cluster results within an inner loop.
    // The 'loop_idxs' must specify a range only in the inner dim.
    // Indices must be rank-relative.
    // Indices must be normalized, i.e., already divided by VLEN_*.
    void StencilBundleBase::calc_loop_of_clusters(int region_thread_idx,
                                                  int block_thread_idx,
                                                  const ScanIndices& loop_idxs) {
        STATE_VARS(this);
        TRACE_MSG("calc_loop_of_clusters: local vector-indices [" <<
                   loop_idxs.start.makeValStr() <<
                   " ... " << loop_idxs.stop.makeValStr() <<
                   ") by region thread " << region_thread_idx <<
                   " and block thread " << block_thread_idx);

#ifdef CHECK
        // Check that only the inner dim has a range greater than one cluster.
        DOMAIN_VAR_LOOP(i, j) {
            if (i != inner_posn)
                assert(loop_idxs.start[i] + dims->_cluster_mults[j] >=
                       loop_idxs.stop[i]);
        }
#endif

        // Need all starting indices.
        const Indices& start_idxs = loop_idxs.start;

        // Need stop for inner loop only.
        idx_t stop_inner = loop_idxs.stop[inner_posn];

        // Call code from stencil compiler.
        calc_loop_of_clusters(region_thread_idx, block_thread_idx, start_idxs, stop_inner);
    }

    // Calculate a series of vector results within an inner loop.
    // The 'loop_idxs' must specify a range only in the inner dim.
    // Indices must be rank-relative.
    // Indices must be normalized, i.e., already divided by VLEN_*.
    void StencilBundleBase::calc_loop_of_vectors(int region_thread_idx,
                                                 int block_thread_idx,
                                                 const ScanIndices& loop_idxs,
                                                 idx_t write_mask) {
        STATE_VARS(this);
        TRACE_MSG("calc_loop_of_vectors: local vector-indices [" <<
                   loop_idxs.start.makeValStr() <<
                  " ... " << loop_idxs.stop.makeValStr() <<
                   ") w/write-mask = 0x" << hex << write_mask << dec <<
                   " by region thread " << region_thread_idx <<
                   " and block thread " << block_thread_idx);

#ifdef CHECK
        // Check that only the inner dim has a range greater than one vector.
        for (int i = 0; i < nsdims; i++) {
            if (i != step_posn && i != inner_posn)
                assert(loop_idxs.start[i] + 1 >= loop_idxs.stop[i]);
        }
#endif

        // Need all starting indices.
        const Indices& start_idxs = loop_idxs.start;

        // Need stop for inner loop only.
        idx_t stop_inner = loop_idxs.stop[inner_posn];

        // Call code from stencil compiler.
        calc_loop_of_vectors(region_thread_idx, block_thread_idx, start_idxs, stop_inner, write_mask);
    }

    // If this bundle is updating scratch var(s),
    // expand begin & end of 'idxs' by sizes of halos.
    // Stride indices may also change.
    // NB: it is not necessary that the domain of each var
    // is the same as the span of 'idxs'. However, it should be
    // at least that large to ensure that var is able to hold
    // calculated results. This is checked when 'CHECK' is defined.
    // In other words, var can be larger than span of 'idxs', but
    // its halo sizes are still used to specify how much to
    // add to 'idxs'.
    // Returns adjusted indices.
    ScanIndices StencilBundleBase::adjust_span(int region_thread_idx,
                                               const ScanIndices& idxs) const {
        STATE_VARS(this);
        ScanIndices adj_idxs(idxs);

        // Loop thru vecs of scratch vars for this bundle.
        for (auto* sv : outputScratchVecs) {
            assert(sv);

            // Get the one for this thread.
            auto& gp = sv->at(region_thread_idx);
            assert(gp);
            auto& gb = gp->gb();
            assert(gb.is_scratch());

            // i: index for stencil dims, j: index for domain dims.
            DOMAIN_VAR_LOOP(i, j) {
                auto& dim = dims->_stencil_dims.getDim(i);
                auto& dname = dim.getName();

                // Is this dim used in this var?
                int posn = gb.get_dim_posn(dname);
                if (posn >= 0) {

                    // Get halos, which need to be written to for
                    // scratch vars.
                    idx_t lh = gp->get_left_halo_size(posn);
                    idx_t rh = gp->get_right_halo_size(posn);

                    // Round up halos to fold sizes.
                    lh = ROUND_UP(lh, fold_pts[j]);
                    rh = ROUND_UP(rh, fold_pts[j]);

                    // Adjust begin & end scan indices based on halos.
                    adj_idxs.begin[i] = idxs.begin[i] - lh;
                    adj_idxs.end[i] = idxs.end[i] + rh;

                    // Make sure var covers index bounds.
                    TRACE_MSG("adjust_span: mini-blk [" << 
                              idxs.begin[i] << "..." <<
                              idxs.end[i] << ") adjusted to [" << 
                              adj_idxs.begin[i] << "..." <<
                              adj_idxs.end[i] << ") within scratch-var '" << 
                              gp->get_name() << "' allocated [" <<
                              gp->get_first_rank_alloc_index(posn) << "..." <<
                              gp->get_last_rank_alloc_index(posn) << "] in dim '" << dname << "'");
                    assert(adj_idxs.begin[i] >= gp->get_first_rank_alloc_index(posn));
                    assert(adj_idxs.end[i] <= gp->get_last_rank_alloc_index(posn) + 1);

                    // If existing stride is >= whole tile, adjust it also.
                    idx_t width = idxs.end[i] - idxs.begin[i];
                    if (idxs.stride[i] >= width) {
                        idx_t adj_width = adj_idxs.end[i] - adj_idxs.begin[i];
                        adj_idxs.stride[i] = adj_width;
                    }
                }
            }

            // Only need to get info from one var.
            // TODO: check that vars are consistent.
            break;
        }
        return adj_idxs;
    } // adjust_span().

    // Timer methods.
    // Start and stop pack timers for final stats and auto-tuners.
    void BundlePack::start_timers() {
        auto ts = YaskTimer::get_timespec();
        timer.start(&ts);
        getAT().timer.start(&ts);
    }
    void BundlePack::stop_timers() {
        auto ts = YaskTimer::get_timespec();
        timer.stop(&ts);
        getAT().timer.stop(&ts);
    }
    void BundlePack::add_steps(idx_t num_steps) {
        steps_done += num_steps;
        getAT().steps_done += num_steps;
    }

    static void print_var_list(ostream& os, const VarPtrs& gps, const string& type) {
        os << "  num " << type << " vars:";
        for (size_t i = 0; i < max(21ULL - type.length(), 1ULL); i++)
            os << ' ';
        os << gps.size() << endl;
        if (gps.size()) {
            os << "  " << type << " vars:";
            for (size_t i = 0; i < max(25ULL - type.length(), 1ULL); i++)
                os << ' ';
            int i = 0;
            for (auto gp : gps) {
                if (i++) os << ", ";
                os << gp->get_name();
            }
            os << endl;
        }
    }
    
    // Calc the work stats.
    // Requires MPI barriers!
    void BundlePack::init_work_stats() {
        STATE_VARS(this);

        num_reads_per_step = 0;
        num_writes_per_step = 0;
        num_fpops_per_step = 0;

        DEBUG_MSG("Pack '" << get_name() << "':\n" <<
                  " num bundles:                 " << size() << endl <<
                  " pack scope:                  " << _pack_bb.make_range_string(domain_dims));

        // Bundles.
        for (auto* sg : *this) {

            // Stats for this bundle for 1 pt.
            idx_t writes1 = 0, reads1 = 0, fpops1 = 0;
            
            // Loop through all the needed bundles to
            // count stats for scratch bundles.
            // Does not count extra ops needed in scratch halos
            // since this varies depending on block size.
            auto sg_list = sg->get_reqd_bundles();
            for (auto* rsg : sg_list) {
                reads1 += rsg->get_scalar_points_read();
                writes1 += rsg->get_scalar_points_written();
                fpops1 += rsg->get_scalar_fp_ops();
            }

            // Multiply by valid pts in BB for this bundle.
            auto& bb = sg->getBB();
            idx_t writes_bb = writes1 * bb.bb_num_points;
            num_writes_per_step += writes_bb;
            idx_t reads_bb = reads1 * bb.bb_num_points;
            num_reads_per_step += reads_bb;
            idx_t fpops_bb = fpops1 * bb.bb_num_points;
            num_fpops_per_step += fpops_bb;

            DEBUG_MSG(" Bundle '" << sg->get_name() << "':\n" <<
                      "  num reqd scratch bundles:   " << (sg_list.size() - 1));
            // TODO: add info on scratch bundles here.

            if (sg->is_sub_domain_expr())
                DEBUG_MSG("  sub-domain expr:            '" << sg->get_domain_description() << "'");
            if (sg->is_step_cond_expr())
                DEBUG_MSG("  step-condition expr:        '" << sg->get_step_cond_description() << "'");

            DEBUG_MSG("  bundle size (points):       " << makeNumStr(bb.bb_size));
            if (bb.bb_size) {
                DEBUG_MSG("  valid points in bundle:     " << makeNumStr(bb.bb_num_points));
                if (bb.bb_num_points) {
                    DEBUG_MSG("  bundle scope:               " << bb.make_range_string(domain_dims) <<
                              "\n  bundle bounding-box size:   " << bb.make_len_string(domain_dims));
                }
            }
            DEBUG_MSG("  num full rectangles in box: " << sg->getBBs().size());
            if (sg->getBBs().size() > 1) {
                for (size_t ri = 0; ri < sg->getBBs().size(); ri++) {
                    auto& rbb = sg->getBBs()[ri];
                    DEBUG_MSG("   Rectangle " << ri << ":\n"
                              "    num points in rect:       " << makeNumStr(rbb.bb_num_points));
                    if (rbb.bb_num_points) {
                        DEBUG_MSG("    rect scope:               " << rbb.make_range_string(domain_dims) <<
                                  "\n    rect size:                " << rbb.make_len_string(domain_dims));
                    }
                }
            }
            DEBUG_MSG("  var-reads per point:       " << reads1 << endl <<
                      "  var-reads in rank:         " << makeNumStr(reads_bb) << endl <<
                      "  var-writes per point:      " << writes1 << endl <<
                      "  var-writes in rank:        " << makeNumStr(writes_bb) << endl <<
                      "  est FP-ops per point:       " << fpops1 << endl <<
                      "  est FP-ops in rank:         " << makeNumStr(fpops_bb));
                      
            // Classify vars.
            VarPtrs idvars, imvars, odvars, omvars, iodvars, iomvars; // i[nput], o[utput], d[omain], m[isc].
            for (auto gp : sg->inputVarPtrs) {
                auto& gb = gp->gb();
                bool isdom = gb.is_domain_var();
                auto& ogps = sg->outputVarPtrs;
                bool isout = find(ogps.begin(), ogps.end(), gp) != ogps.end();
                if (isout) {
                    if (isdom)
                        iodvars.push_back(gp);
                    else
                        iomvars.push_back(gp);
                } else {
                    if (isdom)
                        idvars.push_back(gp);
                    else
                        imvars.push_back(gp);
                }
            }
            for (auto gp : sg->outputVarPtrs) {
                auto& gb = gp->gb();
                bool isdom = gb.is_domain_var();
                auto& igps = sg->inputVarPtrs;
                bool isin = find(igps.begin(), igps.end(), gp) != igps.end();
                if (!isin) {
                    if (isdom)
                        odvars.push_back(gp);
                    else
                        omvars.push_back(gp);
                }
            }
            yask_output_ptr op = ksbp->get_debug_output();
            ostream& os = op->get_ostream();
            print_var_list(os, idvars, "input-only domain");
            print_var_list(os, odvars, "output-only domain");
            print_var_list(os, iodvars, "input-output domain");
            print_var_list(os, imvars, "input-only other");
            print_var_list(os, omvars, "output-only other");
            print_var_list(os, iomvars, "input-output other");
            
        } // bundles.

        // Sum across ranks.
        tot_reads_per_step = sumOverRanks(num_reads_per_step, env->comm);
        tot_writes_per_step = sumOverRanks(num_writes_per_step, env->comm);
        tot_fpops_per_step = sumOverRanks(num_fpops_per_step, env->comm);
        
    } // init_work_stats().

} // namespace yask.
