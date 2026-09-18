# syntax=docker/dockerfile:1
#
# Build/dev environment for oclgdb — the source-level OpenCL debugger (SegFault 2026, P06).
#
# Everything lives in Linux because the two load-bearing mechanisms of this project
# are Linux-only: ptrace(2) for genuine execution halting, and ELF/DWARF for the
# debug info pocl's kernel compiler emits into the dlopen'd kernel object.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build pkg-config git ca-certificates \
      gdb binutils elfutils file less vim-tiny python3 \
      llvm-18-dev libclang-18-dev clang-18 libzstd-dev zlib1g-dev libtinfo-dev \
      ocl-icd-opencl-dev ocl-icd-libopencl1 clinfo \
      pocl-opencl-icd libpocl2 libpocl2-common \
    && rm -rf /var/lib/apt/lists/*

# pocl's CPU device is the only device we ever want to see.
ENV OCL_ICD_VENDORS=/etc/OpenCL/vendors
ENV POCL_DEVICES=cpu

WORKDIR /work
