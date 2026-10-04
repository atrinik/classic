#!/usr/bin/env python3
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run AVI writer checks with a private runtime read-only stream fixture."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main() -> int:
    executable = Path(sys.argv[1]).resolve(strict=True)
    # __FILE__ may name a synthetic reproducible-build prefix. Create the
    # fixture at runtime and close its handle before the C test reopens it rb.
    with tempfile.TemporaryDirectory(prefix="atrinik-video-avi-") as directory:
        descriptor, fixture = tempfile.mkstemp(dir=directory, prefix="readonly-")
        os.close(descriptor)
        return subprocess.run(
            [os.fspath(executable), fixture],
            check=False,
            timeout=30,
        ).returncode


if __name__ == "__main__":
    raise SystemExit(main())
