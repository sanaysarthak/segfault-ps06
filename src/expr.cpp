#include "expr.h"
#include "dwarf_index.h"
#include "util.h"

#include <cctype>
#include <cmath>
#include <cstring>
#include <llvm/BinaryFormat/Dwarf.h>

using namespace llvm;
using namespace llvm::dwarf;

namespace oclgdb {

namespace {

struct Parser {
    const std::string &src;
    const ExprEnv     &env;
    size_t             pos = 0;
    std::string        err;

    Parser(const std::string &s, const ExprEnv &e) : src(s), env(e) {}

    void skip() { while (pos < src.size() && std::isspace((unsigned char)src[pos])) pos++; }
    bool eof()  { skip(); return pos >= src.size(); }
    char peek() { skip(); return pos < src.size() ? src[pos] : '\0'; }

    bool accept(const char *tok)
    {
        skip();
        size_t n = strlen(tok);
        if (src.compare(pos, n, tok) != 0) return false;
        /* do not let "<" swallow the "<" of "<<" etc. */
        pos += n;
        return true;
    }

    bool fail(const std::string &m) { if (err.empty()) err = m; return false; }

    /* ---------------------------------------------------------- numeric glue */

    static bool is_float(const Value &v) { return type_encoding(v.type) == DW_ATE_float; }

    bool to_double(const Value &v, double &d)
    {
        if (!v.available) return fail(v.error.empty() ? "value is not available" : v.error);
        if (!v.as_double(d)) return fail("value is not numeric");
        return true;
    }

    bool to_int(const Value &v, int64_t &i)
    {
        if (!v.available) return fail(v.error.empty() ? "value is not available" : v.error);
        if (type_encoding(v.type) == DW_ATE_float) {
            double d = 0;
            if (!v.as_double(d)) return fail("value is not numeric");
            i = (int64_t)d;
            return true;
        }
        if (v.as_int(i)) return true;
        uint64_t u = 0;
        if (v.as_uint(u)) { i = (int64_t)u; return true; }
        return fail("value is not an integer");
    }

    /* Arithmetic results have no DWARF type of their own, so they become either a
     * plain integer of the unit's int type or a synthetic double. */
    Value from_double(double d)
    {
        Value v;
        v.available = true;
        v.bytes.resize(8);
        memcpy(v.bytes.data(), &d, 8);
        v.synthetic_float = true;
        return v;
    }

    static bool is_synth_double(const Value &v) { return v.available && v.synthetic_float; }

    bool numeric(const Value &v, double &d)
    {
        if (is_synth_double(v)) { memcpy(&d, v.bytes.data(), 8); return true; }
        return to_double(v, d);
    }

    /* ------------------------------------------------------------- grammar */

    bool parse_primary(Value &out)
    {
        skip();
        if (eof()) return fail("unexpected end of expression");

        char c = src[pos];

        if (c == '(') {
            pos++;
            if (!parse_expr(out)) return false;
            if (!accept(")")) return fail("expected ')'");
            return true;
        }

        if (std::isdigit((unsigned char)c)) {
            size_t start = pos;
            while (pos < src.size() &&
                   (std::isalnum((unsigned char)src[pos]) || src[pos] == '.' ||
                    src[pos] == 'x' || src[pos] == 'X'))
                pos++;
            std::string tok = src.substr(start, pos - start);
            if (tok.find('.') != std::string::npos) {
                out = from_double(strtod(tok.c_str(), nullptr));
                return true;
            }
            int64_t v = 0;
            if (!parse_int(tok, v)) return fail(format("bad integer literal '%s'", tok.c_str()));
            out = make_int_value(v, env.int_type);
            return true;
        }

        if (std::isalpha((unsigned char)c) || c == '_') {
            size_t start = pos;
            while (pos < src.size() &&
                   (std::isalnum((unsigned char)src[pos]) || src[pos] == '_'))
                pos++;
            std::string name = src.substr(start, pos - start);
            std::string lerr;
            if (!env.lookup || !env.lookup(name, out, lerr))
                return fail(lerr.empty() ? format("no symbol '%s' in this context", name.c_str())
                                         : lerr);
            return true;
        }

        return fail(format("unexpected character '%c'", c));
    }

