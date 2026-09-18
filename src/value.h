#pragma once

/* Typed values: bytes plus the DWARF type that explains them. */

#include "dwarf_expr.h"

#include <cstdint>
#include <string>
#include <vector>

#include <llvm/DebugInfo/DWARF/DWARFDie.h>

namespace oclgdb {

struct Value {
    llvm::DWARFDie       type;
    std::vector<uint8_t> bytes;
    bool                 available = false;
    std::string          error;        /* set when !available */

    /* Result of debugger-side arithmetic that has no DWARF type of its own; `bytes`
     * then holds an IEEE double. Kernel values never set this. */
    bool                 synthetic_float = false;

    /* Where the object lives, when it lives somewhere addressable. Needed so that
     * `print tile[3]` and `print &sum` can work. */
    bool     has_address = false;
    uint64_t address = 0;
    Location loc;

    bool as_int(int64_t &out) const;
    bool as_uint(uint64_t &out) const;
    bool as_double(double &out) const;
};

/* Materialise the object described by `loc` and `type`. */
Value read_value(llvm::DWARFDie type, const Location &loc, const EvalContext &ctx);

/* Build a value straight from an integer (for literals and arithmetic results). */
Value make_int_value(int64_t v, llvm::DWARFDie int_type);

/* gdb-flavoured rendering. `depth` limits aggregate expansion. */
std::string format_value(const Value &v, const EvalContext &ctx, unsigned max_elems = 8);

/* A compact one-line rendering used in the work-item table. */
std::string format_value_brief(const Value &v, const EvalContext &ctx);

} // namespace oclgdb
