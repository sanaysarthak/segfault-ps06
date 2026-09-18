# DECISIONS.md — technical choices and why

This project is `oclgdb`, a source-level debugger for OpenCL kernels running on pocl's
CPU device. Each section below is one of the decisions the master prompt (§4) asked to
be made explicitly, with the one-paragraph rationale.

## pocl version and matching LLVM version

**Decision:** pocl 5.0 (`libpocl.so.2.12.0`) as packaged by Ubuntu 24.04
(`pocl-opencl-icd`), which is built against LLVM/Clang 16. `oclgdb` itself links against
LLVM 18 for its own DWARF reader (see below) — the two do not need to match, because
`oclgdb` never links against pocl or its LLVM; it only reads the ELF/DWARF pocl's LLVM 16
happens to emit, and LLVM's DWARF format is version-stable across that gap (DWARF v4
here). We did not build pocl from source: `apt`'s pocl 5.0 already builds and runs
against its own CPU device out of the box (confirmed in Phase 0 — `clinfo` lists a
working `cpu-skylake-avx512` device), and building it ourselves would only have bought
us a newer pocl for no capability we needed. If a future contributor wants a specific
pocl/LLVM pairing (e.g. to chase a newer work-group codegen mode), `docker/Dockerfile`
is the one place that choice lives.

## Halting mechanism: ptrace/INT3 vs. JIT-hook

**Decision:** `ptrace` + software breakpoints (`INT3`, `0xCC`), never a JIT hook.
Phase 0 (docs/PHASE0-FINDINGS.md, F1) found that pocl's CPU device does not use an
LLVM JIT at kernel-launch time at all: its kernel compiler runs once, ahead of time,
compiling each (kernel, work-group shape) pair to a real `.so` on disk, which it then
`dlopen`s into the host process. There is no JIT execution engine to hook. This made the
decision easy rather than a real tradeoff: `oclgdb` `fork()`s the debuggee, has the child
request `PTRACE_TRACEME`, and once the dynamic linker's `r_debug` rendezvous breakpoint
tells us the kernel object has been mapped (`session.cpp`, `find_r_debug`/
`on_rdebug_stop`), it resolves the user's breakpoint line through DWARF and overwrites
the first byte at that address with `0xCC`, restoring and single-stepping across it on
each hit. This is standard, well-understood technology (the same technique GDB itself
uses on Linux) and needed no protocol design of our own.

## DWARF consumption library

**Decision:** LLVM's own `DebugInfo/DWARF` reader (`DWARFContext`, `DWARFDie`,
`DWARFExpression`), not `libdwarf` or `elfutils/libdw`. We are already linking LLVM's
`object` and `binaryformat` libraries to open the kernel `.so` as an `ObjectFile`, so
pulling in LLVM's DWARF reader adds no new dependency, and it has first-class C++ DIE
and location-list APIs that map directly onto the debugger's needs (`dwarf_index.cpp`
wraps `DWARFContext` in a debugger-shaped index: line→address, address→function,
address→in-scope variables). We do not use LLVM's expression *evaluator*, though —
`dwarf_expr.cpp` is a small hand-written stack machine over
`llvm::DWARFExpression`'s decoded operation stream. LLVM's own evaluator is designed for
`llvm-dwarfdump`'s static, no-registers-available printing; ours needs to bind
`DW_OP_breg*`/`DW_OP_reg*` to a specific captured (possibly historical) register file,
which is exactly what a debugger's location evaluator has to do and what LLVM's does not
expose in a reusable form. We never hand-roll DWARF *parsing* — only this small,
necessary evaluation layer on top of LLVM's own decoded expression operations.

## Language/runtime

**Decision:** C++17 for the debugger (`src/*.cpp`), plain C11 for the shared run-spec
parser (`src/runspec.c`) and the debuggee harness (`host/ocl_runner.c`). C++ is the
natural fit for `ptrace`, LLVM's C++-only APIs, and ELF/DWARF work; the run-spec parser
is C specifically so the *same* source file compiles unmodified into both the C++
debugger (which needs to know the NDRange and build options before it ever starts the
inferior) and the plain-C debuggee harness (which must stay a completely ordinary,
un-instrumented OpenCL host program — see below).

