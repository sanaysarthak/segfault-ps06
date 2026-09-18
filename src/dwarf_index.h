#pragma once

/* DWARF index over the kernel object pocl produced.
 *
 * We do not parse DWARF by hand -- LLVM already has a complete, well-tested reader and
 * we are linking LLVM anyway. This class is a thin, debugger-shaped view over
 * DWARFContext: "which addresses start source line N", "which function owns this PC",
 * "which variables are in scope here, and where does DWARF say each one lives".
 *
 * All addresses here are *link* addresses (as they appear in the object file). The
 * session adds the runtime load bias before touching the inferior.
 */

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <llvm/DebugInfo/DWARF/DWARFContext.h>
#include <llvm/Object/ObjectFile.h>

namespace oclgdb {

struct LineRow {
    uint64_t addr = 0;
    unsigned line = 0;
    unsigned column = 0;
    unsigned file_index = 0;
    bool     is_stmt = false;
    bool     end_sequence = false;
    bool     prologue_end = false;
};

struct FuncInfo {
    std::string    name;
    uint64_t       low = 0;
    uint64_t       high = 0;
    llvm::DWARFDie die;
};

struct VarInfo {
    std::string    name;
    llvm::DWARFDie die;       /* the concrete DIE, carrying DW_AT_location */
    llvm::DWARFDie type;      /* resolved through DW_AT_abstract_origin if needed */
    bool           is_param = false;
    unsigned       decl_line = 0;
};

class DwarfIndex {
public:
    bool load(const std::string &object_path, std::string &err);
    bool loaded() const { return ctx_ != nullptr; }

    const std::string &object_path() const { return object_path_; }
    /* The file name Clang recorded. pocl compiles kernels through a temporary file, so
     * this is typically something like /tmp/pocl-cache/tempfile_ab12cd.cl. */
    const std::string &dwarf_source_name() const { return dwarf_source_name_; }
    const std::string &producer() const { return producer_; }

    const std::vector<FuncInfo> &functions() const { return funcs_; }
    const std::vector<LineRow>  &lines() const { return rows_; }

    /* Source lines that actually have machine code, ascending. */
    std::vector<unsigned> executable_lines() const;

    /* Every address at which source line `line` begins, ascending. In pocl's
     * replication mode this returns one address *per work-item*. */
    std::vector<uint64_t> addresses_for_line(unsigned line) const;

    /* Same, restricted to one function. */
    std::vector<uint64_t> addresses_for_line(unsigned line, const FuncInfo &f) const;

    /* First line >= `line` that has code, or 0. */
    unsigned next_line_with_code(unsigned line) const;

    const LineRow  *row_for_addr(uint64_t addr) const;
    const FuncInfo *func_for_addr(uint64_t addr) const;
    const FuncInfo *func_by_name(const std::string &name) const;

    /* Parameters and locals whose scope covers `addr`, outermost first. */
    std::vector<VarInfo> variables_at(uint64_t addr) const;

    /* DWARF location expressions for `die` that are valid at `addr`. An entry with no
     * range is unconditionally valid (a plain DW_AT_location exprloc). */
    bool location_at(const llvm::DWARFDie &die, uint64_t addr,
                     std::vector<uint8_t> &expr_out) const;
    /* All location-list entries, for `info location`. */
    std::vector<std::pair<std::pair<uint64_t, uint64_t>, std::vector<uint8_t>>>
    all_locations(const llvm::DWARFDie &die) const;

    llvm::DWARFContext *ctx() const { return ctx_.get(); }
    llvm::DWARFUnit    *unit() const { return unit_; }

private:
    void index_functions();
    void index_lines();
    void collect_scope(const llvm::DWARFDie &die, uint64_t addr,
                       std::vector<VarInfo> &out) const;

    std::unique_ptr<llvm::MemoryBuffer>                    buffer_;
    std::unique_ptr<llvm::object::ObjectFile>              obj_;
    std::unique_ptr<llvm::DWARFContext>                    ctx_;
    llvm::DWARFUnit                                       *unit_ = nullptr;

    std::string object_path_;
    std::string dwarf_source_name_;
    std::string producer_;

    std::vector<FuncInfo> funcs_;
    std::vector<LineRow>  rows_;                       /* sorted by addr */
    std::map<unsigned, std::vector<uint64_t>> by_line_; /* line -> ascending addrs */
};

/* ------------------------------------------------------------------ type utils */

llvm::DWARFDie strip_typedefs(llvm::DWARFDie t);
std::string    type_name(llvm::DWARFDie t);
uint64_t       type_size(llvm::DWARFDie t);
/* DW_ATE_* for base types, 0 otherwise. */
unsigned       type_encoding(llvm::DWARFDie t);
bool           type_is_pointer(llvm::DWARFDie t);
bool           type_is_array(llvm::DWARFDie t);
llvm::DWARFDie type_pointee(llvm::DWARFDie t);
/* Number of elements for an array type, or 0 if unknown/unbounded. */
uint64_t       array_count(llvm::DWARFDie t);

} // namespace oclgdb
