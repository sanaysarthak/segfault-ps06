#include "util.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace oclgdb {

std::string format(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);
    std::string out;
    if (n > 0) {
        out.resize((size_t)n);
        vsnprintf(&out[0], (size_t)n + 1, fmt, ap2);
    }
    va_end(ap2);
    return out;
}

std::vector<std::string> split(const std::string &s, char sep)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

std::vector<std::string> tokenize(const std::string &s)
{
    std::vector<std::string> out;
    std::istringstream is(s);
    std::string t;
    while (is >> t) out.push_back(t);
    return out;
}

std::string trim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

bool starts_with(const std::string &s, const std::string &p)
{
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

bool ends_with(const std::string &s, const std::string &p)
{
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

std::string basename_of(const std::string &path)
{
    size_t i = path.find_last_of('/');
    return i == std::string::npos ? path : path.substr(i + 1);
}

std::string dirname_of(const std::string &path)
{
    size_t i = path.find_last_of('/');
    return i == std::string::npos ? std::string(".") : path.substr(0, i);
}

bool parse_int(const std::string &s, int64_t &out)
{
    if (s.empty()) return false;
    errno = 0;
    char *end = nullptr;
    long long v = strtoll(s.c_str(), &end, 0);
    if (errno != 0 || end == s.c_str() || *end != 0) return false;
    out = v;
    return true;
}

bool read_file(const std::string &path, std::string &out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

namespace color {
bool enabled = true;
static const char *c(const char *s) { return enabled ? s : ""; }
const char *bold()   { return c("\033[1m"); }
const char *dim()    { return c("\033[2m"); }
const char *red()    { return c("\033[31m"); }
const char *green()  { return c("\033[32m"); }
const char *yellow() { return c("\033[33m"); }
const char *blue()   { return c("\033[34m"); }
const char *cyan()   { return c("\033[36m"); }
const char *reset()  { return c("\033[0m"); }
} // namespace color

} // namespace oclgdb
