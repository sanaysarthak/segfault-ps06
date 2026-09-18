# oclgdb — a source-level debugger for OpenCL kernels on pocl's CPU device

`oclgdb` lets you compile an OpenCL kernel through [pocl](http://portablecl.org/),
set a breakpoint by source line, run it, have execution *genuinely* halt there — via a
real `ptrace` breakpoint in the real machine code pocl generated, not a simulation — for
a work-item you choose, inspect that work-item's variables under their OpenCL C names
(resolved through the DWARF debug info Clang already emits) and relevant memory, then
step or continue correctly. It is a GDB-style CLI (`break`, `run`, `continue`, `step`,
`print`, `select-work-item`, `info ...` — `help` inside `oclgdb` lists all of them).

Built for SegFault 2026, Problem Statement P06. See [MASTER-PROMPT.md](MASTER-PROMPT.md)
and [problem-statement.md](problem-statement.md) for the brief this was built against,
[DECISIONS.md](DECISIONS.md) for why each technical choice was made,
[docs/PHASE0-FINDINGS.md](docs/PHASE0-FINDINGS.md) for the research the whole design
rests on, and [BUILD.md](BUILD.md) to build it.

## Demo script

```sh
docker build -f docker/Dockerfile -t oclgdb-dev:latest docker
scripts/dev.sh bash -c "cd build 2>/dev/null || (mkdir build && cd build && cmake -G Ninja ..); cd build && ninja && \
  POCL_DEVICES=basic ./oclgdb --batch -x ../scripts/demo.txt ../kernels/blur3.ocl-run"
```

runs [scripts/demo.txt](scripts/demo.txt) — compile → set a breakpoint on the buggy
line, filtered to one specific work-item → run → halt → print that work-item's
source-level variables → inspect the input buffer's memory → step → continue to the
verification failure — in under a minute, repeatably. `BUILD.md` has the fully spelled
out steps; this is the compressed one-liner.

## The bug

[kernels/blur3.cl](kernels/blur3.cl) is a 3-point box blur over a 256-element signal,
launched as 4 work-groups of 64 work-items
([kernels/blur3.ocl-run](kernels/blur3.ocl-run)). Each work-item stages its element
into a local-memory tile (with the two edge work-items additionally staging the halo
elements so every work-item's 3-point read stays in bounds), then after a barrier,
averages its own tile neighbourhood into `out`.

The right-edge halo guard is written as:

```c
if (lid == lsz - 1) {
    int right_src = (gid == n) ? n - 1 : gid + 1;   // BUG
    tile[lsz + 1] = in[right_src];
}
```

It should read `gid == n - 1` — "is this the last work-item?" For a 256-element signal
(`n = 256`), the last work-item has `gid = 255`, and `255 == 256` is false. So the guard
never fires, the *un-clamped* branch is always taken, and `right_src` becomes
`gid + 1 = 256` — one past the end of a 256-element buffer. The out-of-bounds read
happens to land on adjacent heap memory rather than crashing, so `out[255]` comes out
silently wrong (`169.667` instead of the correct clamped `254.667`) rather than
segfaulting — the kind of bug that is exactly as easy to walk past in a printf-only
workflow as it is quick to spot once you can halt work-item 255 and read `gid` and `n`
side by side.

`scripts/demo.txt` does exactly that: it filters a breakpoint to
`wi == (255,0,0)`, and once *genuinely* halted there — not "the" kernel state, this
specific work-item's — `print gid`, `print n`, and `print gid == n` show the root cause
directly from source-level state, before the wrong value is ever written.

## How the pieces fit together

```
   .ocl-run spec  ──┐
                     ├──►  ocl-runner (host/ocl_runner.c)   ← the debuggee
   kernel.cl      ──┘        an ordinary, uninstrumented OpenCL host program;
                              it builds the kernel through pocl (clBuildProgram),
                              launches it, reads results back. oclgdb never
                              modifies it -- it's just what gets ptrace'd.

   oclgdb (src/*.cpp)
     ├─ Inferior         fork() + PTRACE_TRACEME + ptrace(2); INT3 breakpoints,
     │                   register/memory access. The actual halting mechanism.
     ├─ DwarfIndex        wraps LLVM's DWARFContext: line↔address, address↔function,
     │                   address↔in-scope variables, over the .so pocl compiled.
     ├─ dwarf_expr         a small stack machine that evaluates DW_AT_location
     │                   expressions against a specific (possibly historical)
     │                   captured register file.
     ├─ pocl.{h,cpp}       the only file that knows pocl-specific facts: symbol
     │                   names, cache layout, the work-group function's ABI.
     ├─ workitem.{h,cpp}   WorkItemSnapshot (per-work-item state isolation) and
     │                   the identity calibration that recovers which work-item
     │                   a trap belongs to.
     └─ Session            owns all of the above; turns raw ptrace traps into
                          "work-item (255,0,0) hit blur3.cl:26".
```

Both `ocl-runner` and `oclgdb` link the same [`src/runspec.c`](src/runspec.c), a small C
parser for `.ocl-run` files — a plain-text description of one kernel launch (source
file, kernel name, build options, NDRange, argument buffers and their initial values).
This is what lets `oclgdb` know a kernel's NDRange and argument layout *before* it ever
starts the traced process, and it means adding a new demo kernel never requires writing
a bespoke host program — see any `kernels/*.ocl-run` file for the format, or `help` →
none needed, they're commented.

## Faithfully modelling GPU semantics

The brief is explicit that this must not be "a debugger that shows the state, as if
there were one thread." Concretely, in this codebase:

- **Multiple work-items per work-group**: `oclgdb` knows the full NDRange
  (`Session::nd()`) and, at a breakpoint, which work-items of the current work-group
  reached it (`info work-items` lists all of them side by side).
- **Per-work-item state isolation**: `WorkItemSnapshot` (`src/workitem.h`) is a
  self-contained bundle — captured register file plus every already-resolved
  source-level variable — for *one* work-item at *its own* trap. This is not a
  convenience; it is required by how pocl actually compiles kernels (Phase 0, F5): a
  work-item's locals live in machine registers with short, PC-scoped live ranges, so
  there is no single "current frame" to read after the fact once execution has moved on
  to the next work-item.
- **Selecting which work-item to inspect**: `select-work-item (x,y,z)` (or the filtered
  form, `break <line> if wi == (x,y,z)`) switches which snapshot `print`/`info locals`
  read from, independent of which work-item (if any) is still physically halted.

## Extensibility toward a real GPU backend

The brief's closing claim is that this CPU-backend, DWARF-based approach is a reasonable
stepping stone toward a real GPU debugger, because the hardest part — driver-level
halting — is exactly what's deferred here. Concretely, in this codebase:

- **`Inferior` (ptrace/INT3) is the only halting-specific code.** Everything above it
  (`DwarfIndex`, `dwarf_expr`, `Session`'s breakpoint/scope logic) operates purely on
  "an address to halt at" and "a register file to read" — abstractions a GPU debug
  backend has its own equivalents of (a hardware/simulator breakpoint register, a
  captured per-lane register file). Porting means writing a new class with `Inferior`'s
  interface (`resume`, `read_mem`, `get_regs`, ...) against gem5-gpu's or MGPUSim's debug
  hooks, or a RISC-V GPGPU target's JTAG/simulator interface — not touching the DWARF or
  work-item logic at all.
- **The DWARF layer needs nothing pocl-specific.** `dwarf_index.cpp` and `dwarf_expr.cpp`
  read whatever DWARF Clang/LLVM emitted for the target; a GPU compiler backend that
  reuses LLVM's DWARF emission for its own ISA (as gem5-gpu's and several RISC-V GPGPU
  toolchains do) needs no changes here at all.
- **`pocl.{h,cpp}` is the isolation boundary.** It is the *only* file that knows pocl's
  symbol-naming scheme, on-disk cache layout, and the work-group function's calling
  convention. A GPU backend's equivalent facts — how *its* compiler names/lays out a
  kernel binary, how *its* launch ABI passes arguments — replace this one file.
- **Identity calibration already assumes nothing about a single execution model.**
  `workitem.cpp`'s `calibrate_identity` was built, out of necessity, to handle two
  *structurally different* ways pocl encodes "which work-item is this" (a replicated
  address vs. an affine register) without the rest of the debugger caring which. Real
  SIMT hardware's actual lane-id mechanism (a lane-id register, a predicate mask) is a
  third case the same calibration framework is built to accommodate, not a redesign.

This was deliberately *not* attempted beyond this design note — the master prompt frames
it as a stretch goal, explicitly gated on the CPU-backend prototype being fully working
first, which is where the 5-week budget went. A slightly longer sketch, including what a
minimal proof-of-concept's next steps would be, is in
[docs/STRETCH-GPU-SIMULATOR.md](docs/STRETCH-GPU-SIMULATOR.md).

## Repository map

| path | what |
|---|---|
| `src/` | the debugger |
| `host/ocl_runner.c` | the debuggee harness |
| `kernels/*.cl`, `kernels/*.ocl-run` | demo kernels and their launch specs |
| `tests/` | end-to-end regression tests (real ptrace, real pocl) + `ctest` glue |
| `scripts/demo.txt` | the recorded/live demo script |
| `scripts/phase0-research.sh`, `docs/phase0/`, `docs/PHASE0-FINDINGS.md` | Phase 0 research and its raw evidence |
| `docker/` | the Linux build/run environment (see BUILD.md) |
| `DECISIONS.md` | the technical decisions and why |
| `BUILD.md` | build and run instructions |
