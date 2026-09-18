#include "dwarf_index.h"
#include "util.h"

#include <algorithm>
#include <llvm/DebugInfo/DWARF/DWARFDebugLine.h>
#include <llvm/DebugInfo/DWARF/DWARFFormValue.h>
#include <llvm/Support/MemoryBuffer.h>

using namespace llvm;
using namespace llvm::dwarf;

namespace oclgdb {

bool DwarfIndex::load(const std::string &path, std::string &err)
{
    auto buf = MemoryBuffer::getFile(path, /*IsText=*/false, /*RequiresNullTerminator=*/false);
    if (!buf) { err = format("cannot read '%s': %s", path.c_str(),
                             buf.getError().message().c_str()); return false; }
    buffer_ = std::move(*buf);

    auto obj = object::ObjectFile::createObjectFile(buffer_->getMemBufferRef());
    if (!obj) {
        consumeError(obj.takeError());
        err = format("'%s' is not an object file we can read", path.c_str());
        return false;
    }
    obj_ = std::move(*obj);

    ctx_ = DWARFContext::create(*obj_);
    if (!ctx_ || ctx_->getNumCompileUnits() == 0) {
        err = format("'%s' has no DWARF compile units -- was the kernel built with -g?",
                     path.c_str());
        ctx_.reset();
        return false;
    }

    unit_ = ctx_->getUnitAtIndex(0);
    object_path_ = path;

    DWARFDie cu = unit_->getUnitDIE(false);
    if (auto n = dwarf::toString(cu.find(DW_AT_name))) dwarf_source_name_ = *n;
    if (auto p = dwarf::toString(cu.find(DW_AT_producer))) producer_ = *p;

    index_functions();
    index_lines();

    if (rows_.empty()) {
        err = format("'%s' has DWARF but no line table", path.c_str());
        return false;
    }
    return true;
}

void DwarfIndex::index_functions()
{
    funcs_.clear();
    DWARFDie cu = unit_->getUnitDIE(false);
    for (DWARFDie child : cu.children()) {
        if (child.getTag() != DW_TAG_subprogram) continue;
        auto lo = child.find(DW_AT_low_pc);
        if (!lo) continue;                       /* abstract instance, no code */
        auto ranges = child.getAddressRanges();
        if (!ranges) { consumeError(ranges.takeError()); continue; }
        if (ranges->empty()) continue;

        FuncInfo f;
        f.die = child;
        f.low = ranges->front().LowPC;
        f.high = ranges->front().HighPC;
        if (const char *n = child.getSubroutineName(DINameKind::ShortName)) f.name = n;
        if (f.name.empty())
            if (auto n = dwarf::toString(child.find(DW_AT_name))) f.name = *n;
        funcs_.push_back(std::move(f));
    }
    std::sort(funcs_.begin(), funcs_.end(),
              [](const FuncInfo &a, const FuncInfo &b) { return a.low < b.low; });
}

void DwarfIndex::index_lines()
{
    rows_.clear();
    by_line_.clear();

    const DWARFDebugLine::LineTable *lt = ctx_->getLineTableForUnit(unit_);
    if (!lt) return;

    for (const auto &r : lt->Rows) {
        LineRow row;
        row.addr = r.Address.Address;
        row.line = r.Line;
        row.column = r.Column;
        row.file_index = r.File;
        row.is_stmt = r.IsStmt;
        row.end_sequence = r.EndSequence;
        row.prologue_end = r.PrologueEnd;
        rows_.push_back(row);
    }
    std::sort(rows_.begin(), rows_.end(),
              [](const LineRow &a, const LineRow &b) { return a.addr < b.addr; });

    /* An address "begins" a source line if it is a statement boundary for that line and
     * the row before it (by address) was for a different line. That is what makes a
     * sensible breakpoint site, and in replication mode it yields exactly one site per
     * work-item. */
    unsigned prev_line = 0;
    bool first = true;
    for (const auto &r : rows_) {
        if (r.end_sequence || r.line == 0) { prev_line = 0; first = false; continue; }
        if (r.is_stmt && (first || r.line != prev_line))
            by_line_[r.line].push_back(r.addr);
        prev_line = r.line;
        first = false;
    }
    for (auto &kv : by_line_) {
        auto &v = kv.second;
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    }
}

std::vector<unsigned> DwarfIndex::executable_lines() const
{
    std::vector<unsigned> out;
    out.reserve(by_line_.size());
    for (const auto &kv : by_line_) out.push_back(kv.first);
    return out;
}

std::vector<uint64_t> DwarfIndex::addresses_for_line(unsigned line) const
{
    auto it = by_line_.find(line);
    return it == by_line_.end() ? std::vector<uint64_t>() : it->second;
}

std::vector<uint64_t> DwarfIndex::addresses_for_line(unsigned line, const FuncInfo &f) const
{
    std::vector<uint64_t> out;
    for (uint64_t a : addresses_for_line(line))
        if (a >= f.low && a < f.high) out.push_back(a);
    return out;
}

unsigned DwarfIndex::next_line_with_code(unsigned line) const
{
    auto it = by_line_.lower_bound(line);
    return it == by_line_.end() ? 0 : it->first;
}

const LineRow *DwarfIndex::row_for_addr(uint64_t addr) const
{
    if (rows_.empty()) return nullptr;
    auto it = std::upper_bound(rows_.begin(), rows_.end(), addr,
                               [](uint64_t a, const LineRow &r) { return a < r.addr; });
    if (it == rows_.begin()) return nullptr;
    --it;
    if (it->end_sequence) return nullptr;
    return &*it;
}

const FuncInfo *DwarfIndex::func_for_addr(uint64_t addr) const
{
    for (const auto &f : funcs_)
        if (addr >= f.low && addr < f.high) return &f;
    return nullptr;
}

const FuncInfo *DwarfIndex::func_by_name(const std::string &name) const
{
    for (const auto &f : funcs_)
        if (f.name == name) return &f;
    return nullptr;
}

/* ------------------------------------------------------------------ scopes */

static bool die_covers(const DWARFDie &die, uint64_t addr)
{
    auto ranges = die.getAddressRanges();
    if (!ranges) { consumeError(ranges.takeError()); return false; }
    for (const auto &r : *ranges)
        if (addr >= r.LowPC && addr < r.HighPC) return true;
    return false;
}

/* Resolve a DIE's "definition" -- for concrete inlined instances the name and type live
 * on the abstract origin, and only the location lives on the concrete DIE. */
static DWARFDie origin_of(const DWARFDie &die)
{
    if (auto spec = die.find(DW_AT_abstract_origin))
        if (auto ref = spec->getAsReference())
            if (DWARFDie o = die.getDwarfUnit()->getDIEForOffset(*ref)) return o;
    if (auto spec = die.find(DW_AT_specification))
        if (auto ref = spec->getAsReference())
            if (DWARFDie o = die.getDwarfUnit()->getDIEForOffset(*ref)) return o;
    return die;
}

void DwarfIndex::collect_scope(const DWARFDie &die, uint64_t addr,
                               std::vector<VarInfo> &out) const
{
    for (DWARFDie child : die.children()) {
        dwarf::Tag tag = child.getTag();
        if (tag == DW_TAG_formal_parameter || tag == DW_TAG_variable) {
            DWARFDie def = origin_of(child);
            VarInfo v;
            v.die = child;
            v.is_param = (tag == DW_TAG_formal_parameter);
            if (auto n = dwarf::toString(def.find(DW_AT_name))) v.name = *n;
            if (v.name.empty()) continue;
            v.type = def.getAttributeValueAsReferencedDie(DW_AT_type);
            if (auto l = dwarf::toUnsigned(def.find(DW_AT_decl_line))) v.decl_line = (unsigned)*l;
            /* An inner scope shadows an outer one. */
            auto dup = std::find_if(out.begin(), out.end(),
                                    [&](const VarInfo &o) { return o.name == v.name; });
            if (dup != out.end()) *dup = v;
            else out.push_back(std::move(v));
        } else if (tag == DW_TAG_lexical_block || tag == DW_TAG_inlined_subroutine) {
            if (die_covers(child, addr)) collect_scope(child, addr, out);
        }
    }
}

std::vector<VarInfo> DwarfIndex::variables_at(uint64_t addr) const
{
    std::vector<VarInfo> out;
    const FuncInfo *f = func_for_addr(addr);
    if (!f) return out;
    collect_scope(f->die, addr, out);
    return out;
}

/* --------------------------------------------------------------- locations */

std::vector<std::pair<std::pair<uint64_t, uint64_t>, std::vector<uint8_t>>>
DwarfIndex::all_locations(const DWARFDie &die) const
{
    std::vector<std::pair<std::pair<uint64_t, uint64_t>, std::vector<uint8_t>>> out;
    auto locs = die.getLocations(DW_AT_location);
    if (!locs) { consumeError(locs.takeError()); return out; }
    for (const auto &l : *locs) {
        std::pair<uint64_t, uint64_t> range{0, ~0ull};
        if (l.Range) range = {l.Range->LowPC, l.Range->HighPC};
        out.push_back({range, std::vector<uint8_t>(l.Expr.begin(), l.Expr.end())});
    }
    return out;
}

bool DwarfIndex::location_at(const DWARFDie &die, uint64_t addr,
                             std::vector<uint8_t> &expr_out) const
{
    auto locs = die.getLocations(DW_AT_location);
    if (!locs) { consumeError(locs.takeError()); return false; }
    for (const auto &l : *locs) {
        if (!l.Range) {                       /* plain exprloc: always in scope */
            expr_out.assign(l.Expr.begin(), l.Expr.end());
            return true;
        }
        if (addr >= l.Range->LowPC && addr < l.Range->HighPC) {
            expr_out.assign(l.Expr.begin(), l.Expr.end());
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------- types */

DWARFDie strip_typedefs(DWARFDie t)
{
    while (t) {
        dwarf::Tag tag = t.getTag();
        if (tag == DW_TAG_typedef || tag == DW_TAG_const_type ||
            tag == DW_TAG_volatile_type || tag == DW_TAG_restrict_type ||
            tag == DW_TAG_atomic_type) {
            DWARFDie next = t.getAttributeValueAsReferencedDie(DW_AT_type);
            if (!next) return DWARFDie();
            t = next;
            continue;
        }
        return t;
    }
    return t;
}

std::string type_name(DWARFDie t)
{
    if (!t) return "void";
    switch (t.getTag()) {
    case DW_TAG_base_type:
    case DW_TAG_structure_type:
    case DW_TAG_union_type:
    case DW_TAG_enumeration_type:
    case DW_TAG_typedef:
        if (auto n = dwarf::toString(t.find(DW_AT_name))) return *n;
        return "<anon>";
    case DW_TAG_pointer_type:
        return type_name(t.getAttributeValueAsReferencedDie(DW_AT_type)) + " *";
    case DW_TAG_const_type:
        return "const " + type_name(t.getAttributeValueAsReferencedDie(DW_AT_type));
    case DW_TAG_volatile_type:
        return "volatile " + type_name(t.getAttributeValueAsReferencedDie(DW_AT_type));
    case DW_TAG_array_type: {
        uint64_t n = array_count(t);
        std::string el = type_name(t.getAttributeValueAsReferencedDie(DW_AT_type));
        return n ? format("%s[%llu]", el.c_str(), (unsigned long long)n) : el + "[]";
    }
    default:
        if (auto n = dwarf::toString(t.find(DW_AT_name))) return *n;
        return "<type>";
    }
}

uint64_t array_count(DWARFDie t)
{
    if (!t || t.getTag() != DW_TAG_array_type) return 0;
    for (DWARFDie c : t.children()) {
        if (c.getTag() != DW_TAG_subrange_type) continue;
        if (auto n = dwarf::toUnsigned(c.find(DW_AT_count))) return *n;
        if (auto ub = dwarf::toUnsigned(c.find(DW_AT_upper_bound))) return *ub + 1;
    }
    return 0;
}

uint64_t type_size(DWARFDie t)
{
    if (!t) return 0;
    if (auto s = dwarf::toUnsigned(t.find(DW_AT_byte_size))) return *s;
    switch (t.getTag()) {
    case DW_TAG_pointer_type: return 8;
    case DW_TAG_typedef:
    case DW_TAG_const_type:
    case DW_TAG_volatile_type:
    case DW_TAG_restrict_type:
    case DW_TAG_atomic_type:
        return type_size(t.getAttributeValueAsReferencedDie(DW_AT_type));
    case DW_TAG_array_type: {
        uint64_t n = array_count(t);
        return n * type_size(t.getAttributeValueAsReferencedDie(DW_AT_type));
    }
    default: return 0;
    }
}

unsigned type_encoding(DWARFDie t)
{
    DWARFDie s = strip_typedefs(t);
    if (!s || s.getTag() != DW_TAG_base_type) return 0;
    if (auto e = dwarf::toUnsigned(s.find(DW_AT_encoding))) return (unsigned)*e;
    return 0;
}

bool type_is_pointer(DWARFDie t)
{
    DWARFDie s = strip_typedefs(t);
    return s && s.getTag() == DW_TAG_pointer_type;
}

bool type_is_array(DWARFDie t)
{
    DWARFDie s = strip_typedefs(t);
    return s && s.getTag() == DW_TAG_array_type;
}

DWARFDie type_pointee(DWARFDie t)
{
    DWARFDie s = strip_typedefs(t);
    if (!s) return DWARFDie();
    if (s.getTag() == DW_TAG_pointer_type || s.getTag() == DW_TAG_array_type)
        return s.getAttributeValueAsReferencedDie(DW_AT_type);
    return DWARFDie();
}

} // namespace oclgdb
