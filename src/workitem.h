#pragma once

/* The GPU-semantics layer: work-items, work-groups, per-work-item state isolation, and
 * the machinery that works out *which* work-item a given trap belongs to.
 *
 * Phase 0 established (docs/PHASE0-FINDINGS.md, F4/F5) that pocl runs a work-group's
 * work-items through a single host stack frame, either by replicating the body once per
 * work-item or by looping over it, and that a work-item's variables live in registers
 * with short live ranges. Two consequences shape everything here:
 *
 *   1. Work-item identity is not stored anywhere we can just read. We recover it by
 *      *calibrating* against the first work-group we observe (see IdentityModel).
 *   2. A work-item's state is only readable while that work-item is executing, so we
 *      snapshot it at its own trap. WorkItemSnapshot is that snapshot, and it is what
 *      makes `select-work-item` answer correctly for work-items that have already run.
 */

#include "dwarf_expr.h"
#include "value.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace oclgdb {

struct NDRange {
    unsigned dim = 1;
    size_t   global[3] = {1, 1, 1};
    size_t   local[3] = {1, 1, 1};

    size_t local_size() const  { return local[0] * local[1] * local[2]; }
    size_t groups(unsigned i) const { return local[i] ? global[i] / local[i] : 1; }
    size_t num_groups() const  { return groups(0) * groups(1) * groups(2); }
    bool   contains_global(const size_t g[3]) const
    {
        for (unsigned i = 0; i < 3; i++) if (g[i] >= global[i]) return false;
        return true;
    }
};

struct WorkItemId {
    size_t group[3] = {0, 0, 0};
    size_t local[3] = {0, 0, 0};
    size_t global[3] = {0, 0, 0};
    size_t flat_local = 0;

    std::string str() const;
    std::string str_global() const;
    bool operator<(const WorkItemId &o) const
    {
        for (unsigned i = 0; i < 3; i++)
            if (global[i] != o.global[i]) return global[i] < o.global[i];
        return false;
    }
    bool operator==(const WorkItemId &o) const
    {
        return global[0] == o.global[0] && global[1] == o.global[1] && global[2] == o.global[2];
    }
};

/* Decompose a flat local index into (x, y, z), x fastest -- the ordering pocl's
 * work-group transformations use. */
WorkItemId make_work_item(const NDRange &nd, const size_t group[3], size_t flat_local);

/* Everything we captured about one work-item at one trap. This is the per-work-item
 * state isolation the brief asks for: each work-item gets its own object, with its own
 * register file and its own resolved source-level variables. */
struct WorkItemSnapshot {
    WorkItemId       id;
    uint64_t         pc = 0;          /* runtime address of the trap        */
    uint64_t         link_pc = 0;     /* same, minus the load bias          */
    unsigned         line = 0;
    RegisterFile     regs;
    int              replica = -1;    /* index among the line's sites, or -1 */
    size_t           visit = 0;       /* which time this work-item hit the line */
    std::map<std::string, Value> vars;
    bool             live = false;    /* true only for the work-item still halted */
};

/* --------------------------------------------------------- identity model */

/* How we decide which work-item a trap belongs to. Calibrated, not assumed. */
struct IdentityModel {
    enum class Kind {
        Unknown,
        ReplicaAddress,  /* pocl replicated the body: the trap address *is* the id   */
        Register,        /* pocl emitted a work-item loop: an affine register holds it */
        Ordinal          /* fallback: n-th distinct arrival at this line             */
    };

    Kind     kind = Kind::Unknown;
    unsigned reg = 0;                /* Kind::Register: DWARF register number    */
    int64_t  base = 0;               /* value = base + scale * flat_local        */
    int64_t  scale = 1;
    unsigned replicas = 0;           /* Kind::ReplicaAddress: site count         */
    bool     cross_checked = false;  /* both candidates agreed                   */
    std::string note;

    bool calibrated() const { return kind != Kind::Unknown; }
    std::string describe() const;
};

/* One observation made during calibration. */
struct CalibrationSample {
    int          replica = -1;
    RegisterFile regs;
};

/* Given every trap observed during one complete work-group, work out the identity
 * model. `n` is the work-group size, `replica_count` the number of breakpoint sites the
 * source line resolved to. Returns a model with kind == Unknown if nothing fits. */
IdentityModel calibrate_identity(const std::vector<CalibrationSample> &samples,
                                 size_t n, unsigned replica_count);

/* Apply a calibrated model to one trap. Returns false if the model cannot explain it. */
bool identity_of(const IdentityModel &m, int replica, const RegisterFile &regs,
                 size_t ordinal, size_t n, size_t &flat_local_out);

} // namespace oclgdb
