#pragma once

/* Evaluator for DWARF location expressions.
 *
 * LLVM decodes the opcode stream for us; the stack machine, and crucially the bindings
 * for "read register" and "read memory", are ours -- because in this debugger those
 * bindings point either at a live ptrace'd register file or at a register file we
 * *snapshotted* when a particular work-item passed through the breakpoint. That
 * indirection is what makes per-work-item inspection possible at all (see F5 in
 * docs/PHASE0-FINDINGS.md).
 */

#include <cstdint>
#include <functional>
#include <string>
#include <sys/user.h>
#include <vector>

namespace oclgdb {

/* A captured x86-64 register file. Copyable, so it can live inside a work-item
 * snapshot long after the work-item itself has finished executing. */
struct RegisterFile {
    user_regs_struct   gp{};
    user_fpregs_struct fp{};
    bool               have_fp = false;

    /* DWARF register number -> value (low 64 bits for vector registers). */
    bool read(unsigned dwarf_reg, uint64_t &out) const;
    bool read_bytes(unsigned dwarf_reg, void *buf, size_t len) const;
    static const char *reg_name(unsigned dwarf_reg);
};

struct Location {
    enum class Kind {
        None,       /* could not be determined                         */
        Memory,     /* the object lives at `address` in the inferior   */
        Register,   /* the object lives in DWARF register `reg`        */
        Value,      /* DW_OP_stack_value: `value` *is* the object      */
        Implicit    /* DW_OP_implicit_value: `bytes` are the object    */
    };
    Kind                 kind = Kind::None;
    uint64_t             address = 0;
    unsigned             reg = 0;
    uint64_t             value = 0;
    std::vector<uint8_t> bytes;
    std::string          error;

    bool ok() const { return kind != Kind::None; }
};

/* Everything the evaluator needs from the outside world. */
struct EvalContext {
    const RegisterFile *regs = nullptr;
    std::function<bool(uint64_t addr, void *buf, size_t len)> read_mem;
    /* Frame base for DW_OP_fbreg, already evaluated. */
    bool     have_frame_base = false;
    uint64_t frame_base = 0;
    /* Runtime load bias of the object the expression came from -- DW_OP_addr operands
     * are link addresses and must be relocated before use. */
    uint64_t load_bias = 0;
};

/* Evaluate a DWARF expression. Never throws; failures come back as Kind::None with
 * `error` set, which the printer renders the way gdb renders it. */
Location eval_dwarf_expr(const std::vector<uint8_t> &expr, const EvalContext &ctx);

/* Human-readable rendering of an expression, for `info location`. */
std::string describe_dwarf_expr(const std::vector<uint8_t> &expr);

} // namespace oclgdb
