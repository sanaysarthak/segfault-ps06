#include "session.h"
#include "util.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <elf.h>
#include <fstream>
#include <link.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <llvm/BinaryFormat/Dwarf.h>

using namespace llvm;
using namespace llvm::dwarf;

namespace oclgdb {

static const uint8_t kInt3 = 0xCC;

Session::Session() = default;
Session::~Session() { kill(); }

std::string Session::UserBp::describe() const
{
    std::string s = format("line %u", line);
    if (filtered)
        s += format(" if work-item %s(%zu,%zu,%zu)",
                    wi_is_local ? "local " : "", wi[0], wi[1], wi[2]);
    return s;
}

/* ============================================================== setup */

bool Session::load_spec(const std::string &path, std::string &err)
{
    char e[512];
    if (rs_parse(path.c_str(), &spec_, e, sizeof e) != 0) { err = e; return false; }

    spec_loaded_ = true;
    spec_path_ = path;

    nd_.dim = spec_.work_dim;
    for (unsigned i = 0; i < 3; i++) {
        nd_.global[i] = spec_.global[i] ? spec_.global[i] : 1;
        nd_.local[i]  = spec_.local[i]  ? spec_.local[i]  : 1;
        if (nd_.global[i] % nd_.local[i]) {
            err = format("global size %zu is not a multiple of local size %zu in dimension %u",
                         nd_.global[i], nd_.local[i], i);
            return false;
        }
    }

    std::string src;
    if (!read_file(spec_.kernel_path, src)) {
        err = format("cannot read kernel source '%s'", spec_.kernel_path);
        return false;
    }
    kernel_source_ = spec_.kernel_path;
    source_lines_ = split(src, '\n');

    cache_dir_ = format("/tmp/oclgdb-cache-%d", (int)getpid());
    mkdir(cache_dir_.c_str(), 0755);

    /* The harness lives next to us. */
    char self[1024] = {0};
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    runner_path_ = n > 0 ? dirname_of(self) + "/ocl-runner" : std::string("./ocl-runner");

    dwarf_ = DwarfIndex();
    object_path_.clear();
    kernel_model_ = IdentityModel();
    calib_installed_ = false;
    return true;
}

/* pocl compiles kernels at full optimisation unless told otherwise, which erases most
 * of the debug info we need (docs/PHASE0-FINDINGS.md F3). Rather than fail, we write an
 * adjusted spec and tell the user what we changed. */
std::string Session::effective_spec_path(std::string &err)
{
    std::string opts = spec_.build_options;
    bool added = false;
    if (opts.find("-g") == std::string::npos) { opts += " -g"; added = true; }
    if (opts.find("-cl-opt-disable") == std::string::npos) {
        opts += " -cl-opt-disable";
        added = true;
    }
    opts = trim(opts);

    std::string raw;
    if (!read_file(spec_path_, raw)) { err = "cannot re-read the spec"; return ""; }

    std::ostringstream out;
    bool wrote_build = false;
    for (const auto &line : split(raw, '\n')) {
        std::string t = trim(line);
        if (starts_with(t, "build")) {
            out << "build " << opts << "\n";
            wrote_build = true;
        } else {
            out << line << "\n";
        }
    }
    if (!wrote_build) out << "build " << opts << "\n";

    std::string p = cache_dir_ + "/effective.ocl-run";
    std::ofstream f(p);
    if (!f) { err = format("cannot write '%s'", p.c_str()); return ""; }
    f << out.str();
    f.close();

    if (added && opts != trim(spec_.build_options))
        printf("%soclgdb:%s kernel build options set to '%s' "
               "(pocl needs -cl-opt-disable to keep source-level debug info)\n",
               color::yellow(), color::reset(), opts.c_str());
    effective_spec_ = p;
    return p;
}

bool Session::ensure_kernel_image(std::string &err)
{
    if (dwarf_.loaded()) return true;
    if (!spec_loaded_) { err = "no spec loaded"; return false; }

    std::string eff = effective_spec_path(err);
    if (eff.empty()) return false;

    /* A plain, untraced run: this is the "compile" step of the demo flow. pocl builds
     * the kernel and leaves the object in its cache, which is what we then index. */
    int pipefd[2];
    if (pipe(pipefd) != 0) { err = format("pipe: %s", strerror(errno)); return false; }

    pid_t p = fork();
    if (p < 0) { err = format("fork: %s", strerror(errno)); return false; }
    if (p == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        setenv("POCL_CACHE_DIR", cache_dir_.c_str(), 1);
        setenv("POCL_DEVICES", "basic", 1);
        execl(runner_path_.c_str(), runner_path_.c_str(), eff.c_str(), (char *)nullptr);
        _exit(127);
    }
    close(pipefd[1]);

    std::string output;
    char buf[4096];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof buf)) > 0) output.append(buf, (size_t)n);
    close(pipefd[0]);
    int status = 0;
    waitpid(p, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        err = format("cannot execute the harness '%s'", runner_path_.c_str());
        return false;
    }
    if (output.find("build failed") != std::string::npos ||
        output.find("BUILD FAILED") != std::string::npos) {
        err = "pocl failed to build the kernel:\n" + output;
        return false;
    }
    if (opts_.verbose && !output.empty()) fputs(output.c_str(), stdout);

    auto found = pocl::scan_cache(cache_dir_, spec_.kernel_name);
    if (found.empty()) {
        err = format("pocl produced no object for kernel '%s'.\nHarness output:\n%s",
                     spec_.kernel_name, output.c_str());
        return false;
    }
    /* Prefer the object compiled for exactly our work-group size. */
    auto best = found.begin();
    for (auto it = found.begin(); it != found.end(); ++it)
        if (it->local[0] == nd_.local[0] && it->local[1] == nd_.local[1] &&
            it->local[2] == nd_.local[2]) { best = it; break; }

    object_path_ = best->path;
    if (!dwarf_.load(object_path_, err)) return false;

    /* Pick the function that plausibly holds the kernel body: the one with the most
     * line-table rows. The choice is re-confirmed at the first trap. */
    exec_func_ = nullptr;
    size_t best_rows = 0;
    for (const auto &f : dwarf_.functions()) {
        size_t rows = 0;
        for (const auto &r : dwarf_.lines())
            if (!r.end_sequence && r.addr >= f.low && r.addr < f.high) rows++;
        if (rows > best_rows) { best_rows = rows; exec_func_ = &f; }
    }
    return true;
}

