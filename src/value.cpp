#include "value.h"
#include "dwarf_index.h"
#include "util.h"

#include <cmath>
#include <cstring>
#include <llvm/BinaryFormat/Dwarf.h>

using namespace llvm;
using namespace llvm::dwarf;

namespace oclgdb {

bool Value::as_uint(uint64_t &out) const
{
    if (!available || bytes.empty() || bytes.size() > 8) return false;
    uint64_t v = 0;
    memcpy(&v, bytes.data(), bytes.size());
    out = v;
    return true;
}

bool Value::as_int(int64_t &out) const
{
    uint64_t u = 0;
    if (!as_uint(u)) return false;
    size_t n = bytes.size();
    if (n < 8) {
        uint64_t sign = 1ull << (n * 8 - 1);
        unsigned enc = type_encoding(type);
        bool is_signed = enc == DW_ATE_signed || enc == DW_ATE_signed_char;
        if (is_signed && (u & sign)) u |= ~((sign << 1) - 1);
    }
    out = (int64_t)u;
    return true;
}

bool Value::as_double(double &out) const
{
    if (!available) return false;
    if (synthetic_float) { memcpy(&out, bytes.data(), 8); return true; }
    unsigned enc = type_encoding(type);
    if (enc == DW_ATE_float) {
        if (bytes.size() == 4) { float f; memcpy(&f, bytes.data(), 4); out = f; return true; }
        if (bytes.size() == 8) { double d; memcpy(&d, bytes.data(), 8); out = d; return true; }
        return false;
    }
    int64_t i = 0;
    if (!as_int(i)) return false;
    out = (double)i;
    return true;
}

Value read_value(DWARFDie type, const Location &loc, const EvalContext &ctx)
{
    Value v;
    v.type = type;
    v.loc = loc;

    if (!loc.ok()) {
        v.error = loc.error.empty() ? "optimized out" : loc.error;
        return v;
    }

    uint64_t size = type_size(type);
    if (size == 0) size = 8;
    if (size > 4096) size = 4096;

    switch (loc.kind) {
    case Location::Kind::Memory:
        v.bytes.resize(size);
        if (!ctx.read_mem || !ctx.read_mem(loc.address, v.bytes.data(), size)) {
            v.bytes.clear();
            v.error = format("cannot read %llu bytes at 0x%llx",
                             (unsigned long long)size, (unsigned long long)loc.address);
            return v;
        }
        v.has_address = true;
        v.address = loc.address;
        break;

    case Location::Kind::Register: {
        if (!ctx.regs) { v.error = "no register file"; return v; }
        v.bytes.resize(size <= 16 ? size : 16);
        if (!ctx.regs->read_bytes(loc.reg, v.bytes.data(), v.bytes.size())) {
            v.bytes.clear();
            v.error = format("cannot read register %s", RegisterFile::reg_name(loc.reg));
            return v;
        }
        break;
    }

    case Location::Kind::Value:
        v.bytes.resize(size <= 8 ? size : 8);
        memcpy(v.bytes.data(), &loc.value, v.bytes.size());
        break;

    case Location::Kind::Implicit:
        v.bytes = loc.bytes;
        if (v.bytes.size() > size) v.bytes.resize(size);
        break;

    default:
        v.error = "optimized out";
        return v;
    }

    v.available = true;
    return v;
}

Value make_int_value(int64_t x, DWARFDie int_type)
{
    Value v;
    v.type = int_type;
    v.available = true;
    uint64_t sz = int_type ? type_size(int_type) : 8;
    if (sz == 0 || sz > 8) sz = 8;
    v.bytes.resize(sz);
    memcpy(v.bytes.data(), &x, sz);
    return v;
}

/* ------------------------------------------------------------- formatting */

static std::string format_scalar(const Value &v)
{
    if (v.synthetic_float) {
        double d = 0;
        memcpy(&d, v.bytes.data(), 8);
        return format("%g", d);
    }
    unsigned enc = type_encoding(v.type);
    DWARFDie s = strip_typedefs(v.type);

    if (s && s.getTag() == DW_TAG_pointer_type) {
        uint64_t p = 0;
        if (!v.as_uint(p)) return "<?>";
        return p ? format("0x%llx", (unsigned long long)p) : std::string("0x0");
    }
    if (s && s.getTag() == DW_TAG_enumeration_type) {
        int64_t i = 0;
        if (!v.as_int(i)) return "<?>";
        for (DWARFDie c : s.children()) {
            if (c.getTag() != DW_TAG_enumerator) continue;
            if (auto cv = dwarf::toSigned(c.find(DW_AT_const_value)))
                if (*cv == i)
                    if (auto n = dwarf::toString(c.find(DW_AT_name)))
                        return format("%s (%lld)", *n, (long long)i);
        }
        return format("%lld", (long long)i);
    }

    switch (enc) {
    case DW_ATE_float: {
        double d = 0;
        if (!v.as_double(d)) return "<?>";
        if (std::isnan(d)) return "nan";
        if (std::isinf(d)) return d < 0 ? "-inf" : "inf";
        return format("%g", d);
    }
    case DW_ATE_boolean: {
        uint64_t u = 0;
        v.as_uint(u);
        return u ? "true" : "false";
    }
    case DW_ATE_signed_char: case DW_ATE_unsigned_char: {
        int64_t i = 0;
        v.as_int(i);
        char c = (char)i;
        if (c >= 32 && c < 127) return format("%lld '%c'", (long long)i, c);
        return format("%lld", (long long)i);
    }
    case DW_ATE_unsigned: {
        uint64_t u = 0;
        if (!v.as_uint(u)) return "<?>";
        return format("%llu", (unsigned long long)u);
    }
    case DW_ATE_signed:
    default: {
        int64_t i = 0;
        if (!v.as_int(i)) {
            uint64_t u = 0;
            if (v.as_uint(u)) return format("%llu", (unsigned long long)u);
            return "<?>";
        }
        return format("%lld", (long long)i);
    }
    }
}

std::string format_value(const Value &v, const EvalContext &ctx, unsigned max_elems)
{
    if (!v.available)
        return format("%s<%s>%s", color::dim(),
                      v.error.empty() ? "optimized out" : v.error.c_str(), color::reset());

    DWARFDie s = strip_typedefs(v.type);

    if (s && s.getTag() == DW_TAG_array_type) {
        DWARFDie el = type_pointee(s);
        uint64_t esz = type_size(el);
        uint64_t n = array_count(s);
        if (!esz) return "<array of unknown element size>";
        uint64_t shown = std::min<uint64_t>(n ? n : max_elems, max_elems);
        std::string out = "{";
        for (uint64_t i = 0; i < shown; i++) {
            if (i) out += ", ";
            Value ev;
            ev.type = el;
            if ((i + 1) * esz <= v.bytes.size()) {
                ev.bytes.assign(v.bytes.begin() + i * esz, v.bytes.begin() + (i + 1) * esz);
                ev.available = true;
            } else if (v.has_address) {
                ev.bytes.resize(esz);
                ev.available = ctx.read_mem && ctx.read_mem(v.address + i * esz,
                                                            ev.bytes.data(), esz);
            }
            out += ev.available ? format_scalar(ev) : "<?>";
        }
        if (n > shown) out += format(", ... /* %llu total */", (unsigned long long)n);
        out += "}";
        return out;
    }

    if (s && (s.getTag() == DW_TAG_structure_type || s.getTag() == DW_TAG_union_type)) {
        std::string out = "{";
        bool first = true;
        for (DWARFDie m : s.children()) {
            if (m.getTag() != DW_TAG_member) continue;
            auto off = dwarf::toUnsigned(m.find(DW_AT_data_member_location));
            DWARFDie mt = m.getAttributeValueAsReferencedDie(DW_AT_type);
            uint64_t msz = type_size(mt);
            if (!off || !msz) continue;
            if (!first) out += ", ";
            first = false;
            if (auto n = dwarf::toString(m.find(DW_AT_name))) out += format("%s = ", *n);
            Value mv;
            mv.type = mt;
            if (*off + msz <= v.bytes.size()) {
                mv.bytes.assign(v.bytes.begin() + *off, v.bytes.begin() + *off + msz);
                mv.available = true;
            }
            out += mv.available ? format_scalar(mv) : "<?>";
        }
        out += "}";
        return out;
    }

    return format_scalar(v);
}

std::string format_value_brief(const Value &v, const EvalContext &ctx)
{
    if (!v.available) return "<opt>";
    std::string s = format_value(v, ctx, 3);
    if (s.size() > 24) s = s.substr(0, 21) + "...";
    return s;
}

} // namespace oclgdb
