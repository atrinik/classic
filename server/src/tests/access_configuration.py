#!/usr/bin/env python3
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Invalid admission configuration must fail before state/listener startup."""
from pathlib import Path
import subprocess
import sys
import tempfile

binary = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="atrinik-access-config-") as temporary:
    root = Path(temporary)
    (root / "server.cfg").write_text("")
    cases = [
        (["--access_required=</absent-access-config", "--access_required=false"], ""),
        ([], "access_required = </absent-access-config\naccess_required = false\n"),
        ([], "access_required tru\naccess_required = false\n"),
        (["--access_required=tru", "--access_required=false"], ""),
        (["--access_initialize=maybe", "--access_initialize=false"], ""),
        (["--access_store=relative", "--access_store=/absent-fixture-store"], ""),
        (["--access_admin_accounts=relative", "--access_admin_accounts=/absent-fixture-list"], ""),
        ([], "access_required = tru\naccess_required = false\n"),
        ([], "access_initialize = maybe\naccess_initialize = false\n"),
        ([], "access_store = relative\n"),
        ([], "access_admin_accounts = relative\n"),
    ]
    for arguments, configuration in cases:
        (root / "server-custom.cfg").write_text(configuration)
        result = subprocess.run([binary, *arguments], cwd=root, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=10, check=False)
        assert result.returncode != 0, arguments
        assert b"Invalid access configuration; refusing game startup" in result.stdout, result.stdout
        assert not (root / "data").exists()
    (root / "server-custom.cfg").unlink()
    for filename in ("server-custom.cfg", "server.cfg"):
        configuration = root / filename
        if configuration.exists():
            configuration.unlink()
        configuration.mkdir()
        result = subprocess.run([binary], cwd=root, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=10, check=False)
        assert result.returncode != 0
        assert b"Cannot read" in result.stdout and b"refusing game startup" in result.stdout
        assert not (root / "data").exists()
        configuration.rmdir()
print("PASS sticky invalid access configuration rejects CLI/config overrides before state startup")