/* ======================================================== breakpoints */

int Session::add_breakpoint(unsigned line, bool filtered, const size_t wi[3],
                            bool wi_is_local, std::string &err)
{
    if (!dwarf_.loaded()) { err = "no kernel image loaded yet (try 'compile')"; return -1; }

    unsigned actual = line;
    if (dwarf_.addresses_for_line(line).empty()) {
        unsigned next = dwarf_.next_line_with_code(line);
        if (!next) {
            err = format("line %u is past the end of the kernel's executable code", line);
            return -1;
        }
        actual = next;
    }

    UserBp bp;
    bp.id = next_bp_id_++;
    bp.line = actual;
    bp.filtered = filtered;
    bp.wi_is_local = wi_is_local;
    if (filtered) {
        for (unsigned i = 0; i < 3; i++) bp.wi[i] = wi[i];
        if (!wi_is_local) {
            if (!nd_.contains_global(bp.wi)) {
                err = format("work-item (%zu,%zu,%zu) is outside the NDRange "
                             "global=(%zu,%zu,%zu)",
                             bp.wi[0], bp.wi[1], bp.wi[2],
                             nd_.global[0], nd_.global[1], nd_.global[2]);
                next_bp_id_--;
                return -1;
            }
            for (unsigned i = 0; i < 3; i++) bp.filter_group[i] = bp.wi[i] / nd_.local[i];
        }
    }

    if (!resolve_breakpoint(bp, err)) { next_bp_id_--; return -1; }
    bps_.push_back(bp);

    if (state_ == State::Stopped && image_loaded_) {
        std::string e2;
        install_kernel_breakpoints(e2);
    }
    if (actual != line)
        printf("%soclgdb:%s line %u has no code; breakpoint moved to line %u\n",
               color::yellow(), color::reset(), line, actual);
    return bp.id;
}

bool Session::resolve_breakpoint(UserBp &bp, std::string &err)
{
    bp.link_addrs.clear();
    for (uint64_t a : dwarf_.addresses_for_line(bp.line)) {
        if (exec_func_ && (a < exec_func_->low || a >= exec_func_->high)) continue;
        bp.link_addrs.push_back(a);
    }
    if (bp.link_addrs.empty()) {
        /* Fall back to every function -- we will narrow down at the first trap. */
        bp.link_addrs = dwarf_.addresses_for_line(bp.line);
    }
    if (bp.link_addrs.empty()) {
        err = format("no machine code is attributed to line %u", bp.line);
        return false;
    }
    return true;
}

bool Session::remove_breakpoint(int id)
{
    auto it = std::find_if(bps_.begin(), bps_.end(),
                           [&](const UserBp &b) { return b.id == id; });
    if (it == bps_.end()) return false;
    drop_sites_for(id);
    bps_.erase(it);
    return true;
}

void Session::clear_breakpoints()
{
    for (const auto &b : bps_) drop_sites_for(b.id);
    bps_.clear();
}

/* ============================================================== sites */

bool Session::insert_site(Site &s)
{
    if (s.inserted || !inf_.alive()) return s.inserted;
    uint8_t orig = 0;
    if (!inf_.read_mem(s.rt_addr, &orig, 1)) return false;
    s.orig = orig;
    if (!inf_.write_mem(s.rt_addr, &kInt3, 1)) return false;
    s.inserted = true;
    return true;
}

bool Session::remove_site(Site &s)
{
    if (!s.inserted || !inf_.alive()) { s.inserted = false; return true; }
    if (!inf_.write_mem(s.rt_addr, &s.orig, 1)) return false;
    s.inserted = false;
    return true;
}

Session::Site *Session::site_at(uint64_t addr)
{
    for (auto &s : sites_) if (s.rt_addr == addr) return &s;
    return nullptr;
}

bool Session::add_site(uint64_t rt, uint64_t link, int bp_id, int replica, unsigned line)
{
    if (site_at(rt)) return true;
    Site s;
    s.rt_addr = rt;
    s.link_addr = link;
    s.bp_id = bp_id;
    s.replica = replica;
    s.line = line;
    if (!insert_site(s)) return false;
    sites_.push_back(s);
    return true;
}

void Session::drop_sites_for(int bp_id)
{
    for (auto &s : sites_) if (s.bp_id == bp_id) remove_site(s);
    sites_.erase(std::remove_if(sites_.begin(), sites_.end(),
                                [&](const Site &s) { return s.bp_id == bp_id; }),
                 sites_.end());
}

bool Session::step_over_site(Site &s, std::string &err)
{
    if (!s.inserted) return true;
    if (!remove_site(s)) { err = "cannot restore the original instruction byte"; return false; }
    Inferior::Stop st = inf_.single_step();
    if (st.kind == Inferior::StopKind::Exited) {
        state_ = State::Exited;
        exit_code_ = st.exit_code;
        return true;
    }
    if (st.kind == Inferior::StopKind::Error) { err = st.message; return false; }
    insert_site(s);
    return true;
}

/* =========================================================== launching */

