#!/usr/bin/env bash
# Run a command inside the oclgdb Linux dev container.
#
# --cap-add=SYS_PTRACE + seccomp=unconfined are required: Docker's default seccomp
# profile blocks ptrace(2), which is the mechanism this debugger halts execution with.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec docker run --rm -it \
  --cap-add=SYS_PTRACE --security-opt seccomp=unconfined \
  -v "${HERE}:/work" -w /work \
  oclgdb-dev:latest "$@"
