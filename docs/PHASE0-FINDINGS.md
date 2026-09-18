# Phase 0 — Research findings

Everything below was measured, not assumed. Raw evidence is in [`docs/phase0/`](phase0/)
(`*.dwarf.txt` = ELF sections, symbol tables, full DIE trees, location lists, line
tables and disassembly; `*.so` = the actual objects pocl produced). Reproduce with
`scripts/phase0-research.sh`.

Environment: Ubuntu 24.04 container, pocl 5.0 (`libpocl.so.2.12.0`, built against
LLVM/Clang 16), LLVM 18 used for the debugger's own DWARF reader.

## F1 — pocl's CPU device compiles each kernel to an ordinary ELF shared object

pocl lowers OpenCL C to LLVM IR, runs its work-group transformation passes, and emits a
**standalone `.so` per (kernel, work-group size, specialisation)**, which it caches and
then `dlopen`s into the host process:

```
$POCL_CACHE_DIR/<2>/<hash>/<kernel>/<LX>-<LY>-<LZ>-goffs0-smallgrid/<kernel>.so
```

Two exported symbols per kernel:

| symbol | role |
| --- | --- |
| `_pocl_kernel_<name>` | the kernel body for a whole work-group |
| `_pocl_kernel_<name>_workgroup` | ABI wrapper pocl calls once per work-group |

Confirmed live: with `POCL_DEVICES=basic` the object shows up in `/proc/<pid>/maps`
as a normal file mapping with an `r-xp` segment, so its load bias is discoverable by
ordinary dynamic-linker means.

**Consequence:** no JIT hooking is required, and nothing about pocl needs patching.
This is an ordinary ELF + DWARF debugging problem in someone else's address space.

## F2 — LLVM's DWARF survives into that object, intact

With `-g` in the `clBuildProgram` options, the `.so` contains `.debug_info`,
`.debug_abbrev`, `.debug_line`, `.debug_loc`, `.debug_frame` and `.debug_str`
(DWARF v4, `DW_AT_producer = "Ubuntu clang version 16.0.6"`,
`DW_AT_language = DW_LANG_OpenCL`).

The kernel appears as an **abstract** `DW_TAG_subprogram` (`DW_AT_inline =
DW_INL_inlined`) carrying the source-level names and types of every parameter and
local, which is then referenced by `DW_TAG_inlined_subroutine` instances inside the
concrete functions via `DW_AT_abstract_origin`. Source-level variable resolution
therefore means: find the concrete inlined instance covering the halted PC, walk to the
abstract origin for names/types, and evaluate `DW_AT_location` for storage.

## F3 — `-cl-opt-disable` is mandatory, and it is the only opt-level lever pocl exposes

pocl has no kernel-compiler optimisation-level environment variable (the full set of
`POCL_*` knobs in 5.0 is listed in `docs/phase0/`), and it rejects `-O0` as a build
option. Without `-cl-opt-disable`, at a work-group size of 64 the entire body of
`vecadd` collapses to three instructions and `gid`, `lid`, `bv` and `n` lose their
locations entirely — source-level debugging is simply not possible against that code.

| build options | WG size | machine addresses for source line 11 | locals with locations |
| --- | --- | --- | --- |
| `-g` | 16 | 33 | 5 |
| `-g` | 64 | 2 (body collapsed) | 2 |
| `-g -cl-opt-disable` | 16 | 16 | 5 |
| `-g -cl-opt-disable` | 64 | 1 | 5 |
| `-g -cl-opt-disable` | 256 | 1 | 5 |

So `oclgdb` requires `-g -cl-opt-disable` and injects it if the user's spec omits it —
exactly the same bargain as building C with `-O0 -g` before opening gdb.

## F4 — pocl has *two* work-item execution models, and both must be supported

Selected by work-group size against `POCL_FULL_REPLICATION_THRESHOLD`:

* **Replication** (small work-groups, e.g. 16). The body is emitted once per work-item.
  Source line 11 resolves to **16 distinct machine addresses**. `get_local_id(0)` is
  constant-folded per copy — visible in DWARF as
  `(0x0f,0x2d): DW_OP_consts +0`, `(0x2d,0x4f): DW_OP_consts +1`, … ascending with
  address. Each work-item therefore has *its own* PC ranges and *its own* location-list
  entries.