bool Session::launch(std::string &err)
{
    std::vector<std::string> args{effective_spec_};
    std::vector<std::string> env{
        "POCL_CACHE_DIR=" + cache_dir_,
        /* Single-threaded, deterministic work-group ordering. See DECISIONS.md D7. */
        "POCL_DEVICES=basic",
    };
    if (!inf_.start(runner_path_, args, env, err)) return false;
    sites_.clear();
    image_loaded_ = false;
    in_group_ = false;
    scanning_ = false;
    snaps_.clear();
    visits_.clear();
    calib_.clear();
    calib_installed_ = false;
    calib_replicas_ = 0;
    kernel_model_ = IdentityModel();
    pending_.clear();
    for (auto &bp : bps_) {
        bp.own_calib_done = false;
        bp.own_calib_failed = false;
        bp.own_model = IdentityModel();
        bp.own_calib.clear();
        bp.own_pending.clear();
    }
    selected_ = -1;
    stop_ = StopReport();
    state_ = State::Stopped;
    return true;
}

bool Session::run_to_entry(std::string &err)
{
    uint64_t entry = 0;
    if (!inf_.auxv(AT_ENTRY, entry)) { err = "cannot read AT_ENTRY from auxv"; return false; }

    uint8_t orig = 0;
    if (!inf_.read_mem(entry, &orig, 1)) { err = "cannot read the entry point"; return false; }
    if (!inf_.write_mem(entry, &kInt3, 1)) { err = "cannot arm the entry breakpoint"; return false; }

    Inferior::Stop st = inf_.resume();
    if (st.kind == Inferior::StopKind::Exited) {
        state_ = State::Exited;
        exit_code_ = st.exit_code;
        err = "the harness exited before reaching its entry point";
        return false;
    }
    if (st.kind != Inferior::StopKind::Trap) { err = st.message; return false; }

    inf_.write_mem(entry, &orig, 1);
    user_regs_struct r{};
    if (!inf_.get_regs(r)) { err = "cannot read registers"; return false; }
    r.rip = entry;
    inf_.set_regs(r);
    return true;
}

/* The dynamic linker publishes a `struct r_debug` through DT_DEBUG and traps into
 * `r_brk` after every map/unmap. That is how we learn where pocl dlopen'd the kernel
 * object, and -- through link_map::l_addr -- its exact load bias. */
bool Session::find_r_debug(std::string &err)
{
    uint64_t phdr_addr = 0, phent = 0, phnum = 0;
    if (!inf_.auxv(AT_PHDR, phdr_addr) || !inf_.auxv(AT_PHENT, phent) ||
        !inf_.auxv(AT_PHNUM, phnum)) {
        err = "cannot read the program headers from auxv";
        return false;
    }

    std::vector<Elf64_Phdr> ph(phnum);
    if (!inf_.read_mem(phdr_addr, ph.data(), phnum * sizeof(Elf64_Phdr))) {
        err = "cannot read the program headers";
        return false;
    }

    uint64_t bias = 0, dyn_vaddr = 0;
    bool have_dyn = false;
    for (const auto &p : ph) {
        if (p.p_type == PT_PHDR) bias = phdr_addr - p.p_vaddr;
        if (p.p_type == PT_DYNAMIC) { dyn_vaddr = p.p_vaddr; have_dyn = true; }
    }
    if (!have_dyn) { err = "the harness has no PT_DYNAMIC segment"; return false; }

    uint64_t dyn = dyn_vaddr + bias;
    for (unsigned i = 0; i < 4096; i++) {
        Elf64_Dyn d{};
        if (!inf_.read_mem(dyn + i * sizeof d, &d, sizeof d)) break;
        if (d.d_tag == DT_NULL) break;
        if (d.d_tag == DT_DEBUG && d.d_un.d_ptr) { r_debug_addr_ = d.d_un.d_ptr; break; }
    }
    if (!r_debug_addr_) { err = "the dynamic linker has not published DT_DEBUG yet"; return false; }

    struct { uint64_t version_pad; uint64_t r_map; uint64_t r_brk; uint64_t r_state;
             uint64_t r_ldbase; } rd{};
    if (!inf_.read_mem(r_debug_addr_, &rd, sizeof rd)) { err = "cannot read r_debug"; return false; }
    r_brk_ = rd.r_brk;
    if (!r_brk_) { err = "r_debug.r_brk is null"; return false; }

    if (!add_site(r_brk_, 0, kBpRDebug, -1, 0)) {
        err = "cannot arm the dynamic-linker rendezvous breakpoint";
        return false;
    }
    return true;
}

bool Session::on_rdebug_stop(std::string &err)
{
    if (image_loaded_) return true;

    struct { uint64_t version_pad; uint64_t r_map; uint64_t r_brk; uint64_t r_state;
             uint64_t r_ldbase; } rd{};
    if (!inf_.read_mem(r_debug_addr_, &rd, sizeof rd)) return true;
    if ((rd.r_state & 0xffffffff) != 0 /* RT_CONSISTENT */) return true;

    std::string want = basename_of(object_path_);
    for (uint64_t lm = rd.r_map; lm; ) {
        struct { uint64_t l_addr; uint64_t l_name; uint64_t l_ld;
                 uint64_t l_next; uint64_t l_prev; } e{};
        if (!inf_.read_mem(lm, &e, sizeof e)) break;
        std::string name;
        if (e.l_name) inf_.read_cstr(e.l_name, name);
        if (!name.empty() && basename_of(name) == want && name.find(cache_dir_) == 0) {
            load_bias_ = e.l_addr;
            image_loaded_ = true;
            if (opts_.verbose)
                printf("%soclgdb:%s kernel object mapped at 0x%llx (%s)\n",
                       color::dim(), color::reset(), (unsigned long long)load_bias_,
                       name.c_str());
            return install_kernel_breakpoints(err);
        }
        lm = e.l_next;
    }
    return true;
}

