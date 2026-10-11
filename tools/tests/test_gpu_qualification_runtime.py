from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from tools.ci import run_gpu_qualification as runtime


class QualificationRuntimeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        (self.root / "atrinik_workspace").mkdir()
        (self.root / "atrinik_workspace/linux_platform.py").touch()
        executable = self.root / "client/build/gpu-qualification/atrinik"
        executable.parent.mkdir(parents=True)
        executable.touch(mode=0o700)
        self.environment = {
            "ATRINIK_QUALIFICATION_WORKSPACE_ROOT": str(self.root),
            "ATRINIK_QUALIFICATION_GPU_KIND": "mesa",
            "ATRINIK_QUALIFICATION_RENDER_DEVICE": "/dev/dri/renderD140",
            "CLASSIC_LINUX_RUNTIME_IMAGE": "ghcr.io/atrinik/linux-build:qualified@sha256:" + "a" * 64,
            "ATRINIK_GPU_CONFORMANCE_OUTPUT": "../build/gpu-qualification/row.jsonl",
            "GITHUB_TOKEN": "must-not-enter-container",
        }
        self.report = {
            "schema_version": 1, "delivery_authority": False, "hardware_qualified": False,
            "docker_arguments": ["--user", "123:456", "--device", "/dev/dri/renderD140"],
        }

    def test_actual_session_arguments_and_only_explicit_environment_are_forwarded(self) -> None:
        with patch.dict(os.environ, self.environment, clear=True), patch.object(
            runtime.subprocess, "run",
            side_effect=[SimpleNamespace(stdout=json.dumps(self.report)), SimpleNamespace(returncode=7)],
        ) as run:
            result = runtime.main(["--source-root", str(self.root), "--", "atrinik", "--gpu-player-view"])
        self.assertEqual(result, 7)
        probe = run.call_args_list[0].args[0]
        self.assertIn("atrinik_workspace.linux_platform", probe)
        self.assertEqual(probe[-2:], ["--render-device", "/dev/dri/renderD140"])
        arguments = run.call_args_list[1].args[0]
        self.assertIn("/dev/dri/renderD140", arguments)
        self.assertIn("ATRINIK_GPU_CONFORMANCE_OUTPUT=../build/gpu-qualification/row.jsonl", arguments)
        self.assertIn("ATRINIK_CONFIG_DIR=/tmp/classic-home/config", arguments)
        self.assertNotIn("GITHUB_TOKEN=must-not-enter-container", arguments)
        self.assertFalse(run.call_args_list[1].kwargs.get("shell", False))

    def test_failed_session_probe_never_launches_docker(self) -> None:
        with patch.dict(os.environ, self.environment, clear=True), patch.object(
            runtime.subprocess, "run", side_effect=subprocess.CalledProcessError(2, "probe"),
        ) as run:
            self.assertEqual(runtime.main(["--source-root", str(self.root), "--", "atrinik"]), 2)
        self.assertEqual(run.call_count, 1)

    def test_report_cannot_claim_authority_or_hardware_acceptance(self) -> None:
        for field in ("delivery_authority", "hardware_qualified"):
            report = {**self.report, field: True}
            with self.subTest(field=field), patch.dict(os.environ, self.environment, clear=True), patch.object(
                runtime.subprocess, "run", return_value=SimpleNamespace(stdout=json.dumps(report)),
            ) as run:
                self.assertEqual(runtime.main(["--source-root", str(self.root), "--", "atrinik"]), 2)
                self.assertEqual(run.call_count, 1)

    def test_missing_assigned_render_device_has_no_default(self) -> None:
        del self.environment["ATRINIK_QUALIFICATION_RENDER_DEVICE"]
        with patch.dict(os.environ, self.environment, clear=True), patch.object(runtime.subprocess, "run") as run:
            self.assertEqual(runtime.main(["--source-root", str(self.root), "--", "atrinik"]), 2)
        run.assert_not_called()

    def test_floating_image_is_rejected_before_any_probe(self) -> None:
        self.environment["CLASSIC_LINUX_RUNTIME_IMAGE"] = "ghcr.io/atrinik/linux-build:latest"
        with patch.dict(os.environ, self.environment, clear=True), patch.object(runtime.subprocess, "run") as run:
            self.assertEqual(runtime.main(["--source-root", str(self.root), "--", "atrinik"]), 2)
        run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
