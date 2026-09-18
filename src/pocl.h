#pragma once

/* Everything oclgdb knows specifically about pocl.
 *
 * Deliberately small and deliberately isolated: the rest of the debugger works in terms
 * of "an ELF object with DWARF, loaded at a bias, containing a function that runs a
 * work-group". Only this file knows how pocl names things, where it caches them, and
 * what its work-group calling convention is. Porting to another software OpenCL
 * backend -- or to a GPU simulator -- means rewriting this file, not the debugger.
 */

#include <cstdint>
#include <string>
#include <sys/user.h>
#include <vector>

namespace oclgdb {

class Inferior;

namespace pocl {

/* The two symbols pocl exports from a compiled kernel object. */
std::string kernel_symbol(const std::string &kernel_name);
std::string workgroup_symbol(const std::string &kernel_name);

/* A kernel object found in pocl's on-disk cache. */
struct CachedKernel {
    std::string path;
    std::string kernel_name;
    size_t      local[3] = {1, 1, 1};
};

/* Scan a POCL_CACHE_DIR for objects belonging to `kernel_name`. The newest match whose
 * work-group size equals `local` is returned first. */
std::vector<CachedKernel> scan_cache(const std::string &cache_dir,
                                     const std::string &kernel_name);

/* Parse pocl's work-group directory name, e.g. "64-1-1-goffs0-smallgrid". */
bool parse_wg_dirname(const std::string &name, size_t local[3]);

/* Arguments of _pocl_kernel_<k>_workgroup at its entry, per the SysV x86-64 ABI.
 * See F6 in docs/PHASE0-FINDINGS.md for how this was established. */
struct WorkgroupCall {
    uint64_t args_ptr = 0;      /* RDI -- void **, one entry per kernel argument */
    uint64_t context_ptr = 0;   /* RSI -- struct pocl_context *                  */
    size_t   group[3] = {0, 0, 0};
};

WorkgroupCall read_workgroup_call(const user_regs_struct &regs);

/* Read the i-th kernel argument slot. For a __global T* argument this yields the
 * buffer's address in the host process, which is what makes global-memory inspection
 * possible. Returns false if the indirection could not be followed. */
bool read_arg_pointer(const Inferior &inf, const WorkgroupCall &call,
                      unsigned index, uint64_t &out);

} // namespace pocl
} // namespace oclgdb
