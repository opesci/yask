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

#pragma once

namespace yask {

    // Classes that support evaluation of one stencil bundle
    // and a 'pack' of bundles.
    // A stencil context contains one or more packs.

    // A pure-virtual class base for a stencil bundle.
    class StencilBundleBase : 
        public ContextLinker {

    protected:
        std::string _name;
        int _scalar_fp_ops = 0;
        int _scalar_points_read = 0;
        int _scalar_points_written = 0;

        // Other bundles that this one depends on.
        StencilBundleSet _depends_on;

        // List of scratch-grid bundles that need to be evaluated
        // before this bundle. Listed in eval order first-to-last.
        StencilBundleList _scratch_children;

        // Whether this updates scratch grid(s);
        bool _is_scratch = false;

        // Overall bounding box for the bundle.
        // This may or may not be solid, i.e., it
        // may contain some invalid points.
        // This must fit inside the extended BB for this rank.
        BoundingBox _bundle_bb;
        
	// Bounding box(es) that indicate where this bundle is valid.
	// These must be non-overlapping. These do NOT contain
        // any invalid points. These will all be inside '_bundle_bb'.
	BBList _bb_list;
	
        // Normalize the indices, i.e., divide by vector len in each dim.
        // Ranks offsets must already be subtracted.
        // Each dim in 'orig' must be a multiple of corresponding vec len.
        void normalize_indices(const Indices& orig, Indices& norm) const {
            STATE_VARS(this);
            assert(orig.getNumDims() == nsdims);
            assert(norm.getNumDims() == nsdims);

            // i: index for stencil dims, j: index for domain dims.
            DOMAIN_VAR_LOOP(i, j) {
            
                // Divide indices by fold lengths as needed by
                // read/writeVecNorm().  Use idiv_flr() instead of '/'
                // because begin/end vars may be negative (if in halo).
                norm[i] = idiv_flr<idx_t>(orig[i], fold_pts[j]);
            
                // Check for no remainder.
                assert(imod_flr<idx_t>(orig[i], fold_pts[j]) == 0);
            }
        }

    public:

        // Grids that are written to by these stencils.
        GridPtrs outputGridPtrs;

        // Grids that are read by these stencils (not necessarify
        // read-only, i.e., a grid can be input and output).
        GridPtrs inputGridPtrs;

        // Vectors of scratch grids that are written to/read from.
        ScratchVecs outputScratchVecs;
        ScratchVecs inputScratchVecs;

        // ctor, dtor.
        StencilBundleBase(StencilContext* context) :
            ContextLinker(context) { }
        virtual ~StencilBundleBase() { }

        // Get name of this bundle.
        const std::string& get_name() const { return _name; }

        // Get estimated number of FP ops done for one scalar eval.
        virtual int get_scalar_fp_ops() const { return _scalar_fp_ops; }

        // Get number of points read and written for one scalar eval.
        virtual int get_scalar_points_read() const { return _scalar_points_read; }
        virtual int get_scalar_points_written() const { return _scalar_points_written; }

        // Scratch accessors.
        bool is_scratch() const { return _is_scratch; }
        void set_scratch(bool is_scratch) { _is_scratch = is_scratch; }

        // Access to BBs.
        BoundingBox& getBB() { return _bundle_bb; }
        BBList& getBBs() { return _bb_list; }

        // Add dependency.
        virtual void add_dep(StencilBundleBase* eg) {
            _depends_on.insert(eg);
        }

        // Get dependencies.
        virtual const StencilBundleSet& get_deps() const {
            return _depends_on;
        }

        // Add needed scratch-bundle.
        virtual void add_scratch_child(StencilBundleBase* eg) {
            _scratch_children.push_back(eg);
        }

        // Get needed scratch-bundle(s).
        const StencilBundleList& get_scratch_children() const {
            return _scratch_children;
        }

        // Get scratch children plus self.
        StencilBundleList get_reqd_bundles() {
            auto sg_list = get_scratch_children(); // Do children first.
            sg_list.push_back(this); // Do self last.
            return sg_list;
        }

        // If this bundle is updating scratch grid(s),
        // expand indices to calculate values in halo.
        // Adjust offsets in grids based on original idxs.
        // Return adjusted indices.
        ScanIndices adjust_span(int thread_idx, const ScanIndices& idxs) const;

        // Set the bounding-box vars for this bundle in this rank.
        void find_bounding_box();

        // Copy BB vars from another.
        void copy_bounding_box(const StencilBundleBase* src);
        
        // Determine whether indices are in [sub-]domain.
        virtual bool
        is_in_valid_domain(const Indices& idxs) const =0;

        // Return true if there is a non-default conditions.
        virtual bool
        is_sub_domain_expr() const { return false; }
        virtual bool
        is_step_cond_expr() const { return false; }

        // Return human-readable description of conditions.
        virtual std::string
        get_domain_description() const =0;
        virtual std::string
        get_step_cond_description() const =0;
        
        // Determine whether step index is enabled.
        virtual bool
        is_in_valid_step(idx_t input_step_index) const =0;

        // If bundle updates grid(s) with the step index,
        // set 'output_step_index' to the step that an update
        // occurs when calling one of the calc_*() methods with
        // 'input_step_index' and return 'true'.
        // Else, return 'false';
        virtual bool
        get_output_step_index(idx_t input_step_index,
                              idx_t& output_step_index) const =0;

        // Calculate one scalar result.
        virtual void
        calc_scalar(int thread_idx, const Indices& idxs) =0;

        // Calculate results within a mini-block.
        void
        calc_mini_block(int region_thread_idx,
                        KernelSettings& settings,
                        const ScanIndices& mini_block_idxs);

        // Calculate results within a sub-block.
        void
        calc_sub_block_vec(int region_thread_idx,
                           int block_thread_idx,
                           KernelSettings& settings,
                           const ScanIndices& mini_block_idxs);
        void
        calc_sub_block_scalar(int region_thread_idx,
                              int block_thread_idx,
                              KernelSettings& settings,
                              const ScanIndices& mini_block_idxs);
        inline void
        calc_sub_block(int region_thread_idx,
                       int block_thread_idx,
                       KernelSettings& settings,
                       const ScanIndices& mini_block_idxs) {
            if (block_thread_idx < 0)
                block_thread_idx = omp_get_thread_num();
            if (settings.force_scalar)
                calc_sub_block_scalar(region_thread_idx, block_thread_idx,
                                      settings, mini_block_idxs);
            else
                calc_sub_block_vec(region_thread_idx, block_thread_idx,
                                   settings, mini_block_idxs);
        }

        // Calculate a series of cluster results within an inner loop.
        // All indices start at 'start_idxs'. Inner loop iterates to
        // 'stop_inner' by 'step_inner'.
        // Indices must be rank-relative.
        // Indices must be normalized, i.e., already divided by VLEN_*.
        virtual void
        calc_loop_of_clusters(int region_thread_idx,
                              int block_thread_idx,
                              const Indices& start_idxs,
                              idx_t stop_inner) =0;

        // Calculate a series of cluster results within an inner loop.
        // The 'loop_idxs' must specify a range only in the inner dim.
        // Indices must be rank-relative.
        // Indices must be normalized, i.e., already divided by VLEN_*.
        void
        calc_loop_of_clusters(int region_thread_idx,
                              int block_thread_idx,
                              const ScanIndices& loop_idxs);

        // Calculate a series of vector results within an inner loop.
        // All indices start at 'start_idxs'. Inner loop iterates to
        // 'stop_inner' by 'step_inner'.
        // Indices must be rank-relative.
        // Indices must be normalized, i.e., already divided by VLEN_*.
        // Each vector write is masked by 'write_mask'.
        virtual void
        calc_loop_of_vectors(int region_thread_idx,
                             int block_thread_idx,
                             const Indices& start_idxs,
                             idx_t stop_inner,
                             idx_t write_mask) =0;

        // Calculate a series of vector results within an inner loop.
        // The 'loop_idxs' must specify a range only in the inner dim.
        // Indices must be rank-relative.
        // Indices must be normalized, i.e., already divided by VLEN_*.
        // Each vector write is masked by 'write_mask'.
        void
        calc_loop_of_vectors(int region_thread_idx,
                             int block_thread_idx,
                             const ScanIndices& loop_idxs,
                             idx_t write_mask);

    };                          // StencilBundleBase.

