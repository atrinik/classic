#!/usr/bin/env bash
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later

# Run only in the isolated CI worker: sources are read-only, and both writable
# directories are disposable tmpfs mounts. stdout is exclusively Cobertura XML.
set -euo pipefail

mkdir /tmp/admin-shutdown-coverage
cd /tmp/admin-shutdown-coverage
gcc -std=c17 -Wall -Wextra -Werror --coverage -DATRINIK_TESTING \
  -shared -fPIC -I/workspace/server/src/include \
  /workspace/server/src/server/admin_shutdown.c -o admin_shutdown.so
python3 /workspace/server/src/tests/admin_shutdown_socket.py \
  ./admin_shutdown.so /run/atrinik-admin-tests >&2
gcovr --root /workspace --filter '/workspace/server/src/server/admin_shutdown\.c$' \
  --xml - .