/* Identity calibration
 * ---------------------
 * Phase 0 (docs/PHASE0-FINDINGS.md, F4/F5) found that pocl keeps kernel locals in
 * registers with PC-scoped live ranges, and runs a work-group's work-items either
 * fully replicated (one machine address per work-item, per line) or through a real
 * per-line machine loop (one address, hit once per work-item).
 *
 * The natural design is to calibrate identity from whatever line the user breaks on.
 * It is also wrong: if that line is inside a divergent branch -- reached by only some
 * work-items, which is exactly the kind of line a debugger gets used on -- it never
 * accumulates a whole work-group's worth of samples, so calibration (and therefore any
 * work-item filter) never completes.
 *
 * So identity is calibrated once, kernel-wide, against a line every work-item is
 * guaranteed to execute: the DWARF prologue-end row, i.e. the kernel body's very first
 * statement. A hidden breakpoint sits there until enough samples have been seen to
 * derive a model (see calibrate_identity in workitem.cpp), after which it is removed
 * and the user's real breakpoints are installed. Any user-breakpoint hits that arrive
 * before that point are queued (`pending_`) and resolved retroactively -- their
 * register state was captured at the correct moment even though their identity is
 * worked out later. */
unsigned Session::pick_calibration_line() const
{
    if (!exec_func_) return 0;
    for (const auto &r : dwarf_.lines()) {
        if (r.addr < exec_func_->low || r.addr >= exec_func_->high) continue;
        if (r.end_sequence || r.line == 0) continue;
        if (r.prologue_end) return r.line;
    }
    /* Some builds omit prologue_end; fall back to the first line change after the
     * function's opening (signature) row. */
    unsigned first = 0;
    for (const auto &r : dwarf_.lines()) {
        if (r.addr < exec_func_->low || r.addr >= exec_func_->high) continue;
        if (r.end_sequence || r.line == 0 || !r.is_stmt) continue;
        if (!first) { first = r.line; continue; }
        if (r.line != first) return r.line;
    }
    return first;
}

void Session::install_user_sites()
{
    for (auto &bp : bps_) {
        if (!bp.enabled) continue;
        for (size_t i = 0; i < bp.link_addrs.size(); i++)
            add_site(bp.link_addrs[i] + load_bias_, bp.link_addrs[i], bp.id, (int)i, bp.line);
    }
}

bool Session::install_kernel_breakpoints(std::string &err)
{
    if (!image_loaded_) return true;

    /* The work-group entry tells us the group id and the argument block. */
    const FuncInfo *wg = dwarf_.func_by_name(pocl::workgroup_symbol(spec_.kernel_name));
    if (wg && !site_at(wg->low + load_bias_))
        add_site(wg->low + load_bias_, wg->low, kBpWgEntry, -1, 0);

    if (!calib_installed_ && exec_func_) {
        calib_installed_ = true;
        unsigned line = pick_calibration_line();
        auto addrs = line ? dwarf_.addresses_for_line(line, *exec_func_)
                          : std::vector<uint64_t>();
        if (!addrs.empty()) {
            calib_replicas_ = (unsigned)addrs.size();
            for (size_t i = 0; i < addrs.size(); i++)
                add_site(addrs[i] + load_bias_, addrs[i], kBpCalib, (int)i, line);
        } else {
            /* No safe line could be found at all -- fall back to arrival order so the
             * debugger stays usable, with a visible warning (see IdentityModel::describe). */
            kernel_model_.kind = IdentityModel::Kind::Ordinal;
            kernel_model_.note = "no calibration line was found in this kernel's DWARF; "
                                 "identity falls back to arrival order";
            install_user_sites();
        }
    }

    /* User breakpoints are armed immediately, whether or not identity is calibrated
     * yet -- NOT gated on it. In replication mode especially, a work-item's whole body
     * (a handful of instructions) can finish executing before the *last* work-item's
     * copy has even produced the final calibration sample (calibration necessarily
     * needs to see every replica, and replicas run strictly in sequence); gating site
     * installation on calibration would let earlier work-items' hits execute completely
     * unobserved. Arming the sites up front means every hit is trapped and (if identity
     * is not yet known) queued in `pending_` by on_line_hit, to be resolved the moment
     * calibration completes -- see "Identity calibration" above. */
    install_user_sites();
    return true;
}

/* ============================================================== running */

RegisterFile Session::capture_regs() const
{
    RegisterFile rf;
    inf_.get_regs(rf.gp);
    rf.have_fp = inf_.get_fpregs(rf.fp);
    return rf;
}

uint64_t Session::frame_base_for(uint64_t link_pc, const RegisterFile &regs) const
{
    const FuncInfo *f = dwarf_.func_for_addr(link_pc);
    if (!f) return 0;
    auto fb = f->die.find(DW_AT_frame_base);
    if (!fb) return 0;
    auto block = fb->getAsBlock();
    if (!block) return 0;
    std::vector<uint8_t> expr(block->begin(), block->end());

    EvalContext ctx;
    ctx.regs = &regs;
    ctx.load_bias = load_bias_;
    Location l = eval_dwarf_expr(expr, ctx);
    if (l.kind == Location::Kind::Register) {
        uint64_t v = 0;
        if (regs.read(l.reg, v)) return v;
    } else if (l.kind == Location::Kind::Memory) {
        return l.address;
    } else if (l.kind == Location::Kind::Value) {
        return l.value;
    }
    return 0;
}

