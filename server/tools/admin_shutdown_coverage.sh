#!/usr/bin/env bash
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later

# Run only in the isolated CI worker: sources are read-only, and both writable
# directories are disposable tmpfs mounts. stdout contains only LCOV.
set -euo pipefail

mkdir /tmp/admin-shutdown-coverage
cd /tmp/admin-shutdown-coverage
export LLVM_PROFILE_FILE="$(python3 /workspace/tools/ci/llvm_coverage.py prepare \
  --build-dir /tmp/admin-shutdown-coverage)"
clang -std=c17 -Wall -Wextra -Werror \
  -fprofile-instr-generate -fcoverage-mapping -fprofile-update=atomic -DATRINIK_TESTING \
  --rtlib=compiler-rt --unwindlib=libunwind -fuse-ld=lld \
  -shared -fPIC -I/workspace/server/src/include \
  /workspace/server/src/server/admin_shutdown.c -o admin_shutdown.so
python3 /workspace/server/src/tests/admin_shutdown_socket.py \
  ./admin_shutdown.so /run/atrinik-admin-tests >&2
python3 /workspace/tools/ci/llvm_coverage.py export \
  --build-dir /tmp/admin-shutdown-coverage --source-root /workspace \
  --component server --compiler clang --output -
