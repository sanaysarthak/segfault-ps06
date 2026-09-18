#pragma once

/* A small C expression evaluator over DWARF-typed values.
 *
 * Enough of C to interrogate a halted work-item the way you actually want to:
 *   print sum
 *   print in[gid + 1]
 *   print *out
 *   print tile[lid] + tile[lid + 1]
 *   print &c
 * Identifiers resolve through the caller's lookup hook, which in practice means "the
 * selected work-item's snapshot, then the kernel's arguments".
 */

#include "value.h"

#include <functional>
#include <string>

namespace oclgdb {

struct ExprEnv {
    std::function<bool(const std::string &name, Value &out, std::string &err)> lookup;
    EvalContext    eval;
    llvm::DWARFDie int_type;      /* type given to integer literals and arithmetic */
};

bool eval_expression(const std::string &src, const ExprEnv &env,
                     Value &out, std::string &err);

} // namespace oclgdb
