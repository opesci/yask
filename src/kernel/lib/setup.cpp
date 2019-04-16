/*****************************************************************************

YASK: Yet Another Stencil Kernel
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

// This file contains implementations of StencilContext and
// StencilBundleBase methods specific to the preparation steps.

#include "yask_stencil.hpp"
using namespace std;

namespace yask {

    // Stop collecting VTune data when a factory is defined.
    // Even better to use -start-paused option.
    yk_factory::yk_factory() {
        VTUNE_PAUSE;
    }
    
    // ScanIndices ctor.
    ScanIndices::ScanIndices(const Dims& dims, bool use_vec_align, IdxTuple* ofs) :
        ndims(NUM_STENCIL_DIMS),
        begin(idx_t(0), ndims),
        end(idx_t(0), ndims),
        stride(idx_t(1), ndims),
        align(idx_t(1), ndims),
        align_ofs(idx_t(0), ndims),
        group_size(idx_t(1), ndims),
        num_indices(idx_t(1), ndims),
        start(idx_t(0), ndims),
        stop(idx_t(0), ndims),
        index(idx_t(0), ndims) {

        // i: index for stencil dims, j: index for domain dims.
        DOMAIN_VAR_LOOP(i, j) {

            // Set alignment to vector lengths.
            if (use_vec_align)
                align[i] = fold_pts[j];

            // Set alignment offset.
            if (ofs) {
                assert(ofs->getNumDims() == ndims - 1);
                align_ofs[i] = ofs->getVal(j);
            }
        }
    }

    // Context ctor.
    StencilContext::StencilContext(KernelEnvPtr& kenv,
                                   KernelSettingsPtr& ksettings) :
        KernelStateBase(kenv, ksettings),
        _at(this, ksettings.get())
    {
        STATE_VARS(this);

        // Init various tuples to make sure they have the correct dims.
        rank_domain_offsets = domain_dims;
        rank_domain_offsets.setValsSame(-1); // indicates prepare_solution() not called. TODO: add flag.
        max_halos = domain_dims;
        wf_angles = domain_dims;
        wf_shift_pts = domain_dims;
        tb_angles = domain_dims;
        tb_widths = domain_dims;
        tb_tops = domain_dims;
        mb_angles = domain_dims;
        left_wf_exts = domain_dims;
        right_wf_exts = domain_dims;
    }

    // Init MPI-related vars and other vars related to my rank's place in
    // the global problem: rank index, offset, etc.  Need to call this even
    // if not using MPI to properly init these vars.  Called from
    // prepare_solution().
    void StencilContext::setupRank() {
        STATE_VARS(this);
        TRACE_MSG("setupRank()...");
        auto me = env->my_rank;
        auto nr = env->num_ranks;

        // All ranks should have the same settings for certain options.
        assertEqualityOverRanks(nr, env->comm, "total number of MPI ranks");
        assertEqualityOverRanks(idx_t(opts->use_shm), env->comm, "use_shm setting");
        assertEqualityOverRanks(idx_t(opts->find_loc), env->comm, "defined rank indices");
        DOMAIN_VAR_LOOP(i, j) {
            auto& dname = domain_dims.getDimName(j);
            assertEqualityOverRanks(opts->_global_sizes[i], env->comm,
                                    "global-domain size in '" + dname + "' dimension");
            assertEqualityOverRanks(opts->_num_ranks[j], env->comm,
                                    "number of ranks in '" + dname + "' dimension");

            // Check that either local or global size is set.
            if (!opts->_global_sizes[i] && !opts->_rank_sizes[i])
                THROW_YASK_EXCEPTION("Error: both local-domain size and "
                                     "global-domain size are zero in '" +
                                     dname + "' dimension on rank " +
                                     to_string(me) + "; specify one, "
                                     "and the other will be calculated");
        }

#ifndef USE_MPI

        // Simple settings.
        opts->_num_ranks.setValsSame(0);
        opts->_rank_indices.setValsSame(0);
        rank_domain_offsets.setValsSame(0);
        
        // Init vars w/o MPI.
        DOMAIN_VAR_LOOP(i, j) {

            // Need to set local size.
            if (!opts->_rank_sizes[i])
                opts->_rank_sizes[i] = opts->_global_sizes[i];

            // Need to set global size.
            else if (!opts->_global_sizes[i])
                opts->_global_sizes[i] = opts->_rank_sizes[i];

            // Check that settings are equal.
            else if (opts->_global_sizes[i] != opts->_rank_sizes[i]) {
                auto& dname = domain_dims.getDimName(j);
                FORMAT_AND_THROW_YASK_EXCEPTION("Error: specified local-domain size of " <<
                                                opts->_rank_sizes[i] <<
                                                " does not equal specified global-domain size of " <<
                                                opts->_global_sizes[i] << " in '" << dname <<
                                                "' dimension");
            }
        }        
        
#else
        // Set number of ranks in each dim if any is unset (zero).
        if (!opts->_num_ranks.product()) {

            // Make list of factors of number of ranks.
            vector<idx_t> facts;
            for (idx_t n = 1; n <= nr; n++)
                if (nr % n == 0)
                    facts.push_back(n);

            // Keep track of "best" result, where the best is most compact.
            IdxTuple best;

            // Try every combo of N-1 factors, where N is the number of dims.
            // TODO: make more efficient--need algorithm to directly get
            // set of N factors that are valid.
            IdxTuple combos;
            DOMAIN_VAR_LOOP(i, j) {
                auto& dname = domain_dims.getDimName(j);

                // Number of factors.
                auto sz = facts.size();

                // Set first number of options 1 because it will be
                // calculated based on the other values, i.e., we don't need
                // to search over first dim.  Also don't need to search any
                // specified value.
                if (j == 0 || opts->_num_ranks[j])
                    sz = 1;
                
                combos.addDimBack(dname, sz);
            }
            TRACE_MSG("setupRank(): checking " << combos.product() << " rank layouts");
            combos.visitAllPoints
                ([&](const IdxTuple& combo, size_t idx)->bool {

                     // Make tuple w/factors at given indices.
                     auto num_ranks = combo.mapElements([&](idx_t in) {
                                                            return facts.at(in);
                                                        });

                     // Override with specified values.
                     DOMAIN_VAR_LOOP(i, j) {
                         if (opts->_num_ranks[j])
                             num_ranks[j] = opts->_num_ranks[j];
                         else if (j == 0)
                             num_ranks[j] = -1; // -1 => needs to be calculated.
                     }

                     // Replace first factor with computed value if not set.
                     if (num_ranks[0] == -1) {
                         num_ranks[0] = 1;
                         num_ranks[0] = nr / num_ranks.product();
                     }

                     // Valid?
                     if (num_ranks.product() == nr) {
                         TRACE_MSG("  valid layout " << num_ranks.makeDimValStr(" * ") <<
                                   " has max size " << num_ranks.max());

                         // Best so far?
                         // Layout is better if max size is smaller.
                         if (best.size() == 0 ||
                             num_ranks.max() < best.max())
                             best = num_ranks;
                     }
                     
                     return true; // keep looking.
                 });
            assert(best.size());
            assert(best.product());
            TRACE_MSG("  layout " << best.makeDimValStr(" * ") << " selected");
            opts->_num_ranks = best;
        }

        // Check ranks.
        idx_t req_ranks = opts->_num_ranks.product();
        if (req_ranks != nr)
            FORMAT_AND_THROW_YASK_EXCEPTION("error: " << req_ranks << " rank(s) requested (" +
                                            opts->_num_ranks.makeDimValStr(" * ") + "), but " <<
                                            nr << " rank(s) are active");

        // Determine my coordinates if not provided already.
        // TODO: do this more intelligently based on proximity.
        if (opts->find_loc)
            opts->_rank_indices = opts->_num_ranks.unlayout(me);

        // Check rank indices.
        DOMAIN_VAR_LOOP(i, j) {
            auto& dname = domain_dims.getDimName(j);
            if (opts->_rank_indices[j] < 0 ||
                opts->_rank_indices[j] >= opts->_num_ranks[j])
                THROW_YASK_EXCEPTION("Error: rank index of " +
                                     to_string(opts->_rank_indices[j]) +
                                     " is not within allowed range [0 ... " +
                                     to_string(opts->_num_ranks[j] - 1) +
                                     "] in '" + dname + "' dimension on rank " +
                                     to_string(me));
        }
        
        // Init starting indices for this rank.
        rank_domain_offsets.setValsSame(0);

        // Tables to share data across ranks.
        idx_t coords[nr][nddims]; // rank indices.
        idx_t rsizes[nr][nddims]; // rank sizes.

        // Two passes over ranks:
        // 0: sum all specified local sizes.
        // 1: set final sums and offsets.
        for (int pass : { 0, 1 }) {

            // Init rank-size sums.
            IdxTuple rank_domain_sums(domain_dims);
            rank_domain_sums.setValsSame(0);

            // Init tables for this rank.
            DOMAIN_VAR_LOOP(i, j) {
                coords[me][j] = opts->_rank_indices[j];
                rsizes[me][j] = opts->_rank_sizes[i];
            }

            // Exchange coord and size info between all ranks.
            for (int rn = 0; rn < nr; rn++) {
                MPI_Bcast(&coords[rn][0], nddims, MPI_INTEGER8,
                          rn, env->comm);
                MPI_Bcast(&rsizes[rn][0], nddims, MPI_INTEGER8,
                          rn, env->comm);
            }
            // Now, the tables are filled in for all ranks.
            // Some rank sizes may be zero on the 1st pass,
            // but they should all be non-zero on 2nd pass.
            
            // Loop over all ranks, including myself.
            int num_neighbors = 0;
            for (int rn = 0; rn < nr; rn++) {

                // Coord offset of rn from me: prev => negative, self => 0, next => positive.
                IdxTuple rcoords(domain_dims);
                IdxTuple rdeltas(domain_dims);
                DOMAIN_VAR_LOOP(i, di) {
                    rcoords[di] = coords[rn][di];
                    rdeltas[di] = coords[rn][di] - coords[me][di];
                }

                // Manhattan distance from rn (sum of abs deltas in all dims).
                // Max distance in any dim.
                int mandist = 0;
                int maxdist = 0;
                DOMAIN_VAR_LOOP(i, di) {
                    mandist += abs(rdeltas[di]);
                    maxdist = max(maxdist, abs(int(rdeltas[di])));
                }

                // Myself.
                if (rn == me) {
                    if (mandist != 0)
                        FORMAT_AND_THROW_YASK_EXCEPTION
                            ("Internal error: distance to own rank == " << mandist);
                }

                // Someone else.
                else {
                    if (mandist == 0)
                        FORMAT_AND_THROW_YASK_EXCEPTION
                            ("Error: ranks " << me <<
                             " and " << rn << " at same coordinates");
                }

                // Loop through domain dims.
                DOMAIN_VAR_LOOP(i, di) {
                    auto& dname = domain_dims.getDimName(di);

                    // Is rank 'rn' in-line with my rank in 'dname' dim?
                    // True when deltas in all other dims are zero.
                    bool is_inline = true;
                    DOMAIN_VAR_LOOP(j, dj) {
                        if (di != dj && rdeltas[dj] != 0) {
                            is_inline = false;
                            break;
                        }
                    }

                    // Process this rank if it is in-line with me in 'dname', including myself.
                    if (is_inline) {

                        // Sum rank sizes in this dim.
                        rank_domain_sums[di] += rsizes[rn][di];

                        if (pass == 1) {

                            // Make sure all the other dims are the same size.
                            // This ensures that all the ranks' domains line up
                            // properly along their edges and at their corners.
                            DOMAIN_VAR_LOOP(j, dj) {
                                if (di != dj) {
                                    auto& dnamej = domain_dims.getDimName(dj);
                                    auto mysz = rsizes[me][dj];
                                    auto rnsz = rsizes[rn][dj];
                                    if (mysz != rnsz) {
                                        FORMAT_AND_THROW_YASK_EXCEPTION
                                            ("Error: rank " << rn << " and " << me <<
                                             " are both at rank-index " << coords[me][di] <<
                                             " in the '" << dname <<
                                             "' dimension, but their local-domain sizes are " <<
                                             rnsz << " and " << mysz <<
                                             " (resp.) in the '" << dnamej <<
                                             "' dimension, making them unaligned");
                                    }
                                }
                            }

                            // Adjust my offset in the global problem by adding all domain
                            // sizes from prev ranks only.
                            if (rdeltas[di] < 0)
                                rank_domain_offsets[dname] += rsizes[rn][di];

                        } // 2nd pass.
                    } // is inline w/me.
                } // dims.

                // Rank rn is myself or my immediate neighbor if its distance <= 1 in
                // every dim.  Assume we do not need to exchange halos except
                // with immediate neighbor. We enforce this assumption below by
                // making sure that the rank domain size is at least as big as the
                // largest halo.
                if (pass == 1 && maxdist <= 1) {

                    // At this point, rdeltas contains only -1..+1 for each domain dim.
                    // Add one to -1..+1 to get 0..2 range for my_neighbors offsets.
                    IdxTuple roffsets = rdeltas.addElements(1);
                    assert(rdeltas.min() >= -1);
                    assert(rdeltas.max() <= 1);
                    assert(roffsets.min() >= 0);
                    assert(roffsets.max() <= 2);

                    // Convert the offsets into a 1D index.
                    auto rn_ofs = mpiInfo->getNeighborIndex(roffsets);
                    TRACE_MSG("neighborhood size = " << mpiInfo->neighborhood_sizes.makeDimValStr() <<
                              " & roffsets of rank " << rn << " = " << roffsets.makeDimValStr() <<
                              " => " << rn_ofs);
                    assert(idx_t(rn_ofs) < mpiInfo->neighborhood_size);

                    // Save rank of this neighbor into the MPI info object.
                    mpiInfo->my_neighbors.at(rn_ofs) = rn;
                    if (rn == me) {
                        assert(mpiInfo->my_neighbor_index == rn_ofs);
                        mpiInfo->shm_ranks.at(rn_ofs) = env->my_shm_rank;
                    }
                    else {
                        num_neighbors++;
                        os << "Neighbor #" << num_neighbors << " is MPI rank " << rn <<
                            " at absolute rank indices " << rcoords.makeDimValStr() <<
                            " (" << rdeltas.makeDimValOffsetStr() << " relative to rank " <<
                            me << ")";

                        // Determine whether neighbor is in my shm group.
                        // If so, record rank number in shmcomm.
                        if (opts->use_shm && env->shm_comm != MPI_COMM_NULL) {
                            int g_rank = rn;
                            int s_rank = MPI_PROC_NULL;
                            MPI_Group_translate_ranks(env->group, 1, &g_rank,
                                                      env->shm_group, &s_rank);
                            if (s_rank != MPI_UNDEFINED) {
                                mpiInfo->shm_ranks.at(rn_ofs) = s_rank;
                                os << " and is MPI shared-memory rank " << s_rank;
                            } else {
                                os << " and will not use shared-memory";
                            }
                        }
                        os << ".\n";
                    }

                    // Save manhattan dist.
                    mpiInfo->man_dists.at(rn_ofs) = mandist;

                    // Loop through domain dims.
                    bool vlen_mults = true;
                    DOMAIN_VAR_LOOP(i, j) {
                        auto& dname = domain_dims.getDimName(j);
                        auto nranks = opts->_num_ranks[j];
                        bool is_last = (opts->_rank_indices[j] == nranks - 1);

                        // Does rn have all VLEN-multiple sizes?
                        // TODO: allow last rank in each dim to be non-conformant.
                        auto rnsz = rsizes[rn][j];
                        auto vlen = fold_pts[j];
                        if (rnsz % vlen != 0) {
                            TRACE_MSG("cannot use vector halo exchange with rank " << rn <<
                                      " because its size in '" << dname << "' is " << rnsz);
                            vlen_mults = false;
                        }
                    }

                    // Save vec-mult flag.
                    mpiInfo->has_all_vlen_mults.at(rn_ofs) = vlen_mults;

                } // self or immediate neighbor in any direction.
            } // ranks.

            // At end of 1st pass, known ranks sizes have
            // been summed in each dim. Determine global size
            // or other rank sizes for each dim.
            if (pass == 0) {
                DOMAIN_VAR_LOOP(i, j) {
                    auto& dname = domain_dims.getDimName(j);
                    auto nranks = opts->_num_ranks[j];
                    auto gsz = opts->_global_sizes[i];
                    bool is_last = (opts->_rank_indices[j] == nranks - 1);

                    // Need to determine my rank size.
                    if (!opts->_rank_sizes[i]) {
                        if (rank_domain_sums[j] != 0)
                            FORMAT_AND_THROW_YASK_EXCEPTION
                                ("Error: local-domain size is not specified in the '" <<
                                 dname << "' dimension on rank " << me <<
                                 ", but it is specified on another rank; "
                                 "it must be specified or unspecified consistently across all ranks");

                        // Divide sum by num of ranks in this dim.
                        auto rsz = CEIL_DIV(gsz, nranks);
                        
                        // Round up to whole vector-clusters.
                        rsz = ROUND_UP(rsz, dims->_cluster_pts[j]);

                        // Remainder for last rank.
                        auto rem = gsz - (rsz * (nranks - 1));
                        if (rem <= 0)
                            FORMAT_AND_THROW_YASK_EXCEPTION
                                ("Error: global-domain size of " << gsz <<
                                 " is not large enough to split across " << nranks <<
                                 " ranks in the '" << dname << "' dimension");
                        if (is_last)
                            rsz = rem;

                        // Set rank size depending on whether it is last one.
                        opts->_rank_sizes[i] = rsz;
                        TRACE_MSG("local-domain-size[" << dname << "] = " << rem);
                    }

                    // Need to determine global size.
                    // Set it to sum of rank sizes.
                    else if (!opts->_global_sizes[i])
                        opts->_global_sizes[i] = rank_domain_sums[j];
                }
            }

            // After 2nd pass, check for consistency.
            else {
                DOMAIN_VAR_LOOP(i, j) {
                    auto& dname = domain_dims.getDimName(j);
                    if (opts->_global_sizes[i] != rank_domain_sums[j]) {
                        FORMAT_AND_THROW_YASK_EXCEPTION("Error: sum of local-domain sizes across " <<
                                                        nr << " ranks is " <<
                                                        rank_domain_sums[j] <<
                                                        ", which does not equal global-domain size of " <<
                                                        opts->_global_sizes[i] << " in '" << dname <<
                                                        "' dimension");
                    }
                }
            }

        } // passes.
#endif

    } // setupRank().

    // Set non-scratch grid sizes and offsets based on settings.
    // Set wave-front settings.
    // This should be called anytime a setting or rank offset is changed.
    void StencilContext::update_grid_info(bool force) {
        STATE_VARS(this);
        TRACE_MSG("update_grid_info(" << force << ")...");

        // If we haven't finished constructing the context, it's too early
        // to do this.
        if (!stPacks.size())
            return;

        // Reset max halos to zero.
        max_halos = dims->_domain_dims;

        // Loop through each domain dim.
        for (auto& dim : domain_dims.getDims()) {
            auto& dname = dim.getName();
            
            // Each non-scratch grid.
            for (auto gp : gridPtrs) {
                assert(gp);
                if (!gp->is_dim_used(dname))
                    continue;
                auto& gb = gp->gb();

                // Don't resize manually-sized grid
                // unless it is a solution grid and 'force' is 'true'.
                if (!gp->is_fixed_size() ||
                    (!gb.is_user_grid() && force)) {

                    // Rank domains.
                    gp->_set_domain_size(dname, opts->_rank_sizes[dname]);
                    
                    // Pads.
                    // Set via both 'extra' and 'min'; larger result will be used.
                    gp->set_extra_pad_size(dname, opts->_extra_pad_sizes[dname]);
                    gp->set_min_pad_size(dname, opts->_min_pad_sizes[dname]);
                        
                    // Offsets.
                    gp->_set_rank_offset(dname, rank_domain_offsets[dname]);
                    gp->_set_local_offset(dname, 0);
                }

                // Update max halo across grids, used for temporal angles.
                if (!gb.is_user_grid()) {
                    max_halos[dname] = max(max_halos[dname], gp->get_left_halo_size(dname));
                    max_halos[dname] = max(max_halos[dname], gp->get_right_halo_size(dname));
                }
            }
        }

        // Calculate wave-front shifts.
        // See the wavefront diagram in run_solution() for description
        // of angles and extensions.
        idx_t tb_steps = opts->_block_sizes[step_dim]; // use requested size; actual may be less.
        assert(tb_steps >= 0);
        wf_steps = opts->_region_sizes[step_dim];
        wf_steps = max(wf_steps, tb_steps); // round up WF steps if less than TB steps.
        assert(wf_steps >= 0);
        num_wf_shifts = 0;
        if (wf_steps > 0) {

            // Need to shift for each bundle pack.
            assert(stPacks.size() > 0);
            num_wf_shifts = idx_t(stPacks.size()) * wf_steps;

            // Don't need to shift first one.
            if (num_wf_shifts > 0)
                num_wf_shifts--;
        }
        assert(num_wf_shifts >= 0);

        // Determine whether separate tuners can be used.
        state->_use_pack_tuners = opts->_allow_pack_tuners && (tb_steps == 0) && (stPacks.size() > 1);

        // Calculate angles and related settings.
        for (auto& dim : domain_dims.getDims()) {
            auto& dname = dim.getName();
            auto rnsize = opts->_region_sizes[dname];
            auto rksize = opts->_rank_sizes[dname];
            auto nranks = opts->_num_ranks[dname];

            // Req'd shift in this dim based on max halos.
            idx_t angle = ROUND_UP(max_halos[dname], dims->_fold_pts[dname]);
            
            // Determine the spatial skewing angles for WF tiling.  We
            // only need non-zero angles if the region size is less than the
            // rank size or there are other ranks in this dim, i.e., if
            // the region covers the *global* domain in a given dim, no
            // wave-front shifting is needed in that dim.
            idx_t wf_angle = 0;
            if (rnsize < rksize || nranks > 1)
                wf_angle = angle;
            wf_angles.addDimBack(dname, wf_angle);
            assert(angle >= 0);

            // Determine the total WF shift to be added in each dim.
            idx_t shifts = wf_angle * num_wf_shifts;
            wf_shift_pts[dname] = shifts;
            assert(shifts >= 0);

            // Is domain size at least as large as halo + wf_ext in direction
            // when there are multiple ranks?
            auto min_size = max_halos[dname] + shifts;
            if (opts->_num_ranks[dname] > 1 && rksize < min_size) {
                FORMAT_AND_THROW_YASK_EXCEPTION
                    ("Error: local-domain size of " << rksize << " in '" <<
                     dname << "' dim is less than minimum size of " << min_size <<
                     ", which is based on stencil halos and temporal wave-front sizes");
            }

            // If there is another rank to the left, set wave-front
            // extension on the left.
            left_wf_exts[dname] = opts->is_first_rank(dname) ? 0 : shifts;

            // If there is another rank to the right, set wave-front
            // extension on the right.
            right_wf_exts[dname] = opts->is_last_rank(dname) ? 0 : shifts;
        }

        // Now that wave-front settings are known, we can push this info
        // back to the grids. It's useful to store this redundant info
        // in the grids, because there it's indexed by grid dims instead
        // of domain dims. This makes it faster to do grid indexing.
        for (auto gp : origGridPtrs) {
            assert(gp);

            // Loop through each domain dim.
            for (auto& dim : domain_dims.getDims()) {
                auto& dname = dim.getName();
                if (gp->is_dim_used(dname)) {
                    // Set extensions to be the same as the global ones.
                    gp->_set_left_wf_ext(dname, left_wf_exts[dname]);
                    gp->_set_right_wf_ext(dname, right_wf_exts[dname]);
                }
            }
        } // grids.

        // Calculate temporal-block shifts.
        // NB: this will change if/when block sizes change.
        update_tb_info();
        
    } // update_grid_info().

    // Set temporal blocking data.  This should be called anytime a block
    // size is changed.  Must be called after update_grid_info() to ensure
    // angles are properly set.  TODO: calculate 'tb_steps' dynamically
    // considering temporal conditions; this assumes worst-case, which is
    // all packs always done.
    void StencilContext::update_tb_info() {
        STATE_VARS(this);
        TRACE_MSG("update_tb_info()...");

        // Get requested size.
        tb_steps = opts->_block_sizes[step_dim];

        // Reset all TB and MB vars.
        num_tb_shifts = 0;
        tb_angles.setValsSame(0);
        tb_widths.setValsSame(0);
        tb_tops.setValsSame(0);
        mb_angles.setValsSame(0);

        // Set angles.
        // Determine max temporal depth based on block sizes
        // and requested temporal depth.
        // When using temporal blocking, all block sizes
        // across all packs must be the same.
        TRACE_MSG("update_tb_info: original TB steps = " << tb_steps);
        if (tb_steps > 0) {

            // TB is inside WF, so can't be larger.
            idx_t max_steps = min(tb_steps, wf_steps);
            TRACE_MSG("update_tb_info: min(TB, WF) steps = " << max_steps);

            // Loop through each domain dim.
            DOMAIN_VAR_LOOP(i, j) {
                auto& dim = domain_dims.getDim(j);
                auto& dname = dim.getName();
                auto rnsize = opts->_region_sizes[i];

                // There must be only one block size when using TB, so get
                // sizes from context settings instead of packs.
                assert(state->_use_pack_tuners == false);
                auto blksize = opts->_block_sizes[i];
                auto mblksize = opts->_mini_block_sizes[i];

                // Req'd shift in this dim based on max halos.
                // Can't use separate L & R shift because of possible data reuse in grids.
                // Can't use separate shifts for each pack for same reason.
                // TODO: make round-up optional.
                auto fpts = dims->_fold_pts[j];
                idx_t angle = ROUND_UP(max_halos[j], fpts);
            
                // Determine the spatial skewing angles for MB.
                // If MB covers whole blk, no shifting is needed in that dim.
                idx_t mb_angle = 0;
                if (mblksize < blksize)
                    mb_angle = angle;
                mb_angles[j] = mb_angle;

                // Determine the max spatial skewing angles for TB.
                // If blk covers whole region, no shifting is needed in that dim.
                idx_t tb_angle = 0;
                if (blksize < rnsize)
                    tb_angle = angle;
                tb_angles[j] = tb_angle;

                // Calculate max number of temporal steps in
                // allowed this dim.
                if (tb_angle > 0) {

                    // min_blk_sz = min_top_sz + 2 * angle * (npacks * nsteps - 1).
                    // bs = ts + 2*a*np*ns - 2*a.
                    // 2*a*np*ns = bs - ts + 2*a.
                    // s = flr[ (bs - ts + 2*a) / 2*a*np ].
                    idx_t top_sz = fpts; // min pts on top row. TODO: is zero ok?
                    idx_t sh_pts = tb_angle * 2 * stPacks.size(); // pts shifted per step.
                    idx_t nsteps = (blksize - top_sz + tb_angle * 2) / sh_pts; // might be zero.
                    TRACE_MSG("update_tb_info: max TB steps in dim '" <<
                              dname << "' = " << nsteps <<
                              " due to base block size of " << blksize <<
                              ", TB angle of " << tb_angle <<
                              ", and " << stPacks.size() << " pack(s)");
                    max_steps = min(max_steps, nsteps);
                }
            }
            tb_steps = min(tb_steps, max_steps);
            TRACE_MSG("update_tb_info: final TB steps = " << tb_steps);
        }
        assert(tb_steps >= 0);

        // Calc number of shifts based on steps.
        if (tb_steps > 0) {

            // Need to shift for each bundle pack.
            assert(stPacks.size() > 0);
            num_tb_shifts = idx_t(stPacks.size()) * tb_steps;

            // Don't need to shift first one.
            if (num_tb_shifts > 0)
                num_tb_shifts--;
        }
        assert(num_tb_shifts >= 0);
        TRACE_MSG("update_tb_info: num TB shifts = " << num_tb_shifts);

        // Calc size of base of phase 0 trapezoid.
        // Initial width is half of base plus one shift distance.  This will
        // make 'up' and 'down' trapezoids approx same size.

        //   x->
        // ^   ----------------------
        // |  /        \            /^
        // t /  phase 0 \ phase 1  / |
        //  /            \        /  |
        //  ----------------------   |
        //  ^             ^       ^  |
        //  |<-blk_width->|    -->|  |<--sa=nshifts*angle
        //  |             |       |
        // blk_start  blk_stop  next_blk_start
        //  |                     |
        //  |<-----blk_sz-------->|
        // blk_width = blk_sz/2 + sa.

        // Ex: blk_sz=12, angle=4, nshifts=1, fpts=4,
        // sa=1*4=4, blk_width=rnd_up(12/2+4,4)=12.
        //     111122222222
        // 111111111111

        // Ex: blk_sz=16, angle=4, nshifts=1, fpts=4,
        // sa=1*4=4, blk_width=rnd_up(16/2+4,4)=12.
        //     1111222222222222
        // 1111111111112222

        // Ex: blk_sz=16, angle=2, nshifts=2, fpts=2,
        // sa=2*2=4, blk_width=rnd_up(16/2+4,2)=12.
        //     1111222222222222
        //   1111111122222222
        // 1111111111112222

        // TODO: use actual number of shifts dynamically instead of this
        // max.
        DOMAIN_VAR_LOOP(i, j) {
            auto blk_sz = opts->_block_sizes[i];
            auto tb_angle = tb_angles[j];
            tb_widths[j] = blk_sz;
            tb_tops[j] = blk_sz;

            // If no shift or angle in this dim, we don't need
            // bridges at all, so base is entire block.
            if (num_tb_shifts > 0 && tb_angle > 0) {
                
                // See equations above for block size.
                auto fpts = dims->_fold_pts[j];
                idx_t min_top_sz = fpts;
                idx_t sa = num_tb_shifts * tb_angle;
                idx_t min_blk_width = min_top_sz + 2 * sa;
                idx_t blk_width = ROUND_UP(CEIL_DIV(blk_sz, idx_t(2)) + sa, fpts);
                blk_width = max(blk_width, min_blk_width);
                idx_t top_sz = max(blk_width - 2 * sa, idx_t(0));
                tb_widths[j] = blk_width;
                tb_tops[j] = top_sz;
            }
        }
        TRACE_MSG("update_tb_info: trapezoid bases = " << tb_widths.makeDimValStr() <<
                  ", tops = " << tb_tops.makeDimValStr());
    } // update_tb_info().

    // Init all grids & params by calling initFn.
    void StencilContext::initValues(function<void (YkGridPtr gp,
                                                   real_t seed)> realInitFn) {
        STATE_VARS(this);

        real_t seed = 0.1;
        os << "Initializing grids...\n" << flush;
        YaskTimer itimer;
        itimer.start();
        for (auto gp : gridPtrs) {
            realInitFn(gp, seed);
            seed += 0.01;
        }
        itimer.stop();
        os << "Grid initialization done in " <<
            makeNumStr(itimer.get_elapsed_secs()) << " secs.\n" << flush;
    }

    // Set the bounding-box for each stencil-bundle and whole domain.
    void StencilContext::find_bounding_boxes()
    {
        STATE_VARS(this);
        os << "Constructing bounding boxes for " <<
            stBundles.size() << " stencil-bundles(s)...\n" << flush;
        YaskTimer bbtimer;
        bbtimer.start();

        // Rank BB is based only on rank offsets and rank domain sizes.
        rank_bb.bb_begin = rank_domain_offsets;
        rank_bb.bb_end = rank_domain_offsets.addElements(opts->_rank_sizes, false);
        rank_bb.update_bb("rank", *this, true, &os);

        // BB may be extended for wave-fronts.
        ext_bb.bb_begin = rank_bb.bb_begin.subElements(left_wf_exts);
        ext_bb.bb_end = rank_bb.bb_end.addElements(right_wf_exts);
        ext_bb.update_bb("extended-rank", *this, true);

        // Remember sub-domain for each bundle.
        map<string, StencilBundleBase*> bb_descrs;

        // Find BB for each pack.
        for (auto sp : stPacks) {
            auto& spbb = sp->getBB();
            spbb.bb_begin = domain_dims;
            spbb.bb_end = domain_dims;

            // Find BB for each bundle in this pack.
            for (auto sb : *sp) {

                // Already done?
                auto bb_descr = sb->get_domain_description();
                if (bb_descrs.count(bb_descr)) {

                    // Copy existing.
                    auto* src = bb_descrs.at(bb_descr);
                    sb->copy_bounding_box(src);
                }
                
                // Find bundle BB.
                else {
                    sb->find_bounding_box();
                    bb_descrs[bb_descr] = sb;
                }

                auto& sbbb = sb->getBB();

                // Expand pack BB to encompass bundle BB.
                spbb.bb_begin = spbb.bb_begin.minElements(sbbb.bb_begin);
                spbb.bb_end = spbb.bb_end.maxElements(sbbb.bb_end);
            }
            spbb.update_bb(sp->get_name(), *this, false);
        }

        // Init MPI interior to extended BB.
        mpi_interior = ext_bb;

        bbtimer.stop();
        os << "Bounding-box construction done in " <<
            makeNumStr(bbtimer.get_elapsed_secs()) << " secs.\n" << flush;
    }

    // Copy BB vars from another.
    void StencilBundleBase::copy_bounding_box(const StencilBundleBase* src) {
        STATE_VARS(this);
        TRACE_MSG("copy_bounding_box for '" << get_name() << "' from '" <<
                   src->get_name() << "'...");

        _bundle_bb = src->_bundle_bb;
        assert(_bundle_bb.bb_valid);
        _bb_list = src->_bb_list;
    }
    
    // Find the bounding-boxes for this bundle in this rank.
    // Only tests domain-var values, not step-vars.
    // Step-vars are tested dynamically for each step
    // as it is executed.
    void StencilBundleBase::find_bounding_box() {
        STATE_VARS(this);
        TRACE_MSG("find_bounding_box for '" << get_name() << "'...");

        // Init overall bundle BB to that of parent and clear list.
        assert(_context);
        _bundle_bb = _context->ext_bb;
        assert(_bundle_bb.bb_valid);
        _bb_list.clear();
        
        // If BB is empty, we are done.
        if (!_bundle_bb.bb_size)
            return;

        // If there is no condition, just add full BB to list.
        if (!is_sub_domain_expr()) {
            TRACE_MSG("adding 1 sub-BB: [" << _bundle_bb.bb_begin.makeDimValStr() <<
                       " ... " << _bundle_bb.bb_end.makeDimValStr() << ")");
            _bb_list.push_back(_bundle_bb);
            return;
        }

        // Goal: Create list of full BBs (non-overlapping & with no invalid
        // points) inside overall BB.
        YaskTimer bbtimer;
        bbtimer.start();

        // Divide the overall BB into a slice for each thread
        // across the outer dim.
        const int odim = 0;     // Use 0 instead of outer_posn because BB lens are in domain dims.
        idx_t outer_len = _bundle_bb.bb_len[odim];
        idx_t nthreads = yask_get_num_threads();
        idx_t len_per_thr = CEIL_DIV(outer_len, nthreads);
        TRACE_MSG("find_bounding_box: running " << nthreads << " thread(s) over " <<
                   outer_len << " point(s) in outer dim");

        // List of full BBs for each thread.
        BBList bb_lists[nthreads];

        // Run rect-finding code on each thread.
        // When these are done, we will merge the
        // rects from all threads.
        yask_for
            (0, nthreads, 1,
             [&](idx_t start, idx_t stop, idx_t thread_num) {
                auto& cur_bb_list = bb_lists[start];

                // Begin and end of this slice.
                // These tuples contain domain dims.
                IdxTuple slice_begin(_bundle_bb.bb_begin);
                slice_begin[odim] += start * len_per_thr;
                IdxTuple slice_end(_bundle_bb.bb_end);
                slice_end[odim] = min(slice_end[odim], slice_begin[odim] + len_per_thr);
                if (slice_end[odim] <= slice_begin[odim])
                    return; // from lambda.
                Indices islice_begin(slice_begin);
                Indices islice_end(slice_end);

                // Construct len of slice in all dims.
                IdxTuple slice_len = slice_end.subElements(slice_begin);
                Indices islice_len(slice_len);
                
                // Visit all points in slice, looking for a new
                // valid beginning point, 'ib*pt'.
                Indices ibspt(stencil_dims); // in stencil dims.
                Indices ibdpt(domain_dims);  // in domain dims.
                slice_len.visitAllPoints
                    ([&](const IdxTuple& ofs, size_t idx) {

                        // Find global point from 'ofs' in domain
                        // and stencil dims.
                        Indices iofs(ofs);
                        ibdpt = islice_begin.addElements(iofs); // domain tuple.
                        DOMAIN_VAR_LOOP(i, j) {
                            ibspt[i] = ibdpt[j];            // stencil tuple.
                        }

                        // Valid point must be in sub-domain and
                        // not seen before in this slice.
                        bool is_valid = is_in_valid_domain(ibspt);
                        if (is_valid) {
                            for (auto& bb : cur_bb_list) {
                                if (bb.is_in_bb(ibdpt)) {
                                    is_valid = false;
                                    break;
                                }
                            }
                        }
                        
                        // Process this new rect starting at 'ib*pt'.
                        if (is_valid) {

                            // Scan from 'ib*pt' to end of this slice
                            // looking for end of rect.
                            IdxTuple bdpt(domain_dims);
                            ibdpt.setTupleVals(bdpt);
                            IdxTuple scan_len = slice_end.subElements(bdpt);

                            // End point to be found, 'ie*pt'.
                            Indices iespt(stencil_dims); // stencil dims.
                            Indices iedpt(domain_dims);  // domain dims.

                            // Repeat scan until no adjustment is made.
                            bool do_scan = true;
                            while (do_scan) {
                                do_scan = false;

                                TRACE_MSG("scanning " << scan_len.makeDimValStr(" * ") <<
                                           " starting at " << bdpt.makeDimValStr());
                                scan_len.visitAllPoints
                                    ([&](const IdxTuple& eofs, size_t eidx) {

                                        // Make sure scan_len range is observed.
                                        for (int i = 0; i < nddims; i++)
                                            assert(eofs[i] < scan_len[i]);

                                        // Find global point from 'eofs'.
                                        Indices ieofs(eofs);
                                        iedpt = ibdpt.addElements(ieofs); // domain tuple.
                                        DOMAIN_VAR_LOOP(i, j) {
                                            iespt[i] = iedpt[j];            // stencil tuple.
                                        }

                                        // Valid point must be in sub-domain and
                                        // not seen before in this slice.
                                        bool is_evalid = is_in_valid_domain(iespt);
                                        if (is_evalid) {
                                            for (auto& bb : cur_bb_list) {
                                                if (bb.is_in_bb(iedpt)) {
                                                    is_evalid = false;
                                                    break;
                                                }
                                            }
                                        }

                                        // If this is an invalid point, adjust
                                        // scan range appropriately.
                                        if (!is_evalid) {

                                            // Adjust 1st dim that is beyond its starting pt.
                                            // This will reduce the range of the scan.
                                            for (int i = 0; i < nddims; i++) {

                                                // Beyond starting point in this dim?
                                                if (iedpt[i] > ibdpt[i]) {
                                                    scan_len[i] = iedpt[i] - ibdpt[i];

                                                    // restart scan for
                                                    // remaining dims.
                                                    // TODO: be smarter
                                                    // about where to
                                                    // restart scan.
                                                    if (i < nddims - 1)
                                                        do_scan = true;

                                                    return false; // stop this scan.
                                                }
                                            }
                                        }

                                        return true; // keep looking for invalid point.
                                    }); // Looking for invalid point.
                            } // while scan is adjusted.
                            TRACE_MSG("found BB " << scan_len.makeDimValStr(" * ") <<
                                       " starting at " << bdpt.makeDimValStr());

                            // 'scan_len' now contains sizes of the new BB.
                            BoundingBox new_bb;
                            new_bb.bb_begin = bdpt;
                            new_bb.bb_end = bdpt.addElements(scan_len);
                            new_bb.update_bb("sub-bb", *_context, true);
                            cur_bb_list.push_back(new_bb);
                            
                        } // new rect found.

                        return true;  // from labmda; keep looking.
                    }); // Looking for new rects.
            }); // threads/slices.
        TRACE_MSG("sub-bbs found in " <<
                   bbtimer.get_secs_since_start() << " secs.");
        // At this point, we have a set of full BBs.

        // Reset overall BB.
        _bundle_bb.bb_num_points = 0;
            
        // Collect BBs in all slices.
        // TODO: merge in a parallel binary tree instead of sequentially.
        for (int n = 0; n < nthreads; n++) {
            auto& cur_bb_list = bb_lists[n];
            TRACE_MSG("processing " << cur_bb_list.size() <<
                       " sub-BB(s) in bundle '" << get_name() <<
                       "' from thread " << n);

            // BBs in slice 'n'.
            for (auto& bbn : cur_bb_list) {
                TRACE_MSG(" sub-BB: [" << bbn.bb_begin.makeDimValStr() <<
                           " ... " << bbn.bb_end.makeDimValStr() << ")");

                // Don't bother with empty BB.
                if (bbn.bb_size == 0)
                    continue;

                // Init or update overall BB.
                if (!_bundle_bb.bb_num_points) {
                    _bundle_bb.bb_begin = bbn.bb_begin;
                    _bundle_bb.bb_end = bbn.bb_end;
                } else {
                    _bundle_bb.bb_begin = _bundle_bb.bb_begin.minElements(bbn.bb_begin);
                    _bundle_bb.bb_end = _bundle_bb.bb_end.maxElements(bbn.bb_end);
                }
                _bundle_bb.bb_num_points += bbn.bb_size;

                // Scan existing final BBs looking for one to merge with.
                bool do_merge = false;
                for (auto& bb : _bb_list) {

                    // Can 'bbn' be merged with 'bb'?
                    do_merge = true;
                    for (int i = 0; i < nddims && do_merge; i++) {

                        // Must be adjacent in outer dim.
                        if (i == odim) {
                            if (bb.bb_end[i] != bbn.bb_begin[i])
                                do_merge = false;
                        }

                        // Must be aligned in other dims.
                        else {
                            if (bb.bb_begin[i] != bbn.bb_begin[i] ||
                                bb.bb_end[i] != bbn.bb_end[i])
                                do_merge = false;
                        }
                    }
                    if (do_merge) {

                        // Merge by just increasing the size of 'bb'.
                        bb.bb_end[odim] = bbn.bb_end[odim];
                        TRACE_MSG("  merging to form [" << bb.bb_begin.makeDimValStr() <<
                                   " ... " << bb.bb_end.makeDimValStr() << ")");
                        bb.update_bb("sub-bb", *_context, true);
                        break;
                    }
                }

                // If not merged, add 'bbn' as new.
                if (!do_merge) {
                    _bb_list.push_back(bbn);
                    TRACE_MSG("  adding as final sub-BB #" << _bb_list.size());
                }
            }
        }

        // Finalize overall BB.
        _bundle_bb.update_bb(get_name(), *_context, false);
        bbtimer.stop();
        TRACE_MSG("find-bounding-box: done in " <<
                   bbtimer.get_elapsed_secs() << " secs.");
    }

    // Compute convenience values for a bounding-box.
    void BoundingBox::update_bb(const string& name,
                                StencilContext& context,
                                bool force_full,
                                ostream* os) {

        auto dims = context.get_dims();
        auto& domain_dims = dims->_domain_dims;
        bb_len = bb_end.subElements(bb_begin);
        bb_size = bb_len.product();
        if (force_full)
            bb_num_points = bb_size;

        // Solid rectangle?
        bb_is_full = true;
        if (bb_num_points != bb_size) {
            if (os)
                *os << "Note: '" << name << "' domain has only " <<
                    makeNumStr(bb_num_points) <<
                    " valid point(s) inside its bounding-box of " <<
                    makeNumStr(bb_size) <<
                    " point(s); multiple sub-boxes will be used.\n";
            bb_is_full = false;
        }

        // Does everything start on a vector-length boundary?
        bb_is_aligned = true;
        for (auto& dim : domain_dims.getDims()) {
            auto& dname = dim.getName();
            if ((bb_begin[dname] - context.rank_domain_offsets[dname]) %
                dims->_fold_pts[dname] != 0) {
                if (os)
                    *os << "Note: '" << name << "' domain"
                        " has one or more starting edges not on vector boundaries;"
                        " masked calculations will be used in peel and remainder sub-blocks.\n";
                bb_is_aligned = false;
                break;
            }
        }

        // Lengths are cluster-length multiples?
        bb_is_cluster_mult = true;
        for (auto& dim : domain_dims.getDims()) {
            auto& dname = dim.getName();
            if (bb_len[dname] % dims->_cluster_pts[dname] != 0) {
                if (bb_is_full && bb_is_aligned)
                    if (os && bb_is_aligned)
                        *os << "Note: '" << name << "' domain"
                            " has one or more sizes that are not vector-cluster multiples;"
                            " masked calculations will be used in peel and remainder sub-blocks.\n";
                bb_is_cluster_mult = false;
                break;
            }
        }

        // All done.
        bb_valid = true;
    }

} // namespace yask.