    bool index_into(Value &base, int64_t idx)
    {
        DWARFDie s = strip_typedefs(base.type);
        if (!s) return fail("cannot subscript a value of unknown type");

        DWARFDie el = type_pointee(s);
        if (!el) return fail(format("cannot subscript a value of type '%s'",
                                    type_name(base.type).c_str()));
        uint64_t esz = type_size(el);
        if (!esz) return fail("element type has unknown size");

        uint64_t addr = 0;
        if (s.getTag() == DW_TAG_pointer_type) {
            if (!base.available)
                return fail(base.error.empty() ? "pointer is not available" : base.error);
            if (!base.as_uint(addr)) return fail("cannot read pointer value");
        } else if (s.getTag() == DW_TAG_array_type) {
            if (!base.has_address) return fail("array is not addressable here");
            addr = base.address;
        } else {
            return fail("value is not subscriptable");
        }

        Location loc;
        loc.kind = Location::Kind::Memory;
        loc.address = addr + (uint64_t)((int64_t)esz * idx);
        base = read_value(el, loc, env.eval);
        return true;
    }

    bool parse_postfix(Value &out)
    {
        if (!parse_primary(out)) return false;
        for (;;) {
            skip();
            if (pos < src.size() && src[pos] == '[') {
                pos++;
                Value iv;
                if (!parse_expr(iv)) return false;
                if (!accept("]")) return fail("expected ']'");
                int64_t idx = 0;
                if (!to_int(iv, idx)) return false;
                if (!index_into(out, idx)) return false;
                continue;
            }
            if (pos + 1 < src.size() && src[pos] == '-' && src[pos + 1] == '>') {
                pos += 2;
                std::string field;
                if (!parse_ident(field)) return false;
                if (!deref(out)) return false;
                if (!member(out, field)) return false;
                continue;
            }
            if (pos < src.size() && src[pos] == '.') {
                pos++;
                std::string field;
                if (!parse_ident(field)) return false;
                if (!member(out, field)) return false;
                continue;
            }
            return true;
        }
    }

    bool parse_ident(std::string &out)
    {
        skip();
        size_t start = pos;
        while (pos < src.size() && (std::isalnum((unsigned char)src[pos]) || src[pos] == '_'))
            pos++;
        if (start == pos) return fail("expected a field name");
        out = src.substr(start, pos - start);
        return true;
    }

    bool member(Value &v, const std::string &field)
    {
        DWARFDie s = strip_typedefs(v.type);
        if (!s || (s.getTag() != DW_TAG_structure_type && s.getTag() != DW_TAG_union_type))
            return fail(format("'%s' is not a structure", type_name(v.type).c_str()));
        for (DWARFDie m : s.children()) {
            if (m.getTag() != DW_TAG_member) continue;
            auto n = dwarf::toString(m.find(DW_AT_name));
            if (!n || field != *n) continue;
            auto off = dwarf::toUnsigned(m.find(DW_AT_data_member_location));
            DWARFDie mt = m.getAttributeValueAsReferencedDie(DW_AT_type);
            if (!off) return fail("member has no offset");
            if (v.has_address) {
                Location loc;
                loc.kind = Location::Kind::Memory;
                loc.address = v.address + *off;
                v = read_value(mt, loc, env.eval);
            } else {
                uint64_t msz = type_size(mt);
                if (*off + msz > v.bytes.size()) return fail("member is out of range");
                Value nv;
                nv.type = mt;
                nv.bytes.assign(v.bytes.begin() + *off, v.bytes.begin() + *off + msz);
                nv.available = true;
                v = nv;
            }
            return true;
        }
        return fail(format("no member named '%s'", field.c_str()));
    }

    bool deref(Value &v)
    {
        DWARFDie s = strip_typedefs(v.type);
        DWARFDie el = type_pointee(s);
        if (!el) return fail(format("cannot dereference a value of type '%s'",
                                    type_name(v.type).c_str()));
        uint64_t addr = 0;
        if (s && s.getTag() == DW_TAG_array_type) {
            if (!v.has_address) return fail("array is not addressable here");
            addr = v.address;
        } else {
            if (!v.available) return fail(v.error.empty() ? "pointer is not available" : v.error);
            if (!v.as_uint(addr)) return fail("cannot read pointer value");
        }
        Location loc;
        loc.kind = Location::Kind::Memory;
        loc.address = addr;
        v = read_value(el, loc, env.eval);
        return true;
    }