EvalContext Session::eval_context_for(const WorkItemSnapshot &s) const
{
    EvalContext ctx;
    ctx.regs = &s.regs;
    ctx.load_bias = load_bias_;
    ctx.read_mem = [this](uint64_t a, void *b, size_t n) { return inf_.read_mem(a, b, n); };
    ctx.frame_base = frame_base_for(s.link_pc, s.regs);
    ctx.have_frame_base = ctx.frame_base != 0;
    return ctx;
}

WorkItemSnapshot Session::build_snapshot(const Site &s, const RegisterFile &regs,
                                         const WorkItemId &id, size_t visit) const
{
    WorkItemSnapshot snap;
    snap.id = id;
    snap.pc = s.rt_addr;
    snap.link_pc = s.link_addr;
    snap.line = s.line;
    snap.regs = regs;
    snap.replica = s.replica;
    snap.visit = visit;

    EvalContext ctx = eval_context_for(snap);

    /* This is the moment that makes cross-work-item inspection possible at all: the
     * work-item's values exist only while it is the one executing, so we resolve every
     * in-scope variable now and keep the result. */
    for (const auto &v : dwarf_.variables_at(s.link_addr)) {
        std::vector<uint8_t> expr;
        Location loc;
        if (dwarf_.location_at(v.die, s.link_addr, expr)) loc = eval_dwarf_expr(expr, ctx);
        else loc.error = "optimized out at this line";
        snap.vars[v.name] = read_value(v.type, loc, ctx);
    }
    return snap;
}

Session::Action Session::on_wg_entry()
{
    user_regs_struct r{};
    if (!inf_.get_regs(r)) return Action::Continue;
    wg_call_ = pocl::read_workgroup_call(r);

    /* Settle any breakpoint whose own-line calibration was still in flight when the
     * previous work-group ended: fewer than a full group's worth of hits means that
     * line is divergent, so it falls back to the kernel-wide model from here on. Must
     * happen before cur_group_/snaps_ below are overwritten for the new group, since
     * any hits resolved here still belong to the group that just finished. */
    Action fin = finalize_group_calibrations();
    if (fin == Action::Report) return Action::Report;

    if (scanning_) {           /* previous group ended without passing its return site */
        finish_group_scan(false);
        return Action::Report;
    }

    for (unsigned i = 0; i < 3; i++) cur_group_[i] = wg_call_.group[i];
    in_group_ = true;
    snaps_.clear();
    visits_.clear();
    selected_ = -1;

    /* Arm the work-group's own return address so we can tell when the group is over,
     * which is what bounds a scan when the breakpoint line is divergent. */
    uint64_t ret = 0;
    if (inf_.read_mem(r.rsp, &ret, 8) && ret) {
        wg_return_addr_ = ret;
        add_site(ret, 0, kBpWgExit, -1, 0);
    }
    return Action::Continue;
}

Session::Action Session::on_wg_exit()
{
    Action fin = finalize_group_calibrations();
    if (fin == Action::Report) return Action::Report;

    if (scanning_) { finish_group_scan(false); return Action::Report; }
    in_group_ = false;
    return Action::Continue;
}

Session::Action Session::finalize_group_calibrations()
{
    for (auto &bp : bps_) {
        if (bp.own_calib_done || bp.own_calib_failed) continue;
        if (bp.own_calib.empty()) continue;   /* never hit this group; try again next time */

        /* Partial coverage is the signature of a divergent line: some, but not all, of
         * the work-group reached it. Its own-line calibration can never complete, so it
         * falls back to the kernel-wide model (calibrated separately, from a line every
         * work-item is guaranteed to execute) for every hit from here on. */
        bp.own_calib_failed = true;
        bp.own_calib.clear();

        std::vector<PendingHit> items;
        items.swap(bp.own_pending);
        if (kernel_model_.calibrated()) {
            for (auto &p : items) {
                Action a = resolve_hit(&bp, kernel_model_, p.replica, p.regs, p.rt_addr,
                                       p.link_addr, p.line);
                if (a == Action::Report) return Action::Report;
            }
        } else {
            for (auto &p : items) pending_.push_back(p);
        }
    }
    return Action::Continue;
}

Session::Action Session::on_line_hit(const Site &site)
{
    UserBp *bp = nullptr;
    for (auto &b : bps_) if (b.id == site.bp_id) bp = &b;
    if (!bp) return Action::Continue;

    /* The first trap settles which of the object's functions is actually executing;
     * sites in the other one are dead weight and would corrupt the replica count. */
    if (!exec_func_ || site.link_addr < exec_func_->low || site.link_addr >= exec_func_->high) {
        const FuncInfo *f = dwarf_.func_for_addr(site.link_addr);
        if (f && f != exec_func_) {
            exec_func_ = f;
            for (auto &b : bps_) {
                std::string e;
                resolve_breakpoint(b, e);
            }
            std::vector<uint64_t> keep;
            for (auto &s : sites_) {
                if (s.bp_id <= 0) continue;
                if (s.link_addr < exec_func_->low || s.link_addr >= exec_func_->high)
                    remove_site(s);
            }
            sites_.erase(std::remove_if(sites_.begin(), sites_.end(),
                                        [&](const Site &s) {
                                            return s.bp_id > 0 && !s.inserted;
                                        }),
                         sites_.end());
        }
    }

    bp->hits++;
    RegisterFile regs = capture_regs();

    /* Fast path: this breakpoint's own line, calibrated from its own hits. Correct for
     * any *non-divergent* line -- one every work-item in the group actually reaches --
     * in whichever loop/region it happens to sit in, which matters because pocl gives
     * the code either side of a barrier separate loops with independently-allocated
     * identity registers (see "Identity calibration" below). */
    if (bp->own_calib_done) {
        return resolve_hit(bp, bp->own_model, site.replica, regs, site.rt_addr,
                           site.link_addr, site.line);
    }

    if (!bp->own_calib_failed) {
        CalibrationSample cs;
        cs.replica = site.replica;
        cs.regs = regs;
        bp->own_calib.push_back(cs);

        PendingHit ph;
        ph.bp_id = bp->id;
        ph.replica = site.replica;
        ph.line = site.line;
        ph.rt_addr = site.rt_addr;
        ph.link_addr = site.link_addr;
        ph.regs = regs;
        bp->own_pending.push_back(ph);

        size_t n = nd_.local_size();
        if (bp->own_calib.size() < n) return Action::Continue;

        bp->own_model = calibrate_identity(bp->own_calib, n, (unsigned)bp->link_addrs.size());
        bp->own_calib_done = true;
        bp->own_calib.clear();
        return drain_own_pending(*bp);
    }

    /* This line is divergent (a work-group ended before it saw every work-item -- see
     * on_wg_entry/on_wg_exit) and has already fallen back to the kernel-wide model. */
    if (!kernel_model_.calibrated()) {
        PendingHit ph;
        ph.bp_id = bp->id;
        ph.replica = site.replica;
        ph.line = site.line;
        ph.rt_addr = site.rt_addr;
        ph.link_addr = site.link_addr;
        ph.regs = regs;
        pending_.push_back(ph);
        return Action::Continue;
    }
    return resolve_hit(bp, kernel_model_, site.replica, regs, site.rt_addr, site.link_addr,
                       site.line);
}

