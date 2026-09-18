#include "cli.h"
#include "util.h"

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstring>
#include <llvm/BinaryFormat/Dwarf.h>

using namespace llvm;
using namespace llvm::dwarf;

namespace oclgdb {

void Cli::banner() const
{
    printf("%soclgdb%s -- source-level OpenCL debugger for pocl's CPU device\n",
           color::bold(), color::reset());
    printf("%stype 'help' for commands, 'quit' to leave%s\n\n", color::dim(), color::reset());
}

void Cli::prompt() const
{
    printf("%s(oclgdb)%s ", color::cyan(), color::reset());
    fflush(stdout);
}

/* ---------------------------------------------------------------- helpers */

static bool parse_triple(const std::string &s, size_t out[3])
{
    std::string t;
    for (char c : s) t += (c == '(' || c == ')' || c == ',') ? ' ' : c;
    auto toks = tokenize(t);
    if (toks.empty() || toks.size() > 3) return false;
    out[0] = out[1] = out[2] = 0;
    for (size_t i = 0; i < toks.size(); i++) {
        int64_t v;
        if (!parse_int(toks[i], v) || v < 0) return false;
        out[i] = (size_t)v;
    }
    return true;
}

void Cli::show_source_line(unsigned line, bool arrow) const
{
    const auto &src = s_.source_lines();
    if (line == 0 || line > src.size()) return;
    printf("%s%s%4u%s  %s%s\n",
           arrow ? color::green() : "", arrow ? "=> " : "   ",
           line, color::reset(), src[line - 1].c_str(), color::reset());
}

/* ------------------------------------------------------------- reporting */

void Cli::report_stop()
{
    if (s_.state() == Session::State::Exited) {
        printf("\n%s[the harness exited with code %d]%s\n",
               color::dim(), s_.exit_code(), color::reset());
        return;
    }

    const auto &st = s_.last_stop();
    if (!st.valid) return;
    if (!st.message.empty() && st.line == 0) {
        printf("\n%s%s%s\n", color::red(), st.message.c_str(), color::reset());
        return;
    }

    printf("\n");
    if (st.bp_id > 0)
        printf("%sBreakpoint %d%s, %s:%u in kernel %s%s%s\n",
               color::bold(), st.bp_id, color::reset(),
               basename_of(s_.kernel_source()).c_str(), st.line,
               color::bold(), s_.spec().kernel_name, color::reset());
    else
        printf("%sStopped%s at %s:%u\n", color::bold(), color::reset(),
               basename_of(s_.kernel_source()).c_str(), st.line);

    printf("  work-group (%zu,%zu,%zu) of (%zu,%zu,%zu)"
           "   --   %zu of %zu work-items reached this line\n",
           st.group[0], st.group[1], st.group[2],
           s_.nd().groups(0), s_.nd().groups(1), s_.nd().groups(2),
           st.reached, st.group_size);

    const WorkItemSnapshot *sel = s_.selected_snapshot();
    if (sel) {
        printf("  %s%s work-item%s %s   local (%zu,%zu,%zu)\n",
               color::bold(), sel->live ? "halted at" : "selected", color::reset(),
               sel->id.str_global().c_str(),
               sel->id.local[0], sel->id.local[1], sel->id.local[2]);
    }
    if (!st.live)
        printf("  %s(no work-item is physically halted here: the work-group ran past this "
               "line.\n   state below is from per-work-item snapshots -- see 'help snapshots')%s\n",
               color::dim(), color::reset());
    if (!st.message.empty()) printf("  %s%s%s\n", color::yellow(), st.message.c_str(), color::reset());

    printf("\n");
    unsigned ctx = s_.options().list_context;
    unsigned from = st.line > ctx ? st.line - 2 : 1;
    for (unsigned l = from; l <= st.line + 2; l++) show_source_line(l, l == st.line);
}

void Cli::show_work_item_table() const
{
    const auto &snaps = s_.snapshots();
    if (snaps.empty()) { printf("no work-item state has been captured\n"); return; }

    /* Choose up to four scalar variables to show as columns. */
    std::vector<std::string> cols;
    for (const auto &kv : snaps.front().vars) {
        DWARFDie t = strip_typedefs(kv.second.type);
        if (t && (t.getTag() == DW_TAG_base_type)) cols.push_back(kv.first);
        if (cols.size() >= 4) break;
    }

    printf("   %-3s %-13s %-11s %-6s %-18s", "", "global id", "local id", "visit", "pc");
    for (const auto &c : cols) printf(" %-12s", c.c_str());
    printf("\n");

    int sel = s_.selected();
    for (size_t i = 0; i < snaps.size(); i++) {
        const auto &sn = snaps[i];
        const char *mark = (int)i == sel ? "*" : (sn.live ? "+" : " ");
        printf("%s%s  %-3zu %-13s %-11s %-6zu 0x%-16llx%s",
               (int)i == sel ? color::bold() : "",
               mark, i,
               sn.id.str_global().c_str(),
               format("(%zu,%zu,%zu)", sn.id.local[0], sn.id.local[1], sn.id.local[2]).c_str(),
               sn.visit, (unsigned long long)sn.pc,
               color::reset());
        for (const auto &c : cols) {
            auto it = sn.vars.find(c);
            std::string v = it == sn.vars.end()
                          ? "-"
                          : format_value_brief(it->second, EvalContext{});
            printf(" %-12s", v.c_str());
        }
        printf("\n");
    }
    printf("\n%s* = selected, + = physically halted here%s\n", color::dim(), color::reset());
}

void Cli::show_locals(bool params_only, bool all) const
{
    const WorkItemSnapshot *s = s_.selected_snapshot();
    if (!s) { printf("no work-item is selected\n"); return; }

    auto vars = s_.locals_at_selection();
    EvalContext ctx = s_.eval_context_for(*s);
    bool any = false;
    for (const auto &v : vars) {
        if (!all && params_only != v.is_param) continue;
        auto it = s->vars.find(v.name);
        if (it == s->vars.end()) continue;
        any = true;
        printf("  %s%-10s%s %s%-16s%s = %s\n",
               color::bold(), v.name.c_str(), color::reset(),
               color::dim(), type_name(v.type).c_str(), color::reset(),
               format_value(it->second, ctx).c_str());
    }
    if (!any) printf("  (none in scope at this line)\n");
}

/* --------------------------------------------------------------- commands */

void Cli::cmd_help(const std::vector<std::string> &a)
{
    if (a.size() > 1 && a[1] == "snapshots") {
        printf(
"Why snapshots?\n"
"\n"
"  pocl runs every work-item of a work-group through one host stack frame, and it\n"
"  keeps kernel variables in machine registers with short live ranges. A work-item's\n"
"  values therefore only exist while that work-item is the one executing -- by the\n"
"  time work-item 63 is running, work-item 0's registers have been reused.\n"
"\n"
"  So oclgdb resolves every in-scope variable at each work-item's *own* trap and keeps\n"
"  the result. 'info work-items' and 'print' then answer correctly for any work-item\n"
"  in the group, not just the one still halted.\n"
"\n"
"  If you want a work-item genuinely halted mid-flight -- to step it -- put the filter\n"
"  on the breakpoint instead:   break 24 if wi == (255,0,0)\n");
        return;
    }
    printf(
"setup\n"
"  load <spec.ocl-run>          read an OpenCL launch description\n"
"  compile                      build the kernel through pocl and index its DWARF\n"
"  list [from[,to]]             show kernel source\n"
"\n"
"breakpoints\n"
"  break <line>                 halt at a source line (all work-items)\n"
"  break <file>:<line>          same, file name is checked against the kernel\n"
"  break <line> if wi == (x,y,z)    halt only for that global work-item\n"
"  break <line> if lid == (x,y,z)   halt only for that local id, in every work-group\n"
"  info breakpoints             list them\n"
"  delete [id]                  remove one, or all\n"
"\n"
"execution\n"
"  run                          start the harness under ptrace\n"
"  continue                     resume\n"
"  step                         advance the halted work-item one source line\n"
"  kill                         stop the harness\n"
"\n"
"work-items\n"
"  info work-items              every work-item that reached this line, with values\n"
"  select-work-item <x[,y,z]>   choose whose state to inspect  (alias: wi)\n"
"  info workgroup               the current work-group and NDRange\n"
"  info wi-model                how work-item identity was established\n"
"\n"
"inspection\n"
"  print <expr>                 e.g. print sum,  print in[gid+1],  print tile[lid]\n"
"  info locals | info args      variables in scope, resolved through DWARF\n"
"  info registers               the selected work-item's captured register file\n"
"  info location <var>          where DWARF says a variable lives\n"
"  info buffers                 __global buffers and their host addresses\n"
"  x/<count><fmt> <expr>        examine memory; fmt is one of x d u f c, e.g. x/8f in\n"
"\n"
"options\n"
"  set scan on|off              capture the whole work-group at a breakpoint (default on)\n"
"  set verbose on|off           show loader and cache detail\n"
"\n"
"  help snapshots               why cross-work-item inspection works the way it does\n");
}

void Cli::cmd_list(const std::vector<std::string> &a)
{
    if (!s_.spec_loaded()) { printf("no spec loaded\n"); return; }
    const auto &src = s_.source_lines();
    unsigned from = 1, to = (unsigned)src.size();
    if (a.size() > 1) {
        auto parts = split(a[1], ',');
        int64_t v;
        if (parse_int(parts[0], v)) from = (unsigned)std::max<int64_t>(1, v);
        to = from + 20;
        if (parts.size() > 1 && parse_int(parts[1], v)) to = (unsigned)v;
    }
    to = std::min<unsigned>(to, (unsigned)src.size());

    std::vector<unsigned> code;
    if (s_.have_image()) code = s_.dwarf().executable_lines();

    for (unsigned l = from; l <= to; l++) {
        bool has_code = std::binary_search(code.begin(), code.end(), l);
        printf("%s%4u%s %s %s\n", color::dim(), l, color::reset(),
               has_code ? "*" : " ", src[l - 1].c_str());
    }
    if (!code.empty())
        printf("%s(* marks lines with machine code -- those are the ones you can break on)%s\n",
               color::dim(), color::reset());
}

void Cli::cmd_break(const std::string &rest)
{
    std::string spec = trim(rest);
    if (spec.empty()) { printf("usage: break <line> [if wi == (x,y,z)]\n"); return; }

    bool filtered = false, is_local = false;
    size_t wi[3] = {0, 0, 0};

    size_t ifpos = spec.find(" if ");
    if (ifpos != std::string::npos) {
        std::string cond = trim(spec.substr(ifpos + 4));
        spec = trim(spec.substr(0, ifpos));
        std::string lhs = cond.substr(0, cond.find("=="));
        lhs = trim(lhs);
        size_t eq = cond.find("==");
        if (eq == std::string::npos) {
            printf("the only supported condition is 'wi == (x,y,z)' or 'lid == (x,y,z)'\n");
            return;
        }
        std::string rhs = trim(cond.substr(eq + 2));
        if (lhs == "wi" || lhs == "gid" || lhs == "global") is_local = false;
        else if (lhs == "lid" || lhs == "local") is_local = true;
        else {
            printf("unknown condition target '%s'; use 'wi' or 'lid'\n", lhs.c_str());
            return;
        }
        if (!parse_triple(rhs, wi)) { printf("cannot parse work-item id '%s'\n", rhs.c_str()); return; }
        filtered = true;
    }

    /* Accept file:line, and check the file name against the kernel we loaded. */
    size_t colon = spec.find(':');
    if (colon != std::string::npos) {
        std::string file = spec.substr(0, colon);
        std::string want = basename_of(s_.kernel_source());
        if (basename_of(file) != want) {
            printf("this session only has one source file, '%s'\n", want.c_str());
            return;
        }
        spec = spec.substr(colon + 1);
    }

    int64_t line = 0;
    if (!parse_int(trim(spec), line) || line <= 0) {
        printf("cannot parse a line number from '%s'\n", spec.c_str());
        return;
    }

    std::string err;
    int id = s_.add_breakpoint((unsigned)line, filtered, wi, is_local, err);
    if (id < 0) { printf("%serror:%s %s\n", color::red(), color::reset(), err.c_str()); return; }

    const auto &bp = s_.breakpoints().back();
    printf("Breakpoint %d at %s:%u", id, basename_of(s_.kernel_source()).c_str(), bp.line);
    if (bp.link_addrs.size() > 1)
        printf("  (%zu machine addresses -- pocl replicated the body once per work-item)",
               bp.link_addrs.size());
    if (filtered)
        printf("\n  halting only for %s work-item (%zu,%zu,%zu)",
               is_local ? "local" : "global", wi[0], wi[1], wi[2]);
    printf("\n");
    show_source_line(bp.line, false);
}

void Cli::cmd_info(const std::vector<std::string> &a)
{
    std::string what = a.size() > 1 ? a[1] : "";

    if (what == "breakpoints" || what == "break" || what == "b") {
        if (s_.breakpoints().empty()) { printf("no breakpoints\n"); return; }
        for (const auto &b : s_.breakpoints()) {
            printf("  %d   %s:%u   %zu hit%s   %zu site%s%s\n", b.id,
                   basename_of(s_.kernel_source()).c_str(), b.line,
                   b.hits, b.hits == 1 ? "" : "s",
                   b.link_addrs.size(), b.link_addrs.size() == 1 ? "" : "s",
                   b.filtered ? format("   only work-item (%zu,%zu,%zu)",
                                       b.wi[0], b.wi[1], b.wi[2]).c_str() : "");
        }
        return;
    }

    if (what == "work-items" || what == "workitems" || what == "wi") {
        show_work_item_table();
        return;
    }

    if (what == "workgroup" || what == "wg" || what == "ndrange") {
        const auto &nd = s_.nd();
        printf("  NDRange      global (%zu,%zu,%zu)   local (%zu,%zu,%zu)   dim %u\n",
               nd.global[0], nd.global[1], nd.global[2],
               nd.local[0], nd.local[1], nd.local[2], nd.dim);
        printf("  work-groups  (%zu,%zu,%zu) = %zu groups of %zu work-items\n",
               nd.groups(0), nd.groups(1), nd.groups(2), nd.num_groups(), nd.local_size());
        if (s_.last_stop().valid) {
            const auto &st = s_.last_stop();
            printf("  current      work-group (%zu,%zu,%zu)\n",
                   st.group[0], st.group[1], st.group[2]);
        }
        return;
    }

    if (what == "wi-model" || what == "model") {
        int bp_id = s_.last_stop().valid ? s_.last_stop().bp_id : -1;
        const IdentityModel *m = s_.identity_model_for(bp_id);
        if (!m) { printf("identity has not been calibrated yet for this breakpoint (its "
                        "line hasn't been reached by a full work-group)\n"); return; }
        printf("  %s\n", m->describe().c_str());
        if (m->cross_checked) printf("  %s\n", m->note.c_str());
        else if (!m->note.empty())
            printf("  %swarning:%s %s\n", color::yellow(), color::reset(), m->note.c_str());
        return;
    }

    if (what == "locals") { show_locals(false, false); return; }
    if (what == "args")   { show_locals(true, false);  return; }
    if (what == "all")    { show_locals(false, true);  return; }

    if (what == "kernel") {
        if (!s_.have_image()) { printf("no kernel image loaded (try 'compile')\n"); return; }
        const auto &d = s_.dwarf();
        printf("  kernel        %s\n", s_.spec().kernel_name);
        printf("  source        %s\n", s_.kernel_source().c_str());
        printf("  object        %s\n", s_.object_path().c_str());
        printf("  producer      %s\n", d.producer().c_str());
        printf("  DWARF source  %s %s(pocl compiles through a temporary file)%s\n",
               d.dwarf_source_name().c_str(), color::dim(), color::reset());
        if (s_.load_bias())
            printf("  loaded at     0x%llx\n", (unsigned long long)s_.load_bias());
        printf("  functions\n");
        for (const auto &f : d.functions())
            printf("      %-40s 0x%08llx-0x%08llx\n", f.name.c_str(),
                   (unsigned long long)f.low, (unsigned long long)f.high);
        return;
    }

    if (what == "registers" || what == "reg") {
        const WorkItemSnapshot *s = s_.selected_snapshot();
        if (!s) { printf("no work-item is selected\n"); return; }
        for (unsigned r = 0; r < 16; r++) {
            uint64_t v = 0;
            s->regs.read(r, v);
            printf("  %-5s 0x%016llx %20lld\n", RegisterFile::reg_name(r),
                   (unsigned long long)v, (long long)v);
            if (r % 2 == 1) {}
        }
        printf("  %-5s 0x%016llx\n", "rip", (unsigned long long)s->regs.gp.rip);
        return;
    }

    if (what == "location") {
        if (a.size() < 3) { printf("usage: info location <variable>\n"); return; }
        const WorkItemSnapshot *s = s_.selected_snapshot();
        if (!s) { printf("no work-item is selected\n"); return; }
        for (const auto &v : s_.locals_at_selection()) {
            if (v.name != a[2]) continue;
            printf("  %s : %s\n", v.name.c_str(), type_name(v.type).c_str());
            auto locs = s_.dwarf().all_locations(v.die);
            if (locs.empty()) { printf("    (no DW_AT_location)\n"); return; }
            for (const auto &l : locs) {
                bool here = s->link_pc >= l.first.first && s->link_pc < l.first.second;
                printf("    %s[0x%06llx,0x%06llx)  %s%s\n",
                       here ? color::green() : color::dim(),
                       (unsigned long long)l.first.first, (unsigned long long)l.first.second,
                       describe_dwarf_expr(l.second).c_str(), color::reset());
            }
            printf("    %s(green = the range covering this work-item's pc 0x%llx)%s\n",
                   color::dim(), (unsigned long long)s->link_pc, color::reset());
            return;
        }
        printf("no variable named '%s' in scope here\n", a[2].c_str());
        return;
    }

    if (what == "buffers") {
        bool any = false;
        for (unsigned i = 0; i < s_.spec().nargs; i++) {
            const auto &arg = s_.spec().args[i];
            if (arg.kind != RS_GLOBAL) continue;
            any = true;
            uint64_t addr = 0;
            std::string err;
            if (s_.buffer_address(arg.name, addr, err))
                printf("  %-10s %-6s x %-6zu  at 0x%llx   %s\n", arg.name,
                       rs_type_name(arg.type), arg.count, (unsigned long long)addr,
                       arg.dir == RS_OUT ? "(output)" : arg.dir == RS_INOUT ? "(in/out)" : "(input)");
            else
                printf("  %-10s %-6s x %-6zu  %s\n", arg.name, rs_type_name(arg.type),
                       arg.count, err.c_str());
        }
        if (!any) printf("  (this kernel takes no __global buffers)\n");
        return;
    }

    printf("info what? try: breakpoints, work-items, workgroup, wi-model, locals, args, "
           "kernel, registers, location <var>, buffers\n");
}

void Cli::cmd_print(const std::string &rest)
{
    std::string e = trim(rest);
    if (e.empty()) { printf("usage: print <expression>\n"); return; }
    Value v;
    std::string err;
    if (!s_.eval(e, v, err)) {
        printf("%serror:%s %s\n", color::red(), color::reset(), err.c_str());
        return;
    }
    const WorkItemSnapshot *s = s_.selected_snapshot();
    EvalContext ctx = s ? s_.eval_context_for(*s) : EvalContext{};
    printf("  %s = %s", e.c_str(), format_value(v, ctx).c_str());
    if (v.type) printf("   %s(%s)%s", color::dim(), type_name(v.type).c_str(), color::reset());
    printf("\n");
}

void Cli::cmd_examine(const std::string &spec, const std::string &rest)
{
    unsigned count = 1;
    char fmt = 'x';
    size_t i = 0;
    std::string digits;
    while (i < spec.size() && std::isdigit((unsigned char)spec[i])) digits += spec[i++];
    if (!digits.empty()) count = (unsigned)strtoul(digits.c_str(), nullptr, 10);
    if (i < spec.size()) fmt = spec[i];
    if (count == 0 || count > 4096) { printf("count out of range\n"); return; }

    Value base;
    std::string err;
    if (!s_.eval(trim(rest), base, err)) {
        printf("%serror:%s %s\n", color::red(), color::reset(), err.c_str());
        return;
    }

    uint64_t addr = 0;
    DWARFDie el = type_pointee(base.type);
    if (type_is_pointer(base.type)) base.as_uint(addr);
    else if (base.has_address) addr = base.address;
    else if (!base.as_uint(addr)) { printf("cannot turn that into an address\n"); return; }

    size_t esz = fmt == 'c' ? 1 : (fmt == 'f' ? (el ? type_size(el) : 4) : 4);
    if (fmt == 'f' && esz != 4 && esz != 8) esz = 4;

    printf("  0x%llx:", (unsigned long long)addr);
    for (unsigned k = 0; k < count; k++) {
        unsigned char buf[8] = {0};
        if (!s_.read_memory(addr + (uint64_t)k * esz, buf, esz)) { printf("  <unreadable>"); break; }
        if (k && k % 8 == 0) printf("\n  0x%llx:", (unsigned long long)(addr + (uint64_t)k * esz));
        switch (fmt) {
        case 'f': {
            if (esz == 4) { float f; memcpy(&f, buf, 4); printf(" %10g", f); }
            else          { double d; memcpy(&d, buf, 8); printf(" %10g", d); }
            break;
        }
        case 'd': { int32_t v; memcpy(&v, buf, 4); printf(" %10d", v); break; }
        case 'u': { uint32_t v; memcpy(&v, buf, 4); printf(" %10u", v); break; }
        case 'c': printf(" %4d", (int)(signed char)buf[0]); break;
        default:  { uint32_t v; memcpy(&v, buf, 4); printf(" 0x%08x", v); break; }
        }
    }
    printf("\n");
}

void Cli::cmd_select(const std::string &rest)
{
    size_t g[3];
    if (!parse_triple(trim(rest), g)) { printf("usage: select-work-item <x[,y,z]>\n"); return; }
    std::string err;
    if (!s_.select_global(g, err)) {
        printf("%serror:%s %s\n", color::red(), color::reset(), err.c_str());
        return;
    }
    const WorkItemSnapshot *s = s_.selected_snapshot();
    printf("work-item %s selected   %s\n", s->id.str_global().c_str(),
           s->live ? "(this one is physically halted here)" : "(from its snapshot)");
    show_locals(false, true);
}

void Cli::cmd_set(const std::vector<std::string> &a)
{
    if (a.size() < 3) { printf("usage: set <scan|verbose> <on|off>\n"); return; }
    bool on = a[2] == "on" || a[2] == "1" || a[2] == "true";
    if (a[1] == "scan") {
        s_.options().scan_work_group = on;
        printf("work-group scanning %s\n", on ? "on" : "off");
    } else if (a[1] == "verbose") {
        s_.options().verbose = on;
        printf("verbose %s\n", on ? "on" : "off");
    } else {
        printf("unknown option '%s'\n", a[1].c_str());
    }
}

/* ------------------------------------------------------------- dispatch */

bool Cli::execute(const std::string &raw)
{
    std::string line = trim(raw);
    if (line.empty() || line[0] == '#') return true;
    if (echo) printf("%s(oclgdb)%s %s\n", color::cyan(), color::reset(), line.c_str());

    auto a = tokenize(line);
    std::string cmd = a[0];
    std::string rest = trim(line.substr(cmd.size()));

    if (cmd == "quit" || cmd == "q" || cmd == "exit") return false;

    if (cmd == "help" || cmd == "h" || cmd == "?") { cmd_help(a); return true; }

    if (cmd == "load") {
        if (a.size() < 2) { printf("usage: load <spec.ocl-run>\n"); return true; }
        std::string err;
        if (!s_.load_spec(a[1], err)) {
            printf("%serror:%s %s\n", color::red(), color::reset(), err.c_str());
            return true;
        }
        printf("loaded %s: kernel '%s' from %s\n", a[1].c_str(),
               s_.spec().kernel_name, s_.spec().kernel_path);
        printf("  NDRange global (%zu,%zu,%zu)  local (%zu,%zu,%zu)  ->  %zu work-groups "
               "of %zu work-items\n",
               s_.nd().global[0], s_.nd().global[1], s_.nd().global[2],
               s_.nd().local[0], s_.nd().local[1], s_.nd().local[2],
               s_.nd().num_groups(), s_.nd().local_size());
        return true;
    }

    if (cmd == "compile") {
        std::string err;
        if (!s_.ensure_kernel_image(err)) {
            printf("%serror:%s %s\n", color::red(), color::reset(), err.c_str());
            return true;
        }
        printf("compiled through pocl: %s\n", s_.object_path().c_str());
        printf("  DWARF from %s\n", s_.dwarf().producer().c_str());
        printf("  %zu source lines carry machine code\n", s_.dwarf().executable_lines().size());
        return true;
    }

    if (cmd == "list" || cmd == "l")  { cmd_list(a); return true; }
    if (cmd == "break" || cmd == "b") { cmd_break(rest); return true; }
    if (cmd == "info" || cmd == "i")  { cmd_info(a); return true; }

    if (cmd == "delete" || cmd == "d") {
        if (a.size() < 2) { s_.clear_breakpoints(); printf("all breakpoints deleted\n"); return true; }
        int64_t id;
        if (!parse_int(a[1], id) || !s_.remove_breakpoint((int)id))
            printf("no breakpoint %s\n", a[1].c_str());
        return true;
    }

    if (cmd == "run" || cmd == "r") {
        std::string err;
        if (!s_.start(err)) {
            printf("%serror:%s %s\n", color::red(), color::reset(), err.c_str());
            return true;
        }
        report_stop();
        return true;
    }

    if (cmd == "continue" || cmd == "c") {
        std::string err;
        if (!s_.resume_exec(err)) {
            printf("%serror:%s %s\n", color::red(), color::reset(), err.c_str());
            return true;
        }
        report_stop();
        return true;
    }

    if (cmd == "step" || cmd == "s" || cmd == "next" || cmd == "n") {
        std::string err;
        if (!s_.step_line(err)) {
            printf("%serror:%s %s\n", color::red(), color::reset(), err.c_str());
            return true;
        }
        report_stop();
        return true;
    }

    if (cmd == "kill") { s_.kill(); printf("harness killed\n"); return true; }

    if (cmd == "select-work-item" || cmd == "wi" || cmd == "swi") {
        cmd_select(rest);
        return true;
    }

    if (cmd == "print" || cmd == "p") { cmd_print(rest); return true; }

    if (starts_with(cmd, "x/")) { cmd_examine(cmd.substr(2), rest); return true; }
    if (cmd == "x") {
        auto sp = tokenize(rest);
        if (!sp.empty() && sp[0][0] == '/')
            cmd_examine(sp[0].substr(1), trim(rest.substr(sp[0].size())));
        else
            printf("usage: x/<count><fmt> <expr>\n");
        return true;
    }

    if (cmd == "set") { cmd_set(a); return true; }

    printf("unknown command '%s' -- try 'help'\n", cmd.c_str());
    return true;
}

} // namespace oclgdb
