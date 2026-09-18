#include "workitem.h"
#include "util.h"

#include <algorithm>
#include <set>

namespace oclgdb {

std::string WorkItemId::str() const
{
    return format("global (%zu,%zu,%zu)  local (%zu,%zu,%zu)  group (%zu,%zu,%zu)",
                  global[0], global[1], global[2],
                  local[0], local[1], local[2],
                  group[0], group[1], group[2]);
}

std::string WorkItemId::str_global() const
{
    return format("(%zu,%zu,%zu)", global[0], global[1], global[2]);
}

WorkItemId make_work_item(const NDRange &nd, const size_t group[3], size_t flat)
{
    WorkItemId id;
    id.group[0] = group[0];
    id.group[1] = group[1];
    id.group[2] = group[2];
    id.flat_local = flat;

    size_t lx = nd.local[0] ? nd.local[0] : 1;
    size_t ly = nd.local[1] ? nd.local[1] : 1;
    id.local[0] = flat % lx;
    id.local[1] = (flat / lx) % ly;
    id.local[2] = flat / (lx * ly);

    for (unsigned i = 0; i < 3; i++)
        id.global[i] = group[i] * nd.local[i] + id.local[i];
    return id;
}

std::string IdentityModel::describe() const
{
    switch (kind) {
    case Kind::ReplicaAddress:
        return format("replica-address (pocl replicated the body %u times; "
                      "each work-item has its own machine addresses)", replicas);
    case Kind::Register:
        return format("register %s (work-item loop; value = %lld %c %lld * local_id)",
                      RegisterFile::reg_name(reg), (long long)base,
                      scale < 0 ? '-' : '+', (long long)(scale < 0 ? -scale : scale));
    case Kind::Ordinal:
        return "arrival ordinal (fallback: n-th work-item to reach this line)";
    default:
        return "not yet calibrated";
    }
}

/* ------------------------------------------------------------- calibration */

/* The expected identity sequence for a whole work-group is 0,1,...,n-1, each id
 * appearing at least once and ids never going backwards -- both pocl work-group
 * transformations run work-items in order, and a source-level loop inside the kernel
 * only makes an id repeat, never regress. That is a strong enough signature to pick the
 * right candidate out of the machine state, and weak enough to survive loops. */
static bool sequence_is_valid(const std::vector<int64_t> &ids, size_t n)
{
    if (ids.empty()) return false;
    int64_t prev = -1;
    std::set<int64_t> seen;
    for (int64_t v : ids) {
        if (v < 0 || (size_t)v >= n) return false;
        if (v < prev) return false;
        prev = v;
        seen.insert(v);
    }
    /* Every work-item must have shown up: a candidate that only covers half the group
     * is describing something else (a loop counter, a pointer stride, ...). */
    return seen.size() == n;
}

IdentityModel calibrate_identity(const std::vector<CalibrationSample> &samples,
                                 size_t n, unsigned replica_count)
{
    IdentityModel model;
    if (samples.empty() || n == 0) return model;

    /* --- candidate 1: the trap address itself (pocl replication mode) --- */
    bool replica_ok = false;
    if (replica_count == n && n > 1) {
        std::vector<int64_t> ids;
        ids.reserve(samples.size());
        for (const auto &s : samples) ids.push_back(s.replica);
        replica_ok = sequence_is_valid(ids, n);
    }

    /* --- candidate 2: an affine function of a general-purpose register --- */
    IdentityModel reg_model;
    for (unsigned r = 0; r < 16 && !reg_model.calibrated(); r++) {
        std::vector<uint64_t> vals;
        vals.reserve(samples.size());
        bool readable = true;
        for (const auto &s : samples) {
            uint64_t v = 0;
            if (!s.regs.read(r, v)) { readable = false; break; }
            vals.push_back(v);
        }
        if (!readable || vals.size() < 2) continue;

        /* Derive base and scale from the first two *distinct* values. */
        int64_t base = (int64_t)vals.front();
        int64_t scale = 0;
        for (size_t i = 1; i < vals.size(); i++) {
            if ((int64_t)vals[i] != base) { scale = (int64_t)vals[i] - base; break; }
        }
        if (scale == 0) continue;

        std::vector<int64_t> ids;
        ids.reserve(vals.size());
        bool affine = true;
        for (uint64_t v : vals) {
            int64_t d = (int64_t)v - base;
            if (d % scale != 0) { affine = false; break; }
            ids.push_back(d / scale);
        }
        if (!affine || !sequence_is_valid(ids, n)) continue;

        reg_model.kind = IdentityModel::Kind::Register;
        reg_model.reg = r;
        reg_model.base = base;
        reg_model.scale = scale;
    }

    if (replica_ok) {
        model.kind = IdentityModel::Kind::ReplicaAddress;
        model.replicas = replica_count;
        if (reg_model.calibrated()) {
            /* Both agree on the whole group -- say so; it is a real, free consistency
             * check on the single riskiest assumption in the debugger. */
            model.cross_checked = true;
            model.note = format("cross-checked against register %s",
                                RegisterFile::reg_name(reg_model.reg));
        }
        return model;
    }
    if (reg_model.calibrated()) return reg_model;

    /* --- fallback: count distinct arrivals --- */
    model.kind = IdentityModel::Kind::Ordinal;
    model.note = "no structural or register signal found; identity is inferred from "
                 "arrival order, which is wrong if the line sits inside a kernel loop";
    return model;
}

bool identity_of(const IdentityModel &m, int replica, const RegisterFile &regs,
                 size_t ordinal, size_t n, size_t &out)
{
    switch (m.kind) {
    case IdentityModel::Kind::ReplicaAddress:
        if (replica < 0 || (size_t)replica >= n) return false;
        out = (size_t)replica;
        return true;
    case IdentityModel::Kind::Register: {
        uint64_t v = 0;
        if (!regs.read(m.reg, v)) return false;
        int64_t d = (int64_t)v - m.base;
        if (m.scale == 0 || d % m.scale != 0) return false;
        int64_t id = d / m.scale;
        if (id < 0 || (size_t)id >= n) return false;
        out = (size_t)id;
        return true;
    }
    case IdentityModel::Kind::Ordinal:
        if (ordinal >= n) return false;
        out = ordinal;
        return true;
    default:
        return false;
    }
}

} // namespace oclgdb