Session::Action Session::drain_own_pending(UserBp &bp)
{
    std::vector<PendingHit> items;
    items.swap(bp.own_pending);
    for (const auto &p : items) {
        Action a = resolve_hit(&bp, bp.own_model, p.replica, p.regs, p.rt_addr, p.link_addr,
                               p.line);
        if (a == Action::Report) return Action::Report;
    }
    return Action::Continue;
}

Session::Action Session::on_calib_hit(const Site &site)
{
    RegisterFile regs = capture_regs();
    CalibrationSample cs;
    cs.replica = site.replica;
    cs.regs = regs;
    calib_.push_back(cs);

    size_t n = nd_.local_size();
    if (calib_.size() < n) return Action::Continue;

    kernel_model_ = calibrate_identity(calib_, n, calib_replicas_);
    calib_.clear();

    /* One-time cost: the calibration sites have done their job. Remove them (any
     * breakpoint that still needs this model has already fallen back to it). */
    drop_sites_for(kBpCalib);

    return replay_pending();
}

Session::Action Session::replay_pending()
{
    std::vector<PendingHit> items;
    items.swap(pending_);
    for (const auto &p : items) {
        UserBp *bp = nullptr;
        for (auto &b : bps_) if (b.id == p.bp_id) bp = &b;
        if (!bp) continue;
        Action a = resolve_hit(bp, kernel_model_, p.replica, p.regs, p.rt_addr, p.link_addr,
                               p.line);
        if (a == Action::Report) return Action::Report;
    }
    return Action::Continue;
}

Session::Action Session::resolve_hit(UserBp *bp, const IdentityModel &model, int replica,
                                     const RegisterFile &regs, uint64_t rt_addr,
                                     uint64_t link_addr, unsigned line)
{
    size_t n = nd_.local_size();
    size_t flat = 0;
    size_t ordinal = visits_.size();
    if (!identity_of(model, replica, regs, ordinal, n, flat)) return Action::Continue;

    size_t visit = visits_[flat]++;
    WorkItemId id = make_work_item(nd_, cur_group_, flat);

    Site pseudo;
    pseudo.rt_addr = rt_addr;
    pseudo.link_addr = link_addr;
    pseudo.replica = replica;
    pseudo.line = line;

    if (bp->filtered) {
        bool match;
        if (bp->wi_is_local)
            match = id.local[0] == bp->wi[0] && id.local[1] == bp->wi[1] &&
                    id.local[2] == bp->wi[2];
        else
            match = id.global[0] == bp->wi[0] && id.global[1] == bp->wi[1] &&
                    id.global[2] == bp->wi[2];
        if (!match) return Action::Continue;

        snaps_.assign(1, build_snapshot(pseudo, regs, id, visit));
        snaps_.back().live = true;
        selected_ = 0;
        scanning_ = false;
        stop_ = StopReport();
        stop_.valid = true;
        stop_.bp_id = bp->id;
        stop_.line = bp->line;
        for (unsigned i = 0; i < 3; i++) stop_.group[i] = cur_group_[i];
        stop_.reached = 1;
        stop_.group_size = n;
        stop_.live = true;
        return Action::Report;
    }

    snaps_.push_back(build_snapshot(pseudo, regs, id, visit));

    if (!opts_.scan_work_group) {
        snaps_.back().live = true;
        selected_ = (int)snaps_.size() - 1;
        scanning_ = false;
        stop_ = StopReport();
        stop_.valid = true;
        stop_.bp_id = bp->id;
        stop_.line = bp->line;
        for (unsigned i = 0; i < 3; i++) stop_.group[i] = cur_group_[i];
        stop_.reached = visits_.size();
        stop_.group_size = n;
        stop_.live = true;
        return Action::Report;
    }

    scanning_ = true;
    scan_line_ = bp->line;
    scan_bp_ = bp->id;
    if (visits_.size() == n) { finish_group_scan(true); return Action::Report; }
    return Action::Continue;
}

