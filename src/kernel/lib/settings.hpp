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

    // Forward defns.
    class StencilContext;
    class YkGridBase;
    class YkGridImpl;

    // Some derivations from grid types.
    typedef std::shared_ptr<YkGridImpl> YkGridPtr;
    typedef std::set<YkGridPtr> GridPtrSet;
    typedef std::vector<YkGridPtr> GridPtrs;
    typedef std::map<std::string, YkGridPtr> GridPtrMap;
    typedef std::vector<GridPtrs*> ScratchVecs;

    // Environmental settings.
    class KernelEnv :
        public virtual yk_env {

        // An OpenMP lock to use for debug.
        static omp_lock_t _debug_lock;
        static bool _debug_lock_init_done;

    public:

        // MPI vars.
        MPI_Comm comm = MPI_COMM_NULL; // global communicator.
        MPI_Group group = MPI_GROUP_NULL;
        int num_ranks = 1;        // total number of ranks.
        int my_rank = 0;          // MPI-assigned index.

        // Vars for shared-mem ranks.
        MPI_Comm shm_comm = MPI_COMM_NULL; // shm communicator.
        MPI_Group shm_group = MPI_GROUP_NULL;
        int num_shm_ranks = 1;  // ranks in shm_comm.
        int my_shm_rank = 0;    // my index in shm_comm.

        // OMP vars.
        int max_threads=0;      // initial value from OMP.

        KernelEnv() { }
        virtual ~KernelEnv() { }

        // Init MPI, OMP, etc.
        // This is normally called very early in the program.
        virtual void initEnv(int* argc, char*** argv, MPI_Comm comm);

        // Lock.
        static void set_debug_lock() {
            if (!_debug_lock_init_done) {
                omp_init_lock(&_debug_lock);
                _debug_lock_init_done = true;
            }
            omp_set_lock(&_debug_lock);
        }
        static void unset_debug_lock() {
            assert(_debug_lock_init_done);
            omp_unset_lock(&_debug_lock);
        }
        
        // APIs.
        virtual int get_num_ranks() const {
            return num_ranks;
        }
        virtual int get_rank_index() const {
            return my_rank;
        }
        virtual void global_barrier() const {
            MPI_Barrier(comm);
        }
    };
    typedef std::shared_ptr<KernelEnv> KernelEnvPtr;

    // Dimensions for a solution.
    // Similar to the Dimensions class in the YASK compiler
    // from which these values are set.
    struct Dims {

        // Algorithm for vec dims in fold layout.
        VEC_FOLD_LAYOUT_CLASS _vec_fold_layout;

        // Dimensions with 0 values.
        std::string _step_dim;  // usually time, 't'.
        std::string _inner_dim; // the domain dim used in the inner loop.
        IdxTuple _domain_dims;
        IdxTuple _stencil_dims; // step & domain dims.
        IdxTuple _misc_dims;

        // Dimensions and sizes.
        IdxTuple _fold_pts;     // all domain dims.
        IdxTuple _vec_fold_pts; // just those with >1 pts.
        IdxTuple _cluster_pts;  // all domain dims.
        IdxTuple _cluster_mults; // all domain dims.

        // Direction of step.
        // This is a heuristic value used only for stepping the
        // perf-measuring utility and the auto-tuner.
        int _step_dir = 0;    // 0: undetermined, +1: forward, -1: backward.

        // Check whether dim exists and is of allowed type.
        // If not, abort with error, reporting 'fn_name'.
        void checkDimType(const std::string& dim,
                          const std::string& fn_name,
                          bool step_ok,
                          bool domain_ok,
                          bool misc_ok) const;

        // Get linear index into a vector given 'fold_ofs', which are
        // element offsets that must be *exactly* those in _vec_fold_pts.
        idx_t getElemIndexInVec(const Indices& fold_ofs) const {
            assert(fold_ofs.getNumDims() == NUM_VEC_FOLD_DIMS);

            // Use compiler-generated fold macro.
            idx_t i = VEC_FOLD_LAYOUT(fold_ofs);

#ifdef DEBUG_LAYOUT
            // Use compiler-generated fold layout class.
            idx_t j = _vec_fold_layout.layout(fold_ofs);
            assert(i == j);
#endif

            return i;
        }

        // Get linear index into a vector given 'elem_ofs', which are
        // element offsets that may include other dimensions.
        idx_t getElemIndexInVec(const IdxTuple& elem_ofs) const {
            assert(_vec_fold_pts.getNumDims() == NUM_VEC_FOLD_DIMS);
            if (NUM_VEC_FOLD_DIMS == 0)
                return 0;

            // Get required offsets into an Indices obj.
            IdxTuple fold_ofs(_vec_fold_pts);
            fold_ofs.setValsSame(0);
            fold_ofs.setVals(elem_ofs, false); // copy only fold offsets.
            Indices fofs(fold_ofs);

            // Call version that requires vec-fold offsets only.
            idx_t i = getElemIndexInVec(fofs);

            // Use fold layout to find element index.
#ifdef DEBUG_LAYOUT
            idx_t i2 = _vec_fold_pts.layout(fold_ofs, false);
            assert(i == i2);
#endif
            return i;
        }
    };
    typedef std::shared_ptr<Dims> DimsPtr;

    // Utility to determine number of points in a "sizes" var.
    inline idx_t get_num_domain_points(const IdxTuple& sizes) {
        assert(sizes.getNumDims() == NUM_STENCIL_DIMS);
        idx_t pts = 1;
        DOMAIN_VAR_LOOP(i, j)
            pts *= sizes[i];
        return pts;
    }

    // Application settings to control size and perf of stencil code.  Most
    // of these vars can be set via cmd-line options and/or APIs.
    class KernelSettings {

    protected:

        // Default sizes.
        idx_t def_block = 32;   // TODO: calculate this.

        // Make a null output stream.
        // TODO: put this somewhere else.
        yask_output_factory yof;
        yask_output_ptr nullop = yof.new_null_output();

    public:

        // Ptr to problem dimensions (NOT sizes), folding, etc.
        // This is solution info from the YASK compiler.
        DimsPtr _dims;

        // Sizes in elements (points).
        // All these tuples contain stencil dims, even the ones that
        // don't strictly need them.
        IdxTuple _global_sizes;     // Overall problem domain sizes.
        IdxTuple _rank_sizes;     // This rank's domain sizes.
        IdxTuple _region_sizes;   // region size (used for wave-front tiling).
        IdxTuple _block_group_sizes; // block-group size (only used for 'grouped' region loops).
        IdxTuple _block_sizes;       // block size (used for each outer thread).
        IdxTuple _mini_block_group_sizes; // mini-block-group size (only used for 'grouped' block loops).
        IdxTuple _mini_block_sizes;       // mini-block size (used for wave-fronts in blocks).
        IdxTuple _sub_block_group_sizes; // sub-block-group size (only used for 'grouped' mini-block loops).
        IdxTuple _sub_block_sizes;       // sub-block size (used for each nested thread).
        IdxTuple _min_pad_sizes;         // minimum spatial padding (including halos).
        IdxTuple _extra_pad_sizes;       // extra spatial padding (outside of halos).

        // MPI settings.
        IdxTuple _num_ranks;       // number of ranks in each dim.
        IdxTuple _rank_indices;    // my rank index in each dim.
        bool find_loc = true;      // whether my rank index needs to be calculated.
        int msg_rank = 0;          // rank that prints informational messages.
        bool overlap_comms = true; // overlap comms with computation.
        bool use_shm = false;      // use shared memory if possible.
        idx_t _min_exterior = 0;   // minimum size of MPI exterior to calculate.

        // OpenMP settings.
        int max_threads = 0;      // Initial number of threads to use overall; 0=>OMP default.
        int thread_divisor = 1;   // Reduce number of threads by this amount.
        int num_block_threads = 1; // Number of threads to use for a block.
        bool bind_block_threads = false; // Bind block threads to indices.

        // Grid behavior.
        bool _step_wrap = false; // Allow invalid step indices to alias to valid ones.
        
        // Stencil-dim posn in which to apply block-thread binding.
        // TODO: make this a cmd-line parameter.
        int _bind_posn = 1;

        // Tuning.
        bool _do_auto_tune = false;    // whether to do auto-tuning.
        bool _tune_mini_blks = false; // auto-tune mini-blks instead of blks.
        bool _allow_pack_tuners = false; // allow per-pack tuners when possible.
        
        // Debug.
        bool force_scalar = false; // Do only scalar ops.
        bool _trace = false;       // Print verbose tracing.

        // NUMA settings.
        int _numa_pref = NUMA_PREF;
        int _numa_pref_max = 128; // GiB to alloc before using PMEM.

        // Ctor/dtor.
        KernelSettings(DimsPtr dims, KernelEnvPtr env);
        virtual ~KernelSettings() { }

    protected:
        // Add options to set one domain var to a cmd-line parser.
        virtual void _add_domain_option(CommandLineParser& parser,
                                        const std::string& prefix,
                                        const std::string& descrip,
                                        IdxTuple& var,
                                        bool allow_step = false);

        idx_t findNumSubsets(std::ostream& os,
                             IdxTuple& inner_sizes, const std::string& inner_name,
                             const IdxTuple& outer_sizes, const std::string& outer_name,
                             const IdxTuple& mults, const std::string& step_dim);

    public:
        // Add options to a cmd-line parser to set the settings.
        virtual void add_options(CommandLineParser& parser);

        // Print usage message.
        void print_usage(std::ostream& os,
                         CommandLineParser& parser,
                         const std::string& pgmName,
                         const std::string& appNotes,
                         const std::vector<std::string>& appExamples) const;

        // Make sure all user-provided settings are valid by rounding-up
        // values as needed.
        // Called from prepare_solution(), so it doesn't normally need to be called from user code.
        // Prints informational info to 'os'.
        virtual void adjustSettings(std::ostream& os);
        virtual void adjustSettings() {
            adjustSettings(nullop->get_ostream());
        }

        // Determine if this is the first or last rank in given dim.
        virtual bool is_first_rank(const std::string dim) {
            return _rank_indices[dim] == 0;
        }
        virtual bool is_last_rank(const std::string dim) {
            return _rank_indices[dim] == _num_ranks[dim] - 1;
        }
    };
    typedef std::shared_ptr<KernelSettings> KernelSettingsPtr;

    // MPI neighbor info.
    class MPIInfo {

    public:
        // Problem dimensions.
        DimsPtr _dims;

        // Each rank can have up to 3 neighbors in each dim, including self.
        // Example for 2D:
        //   +------+------+------+
        //   |x=prev|x=self|x=next|
        //   |y=next|y=next|y=next|
        //   +------+------+------+
        //   |x=prev|x=self|x=next|
        //   |y=self|y=self|y=self| Center rank is self.
        //   +------+------+------+
        // ^ |x=prev|x=self|x=next|
        // | |y=prev|y=prev|y=prev|
        // y +------+------+------+
        //   x-->
        enum NeighborOffset { rank_prev, rank_self, rank_next, num_offsets };

        // Max number of immediate neighbors in all domain dimensions.
        // Used to describe the n-D space of neighbors.
        // This object is effectively a constant used to convert between
        // n-D and 1-D indices.
        IdxTuple neighborhood_sizes;

        // Neighborhood size includes self.
        // Number of points in n-D space of neighbors.
        // Example: size = 3^3 = 27 for 3D problem.
        // NB: this is the *max* number of neighbors, not necessarily the actual number.
        idx_t neighborhood_size = 0;

        // What getNeighborIndex() returns for myself.
        // Example: trunc(3^3 / 2) = 13 for 3D problem.
        int my_neighbor_index;

        // MPI rank of each neighbor.
        // MPI_PROC_NULL => no neighbor.
        // Vector index is per getNeighborIndex().
        typedef std::vector<int> Neighbors;
        Neighbors my_neighbors;

        // Manhattan distance to each neighbor.
        // Vector index is per getNeighborIndex().
        std::vector<int> man_dists;

        // Whether each neighbor has all its rank-domain
        // sizes as a multiple of the vector length.
        std::vector<bool> has_all_vlen_mults;

        // Rank number in KernelEnv::shmcomm if this neighbor
        // can communicate with shm. MPI_PROC_NULL otherwise.
        std::vector<int> shm_ranks;

        // Window for halo buffers.
        MPI_Win halo_win;

        // Shm halo buffers for each neighbor.
        std::vector<void*> halo_buf_ptrs;
        std::vector<size_t> halo_buf_sizes;
        
        // Ctor based on pre-set problem dimensions.
        MPIInfo(DimsPtr dims) : _dims(dims) {

            // Max neighbors.
            neighborhood_sizes = dims->_domain_dims; // copy dims from domain.
            neighborhood_sizes.setValsSame(num_offsets); // set sizes in each domain dim.
            neighborhood_size = neighborhood_sizes.product(); // neighbors in all dims.

            // Myself.
            IdxTuple noffsets(neighborhood_sizes);
            noffsets.setValsSame(rank_self);
            my_neighbor_index = getNeighborIndex(noffsets);

            // Init arrays.
            my_neighbors.resize(neighborhood_size, MPI_PROC_NULL);
            man_dists.resize(neighborhood_size, 0);
            has_all_vlen_mults.resize(neighborhood_size, false);
            shm_ranks.resize(neighborhood_size, MPI_PROC_NULL);
            halo_buf_ptrs.resize(neighborhood_size, 0);
            halo_buf_sizes.resize(neighborhood_size, 0);
        }

        // Get a 1D index for a neighbor.
        // Input 'offsets': tuple of NeighborOffset vals.
        virtual idx_t getNeighborIndex(const IdxTuple& offsets) const {
            idx_t i = neighborhood_sizes.layout(offsets); // 1D index.
            assert(i >= 0);
            assert(i < neighborhood_size);
            return i;
        }

        // Visit all neighbors.
        // Does NOT visit self.
        virtual void visitNeighbors(std::function<void
                                    (const IdxTuple& offsets, // NeighborOffset vals.
                                     int rank, // MPI rank; might be MPI_PROC_NULL.
                                     int index // simple counter from 0.
                                     )> visitor);
    };
    typedef std::shared_ptr<MPIInfo> MPIInfoPtr;

    // MPI data for one buffer for one neighbor of one grid.
    class MPIBuf {
        
        // Ptr to read/write lock when buffer is in shared mem.
        SimpleLock* _shm_lock = 0;

    public:

        // Descriptive name.
        std::string name;

        // Send or receive buffer.
        std::shared_ptr<char> _base;
        real_t* _elems = 0;

        // Range to copy to/from grid.
        // NB: step index not set properly for grids with step dim.
        IdxTuple begin_pt, last_pt;

        // Number of points to copy to/from grid in each dim.
        IdxTuple num_pts;

        // Whether the number of points is a multiple of the
        // vector length in all dims and buffer is aligned.
        bool vec_copy_ok = false;

        // Safe access to lock.
        void shm_lock_init() {
            if (_shm_lock)
                _shm_lock->init();
        }
        bool is_ok_to_read() const {
            if (_shm_lock)
                return _shm_lock->is_ok_to_read();
            return true;
        }
        void wait_for_ok_to_read() const {
            if (_shm_lock)
                _shm_lock->wait_for_ok_to_read();
        }
        void mark_read_done() {
            if (_shm_lock)
                _shm_lock->mark_read_done();
        }
        bool is_ok_to_write() const {
            if (_shm_lock)
                return _shm_lock->is_ok_to_write();
            return true;
        }
        void wait_for_ok_to_write() const {
            if (_shm_lock)
                _shm_lock->wait_for_ok_to_write();
        }
        void mark_write_done() {
            if (_shm_lock)
                _shm_lock->mark_write_done();
        }
        
        // Number of points overall.
        idx_t get_size() const {
            if (num_pts.size() == 0)
                return 0;
            return num_pts.product();
        }
        idx_t get_bytes() const {
            return get_size() * sizeof(real_t);
        }

        // Set pointer to storage.
        // Free old storage.
        // 'base' should provide get_num_bytes() bytes at offset bytes.
        // Returns raw pointer.
        void* set_storage(std::shared_ptr<char>& base, size_t offset);

        // Same as above, but does not maintain shared storage.
        void* set_storage(char* base, size_t offset);

        // Release storage.
        void release_storage() {
            _base.reset();
            _elems = 0;
            _shm_lock = 0;
        }

        // Reset.
        void clear() {
            name.clear();
            begin_pt.clear();
            last_pt.clear();
            num_pts.clear();
            release_storage();
        }
        ~MPIBuf() {
            clear();
        }
    };

    // MPI data for both buffers for one neighbor of one grid.
    struct MPIBufs {

        // Need one buf for send and one for receive for each neighbor.
        enum BufDir { bufSend, bufRecv, nBufDirs };

        MPIBuf bufs[nBufDirs];

        // Reset lock for send buffer.
        // Another rank owns recv buffer.
        void reset_locks() {
            bufs[bufSend].shm_lock_init();
        }
    };

    // MPI data for one grid.
    // Contains a send and receive buffer for each neighbor
    // and some meta-data.
    struct MPIData {

        MPIInfoPtr _mpiInfo;

        // Buffers for all possible neighbors.
        typedef std::vector<MPIBufs> NeighborBufs;
        NeighborBufs bufs;

        // Arrays for request handles.
        // These are used for async comms.
        std::vector<MPI_Request> recv_reqs;
        std::vector<MPI_Request> send_reqs;
        
        MPIData(MPIInfoPtr mpiInfo) :
            _mpiInfo(mpiInfo) {

            // Init vector of buffers.
            auto n = _mpiInfo->neighborhood_size;
            MPIBufs emptyBufs;
            bufs.resize(n, emptyBufs);

            // Init handles.
            recv_reqs.resize(n, MPI_REQUEST_NULL);
            send_reqs.resize(n, MPI_REQUEST_NULL);
        }

        void reset_locks() {
            for (auto& mb : bufs)
                mb.reset_locks();
        }
        
        // Apply a function to each neighbor rank.
        // Called visitor function will contain the rank index of the neighbor.
        virtual void visitNeighbors(std::function<void (const IdxTuple& neighbor_offsets, // NeighborOffset.
                                                        int rank,
                                                        int index, // simple counter from 0.
                                                        MPIBufs& bufs)> visitor);

        // Access a buffer by direction and neighbor offsets.
        virtual MPIBuf& getBuf(MPIBufs::BufDir bd, const IdxTuple& neighbor_offsets);
    };

    // A collection of solution meta-data whose ownership is shared between
    // various objects.
    struct KernelState {
        virtual ~KernelState() { }

        // Output stream for messages.
        yask_output_ptr _debug;

        // Environment (mostly MPI).
        KernelEnvPtr _env;

        // User settings.
        KernelSettingsPtr _opts;
        bool _use_pack_tuners = false;

        // Problem dims.
        DimsPtr _dims;

        // Position of inner domain dim in stencil-dims tuple.
        // Misc dims will follow this if/when using interleaving.
        // TODO: move to Dims.
        int _inner_posn = -1;   // -1 => not set.

        // Position of outer domain dim in stencil-dims tuple.
        // For 1D stencils, _outer_posn == _inner_posn.
        // TODO: move to Dims.
        int _outer_posn = -1;   // -1 => not set.

        // MPI neighbor info.
        MPIInfoPtr _mpiInfo;
    };
    typedef std::shared_ptr<KernelState> KernelStatePtr;

    // Macro to define and set commonly-needed state vars efficiently.
    // '_ksbp' is pointer to a 'KernelStateBase' object.
    // '*_posn' vars are positions in stencil_dims.
