/*
 * oclgdb -- source-level OpenCL debugger for pocl's CPU device backend.
 *
 * Entry point: parses argv, wires up the session and the CLI, and either drives an
 * interactive GDB-style command loop or replays a script (for the demo runbook and for
 * tests/). See README.md for the command reference and DECISIONS.md for why the tool is
 * built this way.
 */

#include "cli.h"
#include "session.h"
#include "util.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

using namespace oclgdb;

static void usage(const char *argv0)
{
    printf(
"usage: %s [options] [spec.ocl-run]\n"
"\n"
"options\n"
"  -x, --script <file>   run commands from <file>, then continue interactively\n"
"                         (or exit immediately if --batch is also given)\n"
"  --batch                exit after the script finishes instead of dropping to a prompt\n"
"  --no-color              disable ANSI colour in the output\n"
"  -h, --help              this text\n"
"\n"
"If a spec is given on the command line it is loaded before the script runs, "
"equivalent to typing 'load <spec.ocl-run>' as the first command.\n",
        argv0);
}

int main(int argc, char **argv)
{
    std::string script_path;
    std::string spec_path;
    bool batch = false;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        if (a == "-x" || a == "--script") {
            if (i + 1 >= argc) { fprintf(stderr, "%s: --script needs an argument\n", argv[0]); return 2; }
            script_path = argv[++i];
        } else if (a == "--batch") {
            batch = true;
        } else if (a == "--no-color") {
            color::enabled = false;
        } else if (!a.empty() && a[0] == '-') {
            fprintf(stderr, "%s: unknown option '%s'\n", argv[0], a.c_str());
            return 2;
        } else {
            spec_path = a;
        }
    }

    if (!isatty(STDOUT_FILENO)) color::enabled = false;

    /* Line-buffer stdout even when it's not a tty (piped to a log, a recorder, or
     * tests/run_test.sh). Without this, our own printf output sits in a fully-buffered
     * userspace buffer while the traced debuggee -- which shares this same stdout fd
     * and fflush()es on its own schedule -- writes straight through, so the two
     * interleave out of real-time order. Line buffering keeps a demo recording (or a
     * test's `2>&1` capture) in the order things actually happened. */
    setvbuf(stdout, nullptr, _IOLBF, 0);

    Session session;
    Cli cli(session);
    cli.banner();

    if (!spec_path.empty()) {
        if (!cli.execute("load " + spec_path)) return 0;
    }

    if (!script_path.empty()) {
        std::ifstream f(script_path);
        if (!f) {
            fprintf(stderr, "%s: cannot open script '%s'\n", argv[0], script_path.c_str());
            return 2;
        }
        cli.echo = true;
        std::string line;
        while (std::getline(f, line)) {
            if (!cli.execute(line)) return 0;
        }
        cli.echo = false;
        if (batch) return 0;
        printf("\n%s-- end of script, entering interactive mode --%s\n",
               color::dim(), color::reset());
    }

    if (batch) return 0;

    std::string line;
    for (;;) {
        cli.prompt();
        if (!std::getline(std::cin, line)) { printf("\n"); break; }
        if (!cli.execute(line)) break;
    }
    return 0;
}
