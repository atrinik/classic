from __future__ import annotations

import json
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import tempfile
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[2]
SELECTOR = ROOT / "tools/ci/debian-client/historical_producer.py"
PRODUCER = runpy.run_path(str(SELECTOR))
BASELINE = "994e6fdc20da962be9c11055bda28ef5561a1dea"


class HistoricalDebianProducerTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.workspace = Path(temporary.name)
        self.source = self.workspace / "candidate-source"
        self.source.mkdir()
        self.git("init", "--quiet")
        self.git("config", "user.name", "Historical producer fixture")
        self.git("config", "user.email", "fixture@example.invalid")
        for name in ("Dockerfile.historical", "historical_producer.py"):
            target = self.workspace / "tools/ci/debian-client" / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(SELECTOR.parent / name, target)
        self.path = self.source / PRODUCER["SOURCE_PRODUCER"]
        self.path.parent.mkdir(parents=True)
        self.path.write_bytes((ROOT / PRODUCER["TRUSTED_PRODUCER"]).read_bytes())
        self.revision = self.commit()

    def git(self, *args: str) -> str:
        return subprocess.check_output(["git", "-C", str(self.source), *args], text=True).strip()

    def commit(self) -> str:
        self.git("add", ".")
        self.git("-c", "commit.gpgsign=false", "commit", "--quiet", "-m", "fixture")
        return self.git("rev-parse", "HEAD")

    def resolve(self, revision: str | None = None) -> str:
        return PRODUCER["resolve_producer"](self.source, revision or self.revision, self.workspace)

    def test_exact_original_source_producer_is_retained(self) -> None:
        original = subprocess.check_output(["git", "-C", str(ROOT), "show",
            BASELINE + ":" + PRODUCER["SOURCE_PRODUCER"]])
        retained = (ROOT / PRODUCER["TRUSTED_PRODUCER"]).read_bytes()
        self.assertEqual(retained, original)
        self.assertIn(b"binutils build-essential", retained)
        self.assertNotIn(b"ATRINIK_COMPILER_IMAGE", retained)
        self.assertNotIn(b"ENV PATH=", retained)
        schema = subprocess.check_output(["python3", str(ROOT / "tools/release/release_artifacts.py"),
            "--print-source-schema", "--revision", BASELINE, "--source-root", str(ROOT)], text=True)
        self.assertEqual(schema.strip(), "2")
        self.assertEqual(self.resolve(), PRODUCER["TRUSTED_PRODUCER"])

    def test_checkout_must_match_the_immutable_selected_source(self) -> None:
        self.path.write_text("FROM unsupported:latest\n")
        with self.assertRaisesRegex(ValueError, "must be clean at the exact release revision"):
            self.resolve()
        newer = self.commit()
        with self.assertRaisesRegex(ValueError, "must be clean at the exact release revision"):
            self.resolve()
        with self.assertRaisesRegex(ValueError, "no approved historical"):
            self.resolve(newer)
        with self.assertRaisesRegex(ValueError, "exact lowercase"):
            self.resolve("HEAD")

    def test_source_symlink_and_missing_producer_fail_closed(self) -> None:
        self.path.unlink()
        self.path.symlink_to("Dockerfile.historical")
        with self.assertRaisesRegex(ValueError, "regular Git blob"):
            self.resolve(self.commit())
        self.path.unlink()
        with self.assertRaisesRegex(ValueError, "regular Git blob"):
            self.resolve(self.commit())

    def test_trusted_producer_drift_and_symlink_fail_closed(self) -> None:
        retained = self.workspace / PRODUCER["TRUSTED_PRODUCER"]
        retained.write_text("FROM unexpected:latest\n")
        with self.assertRaisesRegex(ValueError, "differs from its approved pin"):
            self.resolve()
        retained.unlink()
        retained.symlink_to(self.path)
        with self.assertRaisesRegex(ValueError, "differs from its approved pin"):
            self.resolve()

    def test_workflow_selects_pinned_gcc_and_rejects_unknown_source_before_docker(self) -> None:
        workflow = (ROOT / ".github/workflows/build-release-candidate.yml").read_text()
        step = workflow.split("      - name: Prepare pinned Debian", 1)[1].split("      - name:", 1)[0]
        script = textwrap.dedent(step.split("        run: |\n", 1)[1])
        binary = self.workspace / "bin"
        binary.mkdir()
        docker = binary / "docker"
        docker.write_text("""#!/usr/bin/env python3
import json, pathlib, sys
with pathlib.Path('docker-calls.jsonl').open('a') as output:
    output.write(json.dumps(sys.argv[1:]) + '\\n')
""")
        docker.chmod(0o755)
        environment = dict(os.environ, GITHUB_WORKSPACE=str(self.workspace),
            RELEASE_REVISION=self.revision, CLASSIC_LINUX_IMAGE="must-not-use-current-LLVM",
            PATH=str(binary) + ":" + os.environ["PATH"])
        result = subprocess.run(["bash", "-euo", "pipefail", "-c", script],
            cwd=self.workspace, env=environment, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.workspace / "docker-calls.jsonl"
        builds = [json.loads(line) for line in calls.read_text().splitlines()
                  if json.loads(line)[0] == "build"]
        self.assertEqual(len(builds), 2)
        for build in builds:
            self.assertEqual(build[build.index("--file") + 1], PRODUCER["TRUSTED_PRODUCER"])
            self.assertNotIn("--build-arg", build)
        calls.unlink()
        self.path.write_text("FROM unapproved:latest\n")
        environment["RELEASE_REVISION"] = self.commit()
        rejected = subprocess.run(["bash", "-euo", "pipefail", "-c", script],
            cwd=self.workspace, env=environment, capture_output=True, text=True)
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn("no approved historical Debian producer", rejected.stderr)
        self.assertFalse(calls.exists())


if __name__ == "__main__":
    unittest.main()
