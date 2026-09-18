#include "dwarf_expr.h"
#include "util.h"

#include <cstring>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/DebugInfo/DWARF/DWARFExpression.h>
#include <llvm/Support/DataExtractor.h>

using namespace llvm;
using namespace llvm::dwarf;

namespace oclgdb {

/* ------------------------------------------------------- x86-64 register map */
/* Numbering per the System V AMD64 ABI, figure 3.36. */

static const char *const kGpNames[16] = {
    "rax", "rdx", "rcx", "rbx", "rsi", "rdi", "rbp", "rsp",
    "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"
};

const char *RegisterFile::reg_name(unsigned r)
{
    static char buf[16];
    if (r < 16) return kGpNames[r];
    if (r == 16) return "rip";
    if (r >= 17 && r <= 32) { snprintf(buf, sizeof buf, "xmm%u", r - 17); return buf; }
    if (r == 49) return "rflags";
    snprintf(buf, sizeof buf, "r#%u", r);
    return buf;
}

bool RegisterFile::read(unsigned r, uint64_t &out) const
{
    unsigned char tmp[8];
    if (!read_bytes(r, tmp, 8)) return false;
    memcpy(&out, tmp, 8);
    return true;
}

bool RegisterFile::read_bytes(unsigned r, void *buf, size_t len) const
{
    if (len > 16) return false;
    unsigned char tmp[16] = {0};

    if (r < 16) {
        static const size_t off[16] = {
            offsetof(user_regs_struct, rax), offsetof(user_regs_struct, rdx),
            offsetof(user_regs_struct, rcx), offsetof(user_regs_struct, rbx),
            offsetof(user_regs_struct, rsi), offsetof(user_regs_struct, rdi),
            offsetof(user_regs_struct, rbp), offsetof(user_regs_struct, rsp),
            offsetof(user_regs_struct, r8),  offsetof(user_regs_struct, r9),
            offsetof(user_regs_struct, r10), offsetof(user_regs_struct, r11),
            offsetof(user_regs_struct, r12), offsetof(user_regs_struct, r13),
            offsetof(user_regs_struct, r14), offsetof(user_regs_struct, r15),
        };
        memcpy(tmp, (const char *)&gp + off[r], 8);
    } else if (r == 16) {
        memcpy(tmp, &gp.rip, 8);
    } else if (r >= 17 && r <= 32) {
        if (!have_fp) return false;
        memcpy(tmp, &fp.xmm_space[(r - 17) * 4], 16);
    } else if (r == 49) {
        memcpy(tmp, &gp.eflags, 8);
    } else {
        return false;
    }
    memcpy(buf, tmp, len);
    return true;
}

/* -------------------------------------------------------------- the machine */

namespace {

struct Machine {
    const EvalContext  &ctx;
    std::vector<uint64_t> stack;
    Location            result;
    bool                stopped = false;

    explicit Machine(const EvalContext &c) : ctx(c) {}

    void fail(const std::string &why)
    {
        result.kind = Location::Kind::None;
        result.error = why;
        stopped = true;
    }

    bool pop(uint64_t &v)
    {
        if (stack.empty()) { fail("DWARF expression underflowed its stack"); return false; }
        v = stack.back();
        stack.pop_back();
        return true;
    }

