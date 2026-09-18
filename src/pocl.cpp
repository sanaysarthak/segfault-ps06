#include "pocl.h"
#include "inferior.h"
#include "util.h"

#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

namespace oclgdb {
namespace pocl {

std::string kernel_symbol(const std::string &k)    { return "_pocl_kernel_" + k; }
std::string workgroup_symbol(const std::string &k) { return "_pocl_kernel_" + k + "_workgroup"; }

bool parse_wg_dirname(const std::string &name, size_t local[3])
{
    /* "<lx>-<ly>-<lz>-goffs<N>-<grid>" */
    unsigned long long a = 0, b = 0, c = 0;
    if (sscanf(name.c_str(), "%llu-%llu-%llu-", &a, &b, &c) != 3) return false;
    local[0] = (size_t)a;
    local[1] = (size_t)b;
    local[2] = (size_t)c;
    return true;
}

static void walk(const std::string &dir, const std::string &kernel_name,
                 std::vector<CachedKernel> &out, int depth)
{
    if (depth > 8) return;
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    while (struct dirent *e = readdir(d)) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        std::string full = dir + "/" + n;
        struct stat st;
        if (stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            walk(full, kernel_name, out, depth + 1);
        } else if (S_ISREG(st.st_mode) && n == kernel_name + ".so") {
            CachedKernel ck;
            ck.path = full;
            ck.kernel_name = kernel_name;
            parse_wg_dirname(basename_of(dirname_of(full)), ck.local);
            out.push_back(std::move(ck));
        }
    }
    closedir(d);
}

std::vector<CachedKernel> scan_cache(const std::string &cache_dir, const std::string &kernel_name)
{
    std::vector<CachedKernel> out;
    walk(cache_dir, kernel_name, out, 0);
    return out;
}

WorkgroupCall read_workgroup_call(const user_regs_struct &r)
{
    WorkgroupCall c;
    c.args_ptr    = r.rdi;
    c.context_ptr = r.rsi;
    c.group[0]    = (size_t)r.rdx;
    c.group[1]    = (size_t)r.rcx;
    c.group[2]    = (size_t)r.r8;
    return c;
}

bool read_arg_pointer(const Inferior &inf, const WorkgroupCall &call,
                      unsigned index, uint64_t &out)
{
    if (!call.args_ptr) return false;
    uint64_t slot = 0;
    if (!inf.read_mem(call.args_ptr + 8ull * index, &slot, 8)) return false;
    if (!slot) return false;
    /* args[i] points at the argument *value*; for a buffer that value is the pointer. */
    uint64_t ptr = 0;
    if (!inf.read_mem(slot, &ptr, 8)) return false;
    out = ptr;
    return true;
}

} // namespace pocl
} // namespace oclgdb