    // A collection of independent stencil bundles.
    // "Independent" implies that they may be evaluated
    // in any order.
    class BundlePack :
        public ContextLinker,
        public std::vector<StencilBundleBase*> {

    protected:
        std::string _name;

        // Union of bounding boxes for all bundles in this pack.
        BoundingBox _pack_bb;

        // Local pack settings.
        // Only some of these will be used.
        KernelSettings _pack_opts;

        // Auto-tuner for pack settings.
        AutoTuner _at;
        
    public:

        // Perf stats for this pack.
        YaskTimer timer;
        idx_t steps_done = 0;
        Stats stats;

        // Work needed across points in this rank.
        idx_t num_reads_per_step = 0;
        idx_t num_writes_per_step = 0;
        idx_t num_fpops_per_step = 0;

        // Work done across all ranks.
        idx_t tot_reads_per_step = 0;
        idx_t tot_writes_per_step = 0;
        idx_t tot_fpops_per_step = 0;
        
        BundlePack(StencilContext* context,
                   const std::string& name) :
            ContextLinker(context),
            _name(name),
            _pack_opts(*context->get_state()->_opts), // init w/a copy of the base settings.
            _at(context, &_pack_opts, name) { }
        virtual ~BundlePack() { }

        const std::string& get_name() {
            return _name;
        }

        // Update the amount of work stats.
        // Print to current debug stream.
        void init_work_stats();

        // Determine whether step index is enabled.
        bool
        is_in_valid_step(idx_t input_step_index) const {
            if (!size())
                return false;

            // All step conditions must be the same, so
            // we call first one.
            return front()->is_in_valid_step(input_step_index);
        }

        // Accessors.
        BoundingBox& getBB() { return _pack_bb; }
        AutoTuner& getAT() { return _at; }
        KernelSettings& getLocalSettings() { return _pack_opts; }

        // If using separate pack tuners, return local settings.
        // Otherwise, return one in context.
        KernelSettings& getActiveSettings() {
            STATE_VARS(this);
            return use_pack_tuners() ? _pack_opts : *opts;
        }

        // Perf-tracking methods.
        void start_timers();
        void stop_timers();
        void add_steps(idx_t num_steps);

    }; // BundlePack.

} // yask namespace.
