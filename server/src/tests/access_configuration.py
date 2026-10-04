#!/usr/bin/env python3
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Invalid admission configuration must fail before state/listener startup."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

binary = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="atrinik-access-config-") as temporary:
    root = Path(temporary)
    (root / "server.cfg").write_text("")
    cases = [
        (["--control_allowed_ips=none"], ""),
        ([], "control_allowed_ips = none\n"),
        (["--access_required=</absent-access-config", "--access_required=false"], ""),
        ([], "access_required = </absent-access-config\naccess_required = false\n"),
        ([], "access_required tru\naccess_required = false\n"),
        (["--access_required=tru", "--access_required=false"], ""),
        (["--access_initialize=maybe", "--access_initialize=false"], ""),
        (["--access_store=relative", "--access_store=/absent-fixture-store"], ""),
        ([], "access_required = tru\naccess_required = false\n"),
        ([], "access_initialize = maybe\naccess_initialize = false\n"),
        ([], "access_store = relative\n"),
    ]
    for arguments, configuration in cases:
        (root / "server-custom.cfg").write_text(configuration)
        result = subprocess.run([binary, *arguments], cwd=root, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=10, check=False)
        assert result.returncode != 0, arguments
        assert b"Invalid access configuration; refusing game startup" in result.stdout, result.stdout
        assert not (root / "data").exists()
    # Removed account authorization must fail closed in both parser paths.
    obsolete_cases = [
        (["--access_admin_accounts=/obsolete-account-list"], "", b"Unknown option"),
        ([], "access_admin_accounts = /obsolete-account-list\n",
         b"Invalid access configuration; refusing game startup"),
    ]
    for arguments, configuration, message in obsolete_cases:
        (root / "server-custom.cfg").write_text(configuration)
        result = subprocess.run([binary, *arguments], cwd=root, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=10, check=False)
        assert result.returncode != 0, arguments
        assert message in result.stdout, result.stdout
        assert not (root / "data").exists()
    (root / "descriptor-value").write_text("7")
    (root / "nested.cfg").write_text("datapath_fd = 7\n")
    (root / "outer.cfg").write_text("config = nested.cfg\n")
    descriptor_cases = [(["--datapath_fd=" + value], "") for value in
                        ("", "0", "2", "03", "-3", "+3", "3x", "2147483648")]
    descriptor_cases += [
        (["--datapath_fd=7", "--datapath_fd=7"], ""),
        (["--datapath_fd=<descriptor-value"], ""),
        (["--config=nested.cfg"], ""),
        (["--config=outer.cfg"], ""),
        ([], "datapath_fd = 7\n"),
        (["--datapath_fd=2147483647", "--datapath=/proc/self/fd/2147483647"], ""),
        (["--datapath_fd=7", "--datapath=./data"], ""),
        (["--datapath_fd=7", "--datapath=/proc/self/fd/7", "--access_initialize=true"], ""),
    ]
    for arguments, configuration in descriptor_cases:
        (root / "server-custom.cfg").write_text(configuration)
        result = subprocess.run([binary, *arguments], cwd=root, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=10, check=False)
        assert result.returncode != 0, arguments
        assert not (root / "data").exists()
        assert any(message in result.stdout for message in
                   (b"Invalid access configuration", b"Invalid inherited data descriptor",
                    b"Cannot retain inherited private server state", b"requires an argument")), result.stdout
    (root / "server-custom.cfg").write_text("")
    for filename, mode in (("not-directory", 0o600), ("public-directory", 0o755)):
        target = root / filename
        if mode == 0o600:
            target.write_text("")
        else:
            target.mkdir(mode=mode)
        descriptor = os.open(target, os.O_RDONLY)
        try:
            (root / "data").symlink_to(f"/proc/self/fd/{descriptor}")
            result = subprocess.run(
                [binary, f"--datapath_fd={descriptor}", "--datapath=./data"],
                cwd=root, pass_fds=(descriptor,), stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, timeout=10, check=False)
            assert result.returncode != 0
            assert b"Invalid inherited data descriptor" in result.stdout, result.stdout
        finally:
            (root / "data").unlink()
            os.close(descriptor)
    private_state = root / "private-state"
    private_state.mkdir(mode=0o700)
    descriptor = os.open(private_state, os.O_RDONLY | os.O_DIRECTORY)
    try:
        (root / "data").symlink_to(f"/proc/self/fd/{descriptor}")
        for mode in (0o770, 0o777):
            root.chmod(mode)
            result = subprocess.run(
                [binary, f"--datapath_fd={descriptor}", "--datapath=./data"],
                cwd=root, pass_fds=(descriptor,), stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, timeout=10, check=False)
            assert result.returncode != 0
            assert b"Invalid inherited data descriptor generation directory" in result.stdout, result.stdout
            assert not list(private_state.iterdir())
    finally:
        root.chmod(0o700)
        (root / "data").unlink()
        os.close(descriptor)
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
