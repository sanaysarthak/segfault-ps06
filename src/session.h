#pragma once

/* The debug session: owns the inferior, the kernel image, the breakpoints, and the
 * work-item bookkeeping that turns raw traps into "work-item (17,0,0) reached
 * blur3.cl:24".
 */

#include "dwarf_index.h"
#include "expr.h"
#include "inferior.h"
#include "pocl.h"
#include "runspec.h"
#include "workitem.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace oclgdb {

class Session {
public:
    struct Options {
        /* Collect a snapshot for every work-item of the work-group that first reaches a
         * breakpoint, instead of stopping at the first arrival. This is what makes
         * `info work-items` able to compare work-items against each other. */
        bool     scan_work_group = true;
        unsigned list_context = 5;
        bool     verbose = false;
    };

    /* A user breakpoint hit that arrived before its governing identity model was ready.
     * Its machine state is captured immediately (registers, and anything resolvable
     * from them); only the work-item *identity* is resolved later. See "Identity
     * calibration" in session.cpp. */
    struct PendingHit {
        int          bp_id = 0;
        int          replica = -1;
        unsigned     line = 0;
        uint64_t     rt_addr = 0;
        uint64_t     link_addr = 0;
        RegisterFile regs;
    };

    struct UserBp {
        int      id = 0;
        unsigned line = 0;
        bool     enabled = true;
        bool     filtered = false;         /* halt only for one work-item */
        size_t   wi[3] = {0, 0, 0};
        bool     wi_is_local = false;      /* filter is a local id within its group */
        size_t   filter_group[3] = {0, 0, 0};
        size_t   hits = 0;
        std::vector<uint64_t> link_addrs;  /* one per replica in replication mode */
        std::string describe() const;

        /* Identity calibration is tried per-breakpoint first: it is fast (needs only
         * this line's own hits) and correct for any *non-divergent* line, in whichever
         * loop/region it happens to sit in -- important because pocl compiles the code
         * either side of a barrier as separate loops with independently-allocated
         * identity registers (see "Identity calibration" in session.cpp). Only a
         * genuinely divergent line -- reached by a subset of the group -- falls back to
         * the kernel-wide model. */
        bool     own_calib_done = false;
        bool     own_calib_failed = false;
        IdentityModel own_model;
        std::vector<CalibrationSample> own_calib;   /* transient, current work-group */
        std::vector<PendingHit> own_pending;        /* transient, current work-group */
    };

    enum class State { NotStarted, Stopped, Exited };

    struct StopReport {
        bool        valid = false;
        int         bp_id = -1;
        unsigned    line = 0;
        size_t      group[3] = {0, 0, 0};
        size_t      reached = 0;       /* work-items that hit the line in this group */
        size_t      group_size = 0;
        bool        live = false;      /* a work-item is genuinely halted in the kernel */
        std::string message;
    };

    Session();
    ~Session();

    Options &options() { return opts_; }

    /* ------------------------------------------------------------- setup */
    bool load_spec(const std::string &path, std::string &err);
    bool spec_loaded() const { return spec_loaded_; }
    const rs_spec &spec() const { return spec_; }
    const NDRange &nd() const { return nd_; }
    const std::string &kernel_source() const { return kernel_source_; }
    const std::vector<std::string> &source_lines() const { return source_lines_; }

    /* Build the kernel through pocl (a plain untraced run of the harness) and index the
     * DWARF in the object pocl cached. Idempotent. */
    bool ensure_kernel_image(std::string &err);
    bool have_image() const { return dwarf_.loaded(); }
    const DwarfIndex &dwarf() const { return dwarf_; }
    const std::string &object_path() const { return object_path_; }
    uint64_t load_bias() const { return load_bias_; }

    /* ------------------------------------------------------- breakpoints */
    int  add_breakpoint(unsigned line, bool filtered, const size_t wi[3], bool wi_is_local,
                        std::string &err);
    bool remove_breakpoint(int id);
    void clear_breakpoints();
    const std::vector<UserBp> &breakpoints() const { return bps_; }

    /* --------------------------------------------------------- execution */
    bool start(std::string &err);
    bool resume_exec(std::string &err);
    bool step_line(std::string &err);
    void kill();
    State state() const { return state_; }
    int  exit_code() const { return exit_code_; }
    const StopReport &last_stop() const { return stop_; }

    /* -------------------------------------------------------- inspection */
    const std::vector<WorkItemSnapshot> &snapshots() const { return snaps_; }
    int  selected() const { return selected_; }
    bool select_global(const size_t g[3], std::string &err);
    bool select_index(size_t i, std::string &err);
    const WorkItemSnapshot *selected_snapshot() const;

    /* The identity model governing a given breakpoint: its own (fast-path, per-line)
     * model if that completed, the kernel-wide fallback if this line turned out to be
     * divergent and needed it, or null if neither has resolved yet. See "Identity
     * calibration" in session.cpp. */
    const IdentityModel *identity_model_for(int bp_id) const;