* **Work-item loop** (larger work-groups, e.g. 64/256). The body is emitted once and
  wrapped in a scalar loop over local ids (verified in the disassembly:
  `inc %rsi; cmp $0x40,%rsi; jne`). One address per source line, hit N times per
  work-group. Not vectorised — the arithmetic stays scalar (`vaddss`), so each work-item
  really is a separate pass over the same instructions.

Both models share one host stack frame across all work-items of a work-group.

## F5 — Variables live in registers, never in stack slots

Even with `-cl-opt-disable` pocl still runs mem2reg/SROA, so there is **not a single
`DW_OP_fbreg` in any object we produced**. Locations are `DW_OP_reg*`, `DW_OP_breg*`,
and `DW_OP_consts … DW_OP_stack_value`, held in `.debug_loc` *location lists* keyed by
PC range, often with short live ranges:

```
gid : (0x0f,0x2d) DW_OP_breg0 RAX+0, DW_OP_stack_value
      (0x2d,0x4f) DW_OP_breg0 RAX+0, DW_OP_lit1, DW_OP_or, DW_OP_stack_value   ...
av  : (0x1f,0x28) DW_OP_reg17 XMM0
      (0x41,0x4a) DW_OP_reg17 XMM0                                             ...
```

**This is the single most design-determining finding.** A work-item's values are only
recoverable while that work-item's own PC range is current; once execution moves on, the
registers are reused and the value is gone. Cross-work-item inspection therefore *cannot*
read a live frame — it has to **snapshot each work-item's state at that work-item's own
trap**. (The master prompt anticipated exactly this.) It also means the debugger needs a
real DWARF expression evaluator with register reads, not just frame-base arithmetic.

## F6 — Work-group ABI

`_pocl_kernel_<name>_workgroup(void **args, struct pocl_context *ctx, size_t group_x,
size_t group_y, size_t group_z)` — SysV x86-64, so at function entry
`RDI`=args, `RSI`=ctx, `RDX`=group_x, `RCX`=group_y, `R8`=group_z. Confirmed against the
disassembly (`shl $0x6,%edx` = group_x x 64 for a 64-wide work-group).

`args[i]` points at the i-th kernel argument value, so for a `__global T*` argument
`*(void**)args[i]` is the buffer's host address — which is what makes source-level
**global memory** inspection (`print in[gid+1]`) possible.

## F7 — Threading: `POCL_DEVICES=basic` gives a single-threaded, deterministic inferior

pocl's default `cpu`/`pthread` device runs work-groups across 9 host threads. The
`basic` device runs everything on the calling thread, sequentially. `oclgdb` sets
`POCL_DEVICES=basic` for the inferior by default — the debugging equivalent of gdb's
`set scheduler-locking on`. It changes nothing the kernel can observe, and it makes
work-group and work-item ordering reproducible. See `DECISIONS.md` (D7).

## F8 — ptrace is available

`ptrace(2)` works inside Docker given `--cap-add=SYS_PTRACE --security-opt
seccomp=unconfined` (both are in `scripts/dev.sh`). We `fork` + `PTRACE_TRACEME` +
`exec` rather than attaching, so `kernel.yama.ptrace_scope=1` is not an obstacle.

## What this implies for the design

1. Halting is **ptrace + INT3** on addresses resolved from the kernel object's DWARF
   line table (see `DECISIONS.md` D2). No JIT hook, no pocl patch.
2. A source line maps to **a set** of addresses, not one — and in replication mode each
   member of that set *is* a specific work-item. Breaking "for work-item 37" is then
   literally arming one address.
3. Because of F5, per-work-item state must be **snapshotted at each work-item's own
   trap**; a shared live frame cannot answer "what does work-item 12 see?".
4. Work-item identity cannot be read from a fixed location, so the debugger
   **calibrates** it against the first work-group it sees — see `DECISIONS.md` D6.
