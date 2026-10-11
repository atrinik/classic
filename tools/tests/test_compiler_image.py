from __future__ import annotations

import json
import os
import shutil
from pathlib import Path
import subprocess
import runpy
import tempfile
import textwrap
import unittest

from tools.release.resolve_compiler_image import LOCK, resolve_descriptor, resolve_image


OLD_IMAGE = "ghcr.io/atrinik/classic-build:2.0.0@sha256:" + "a" * 64
CURRENT_IMAGE = "ghcr.io/atrinik/classic-build:2.1.0@sha256:" + "b" * 64


class CompilerImageTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.git("init", "--quiet")
        self.git("config", "user.name", "Compiler image fixture")
        self.git("config", "user.email", "fixture@example.invalid")

    def git(self, *arguments: str) -> str:
        return subprocess.run(["git", "-C", str(self.root), *arguments], check=True,
                              capture_output=True, text=True).stdout.strip()

    def commit(self, lock: object) -> str:
        path = self.root / LOCK
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(lock))
        self.git("add", LOCK)
        self.git("-c", "commit.gpgsign=false", "commit", "--quiet", "-m", "fixture")
        return self.git("rev-parse", "HEAD")

    def test_old_llvm_recovery_uses_tagged_lock_not_newer_pin_or_dirty_checkout(self) -> None:
        old_revision = self.commit({"compiler": {"image": OLD_IMAGE}})
        self.commit({"compiler": {"image": CURRENT_IMAGE}})
        (self.root / LOCK).write_text('{"compiler":{"image":"malicious-floating-image"}}')
        self.assertEqual(resolve_image(self.root, old_revision, CURRENT_IMAGE), OLD_IMAGE)

    def test_new_llvm_release_uses_current_locked_image(self) -> None:
        revision = self.commit({"compiler": {"image": CURRENT_IMAGE}})
        self.assertEqual(resolve_image(self.root, revision, CURRENT_IMAGE), CURRENT_IMAGE)

    def test_preparation_descriptor_is_exact_tagged_identity_data(self) -> None:
        compiler = {"image": OLD_IMAGE, "version": "23.1.3", "tools": ["clang", "ld.lld"]}
        revision = self.commit({"compiler": compiler, "sources": [{"url": "candidate-not-used"}]})
        self.commit({"compiler": {"image": CURRENT_IMAGE}})
        self.assertEqual(resolve_descriptor(self.root, revision, CURRENT_IMAGE), compiler)
        resolver = Path(__file__).resolve().parents[2] / "tools/release/resolve_compiler_image.py"
        output = self.root / "descriptor.json"
        subprocess.run(["python3", str(resolver), "--source-root", str(self.root),
                        "--revision", revision, "--current-image", CURRENT_IMAGE,
                        "--source-packaging-lock-output", str(output)], check=True, capture_output=True)
        self.assertEqual(output.read_bytes(), subprocess.check_output(
            ["git", "-C", str(self.root), "show", revision + ":" + LOCK]))
        self.assertEqual(json.loads(output.read_text())["compiler"], compiler)
        historical = self.commit({})
        subprocess.run(["python3", str(resolver), "--source-root", str(self.root),
                        "--revision", historical, "--current-image", CURRENT_IMAGE,
                        "--source-packaging-lock-output", str(output)], check=True, capture_output=True)
        self.assertEqual(output.read_bytes(), b"{}")
        self.git("rm", LOCK)
        self.git("-c", "commit.gpgsign=false", "commit", "--quiet", "-m", "no lock")
        subprocess.run(["python3", str(resolver), "--source-root", str(self.root),
                        "--revision", self.git("rev-parse", "HEAD"), "--current-image", CURRENT_IMAGE,
                        "--source-packaging-lock-output", str(output)], check=True, capture_output=True)
        self.assertEqual(output.read_bytes(), b"")

    def test_historical_assembler_accepts_exported_original_lock_and_rejects_new_lock(self) -> None:
        root = Path(__file__).resolve().parents[2]
        baseline = "fd4943252ac872fce2995c286b10a0c6d249aba3"
        source = self.root / "historical/tools/ci/appimage"
        source.mkdir(parents=True)
        for name in ("assemble.py", "packaging.lock.json"):
            content = subprocess.check_output(
                ["git", "-C", str(root), "show", baseline + ":tools/ci/appimage/" + name])
            (source / name).write_bytes(content)
        resolver = root / "tools/release/resolve_compiler_image.py"
        prefix = self.root / "prefix"
        prefix.mkdir()
        exported = prefix / "packaging.lock.json"
        subprocess.run(["python3", str(resolver), "--source-root", str(root),
                        "--revision", baseline, "--current-image", CURRENT_IMAGE,
                        "--source-packaging-lock-output", str(exported)], check=True, capture_output=True)
        self.assertEqual(exported.read_bytes(), (source / "packaging.lock.json").read_bytes())
        historical = runpy.run_path(str(source / "assemble.py"))
        appdir = self.root / "appdir"
        (appdir / "usr/share/doc").mkdir(parents=True)
        # The actual historical assembler passes its lock gate and reaches the
        # first omitted payload fixture. No application build is needed.
        with self.assertRaises(FileNotFoundError) as reached_payload:
            historical["assemble"](appdir, prefix, "5.0.0", baseline)
        self.assertIn("AppRun", str(reached_payload.exception))
        altered = json.loads(exported.read_bytes())
        altered["compiler"] = {"image": CURRENT_IMAGE}
        exported.write_text(json.dumps(altered))
        with self.assertRaisesRegex(ValueError, "packaging lock differs from source lock"):
            historical["assemble"](self.root / "rejected-appdir", prefix, "5.0.0", baseline)
        self.assertFalse((self.root / "rejected-appdir").exists())

    def test_historical_gcc_descriptor_absence_uses_approved_current_pin(self) -> None:
        revision = self.commit({"base_image": "historical-sdk"})
        self.assertEqual(resolve_image(self.root, revision, CURRENT_IMAGE), CURRENT_IMAGE)

    def test_existing_llvm_descriptor_never_falls_back(self) -> None:
        for compiler in ({}, {"image": "ghcr.io/atrinik/classic-build:latest"}, None):
            with self.subTest(compiler=compiler):
                revision = self.commit({"compiler": compiler})
                with self.assertRaisesRegex(ValueError, "compiler (image|descriptor)"):
                    resolve_image(self.root, revision, CURRENT_IMAGE)

    def test_ambiguous_or_nonregular_tagged_lock_is_rejected(self) -> None:
        path = self.root / LOCK
        path.parent.mkdir(parents=True)
        path.write_text('{"compiler":{"image":"' + OLD_IMAGE +
                        '"},"compiler":{"image":"' + CURRENT_IMAGE + '"}}')
        self.git("add", LOCK)
        self.git("-c", "commit.gpgsign=false", "commit", "--quiet", "-m", "duplicate")
        with self.assertRaisesRegex(ValueError, "duplicate source lock key"):
            resolve_image(self.root, self.git("rev-parse", "HEAD"), CURRENT_IMAGE)
        path.unlink()
        path.symlink_to("outside-lock.json")
        self.git("add", LOCK)
        self.git("-c", "commit.gpgsign=false", "commit", "--quiet", "-m", "symlink")
        with self.assertRaisesRegex(ValueError, "regular Git blob"):
            resolve_image(self.root, self.git("rev-parse", "HEAD"), CURRENT_IMAGE)

    def test_historical_finalizer_workflow_gets_no_unsupported_compiler_argument(self) -> None:
        revision = self.commit({"base_image": "historical-sdk"})
        source_root = Path(__file__).resolve().parents[2]
        workflow = (source_root / ".github/workflows/build-release-candidate.yml").read_text()
        step = workflow.split("      - name: Validate and index the complete candidate", 1)[1].split("      - uses:", 1)[0]
        script = textwrap.dedent(step.split("        run: |\n", 1)[1])
        trusted = self.root / "build/ci-automation/tools/release"
        trusted.mkdir(parents=True)
        shutil.copy2(source_root / "tools/release/resolve_compiler_image.py", trusted)
        historical = self.root / "tools/release"
        historical.mkdir(parents=True)
        (historical / "finalize_artifacts.py").write_text("""import argparse, json, pathlib, sys
parser = argparse.ArgumentParser()
for name in ('version', 'revision', 'source-epoch', 'directory'):
    parser.add_argument('--' + name, required=True)
parser.parse_args()
pathlib.Path('received-arguments.json').write_text(json.dumps(sys.argv[1:]))
""")
        result = subprocess.run(["bash", "-euo", "pipefail", "-c", script],
            cwd=self.root, capture_output=True, text=True,
            env={**os.environ, "GITHUB_WORKSPACE": str(self.root), "RUNNER_TEMP": str(self.root),
                 "RELEASE_VERSION": "5.1.0", "RELEASE_REVISION": revision,
                 "RELEASE_SOURCE_EPOCH": "42", "CLASSIC_LINUX_IMAGE": CURRENT_IMAGE})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("--compiler-image", json.loads((self.root / "received-arguments.json").read_text()))
        # The same historical fixture cannot bypass a partial LLVM descriptor.
        revision = self.commit({"compiler": {"version": "23.1.3"}})
        (self.root / "received-arguments.json").unlink()
        environment = {**os.environ, "GITHUB_WORKSPACE": str(self.root), "RUNNER_TEMP": str(self.root),
                       "RELEASE_VERSION": "5.1.0", "RELEASE_REVISION": revision,
                       "RELEASE_SOURCE_EPOCH": "42", "CLASSIC_LINUX_IMAGE": CURRENT_IMAGE}
        rejected = subprocess.run(["bash", "-euo", "pipefail", "-c", script],
                                  cwd=self.root, capture_output=True, text=True, env=environment)
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn("LLVM release lock requires its immutable compiler image", rejected.stderr)
        self.assertFalse((self.root / "received-arguments.json").exists())

    def test_branch_selector_and_unqualified_current_image_are_rejected(self) -> None:
        revision = self.commit({})
        with self.assertRaisesRegex(ValueError, "exact lowercase"):
            resolve_image(self.root, "HEAD", CURRENT_IMAGE)
        with self.assertRaisesRegex(ValueError, "immutable published"):
            resolve_image(self.root, revision, "ghcr.io/atrinik/classic-build:latest")


if __name__ == "__main__":
    unittest.main()
