P06 · Compiler frameworks and tools

## Source-Level OpenCL Debugger for GPU Architectures
pocl
OpenCL
DWARF

## Problem
OpenCL kernels running on GPUs are difficult to debug because developers lack source-level visibility into execution — they're forced to reason about raw ISA state instead of variables, breakpoints, and stepping the way they would with CPU code. Production-grade GPU debuggers exist but rely on vendor-proprietary driver hooks that are inaccessible to most tooling efforts within a short build cycle. There is a need for a debugger that gives developers a genuinely source-level debugging experience for OpenCL kernels — breakpoints, variable inspection, and stepping — without requiring hardware-level driver access, by targeting a software execution backend that models GPU execution semantics (work-items, work-groups, divergence) faithfully enough to be useful.

## Goal
Build a source-level OpenCL debugger targeting pocl (Portable Computing Language) running kernels on its CPU device backend, or an existing OpenCL/GPU simulator (e.g., Oclgrind), rather than real GPU silicon. Since pocl lowers OpenCL C to LLVM IR and executes work-items as software constructs on the CPU, the team can leverage LLVM's mature, already-existing DWARF debug-info generation instead of inventing GPU ISA-level debug metadata from scratch, and can implement breakpoint halting via standard OS-level mechanisms (ptrace, or hooking the interpreter/JIT) instead of GPU driver hooks. The tool should still faithfully model GPU-relevant semantics: multiple work-items per work-group, per-work-item state isolation, and the ability to select which work-item's state to inspect at a breakpoint. Teams with time remaining are encouraged to explore extending the approach toward an open, debuggable GPU simulator target (e.g., gem5-gpu, MGPUSim, or the RISC-V GPGPU work referenced in other tracks of this hackathon) as a stretch direction, since real vendor driver access is not realistically obtainable within the hackathon timeframe.

## Expected outcome
A working prototype that lets a developer compile an OpenCL kernel (via pocl), set a source-line breakpoint, run it, have execution genuinely halt at that line for a selectable work-item, inspect that work-item's variables (resolved to source-level names via LLVM debug info) and relevant memory, then step/continue correctly — demonstrated end-to-end on a real, bug-containing multi-work-item kernel. This gives developers a usable source-level debugging workflow for OpenCL today, and the LLVM-debug-info-based approach is directly extensible to a real GPU backend later, since the hardest, vendor-gated part — hardware-level halting — is exactly what's deferred out of scope for this cycle.