    bool parse_unary(Value &out)
    {
        skip();
        if (pos < src.size()) {
            char c = src[pos];
            if (c == '*') { pos++; if (!parse_unary(out)) return false; return deref(out); }
            if (c == '&') {
                pos++;
                if (!parse_unary(out)) return false;
                if (!out.has_address)
                    return fail("cannot take the address of a value held in a register "
                                "(pocl keeps kernel locals in registers -- see 'info location')");
                out = make_int_value((int64_t)out.address, env.int_type);
                return true;
            }
            if (c == '-') {
                pos++;
                if (!parse_unary(out)) return false;
                double d = 0;
                if (!numeric(out, d)) return false;
                out = from_double(-d);
                return true;
            }
            if (c == '!') {
                pos++;
                if (!parse_unary(out)) return false;
                double d = 0;
                if (!numeric(out, d)) return false;
                out = make_int_value(d == 0 ? 1 : 0, env.int_type);
                return true;
            }
            if (c == '~') {
                pos++;
                if (!parse_unary(out)) return false;
                int64_t i = 0;
                if (!to_int(out, i)) return false;
                out = make_int_value(~i, env.int_type);
                return true;
            }
        }
        return parse_postfix(out);
    }

    /* precedence: * / %  >  + -  >  << >>  >  < <= > >=  >  == != */
    bool parse_binary(Value &out, int level)
    {
        static const char *const ops[][5] = {
            { "*", "/", "%", nullptr, nullptr },
            { "+", "-", nullptr, nullptr, nullptr },
            { "<<", ">>", nullptr, nullptr, nullptr },
            { "<=", ">=", "<", ">", nullptr },
            { "==", "!=", nullptr, nullptr, nullptr },
        };
        const int levels = 5;
        if (level < 0) return parse_unary(out);
        if (!parse_binary(out, level - 1)) return false;

        for (;;) {
            skip();
            const char *matched = nullptr;
            for (int i = 0; i < 5 && ops[level][i]; i++) {
                const char *o = ops[level][i];
                size_t n = strlen(o);
                if (src.compare(pos, n, o) != 0) continue;
                /* "<" must not match the first half of "<<" */
                if (n == 1 && pos + 1 < src.size() &&
                    ((o[0] == '<' && src[pos + 1] == '<') ||
                     (o[0] == '>' && src[pos + 1] == '>') ||
                     src[pos + 1] == '='))
                    continue;
                matched = o;
                pos += n;
                break;
            }
            if (!matched) return true;

            Value rhs;
            if (!parse_binary(rhs, level - 1)) return false;

            double a = 0, b = 0;
            if (!numeric(out, a) || !numeric(rhs, b)) return false;
            bool ints = !is_synth_double(out) && !is_synth_double(rhs) &&
                        type_encoding(out.type) != DW_ATE_float &&
                        type_encoding(rhs.type) != DW_ATE_float;

            std::string op = matched;
            double r = 0;
            if (op == "*") r = a * b;
            else if (op == "/") { if (b == 0) return fail("division by zero"); r = ints ? (double)((int64_t)a / (int64_t)b) : a / b; }
            else if (op == "%") { if ((int64_t)b == 0) return fail("division by zero"); r = (double)((int64_t)a % (int64_t)b); }
            else if (op == "+") r = a + b;
            else if (op == "-") r = a - b;
            else if (op == "<<") r = (double)((int64_t)a << (int64_t)b);
            else if (op == ">>") r = (double)((int64_t)a >> (int64_t)b);
            else if (op == "<")  r = a <  b;
            else if (op == ">")  r = a >  b;
            else if (op == "<=") r = a <= b;
            else if (op == ">=") r = a >= b;
            else if (op == "==") r = a == b;
            else if (op == "!=") r = a != b;

            bool cmp = op == "<" || op == ">" || op == "<=" || op == ">=" ||
                       op == "==" || op == "!=";
            out = (ints || cmp) ? make_int_value((int64_t)r, env.int_type) : from_double(r);
        }
    }

    bool parse_expr(Value &out) { return parse_binary(out, 4); }
};

} // namespace

bool eval_expression(const std::string &src, const ExprEnv &env, Value &out, std::string &err)
{
    Parser p(src, env);
    if (!p.parse_expr(out)) { err = p.err; return false; }
    if (!p.eof()) { err = format("trailing input at '%s'", src.c_str() + p.pos); return false; }
    return true;
}

} // namespace oclgdb