    void push(uint64_t v) { stack.push_back(v); }
};

} // namespace

Location eval_dwarf_expr(const std::vector<uint8_t> &expr, const EvalContext &ctx)
{
    Location loc;
    if (expr.empty()) { loc.error = "empty location expression"; return loc; }

    StringRef data((const char *)expr.data(), expr.size());
    DataExtractor de(data, /*IsLittleEndian=*/true, /*AddressSize=*/8);
    DWARFExpression dex(de, 8, DWARF32);

    Machine m(ctx);

    for (const DWARFExpression::Operation &op : dex) {
        if (m.stopped) break;
        if (op.isError()) { m.fail("malformed DWARF location expression"); break; }

        uint16_t code = op.getCode();
        uint64_t o0 = op.getNumOperands() > 0 ? op.getRawOperand(0) : 0;
        uint64_t o1 = op.getNumOperands() > 1 ? op.getRawOperand(1) : 0;

        /* --- literals --- */
        if (code >= DW_OP_lit0 && code <= DW_OP_lit31) { m.push(code - DW_OP_lit0); continue; }

        /* --- "the object is in this register" --- */
        if (code >= DW_OP_reg0 && code <= DW_OP_reg31) {
            m.result.kind = Location::Kind::Register;
            m.result.reg = code - DW_OP_reg0;
            continue;
        }
        if (code == DW_OP_regx) {
            m.result.kind = Location::Kind::Register;
            m.result.reg = (unsigned)o0;
            continue;
        }

        /* --- "the object's address is register + offset" --- */
        if (code >= DW_OP_breg0 && code <= DW_OP_breg31) {
            if (!ctx.regs) { m.fail("no register file available"); break; }
            uint64_t rv = 0;
            if (!ctx.regs->read(code - DW_OP_breg0, rv)) {
                m.fail(format("cannot read register %s",
                              RegisterFile::reg_name(code - DW_OP_breg0)));
                break;
            }
            m.push(rv + (uint64_t)(int64_t)o0);
            continue;
        }
        if (code == DW_OP_bregx) {
            if (!ctx.regs) { m.fail("no register file available"); break; }
            uint64_t rv = 0;
            if (!ctx.regs->read((unsigned)o0, rv)) {
                m.fail(format("cannot read register %s", RegisterFile::reg_name((unsigned)o0)));
                break;
            }
            m.push(rv + (uint64_t)(int64_t)o1);
            continue;
        }

        switch (code) {
        case DW_OP_addr:
            m.push(o0 + ctx.load_bias);
            break;
        case DW_OP_const1u: case DW_OP_const2u: case DW_OP_const4u:
        case DW_OP_const8u: case DW_OP_constu:
            m.push(o0);
            break;
        case DW_OP_const1s: m.push((uint64_t)(int64_t)(int8_t)o0);  break;
        case DW_OP_const2s: m.push((uint64_t)(int64_t)(int16_t)o0); break;
        case DW_OP_const4s: m.push((uint64_t)(int64_t)(int32_t)o0); break;
        case DW_OP_const8s: case DW_OP_consts: m.push((uint64_t)(int64_t)o0); break;

        case DW_OP_fbreg: {
            if (!ctx.have_frame_base) { m.fail("no frame base for DW_OP_fbreg"); break; }
            m.push(ctx.frame_base + (uint64_t)(int64_t)o0);
            break;
        }

        case DW_OP_dup: { if (m.stack.empty()) { m.fail("stack underflow"); break; }
                          m.push(m.stack.back()); break; }
        case DW_OP_drop: { uint64_t a; m.pop(a); break; }
        case DW_OP_over: { if (m.stack.size() < 2) { m.fail("stack underflow"); break; }
                           m.push(m.stack[m.stack.size() - 2]); break; }
        case DW_OP_pick: { if (m.stack.size() <= o0) { m.fail("stack underflow"); break; }
                           m.push(m.stack[m.stack.size() - 1 - o0]); break; }
        case DW_OP_swap: { uint64_t a, b; if (!m.pop(a) || !m.pop(b)) break;
                           m.push(a); m.push(b); break; }
        case DW_OP_rot:  { if (m.stack.size() < 3) { m.fail("stack underflow"); break; }
                           uint64_t a = m.stack[m.stack.size()-1];
                           uint64_t b = m.stack[m.stack.size()-2];
                           uint64_t c = m.stack[m.stack.size()-3];
                           m.stack[m.stack.size()-1] = b;
                           m.stack[m.stack.size()-2] = c;
                           m.stack[m.stack.size()-3] = a; break; }

        case DW_OP_deref: case DW_OP_deref_size: {
            uint64_t addr;
            if (!m.pop(addr)) break;
            size_t n = code == DW_OP_deref ? 8 : (size_t)o0;
            if (n == 0 || n > 8) { m.fail("bad deref size"); break; }
            uint64_t v = 0;
            if (!ctx.read_mem || !ctx.read_mem(addr, &v, n)) {
                m.fail(format("cannot read %zu bytes at 0x%llx", n, (unsigned long long)addr));
                break;
            }
            m.push(v);
            break;
        }

        case DW_OP_plus:  { uint64_t a, b; if (!m.pop(a) || !m.pop(b)) break; m.push(b + a); break; }
        case DW_OP_minus: { uint64_t a, b; if (!m.pop(a) || !m.pop(b)) break; m.push(b - a); break; }
        case DW_OP_mul:   { uint64_t a, b; if (!m.pop(a) || !m.pop(b)) break; m.push(b * a); break; }
        case DW_OP_div:   { uint64_t a, b; if (!m.pop(a) || !m.pop(b)) break;
                            if (!a) { m.fail("division by zero"); break; }
                            m.push((uint64_t)((int64_t)b / (int64_t)a)); break; }
        case DW_OP_mod:   { uint64_t a, b; if (!m.pop(a) || !m.pop(b)) break;
                            if (!a) { m.fail("division by zero"); break; }
                            m.push(b % a); break; }
        case DW_OP_and:   { uint64_t a, b; if (!m.pop(a) || !m.pop(b)) break; m.push(b & a); break; }
        case DW_OP_or:    { uint64_t a, b; if (!m.pop(a) || !m.pop(b)) break; m.push(b | a); break; }
        case DW_OP_xor:   { uint64_t a, b; if (!m.pop(a) || !m.pop(b)) break; m.push(b ^ a); break; }
        case DW_OP_shl:   { uint64_t a, b; if (!m.pop(a) || !m.pop(b)) break; m.push(a > 63 ? 0 : b << a); break; }
        case DW_OP_shr:   { uint64_t a, b; if (!m.pop(a) || !m.pop(b)) break; m.push(a > 63 ? 0 : b >> a); break; }
        case DW_OP_shra:  { uint64_t a, b; if (!m.pop(a) || !m.pop(b)) break;
                            m.push((uint64_t)(a > 63 ? ((int64_t)b < 0 ? -1 : 0) : ((int64_t)b >> a))); break; }
        case DW_OP_neg:   { uint64_t a; if (!m.pop(a)) break; m.push((uint64_t)(-(int64_t)a)); break; }
        case DW_OP_not:   { uint64_t a; if (!m.pop(a)) break; m.push(~a); break; }
        case DW_OP_abs:   { uint64_t a; if (!m.pop(a)) break;
                            int64_t s = (int64_t)a; m.push((uint64_t)(s < 0 ? -s : s)); break; }
        case DW_OP_plus_uconst: { uint64_t a; if (!m.pop(a)) break; m.push(a + o0); break; }

        case DW_OP_eq: case DW_OP_ne: case DW_OP_lt:
        case DW_OP_gt: case DW_OP_le: case DW_OP_ge: {
            uint64_t a, b;
            if (!m.pop(a) || !m.pop(b)) break;
            int64_t x = (int64_t)b, y = (int64_t)a;
            bool r = code == DW_OP_eq ? x == y : code == DW_OP_ne ? x != y
                   : code == DW_OP_lt ? x <  y : code == DW_OP_gt ? x >  y
                   : code == DW_OP_le ? x <= y : x >= y;
            m.push(r ? 1 : 0);
            break;
        }

        case DW_OP_stack_value: {
            uint64_t v;
            if (!m.pop(v)) break;
            m.result.kind = Location::Kind::Value;
            m.result.value = v;
            m.stopped = true;
            break;
        }

        case DW_OP_implicit_value: {
            /* LLVM hands us the length; the payload follows in the raw stream. We
             * re-extract it directly because the Operation API does not expose it. */
            m.result.kind = Location::Kind::Implicit;
            size_t len = (size_t)o0;
            size_t end = (size_t)op.getEndOffset();
            if (end >= len && end <= expr.size())
                m.result.bytes.assign(expr.begin() + (end - len), expr.begin() + end);
            m.stopped = true;
            break;
        }

        case DW_OP_nop:
            break;

        case DW_OP_piece: case DW_OP_bit_piece:
            /* Composite locations: we take the first piece, which is what every
             * location we have observed pocl emit actually needs. */
            m.stopped = true;
            break;

        case DW_OP_call_frame_cfa:
            m.fail("DW_OP_call_frame_cfa is not supported (kernel objects use "
                   "DW_AT_frame_base = register, not CFA)");
            break;

        default:
            m.fail(format("unsupported DWARF operation 0x%02x", code));
            break;
        }
    }

    if (m.result.kind == Location::Kind::None && !m.result.error.empty()) return m.result;

    if (m.result.kind == Location::Kind::None) {
        if (m.stack.empty()) {
            m.result.error = "location expression produced no result";
            return m.result;
        }
        m.result.kind = Location::Kind::Memory;
        m.result.address = m.stack.back();
    }
    return m.result;
}

std::string describe_dwarf_expr(const std::vector<uint8_t> &expr)
{
    if (expr.empty()) return "<empty>";
    std::string out;
    StringRef data((const char *)expr.data(), expr.size());
    DataExtractor de(data, true, 8);
    DWARFExpression dex(de, 8, DWARF32);
    for (const DWARFExpression::Operation &op : dex) {
        if (op.isError()) { out += " <malformed>"; break; }
        if (!out.empty()) out += ", ";
        uint16_t code = op.getCode();
        const char *name = OperationEncodingString(code).data();
        out += name ? name : format("0x%02x", code);
        if (code >= DW_OP_reg0 && code <= DW_OP_reg31)
            out += format(" [%s]", RegisterFile::reg_name(code - DW_OP_reg0));
        else if (code >= DW_OP_breg0 && code <= DW_OP_breg31)
            out += format(" [%s%+lld]", RegisterFile::reg_name(code - DW_OP_breg0),
                          (long long)(int64_t)op.getRawOperand(0));
        else if (op.getNumOperands() > 0)
            out += format(" %lld", (long long)op.getRawOperand(0));
    }
    return out;
}

} // namespace oclgdb