void Session::finish_group_scan(bool live)
{
    size_t n = nd_.local_size();

    std::stable_sort(snaps_.begin(), snaps_.end(),
                     [](const WorkItemSnapshot &a, const WorkItemSnapshot &b) {
                         if (a.id.flat_local != b.id.flat_local)
                             return a.id.flat_local < b.id.flat_local;
                         return a.visit < b.visit;
                     });

    for (auto &s : snaps_) s.live = false;
    selected_ = snaps_.empty() ? -1 : 0;
    if (live && !snaps_.empty()) {
        /* The work-item we are physically halted inside is the last one to arrive. */
        size_t best = 0;
        for (size_t i = 0; i < snaps_.size(); i++)
            if (snaps_[i].id.flat_local >= snaps_[best].id.flat_local) best = i;
        snaps_[best].live = true;
        selected_ = (int)best;
    }

    stop_ = StopReport();
    stop_.valid = true;
    stop_.bp_id = scan_bp_;
    stop_.line = scan_line_;
    for (unsigned i = 0; i < 3; i++) stop_.group[i] = cur_group_[i];
    stop_.reached = visits_.size();
    stop_.group_size = n;
    stop_.live = live;
    scanning_ = false;
}

bool Session::pump(std::string &err)
{
    for (;;) {
        user_regs_struct r{};
        if (inf_.alive() && inf_.get_regs(r)) {
            if (Site *s = site_at(r.rip)) {
                if (!step_over_site(*s, err)) return false;
                if (state_ == State::Exited) return true;
            }
        }

        Inferior::Stop st = inf_.resume();
        if (st.kind == Inferior::StopKind::Exited) {
            if (scanning_) finish_group_scan(false);
            state_ = State::Exited;
            exit_code_ = st.exit_code;
            return true;
        }
        if (st.kind == Inferior::StopKind::Signal) {
            state_ = State::Stopped;
            stop_ = StopReport();
            stop_.valid = true;
            stop_.message = format("the harness received signal %d (%s)",
                                   st.signo, strsignal(st.signo));
            return true;
        }
        if (st.kind != Inferior::StopKind::Trap) { err = st.message; return false; }

        uint64_t pc = st.pc - 1;
        Site *sp = site_at(pc);
        if (!sp) continue;      /* a trap we did not plant; let the program deal with it */

        user_regs_struct rr{};
        if (inf_.get_regs(rr)) { rr.rip = pc; inf_.set_regs(rr); }

        Site site = *sp;        /* handlers may reallocate sites_ */
        Action a = Action::Continue;
        switch (site.bp_id) {
        case kBpRDebug:
            if (!on_rdebug_stop(err)) return false;
            break;
        case kBpWgEntry: a = on_wg_entry(); break;
        case kBpWgExit:  a = on_wg_exit();  break;
        case kBpCalib:   a = on_calib_hit(site); break;
        default:         a = on_line_hit(site); break;
        }
        if (a == Action::Report) { state_ = State::Stopped; return true; }
    }
}

bool Session::start(std::string &err)
{
    if (!ensure_kernel_image(err)) return false;
    kill();
    if (!launch(err)) return false;
    if (!run_to_entry(err)) return false;
    if (!find_r_debug(err)) return false;
    return pump(err);
}

bool Session::resume_exec(std::string &err)
{
    if (state_ != State::Stopped || !inf_.alive()) { err = "the program is not running"; return false; }
    stop_ = StopReport();
    return pump(err);
}

bool Session::step_line(std::string &err)
{
    if (state_ != State::Stopped || !inf_.alive()) { err = "the program is not running"; return false; }
    if (!image_loaded_) { err = "no kernel code is executing"; return false; }

    user_regs_struct r{};
    if (!inf_.get_regs(r)) { err = "cannot read registers"; return false; }
    uint64_t link_pc = r.rip - load_bias_;
    const LineRow *start_row = dwarf_.row_for_addr(link_pc);
    if (!start_row) { err = "the program is not stopped inside kernel code"; return false; }

    unsigned start_line = start_row->line;
    size_t start_flat = selected_ >= 0 && selected_ < (int)snaps_.size()
                        ? snaps_[selected_].id.flat_local : 0;

    /* Temporarily disarm every site so single-stepping does not trip over our own
     * INT3 bytes, then walk instructions until the source line changes. */
    for (auto &s : sites_) remove_site(s);

    const unsigned kMaxSteps = 200000;
    bool moved = false;
    for (unsigned i = 0; i < kMaxSteps; i++) {
        Inferior::Stop st = inf_.single_step();
        if (st.kind == Inferior::StopKind::Exited) {
            state_ = State::Exited;
            exit_code_ = st.exit_code;
            return true;
        }
        if (st.kind != Inferior::StopKind::Trap) { err = st.message; return false; }
        if (!inf_.get_regs(r)) { err = "cannot read registers"; return false; }
        uint64_t lp = r.rip - load_bias_;
        if (!exec_func_ || lp < exec_func_->low || lp >= exec_func_->high) continue;
        const LineRow *row = dwarf_.row_for_addr(lp);
        if (!row || row->end_sequence || row->line == 0 || row->line == start_line) continue;
        if (!row->is_stmt) continue;
        moved = true;
        break;
    }
    for (auto &s : sites_) insert_site(s);

    if (!moved) {
        err = "stepping left the kernel without reaching another source line";
        return false;
    }

    /* Rebuild the halted work-item's state at the new line. */
    if (!inf_.get_regs(r)) { err = "cannot read registers"; return false; }
    uint64_t lp = r.rip - load_bias_;
    const LineRow *row = dwarf_.row_for_addr(lp);

    Site pseudo;
    pseudo.rt_addr = r.rip;
    pseudo.link_addr = lp;
    pseudo.line = row ? row->line : 0;
    pseudo.replica = -1;

    RegisterFile regs = capture_regs();
    size_t flat = start_flat;
    if (kernel_model_.kind == IdentityModel::Kind::Register) {
        size_t f = 0;
        if (identity_of(kernel_model_, -1, regs, 0, nd_.local_size(), f)) flat = f;
    } else if (kernel_model_.kind == IdentityModel::Kind::ReplicaAddress) {
        /* Replicated code: the work-item is whichever replica window we landed in. */
        auto addrs = dwarf_.addresses_for_line(pseudo.line);
        for (size_t i = 0; i < addrs.size(); i++)
            if (addrs[i] <= lp) flat = i;
    }

    WorkItemId id = make_work_item(nd_, cur_group_, flat);
    WorkItemSnapshot snap = build_snapshot(pseudo, regs, id, 0);
    snap.live = true;
    snaps_.assign(1, snap);
    selected_ = 0;

    stop_ = StopReport();
    stop_.valid = true;
    stop_.bp_id = -1;
    stop_.line = pseudo.line;
    for (unsigned i = 0; i < 3; i++) stop_.group[i] = cur_group_[i];
    stop_.reached = 1;
    stop_.group_size = nd_.local_size();
    stop_.live = true;
    if (flat != start_flat)
        stop_.message = format("execution moved on to work-item %s", id.str_global().c_str());
    return true;
}

