# Stretch goal — sketch: porting to an open GPU simulator

Not implemented (per the brief, this is gated on the CPU-backend prototype being fully
working first, which is where the 5-week budget went — see README.md). This is the
design sketch the brief's Phase 4 asks for: concretely, not just asserted, how oclgdb's
architecture carries over to gem5-gpu or MGPUSim.

## What has to change, and what doesn't

oclgdb already factors into exactly the two kinds of code a port needs to touch, and the
two kinds it doesn't (see README.md, "Extensibility toward a real GPU backend" for the
same split from the shipped side):

| Layer | CPU backend today | GPU backend port |
|---|---|---|
| `Inferior` (`src/inferior.{h,cpp}`) | `ptrace`/`INT3` on a host process | simulator debug hooks |
| `pocl.{h,cpp}` | pocl symbol names, cache layout, work-group ABI | the GPU compiler's equivalents |
| `DwarfIndex`, `dwarf_expr` | unchanged | unchanged |
| `Session`, `workitem.cpp` (identity calibration) | unchanged in structure | unchanged in structure |

## `Inferior`: from ptrace to a simulator breakpoint callback

`Inferior`'s interface is already simulator-shaped, not ptrace-shaped: `start`,
`resume`/`single_step` (returning a `Stop{Trap|Signal|Exited}`), `read_mem`/`write_mem`,
`get_regs`/`set_regs`. gem5-gpu exposes exactly this shape already — its GPU model runs
inside gem5's event-driven simulation loop, which already supports PC-based breakpoints
and can pause on one (this is what `m5 exit`/debug-flag hooks and gem5's own `Debug`
breakpoint infrastructure are for). A `GemGpuInferior` would:

- `start()`: configure a gem5 GPU-model run with the compiled kernel binary instead of
  forking a host process.
- `resume()`: step the event queue until a registered breakpoint PC fires or the kernel
  completes; return the same `Stop` the CPU backend returns.
- `read_mem`/`get_regs`: read the simulated GPU's memory/register-file state for the
  halted wavefront/CTA instead of `/proc/pid/mem` and `PTRACE_GETREGS`.

MGPUSim is architecturally similar (a Go-based, cycle-level GPU simulator with explicit
wavefront state); the same interface maps onto its debug/introspection API. Neither
requires touching `DwarfIndex`, `dwarf_expr`, or `Session`'s breakpoint/scope logic —
they only ever ask `Inferior` for bytes at an address and a register file.

## `pocl.{h,cpp}`: the compiler-specific facts move here

This file already exists *because* something has to own "how does this compiler name
and lay out a kernel binary, and what's its launch ABI" — for pocl that's symbol names
(`_pocl_kernel_<name>`), the on-disk `.so` cache, and the work-group function's SysV
calling convention. A GPU backend's compiler (LLVM-based GPU backends reuse the same
DWARF emission this project already depends on) has its own answers to those same three
questions; porting means writing `gpu_backend.{h,cpp}` with the equivalent facts, not
redesigning anything that reads DWARF.

## Identity calibration already assumes real SIMT, not just pocl's two cases

`workitem.cpp`'s `calibrate_identity` exists because pocl doesn't store a work-item's
`(x,y,z)` anywhere DWARF-visible — it has to be inferred from machine state, and there
are two structurally different ways pocl encodes that (an address per replicated
work-item, or an affine register in a loop). Real SIMT hardware's actual lane-id
mechanism — a lane-id/thread-id register, visible per-lane in a captured wavefront
register file — is a *third*, and simpler, case for the same calibration framework: a
GPU backend can likely skip calibration heuristics entirely and read the lane id
directly out of a known special register, making `IdentityModel::Kind::Register` (already
implemented) the steady state rather than a fallback.

## What a minimal PoC would look like, next

1. Build gem5-gpu (or MGPUSim) with a debug/breakpoint hook exposed at the API level.
2. Write `GemGpuInferior : Inferior` implementing `start`/`resume`/`read_mem`/`get_regs`
   against that hook.
3. Write `gpu_backend.{h,cpp}` mirroring `pocl.{h,cpp}`'s three responsibilities for
   whatever compiler produced the target kernel binary.
4. Point `Session` at the new `Inferior` subclass and backend file; `main.cpp`/`cli.cpp`
   need no changes at all.

Steps 2–3 are where all the actual new work is; step 4 is the point of this whole
factoring — the DWARF and work-item-identity core the brief calls "the hardest,
vendor-gated part" being exactly what's *not* rewritten is the extensibility claim,
demonstrated by which files a port does and doesn't touch.
