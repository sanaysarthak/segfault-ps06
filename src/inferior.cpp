#include "inferior.h"
#include "util.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <sys/personality.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace oclgdb {

Inferior::~Inferior() { kill(); }

bool Inferior::start(const std::string &exe,
                     const std::vector<std::string> &args,
                     const std::vector<std::string> &env,
                     std::string &err)
{
    kill();
    exited_ = false;
    exit_code_ = 0;

    pid_t p = fork();
    if (p < 0) { err = format("fork: %s", strerror(errno)); return false; }

    if (p == 0) {
        /* Child. */
        if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) < 0) _exit(126);

        /* Disabling ASLR is not strictly required -- we resolve the kernel object's
         * load bias from the dynamic linker's link_map either way -- but it makes
         * successive runs of the same session directly comparable, which matters a
         * lot when you are eyeballing addresses across a demo. */
        personality(ADDR_NO_RANDOMIZE);

        for (const auto &kv : env) {
            auto eq = kv.find('=');
            if (eq == std::string::npos) continue;
            setenv(kv.substr(0, eq).c_str(), kv.substr(eq + 1).c_str(), 1);
        }

        std::vector<char *> argv;
        argv.push_back(const_cast<char *>(exe.c_str()));
        for (const auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);

        execvp(exe.c_str(), argv.data());
        _exit(127);
    }

    pid_ = p;

    /* Wait for the post-execvp SIGTRAP. */
    int status = 0;
    if (waitpid(pid_, &status, 0) < 0) {
        err = format("waitpid: %s", strerror(errno));
        pid_ = -1;
        return false;
    }
    if (WIFEXITED(status)) {
        exited_ = true;
        exit_code_ = WEXITSTATUS(status);
        err = exit_code_ == 127 ? format("could not exec '%s'", exe.c_str())
                                : format("inferior exited immediately (%d)", exit_code_);
        return false;
    }

    /* EXITKILL keeps us from leaking a stopped inferior if the debugger dies. */
    ptrace(PTRACE_SETOPTIONS, pid_, 0, PTRACE_O_EXITKILL);

    std::string mem = format("/proc/%d/mem", (int)pid_);
    mem_fd_ = open(mem.c_str(), O_RDWR);
    if (mem_fd_ < 0) {
        err = format("open %s: %s", mem.c_str(), strerror(errno));
        return false;
    }
    return true;
}

void Inferior::kill()
{
    if (mem_fd_ >= 0) { close(mem_fd_); mem_fd_ = -1; }
    if (pid_ > 0 && !exited_) {
        ptrace(PTRACE_KILL, pid_, 0, 0);
        ::kill(pid_, SIGKILL);
        int status = 0;
        waitpid(pid_, &status, 0);
    }
    pid_ = -1;
    exited_ = true;
}

Inferior::Stop Inferior::wait_stop()
{
    Stop s;
    int status = 0;
    if (waitpid(pid_, &status, 0) < 0) {
        s.kind = StopKind::Error;
        s.message = format("waitpid: %s", strerror(errno));
        return s;
    }
    if (WIFEXITED(status)) {
        exited_ = true;
        exit_code_ = WEXITSTATUS(status);
        s.kind = StopKind::Exited;
        s.exit_code = exit_code_;
        return s;
    }
    if (WIFSIGNALED(status)) {
        exited_ = true;
        s.kind = StopKind::Exited;
        s.exit_code = 128 + WTERMSIG(status);
        s.message = format("terminated by signal %d", WTERMSIG(status));
        return s;
    }

    int sig = WSTOPSIG(status);
    user_regs_struct regs{};
    if (get_regs(regs)) s.pc = regs.rip;
    if (sig == SIGTRAP) {
        s.kind = StopKind::Trap;
    } else {
        s.kind = StopKind::Signal;
        s.signo = sig;
    }
    return s;
}

Inferior::Stop Inferior::resume(int deliver_signal)
{
    if (!alive()) { Stop s; s.kind = StopKind::Error; s.message = "no inferior"; return s; }
    if (ptrace(PTRACE_CONT, pid_, 0, (void *)(intptr_t)deliver_signal) < 0) {
        Stop s; s.kind = StopKind::Error;
        s.message = format("PTRACE_CONT: %s", strerror(errno));
        return s;
    }
    return wait_stop();
}