## Interface

**Decision:** a GDB-style CLI command loop (`break`, `run`, `continue`, `step`, `print`,
`select-work-item`, `info ...`), implemented in `src/cli.cpp`. This is what the brief's
own "Expected outcome" paragraph describes end to end, and building anything more (a
GUI/TUI) before the CLI is airtight would have traded a finished, demoable tool for an
unfinished, prettier one. `--batch -x <script>` mode (see `src/main.cpp`) doubles as
both the demo-runbook mechanism and the regression-test harness (`tests/*.txt` +
`tests/run_test.sh`), so the interface investment pays for itself twice.

## Work-item state representation: snapshot, not live-read

**Decision:** snapshot every work-item's resolved variables at the exact moment it
passes through a breakpoint, rather than reading "the" live frame. This follows directly
from Phase 0 finding F5 (docs/PHASE0-FINDINGS.md): pocl's kernel compiler keeps kernel
locals in machine **registers**, not stack slots (`-cl-opt-disable` still runs
mem2reg/SROA), and those registers have short, PC-scoped live ranges that get reused by
the *next* work-item. By the time work-item 40 is executing, work-item 12's registers
have long since been overwritten — there is no "current frame" to read after the fact.
`WorkItemSnapshot` (`workitem.h`) is therefore a self-contained, already-resolved bundle
(register file + every in-scope variable's `Value`, computed once at that work-item's
own trap), and `select-work-item` simply switches which snapshot `print`/`info locals`
read from. This is the concrete form the master prompt's required "per-work-item state
isolation" takes in this codebase.

## Identity calibration: kernel-wide model with a fast per-breakpoint path

Not one of the four questions §4 asked for explicitly, but it was the single hardest
design problem in the project and deserves its own record here (the full account is the
"Identity calibration" comment block in `session.cpp`). Phase 0 found that a source line
maps to **either** N distinct machine addresses (replicated work-items) **or** one
address executed N times by a real loop, and that neither mode stores a work-item's
`(x,y,z)` anywhere `oclgdb` can just read — it has to be inferred from which replica
address fired, or from an affine relationship between a loop-counter register and the
local id (`workitem.cpp`, `calibrate_identity`). The first design (calibrate once,
kernel-wide, from a single always-executed line) turned out to be wrong in two ways
that only showed up under real, divergent, barrier-containing kernels: (1) a line
reached by only some work-items never accumulates a full work-group's worth of samples
on its own, and (2) pocl compiles the code on either side of a `barrier()` as **separate
loops with independently-allocated identity registers**, so a model calibrated on one
side does not carry to the other. The shipped design tries the fast, obviously-correct
path first — calibrate a breakpoint from its *own* hits, which is exact for any
non-divergent line in *any* region — and only falls back to a kernel-wide model
(calibrated from the DWARF prologue-end line, guaranteed to run once per work-item) for
a line that turns out to be genuinely divergent. Hits that arrive before whichever model
governs them is ready are not lost: their register state is captured immediately and
queued (`PendingHit`), and resolved retroactively the moment calibration completes.

## Threading: `POCL_DEVICES=basic`

**Decision:** the debuggee always runs with `POCL_DEVICES=basic` (`session.cpp`,
`launch`), which is pocl's single-threaded CPU device, instead of the default
`pthread`/`cpu` device that spreads work-groups across a thread pool (confirmed in
Phase 0, F7: 9 host threads under the default device). This is the debugging equivalent
of GDB's `set scheduler-locking on` — it changes nothing an OpenCL kernel can observe
(work-group order is not part of the OpenCL execution model's guarantees), and it makes
which work-group hits a breakpoint next fully deterministic, which matters for a tool
whose entire value proposition is being able to reliably reproduce "halt at work-item
(255,0,0)" across runs.