    /* Evaluate a C expression against the selected work-item. */
    bool eval(const std::string &src, Value &out, std::string &err);
    /* Variables in scope at the selected work-item, in declaration order. */
    std::vector<VarInfo> locals_at_selection() const;
    EvalContext eval_context_for(const WorkItemSnapshot &s) const;

    bool read_memory(uint64_t addr, void *buf, size_t len) const;
    /* Host address of a __global buffer argument, by kernel argument name. */
    bool buffer_address(const std::string &arg_name, uint64_t &out, std::string &err) const;

    const pocl::WorkgroupCall &workgroup_call() const { return wg_call_; }
    const Inferior &inferior() const { return inf_; }

private:
    /* --------------------------------------------------- internal plumbing */
    struct Site {
        uint64_t rt_addr = 0;
        uint64_t link_addr = 0;
        uint8_t  orig = 0;
        bool     inserted = false;
        int      bp_id = 0;       /* >0 user breakpoint, <0 internal */
        int      replica = -1;
        unsigned line = 0;
    };

    static constexpr int kBpRDebug  = -1;
    static constexpr int kBpWgEntry = -2;
    static constexpr int kBpWgExit  = -3;
    static constexpr int kBpEntry   = -4;
    static constexpr int kBpCalib   = -5;

    enum class Action { Continue, Report, Done };

    bool  insert_site(Site &s);
    bool  remove_site(Site &s);
    Site *site_at(uint64_t rt_addr);
    bool  add_site(uint64_t rt_addr, uint64_t link_addr, int bp_id, int replica,
                   unsigned line);
    void  drop_sites_for(int bp_id);
    bool  step_over_site(Site &s, std::string &err);

    bool launch(std::string &err);
    bool run_to_entry(std::string &err);
    bool find_r_debug(std::string &err);
    bool on_rdebug_stop(std::string &err);
    bool install_kernel_breakpoints(std::string &err);
    bool resolve_breakpoint(UserBp &bp, std::string &err);

    Action on_wg_entry();
    Action on_wg_exit();
    Action on_line_hit(const Site &s);
    Action on_calib_hit(const Site &s);
    Action resolve_hit(UserBp *bp, const IdentityModel &model, int replica,
                       const RegisterFile &regs, uint64_t rt_addr, uint64_t link_addr,
                       unsigned line);
    Action replay_pending();
    Action drain_own_pending(UserBp &bp);
    Action finalize_group_calibrations();
    void   finish_group_scan(bool live);
    unsigned pick_calibration_line() const;
    void   install_user_sites();

    bool pump(std::string &err);       /* resume until a report or exit */

    RegisterFile capture_regs() const;
    WorkItemSnapshot build_snapshot(const Site &s, const RegisterFile &regs,
                                    const WorkItemId &id, size_t visit) const;
    uint64_t frame_base_for(uint64_t link_pc, const RegisterFile &regs) const;

    std::string effective_spec_path(std::string &err);

    /* --------------------------------------------------------------- state */
    Options   opts_;
    rs_spec   spec_{};
    bool      spec_loaded_ = false;
    NDRange   nd_;
    std::string spec_path_;
    std::string effective_spec_;
    std::string kernel_source_;
    std::vector<std::string> source_lines_;

    std::string cache_dir_;
    std::string runner_path_;
    std::string object_path_;
    uint64_t    load_bias_ = 0;
    DwarfIndex  dwarf_;
    const FuncInfo *exec_func_ = nullptr;  /* the function actually being executed */

    Inferior  inf_;
    State     state_ = State::NotStarted;
    int       exit_code_ = 0;

    std::vector<UserBp> bps_;
    int       next_bp_id_ = 1;
    std::vector<Site> sites_;

    uint64_t  r_debug_addr_ = 0;
    uint64_t  r_brk_ = 0;
    bool      image_loaded_ = false;

    /* per work-group */
    pocl::WorkgroupCall wg_call_;
    size_t    cur_group_[3] = {0, 0, 0};
    bool      in_group_ = false;
    uint64_t  wg_return_addr_ = 0;

    /* the group scan currently in progress */
    bool      scanning_ = false;
    unsigned  scan_line_ = 0;
    int       scan_bp_ = -1;
    std::vector<WorkItemSnapshot> snaps_;
    std::map<size_t, size_t> visits_;        /* flat local id -> times seen */

    /* Identity calibration: a kernel-wide property, established once against a
     * guaranteed-safe (unconditionally executed) line rather than the user's own
     * breakpoint line -- see session.cpp. */
    bool      calib_installed_ = false;
    unsigned  calib_replicas_ = 0;
    std::vector<CalibrationSample> calib_;
    IdentityModel kernel_model_;
    std::vector<PendingHit> pending_;

    int       selected_ = -1;
    StopReport stop_;
    bool      reported_ = false;
};

} // namespace oclgdb