void Session::kill()
{
    inf_.kill();
    sites_.clear();
    if (state_ != State::NotStarted) state_ = State::Exited;
}

/* =========================================================== inspection */

const IdentityModel *Session::identity_model_for(int bp_id) const
{
    for (const auto &bp : bps_) {
        if (bp.id != bp_id) continue;
        if (bp.own_calib_done) return &bp.own_model;
        if (bp.own_calib_failed && kernel_model_.calibrated()) return &kernel_model_;
        return nullptr;
    }
    return kernel_model_.calibrated() ? &kernel_model_ : nullptr;
}

const WorkItemSnapshot *Session::selected_snapshot() const
{
    if (selected_ < 0 || selected_ >= (int)snaps_.size()) return nullptr;
    return &snaps_[(size_t)selected_];
}

bool Session::select_global(const size_t g[3], std::string &err)
{
    for (size_t i = 0; i < snaps_.size(); i++) {
        const auto &id = snaps_[i].id;
        if (id.global[0] == g[0] && id.global[1] == g[1] && id.global[2] == g[2]) {
            selected_ = (int)i;
            return true;
        }
    }
    if (!nd_.contains_global(g)) {
        err = format("work-item (%zu,%zu,%zu) is outside the NDRange global=(%zu,%zu,%zu)",
                     g[0], g[1], g[2], nd_.global[0], nd_.global[1], nd_.global[2]);
        return false;
    }
    err = format("work-item (%zu,%zu,%zu) did not reach this line in work-group (%zu,%zu,%zu)",
                 g[0], g[1], g[2], cur_group_[0], cur_group_[1], cur_group_[2]);
    return false;
}

bool Session::select_index(size_t i, std::string &err)
{
    if (i >= snaps_.size()) { err = "no such work-item in the current stop"; return false; }
    selected_ = (int)i;
    return true;
}

std::vector<VarInfo> Session::locals_at_selection() const
{
    const WorkItemSnapshot *s = selected_snapshot();
    if (!s) return {};
    return dwarf_.variables_at(s->link_pc);
}

bool Session::read_memory(uint64_t addr, void *buf, size_t len) const
{
    return inf_.read_mem(addr, buf, len);
}

bool Session::buffer_address(const std::string &name, uint64_t &out, std::string &err) const
{
    for (unsigned i = 0; i < spec_.nargs; i++) {
        if (name != spec_.args[i].name) continue;
        if (spec_.args[i].kind != RS_GLOBAL) {
            err = format("'%s' is not a __global buffer argument", name.c_str());
            return false;
        }
        if (!wg_call_.args_ptr) { err = "no work-group is active"; return false; }
        if (!pocl::read_arg_pointer(inf_, wg_call_, i, out)) {
            err = format("cannot follow pocl's argument block for '%s'", name.c_str());
            return false;
        }
        return true;
    }
    err = format("'%s' is not a kernel argument in this spec", name.c_str());
    return false;
}

bool Session::eval(const std::string &src, Value &out, std::string &err)
{
    const WorkItemSnapshot *s = selected_snapshot();
    if (!s) { err = "no work-item is selected"; return false; }

    ExprEnv env;
    env.eval = eval_context_for(*s);

    /* Integer literals need *some* DWARF integer type; borrow the kernel's own `int`. */
    DWARFDie int_type;
    if (dwarf_.unit()) {
        for (DWARFDie d : dwarf_.unit()->getUnitDIE(false).children()) {
            if (d.getTag() != DW_TAG_base_type) continue;
            auto n = dwarf::toString(d.find(DW_AT_name));
            if (n && std::string(*n) == "int") { int_type = d; break; }
        }
    }
    env.int_type = int_type;

    env.lookup = [&](const std::string &name, Value &v, std::string &e) -> bool {
        auto it = s->vars.find(name);
        if (it != s->vars.end()) {
            v = it->second;
            /* pocl can drop a __global pointer's location at some PCs; the argument
             * block it passed to the work-group function still has it. */
            if (!v.available && type_is_pointer(v.type)) {
                uint64_t p = 0;
                std::string ignored;
                if (buffer_address(name, p, ignored)) {
                    Value nv;
                    nv.type = v.type;
                    nv.available = true;
                    nv.bytes.resize(8);
                    memcpy(nv.bytes.data(), &p, 8);
                    v = nv;
                    return true;
                }
            }
            if (!v.available) {
                e = format("'%s' is %s here", name.c_str(),
                           v.error.empty() ? "optimized out" : v.error.c_str());
                return false;
            }
            return true;
        }
        e = format("no symbol '%s' in this context", name.c_str());
        return false;
    };

    return eval_expression(src, env, out, err);
}

} // namespace oclgdb