#define STATE_VARS0(_ksbp, pfx)                                         \
    pfx auto* ksbp = _ksbp;                                             \
    assert(ksbp);                                                       \
    pfx auto* state = ksbp->_state.get();                               \
    assert(state);                                                      \
    assert(state->_debug.get());                                        \
    auto& os = state->_debug.get()->get_ostream();                      \
    pfx auto* env = state->_env.get();                                  \
    assert(env);                                                        \
    pfx auto* opts = state->_opts.get();                                \
    assert(opts);                                                       \
    pfx auto* dims = state->_dims.get();                                \
    assert(dims);                                                       \
    pfx auto* mpiInfo = state->_mpiInfo.get();                          \
    assert(mpiInfo);                                                    \
    const auto& step_dim = dims->_step_dim;                             \
    const auto& inner_dim = dims->_inner_dim;                           \
    const auto& domain_dims = dims->_domain_dims;                       \
    constexpr int nddims = NUM_DOMAIN_DIMS;                             \
    assert(nddims == domain_dims.size());                               \
    const auto& stencil_dims = dims->_stencil_dims;                     \
    constexpr int nsdims = NUM_STENCIL_DIMS;                            \
    assert(nsdims == stencil_dims.size());                              \
    auto& misc_dims = dims->_misc_dims;                                 \
    constexpr int step_posn = 0;                                        \
    assert(step_posn == +Indices::step_posn);                           \
    constexpr int outer_posn = 1;                                       \
    const int inner_posn = state->_inner_posn
