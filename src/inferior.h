#pragma once

/* Process control for the debuggee.
 *
 * This is the layer that makes halting *real*: we fork, the child asks to be traced,
 * and from then on every stop is a genuine kernel-delivered ptrace stop of a genuine
 * host thread executing pocl's JIT-compiled-to-.so kernel code. Nothing is simulated.
 */

#include <cstdint>
#include <string>
#include <sys/user.h>
#include <vector>

namespace oclgdb {

struct MapEntry {
    uint64_t    start = 0;
    uint64_t    end = 0;
    bool        exec = false;
    std::string path;
};

class Inferior {
public:
    enum class StopKind { Trap, Signal, Exited, Error };

    struct Stop {
        StopKind kind = StopKind::Error;
        uint64_t pc = 0;        /* RIP at the stop (not yet adjusted for INT3) */
        int      signo = 0;     /* for Signal */
        int      exit_code = 0; /* for Exited */
        std::string message;
    };

    ~Inferior();

    /* fork + PTRACE_TRACEME + execvp. `env` entries are "K=V" and are merged over
     * the debugger's own environment. Returns false and fills `err` on failure. */
    bool start(const std::string &exe,
               const std::vector<std::string> &args,
               const std::vector<std::string> &env,
               std::string &err);

    void kill();
    bool alive() const { return pid_ > 0 && !exited_; }
    pid_t pid() const { return pid_; }
    int exit_code() const { return exit_code_; }

    Stop resume(int deliver_signal = 0);
    Stop single_step(int deliver_signal = 0);

    bool get_regs(user_regs_struct &out) const;
    bool set_regs(const user_regs_struct &in) const;
    bool get_fpregs(user_fpregs_struct &out) const;

    bool read_mem(uint64_t addr, void *buf, size_t len) const;
    bool write_mem(uint64_t addr, const void *buf, size_t len) const;

    /* Convenience: read a NUL-terminated string out of the inferior. */
    bool read_cstr(uint64_t addr, std::string &out, size_t limit = 4096) const;

    std::vector<MapEntry> read_maps() const;

    /* AT_* entries from /proc/<pid>/auxv. Returns false if `type` is absent. */
    bool auxv(uint64_t type, uint64_t &value) const;

private:
    Stop wait_stop();

    pid_t pid_ = -1;
    bool  exited_ = false;
    int   exit_code_ = 0;
    int   mem_fd_ = -1;
};

} // namespace oclgdb
