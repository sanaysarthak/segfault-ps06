# BUILD.md

`oclgdb` is a Linux tool (it needs `ptrace(2)` and ELF/proc-fs), so on any host —
including Windows — the supported path is the provided Docker dev image. If you are
already on Linux with the dependencies installed, skip straight to "Building" below.

## Option A: Docker (recommended, works on Windows/macOS/Linux hosts)

```sh
# Build the dev/runtime image once (Ubuntu 24.04 + pocl 5.0 + LLVM/Clang 18 + build tools).
docker build -f docker/Dockerfile -t oclgdb-dev:latest docker

# Everything below runs inside that image. scripts/dev.sh mounts the repo at /work and
# grants exactly the two things ptrace needs that Docker blocks by default.
scripts/dev.sh bash
```

`scripts/dev.sh` runs the container with:

```
--cap-add=SYS_PTRACE --security-opt seccomp=unconfined
```

Both are required: Docker's default seccomp profile blocks the `ptrace(2)` syscall
outright, and without `SYS_PTRACE` the capability check fails before seccomp is even
consulted. Neither flag grants anything beyond tracing processes inside the container.

## Option B: native Linux

Install (Debian/Ubuntu package names shown; adjust for your distro):

```
build-essential cmake ninja-build pkg-config
llvm-18-dev libclang-18-dev clang-18 libzstd-dev zlib1g-dev libtinfo-dev
ocl-icd-opencl-dev ocl-icd-libopencl1
pocl-opencl-icd libpocl2 libpocl2-common
```

`oclgdb` targets LLVM 18's CMake package specifically (`find_package(LLVM)` in
`CMakeLists.txt`); if your distro ships a different LLVM as the default `llvm-config`,
point CMake at the right one with `-DLLVM_DIR=/usr/lib/llvm-18/cmake`.

## Building

```sh
mkdir -p build && cd build
cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo ..
ninja
```

This produces two binaries:

- `build/oclgdb` — the debugger.
- `build/ocl-runner` — the generic OpenCL host harness `oclgdb` launches as the
  debuggee (see README.md, "How the pieces fit together"). It also runs standalone:
  `./ocl-runner kernels/blur3.ocl-run` builds and runs a kernel with no debugger
  attached at all, exactly like any other OpenCL host program.

## Running the tests

```sh
cd build
ctest --output-on-failure
```

The tests (`tests/phase1_single_workitem.txt`, `phase2_multi_workitem.txt`,
`phase3_bug_diagnosis.txt`) are `oclgdb --batch -x <script>` runs against the real pocl
CPU device under real `ptrace` — nothing about them is mocked or simulated — checked
against `# expect: <substring>` annotations in each script (`tests/run_test.sh`). They
must run with `SYS_PTRACE`/seccomp available, so from Docker: `scripts/dev.sh bash -c
"cd build && ctest --output-on-failure"`.

## Running the demo yourself

```sh
scripts/dev.sh bash
cd build
POCL_DEVICES=basic ./oclgdb kernels/blur3.ocl-run
```

then follow README.md's "Demo script". `POCL_DEVICES=basic` is optional when you invoke
`oclgdb` interactively — `oclgdb` sets it on the debuggee itself (see DECISIONS.md,
"Threading") — but harmless to set for yourself too.

## Known environment risk (read before you hit it)

If `ptrace` calls fail with `EPERM` even with the flags above, check
`/proc/sys/kernel/yama/ptrace_scope` inside the container. `oclgdb` always `fork()`s the
debuggee itself and has it call `PTRACE_TRACEME` (never `PTRACE_ATTACH` to an unrelated
process), which Yama's default scope (1, "restricted") already permits without any
further configuration — this is a fallback check, not something the provided setup
should ever actually hit.
