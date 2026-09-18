Master Prompt — Source-Level OpenCL Debugger (SegFault 2026, P06)
Copy everything below into Claude Code as your opening prompt (or CLAUDE.md project file). It is written to be handed over as-is.

1. Project identity and constraints
You are building a source-level debugger for OpenCL kernels, submitted as Problem Statement P06 ("Source-Level OpenCL Debugger for GPU Architectures") for the SegFault 2026 hackathon. Read this entire prompt before writing any code. Treat it as the spec — do not silently narrow or reinterpret scope.

Non-negotiable target, verbatim from the brief:

A working prototype that lets a developer compile an OpenCL kernel (via pocl), set a source-line breakpoint, run it, have execution genuinely halt at that line for a selectable work-item, inspect that work-item's variables (resolved to source-level names via LLVM debug info) and relevant memory, then step/continue correctly — demonstrated end-to-end on a real, bug-containing multi-work-item kernel.

Hard constraints from the brief (do not violate these — they are what makes the project tractable in 5 weeks):

Target pocl's CPU device backend (or, as a fallback, Oclgrind) — never real GPU silicon. There is no vendor driver access requirement.
Reuse LLVM's existing DWARF debug-info generation. Do not invent a custom debug-info format — pocl already lowers OpenCL C → LLVM IR, and Clang/LLVM already knows how to emit DWARF for that IR.
Implement breakpoint halting via standard OS-level mechanisms — ptrace on Linux, or hooking pocl's execution loop / LLVM's JIT (ORC/MCJIT) directly — not GPU driver hooks.
The debugger must still faithfully model GPU-relevant semantics: multiple work-items per work-group, per-work-item state isolation, and the ability to select which work-item's state to inspect at a breakpoint. A debugger that only shows "the" state (as if there were one thread) does not meet the brief.
A stretch goal only, do not let it eat core-feature time: explore extending toward an open GPU simulator (gem5-gpu, MGPUSim, or a RISC-V GPGPU target). Only attempt this after the core CPU-backend debugger is fully working and demoed.
Do not build: a GPU ISA-level debugger, anything requiring proprietary NVIDIA/AMD/Intel driver hooks, a custom compiler frontend, or a custom debug-info format. All of these are explicitly out of scope per the brief and would burn the 5-week budget on infrastructure the brief tells you to avoid.

2. Why this architecture (context for your own design decisions)
pocl compiles OpenCL C kernels to LLVM IR and then executes each work-item as a software construct on the host CPU — typically as a function call per work-item (or a work-group loop with an optional "work-item loop" transformation), not as real SIMT hardware threads. This means:

Each work-item's "execution" is a real, ordinary stack frame / instruction pointer on the host CPU.
If Clang/pocl is told to emit debug info (-g) when compiling the kernel, you get real DWARF line tables and variable-location info mapping OpenCL C source lines/variables to LLVM IR and then to host machine code — the same mechanism GDB uses for any compiled C program.
Therefore "genuinely halting execution at a source line" reduces to setting a real breakpoint in the generated host machine code (via ptrace + INT3, or via LLVM JIT breakpoint hooks) and using DWARF debug info to translate between the halted machine state and OpenCL source-level concepts (which line, which variables, which work-item).
The "GPU semantics" you must add on top of a normal CPU debugger are: (a) knowing which OpenCL kernel invocation/work-group/work-item corresponds to the halted execution context, and (b) letting the user switch between work-items whose state may all be sitting in different stack frames or private-memory regions simultaneously (if pocl runs them in a loop) or one at a time per breakpoint hit (if pocl runs one work-item to completion before the next).
This is the whole intellectual core of the project. Understanding pocl's actual work-item execution model (do work-items run strictly sequentially per breakpoint hit, or can several be "in flight" at a suspend point?) is the first research task, not an assumption to hand-wave.

3. Suggested phased plan (5-week hackathon timeline)
Build in this order. Each phase should end with something runnable and demoable — do not move to the next phase with a broken previous one.