#define STATE_VARS(_ksbp) STATE_VARS0(_ksbp,)
#define STATE_VARS_CONST(_ksbp) STATE_VARS0(_ksbp, const)

    // A base class containing a shared pointer to a kernel state.
    // Used to ensure that the shared state object stays allocated when
    // at least one of its owners exists.
    class KernelStateBase {
    protected:

        // Common state. This is a separate object to allow
        // multiple objects to keep it alive via shared ptrs.
        KernelStatePtr _state;

    public:
        KernelStateBase(KernelStatePtr& state) :
            _state(state) {}
        KernelStateBase(KernelEnvPtr& env,
                        KernelSettingsPtr& settings);
        virtual ~KernelStateBase() {}

        // Access to state.
        KernelStatePtr& get_state() {
            assert(_state);
            return _state;
        }
        const KernelStatePtr& get_state() const {
            assert(_state);
            return _state;
        }
        KernelSettingsPtr& get_settings() { return _state->_opts; }
        const KernelSettingsPtr& get_settings() const { return _state->_opts; }
        KernelEnvPtr& get_env() { return _state->_env; }
        const KernelEnvPtr& get_env() const { return _state->_env; }
        DimsPtr& get_dims() { return _state->_dims; }
        const DimsPtr& get_dims() const { return _state->_dims; }
        MPIInfoPtr& get_mpi_info() { return _state->_mpiInfo; }
        const MPIInfoPtr& get_mpi_info() const { return _state->_mpiInfo; }
        bool use_pack_tuners() const { return _state->_use_pack_tuners; }
        virtual yask_output_ptr get_debug_output() const { return _state->_debug; }
        virtual void set_debug_output(yask_output_ptr debug) { _state->_debug = debug; }

        // Set debug output to cout if my_rank == msg_rank
        // or a null stream otherwise.
        std::ostream& set_ostr();

        // Set number of threads w/o using thread-divisor.
        // Return number of threads.
        // Do nothing and return 0 if not properly initialized.
        int set_max_threads();

        // Get total number of computation threads to use.
        int get_num_comp_threads(int& region_threads, int& blk_threads) const;
        
        // Set number of threads to use for a region.
        // Enable nested OMP if there are >1 block threads,
        // disable otherwise.
        // Return number of threads.
        // Do nothing and return 0 if not properly initialized.
        int set_region_threads();

        // Set number of threads for a block.
        // Return number of threads.
        // Do nothing and return 0 if not properly initialized.
        int set_block_threads();

    };

    // An object that is created from a context, shares ownership of the
    // state, and keeps a pointer back to the context. However, it does not
    // share ownership of the context itself. That would create an ownership
    // loop that would not allow the context to be deleted.
    class ContextLinker :
        public KernelStateBase {

    protected:
        StencilContext* _context;

    public:
        ContextLinker(StencilContext* context);
        virtual ~ContextLinker() { }
    };

} // yask namespace.