Inferior::Stop Inferior::single_step(int deliver_signal)
{
    if (!alive()) { Stop s; s.kind = StopKind::Error; s.message = "no inferior"; return s; }
    if (ptrace(PTRACE_SINGLESTEP, pid_, 0, (void *)(intptr_t)deliver_signal) < 0) {
        Stop s; s.kind = StopKind::Error;
        s.message = format("PTRACE_SINGLESTEP: %s", strerror(errno));
        return s;
    }
    return wait_stop();
}

bool Inferior::get_regs(user_regs_struct &out) const
{
    return pid_ > 0 && ptrace(PTRACE_GETREGS, pid_, nullptr, &out) == 0;
}

bool Inferior::set_regs(const user_regs_struct &in) const
{
    return pid_ > 0 && ptrace(PTRACE_SETREGS, pid_, nullptr, (void *)&in) == 0;
}

bool Inferior::get_fpregs(user_fpregs_struct &out) const
{
    return pid_ > 0 && ptrace(PTRACE_GETFPREGS, pid_, nullptr, &out) == 0;
}

bool Inferior::read_mem(uint64_t addr, void *buf, size_t len) const
{
    if (mem_fd_ < 0) return false;
    size_t done = 0;
    while (done < len) {
        ssize_t n = pread(mem_fd_, (char *)buf + done, len - done, (off_t)(addr + done));
        if (n <= 0) return false;
        done += (size_t)n;
    }
    return true;
}

bool Inferior::write_mem(uint64_t addr, const void *buf, size_t len) const
{
    if (mem_fd_ < 0) return false;
    size_t done = 0;
    while (done < len) {
        ssize_t n = pwrite(mem_fd_, (const char *)buf + done, len - done, (off_t)(addr + done));
        if (n <= 0) {
            /* /proc/pid/mem writes can be refused for some mappings; fall back to
             * PTRACE_POKEDATA, which goes through a different kernel path. */
            uint64_t word_addr = (addr + done) & ~7ull;
            errno = 0;
            long word = ptrace(PTRACE_PEEKDATA, pid_, (void *)word_addr, nullptr);
            if (errno != 0) return false;
            unsigned char tmp[8];
            memcpy(tmp, &word, 8);
            size_t off = (size_t)((addr + done) - word_addr);
            size_t n2 = std::min<size_t>(8 - off, len - done);
            memcpy(tmp + off, (const char *)buf + done, n2);
            memcpy(&word, tmp, 8);
            if (ptrace(PTRACE_POKEDATA, pid_, (void *)word_addr, (void *)word) < 0) return false;
            done += n2;
            continue;
        }
        done += (size_t)n;
    }
    return true;
}

bool Inferior::read_cstr(uint64_t addr, std::string &out, size_t limit) const
{
    out.clear();
    char chunk[128];
    while (out.size() < limit) {
        size_t want = std::min(sizeof chunk, limit - out.size());
        if (!read_mem(addr + out.size(), chunk, want)) return !out.empty();
        for (size_t i = 0; i < want; i++) {
            if (chunk[i] == 0) return true;
            out.push_back(chunk[i]);
        }
    }
    return true;
}

std::vector<MapEntry> Inferior::read_maps() const
{
    std::vector<MapEntry> out;
    std::ifstream in(format("/proc/%d/maps", (int)pid_));
    std::string line;
    while (std::getline(in, line)) {
        MapEntry m;
        char perms[8] = {0};
        unsigned long long a = 0, b = 0;
        int pathpos = 0;
        if (sscanf(line.c_str(), "%llx-%llx %7s %*x %*s %*u %n", &a, &b, perms, &pathpos) < 3)
            continue;
        m.start = a;
        m.end = b;
        m.exec = strchr(perms, 'x') != nullptr;
        if (pathpos > 0 && (size_t)pathpos < line.size()) m.path = trim(line.substr((size_t)pathpos));
        out.push_back(std::move(m));
    }
    return out;
}

bool Inferior::auxv(uint64_t type, uint64_t &value) const
{
    std::ifstream in(format("/proc/%d/auxv", (int)pid_), std::ios::binary);
    if (!in) return false;
    uint64_t pair[2];
    while (in.read((char *)pair, sizeof pair)) {
        if (pair[0] == 0) break;
        if (pair[0] == type) { value = pair[1]; return true; }
    }
    return false;
}

} // namespace oclgdb