Phase 0 — Research spike (days 1–3)
Build pocl from source with debug/dev build flags. Confirm you can compile and run a trivial OpenCL kernel (e.g. vector add) against pocl's CPU device.
Compile a kernel with -g (pocl passes this through to its internal Clang) and dump the resulting LLVM IR / object code to confirm DWARF debug info is actually being generated and survives into the final host binary/JIT'd code.
Read (don't guess) pocl's source for how it schedules work-items on the CPU device (lib/CL/devices/basic or lib/CL/devices/pthread, and the kernel-compiler pass that does the work-item loop / work-item function transformation). Document your findings — this determines your breakpoint/inspection design in Phase 2.
Decide early: ptrace-based halting (attach to the pocl host process, insert INT3 at the DWARF-resolved address) vs. JIT-hook halting (if pocl uses LLVM's ORC JIT, register a breakpoint callback before code generation finishes). Ptrace is likely simpler to get working end-to-end fast; document the tradeoff you chose and why in your README.
Exit criterion: you can programmatically compile a kernel with debug info, run it under pocl, and print the DWARF line table for at least one kernel function.
Phase 1 — Minimal single-work-item breakpoint + variable inspection (week 1–2)
Implement: set a breakpoint by (kernel name, source line) → resolve to an address via DWARF → insert breakpoint (ptrace INT3 or JIT hook) → run → halt.
On halt: read DWARF variable-location expressions (DW_AT_location, possibly involving DW_OP_fbreg etc.) to resolve and print local variable values for the current work-item's frame.
Implement continue and basic step (line-level, using the DWARF line table to know where the next source line's machine code starts).
Test against a trivial single-work-item-group kernel first (global/local size 1×1×1) so you don't yet have to deal with multiple concurrent contexts.
Exit criterion: a CLI session where you set a breakpoint by line number, run, see it halt, print a variable, step, continue, program finishes normally.
Phase 2 — Multi-work-item support (week 2–3)
Extend to real work-group sizes (e.g. global size 256, local size 64+).
Confirm and handle whichever execution model Phase 0 found: if pocl runs work-items in an internal loop within one host thread, you need to capture/snapshot each work-item's private variable state as it's produced (since they may share one stack frame across loop iterations) — likely by reading pocl's per-work-item context struct / stack allocation directly, not just "the current frame."
Implement work-item selection: select-work-item (gx, gy, gz) (or local id + group id) at a breakpoint hit, then re-run variable inspection scoped to that work-item's state.
Implement per-work-item state isolation clearly in your data model — this is explicitly called out in the brief as required, not optional polish.
Exit criterion: hit a breakpoint inside a kernel launched with a real multi-work-item NDRange, list which work-items hit it, select different ones, see correctly different variable values for each.
Phase 3 — End-to-end demo kernel with a real bug (week 3–4)
Write (or find) an OpenCL kernel with a genuine, non-contrived bug that is naturally diagnosed by source-level stepping/inspection across work-items — e.g. an off-by-one in a per-work-item index causing one work-item to read/write out of bounds, or a race/divergence-sensitive bug. This is the brief's required "real, bug-containing multi-work-item kernel" demo — do not use a kernel with no actual defect.
Add memory inspection: reading "relevant memory" (per the brief) — at minimum, private/local memory relevant to the halted work-item; stretch: global buffer contents at the point of the halt.
Polish the interaction (CLI is fine — see Section 5 on scope of the UI) so the demo flow is: compile kernel → set breakpoint at the buggy line → run → halt → inspect a specific work-item's variables → see the bug's cause directly from source-level state → step to confirm → continue to completion.
Exit criterion: you can perform the full demo flow in under 3 minutes, repeatably, on a clean machine/VM.
Phase 4 — Hardening, docs, and (only if time remains) stretch goal (week 4–5)
Write the README/demo script (see Section 6).
Add error handling for the obvious failure modes (bad line number, kernel not compiled with -g, breakpoint on a non-code line, watching a work-item id outside the NDRange).
Only now, if time remains: sketch (not necessarily fully implement) how the same DWARF-based approach would extend to an open GPU simulator (gem5-gpu / MGPUSim / RISC-V GPGPU). A short design note or a minimal proof-of-concept is enough — the brief frames this as a stretch, and judges are evaluating the CPU-backend prototype as the core deliverable.
4. Technical decisions to make explicitly (and document your reasoning for each)
Claude Code: for each of these, research the actual current state of the relevant project (pocl version, LLVM version compatibility) before committing, and write your decision + one-paragraph rationale into DECISIONS.md.

pocl version and matching LLVM version. pocl has fairly strict LLVM version compatibility; pick a stable pocl release and the LLVM version it's built against, don't mix and match.
Halting mechanism: ptrace/INT3 vs. JIT-hook. (See Phase 0.)
DWARF consumption library: don't hand-roll a DWARF parser. Use libdwarf, elfutils/libdw, or LLVM's own DebugInfo / llvm-dwarfdump libraries (you're already linking LLVM for other reasons, so reusing LLVM's DWARF reader is likely least-friction).
Language/runtime for the debugger tool itself: C or C++ integrates most naturally with ptrace, LLVM's C++ APIs, and pocl's C codebase; Rust is workable via nix/ptrace crates and gimli for DWARF if the team prefers it and has the LLVM-FFI experience to justify it. Pick based on team strength, not novelty.
Interface: a CLI (GDB-style command loop: break, run, continue, step, print, select-work-item) is entirely sufficient and is what the brief's expected outcome describes. Do not spend hackathon time building a GUI unless the core debugger is fully working with days to spare — a CLI demo is what will be judged, per the brief's own phrasing ("set a source-line breakpoint, run it, have execution halt... inspect... step/continue").
Work-item state representation: decide explicitly whether you snapshot state per work-item as it executes (needed if pocl loops work-items within a shared frame) or read live stack state directly (sufficient if each work-item genuinely gets its own thread/frame). This follows directly from your Phase 0 research — don't guess.
5. Explicit scope boundaries (say no to these unless core work is done early)
No GUI/IDE integration (VS Code extension, etc.) unless everything above is solid with meaningful time left.
No support for OpenCL C++ or SPIR-V input — plain OpenCL C compiled through pocl's normal path only.
No multi-device / multi-platform (AMD/NVIDIA/Intel) support — pocl's CPU device only.
No attempt at real GPU driver hooks, ever, in this cycle — this is explicitly deferred in the brief, and attempting it is the single biggest way to run out of time.
No custom debug-info format — if you find yourself designing your own metadata schema instead of reading DWARF that Clang/LLVM already emits, stop and re-read the brief.
6. Deliverables checklist (map directly to what SegFault judges will look for)
 Working CLI debugger binary/build with clear build instructions (BUILD.md).
 At least one real, bug-containing, multi-work-item OpenCL kernel used as the flagship demo, with the bug's actual cause explained in the README.
 A scripted/recorded demo (or a live-demo runbook) showing the exact end-to-end flow from the brief's "Expected outcome" paragraph: compile → breakpoint → run → halt → select work-item → inspect variables (source-level names, via DWARF) → inspect relevant memory → step → continue.
 DECISIONS.md documenting the technical choices in Section 4 and why they were made — judges evaluating an "explainable"/"tools" track project will reward showing your reasoning, not just the artifact.
 A short "extensibility" note explaining concretely (not just asserted) how this DWARF-based, driver-independent approach would carry over to a real GPU backend later — this directly answers the brief's closing claim about why the CPU-backend approach is a reasonable stepping stone.
 (Optional, stretch) A minimal design sketch or proof-of-concept for a debuggable open GPU simulator target.
7. How to work with me (Claude Code) on this
Before writing implementation code, do the Phase 0 research and report back what you found about pocl's work-item execution model — this materially changes the design and I'd rather adjust the plan than have you build against a wrong assumption.
Flag early and clearly if pocl/LLVM version compatibility, ptrace permissions (e.g. running in a sandboxed/container environment without CAP_SYS_PTRACE), or JIT internals make the chosen halting mechanism infeasible — that's a Phase-0-level risk, not something to discover in week 4.
Prioritize a thin, fully-working vertical slice (Phase 1) over a broad, partially-working feature set. A working single-work-item breakpoint-and-inspect loop by the end of week 2 is a better position than five half-built features.
Keep commits/documentation clean enough that the five-minute jury pitch can walk through the architecture on a whiteboard from your README/DECISIONS.md alone.