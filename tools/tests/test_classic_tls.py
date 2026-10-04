from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

from tools.ci import bootstrap_classic_tls as tls

ROOT = Path(__file__).resolve().parents[2]


class ClassicTlsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.prefix = Path(self.temporary.name) / "prefix"
        self.inputs = {
            "linux/tls/build.py": b"# recipe fixture\n",
            "linux/tls/manifest.json": b'{"fixture": true}\n',
            "tools/install_classic_shader_toolchain.py": b"# helper fixture\n",
        }
        self.lock = dict(tls.load_lock(), prefix=str(self.prefix), files={
            name: hashlib.sha256(value).hexdigest()
            for name, value in self.inputs.items()
        })

    def retrieve(self, url: str) -> bytes:
        for name, data in self.inputs.items():
            if url.endswith("/" + name):
                return data
        self.fail("unexpected producer URL")

    def populate(self, *args, **kwargs) -> None:
        for name in tls.REQUIRED_FILES:
            path = self.prefix / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"fixture")
        manifest = self.prefix / "share/atrinik/classic-tls.json"
        manifest.parent.mkdir(parents=True, exist_ok=True)
        manifest.write_bytes(self.inputs["linux/tls/manifest.json"])

    def test_immutable_producer_lock(self) -> None:
        lock = tls.load_lock()
        self.assertEqual(lock["revision"], "31e46c2dd30fdb7121257628384e9edb011a84a4")
        self.assertEqual(lock["prefix"], "/opt/atrinik/tls")
        self.assertEqual(lock["files"]["linux/tls/manifest.json"],
                         "aa4fe805c81aefef8a784ac840b37ee95733a88cfd5b885ca5cf11f8758aa90c")
        for field, value in (("revision", "main"), ("repository", "other/repo"),
                             ("prefix", "/usr")):
            path = Path(self.temporary.name) / "invalid.json"
            path.write_text(json.dumps(dict(lock, **{field: value})))
            with self.subTest(field=field), self.assertRaises(tls.BootstrapError):
                tls.load_lock(path)

    def test_checksum_failure_executes_no_recipe(self) -> None:
        self.lock["files"]["tools/install_classic_shader_toolchain.py"] = "0" * 64
        with mock.patch.object(tls, "retrieve", side_effect=self.retrieve), \
                mock.patch.object(tls.subprocess, "run") as run:
            with self.assertRaisesRegex(tls.BootstrapError, "checksum"):
                tls.install(self.lock)
            run.assert_not_called()
        self.assertFalse(self.prefix.exists())

    def test_download_failure_executes_no_recipe(self) -> None:
        with mock.patch.object(tls, "retrieve", side_effect=OSError("unavailable")), \
                mock.patch.object(tls.subprocess, "run") as run:
            with self.assertRaises(OSError):
                tls.install(self.lock)
            run.assert_not_called()

    def test_recipe_failure_propagates(self) -> None:
        with mock.patch.object(tls, "retrieve", side_effect=self.retrieve), \
                mock.patch.object(tls.subprocess, "run",
                                  side_effect=subprocess.CalledProcessError(7, "recipe")):
            with self.assertRaises(subprocess.CalledProcessError):
                tls.install(self.lock)
        self.assertFalse(self.prefix.exists())

    def test_verified_recipe_is_colocated_and_system_tls_environment_preserved(self) -> None:
        overrides = {name: "host-provider" for name in (
            "LD_LIBRARY_PATH", "OPENSSL_CONF", "OPENSSL_MODULES",
            "CMAKE_PREFIX_PATH", "PKG_CONFIG_PATH", "PYTHONPATH")}
        overrides["PATH"] = os.environ["PATH"]
        def build(command, **kwargs):
            recipe = Path(command[1])
            for name, value in self.inputs.items():
                self.assertEqual((recipe.parent / Path(name).name).read_bytes(), value)
            self.assertIn("--manifest", command)
            self.assertEqual(command[-2:], ["--jobs", "2"])
            self.assertTrue(kwargs["check"])
            for name in overrides:
                if name != "PATH":
                    self.assertNotIn(name, kwargs["env"])
            self.assertEqual(kwargs["env"]["PATH"], overrides["PATH"])
            self.populate()
        with mock.patch.dict(os.environ, overrides), \
                mock.patch.object(tls, "retrieve", side_effect=self.retrieve), \
                mock.patch.object(tls.subprocess, "run", side_effect=build):
            tls.install(self.lock)
            for name, value in overrides.items():
                self.assertEqual(os.environ[name], value)
        tls.validate_prefix(self.prefix, self.lock)

    def test_absent_or_incomplete_prefix_cannot_be_consumed(self) -> None:
        tls.require_empty_prefix(self.prefix)
        with self.assertRaises(tls.BootstrapError):
            tls.validate_prefix(self.prefix, self.lock)
        self.populate()
        (self.prefix / "lib/libssl.so.4").unlink()
        with self.assertRaisesRegex(tls.BootstrapError, "incomplete"):
            tls.validate_prefix(self.prefix, self.lock)

    def test_existing_prefix_is_never_adopted_or_overwritten(self) -> None:
        self.populate()
        with mock.patch.object(tls, "retrieve") as retrieve:
            with self.assertRaisesRegex(tls.BootstrapError, "overwrite or adopt"):
                tls.install(self.lock)
            retrieve.assert_not_called()
        tls.validate_prefix(self.prefix, self.lock)

    def test_manifest_mismatch_and_escaping_library_symlink_fail_closed(self) -> None:
        self.populate()
        manifest = self.prefix / "share/atrinik/classic-tls.json"
        manifest.write_text("different cohort")
        with self.assertRaisesRegex(tls.BootstrapError, "checksum"):
            tls.validate_prefix(self.prefix, self.lock)
        manifest.write_bytes(self.inputs["linux/tls/manifest.json"])
        library = self.prefix / "lib/libssl.so.4"
        library.unlink()
        library.symlink_to(__file__)
        with self.assertRaisesRegex(tls.BootstrapError, "incomplete"):
            tls.validate_prefix(self.prefix, self.lock)

    def test_symlink_prefix_or_parent_cannot_be_populated(self) -> None:
        actual = Path(self.temporary.name) / "actual"
        actual.mkdir()
        self.prefix.symlink_to(actual, target_is_directory=True)
        for prefix in (self.prefix, self.prefix / "child"):
            with self.subTest(prefix=prefix), self.assertRaises(tls.BootstrapError):
                tls.require_empty_prefix(prefix)

    def test_linux_workflows_mount_private_prefix_for_every_native_runner(self) -> None:
        for name in ("check.yml", "pr-benchmarks.yml", "daily-client-performance.yml"):
            text = (ROOT / ".github/workflows" / name).read_text()
            invocations = [line for line in text.splitlines()
                           if "tools/ci/run_linux_check.sh " in line and "--material" not in line]
            with self.subTest(workflow=name):
                self.assertEqual(text.count('"${RUNNER_TEMP}/classic-tls:/opt/atrinik/tls:ro"'),
                                 len(invocations))
                self.assertEqual(text.count("name: Bootstrap the immutable private Classic TLS cohort"),
                                 len(invocations))
                self.assertEqual(text.count("--env ATRINIK_CLASSIC_TLS_PREFIX=/opt/atrinik/tls"),
                                 len(invocations))
                self.assertNotIn("--env LD_LIBRARY_PATH", text)
                self.assertNotIn("--env OPENSSL_MODULES", text)
        check = (ROOT / ".github/workflows/check.yml").read_text()
        self.assertIn("classic-build:1.2.3@sha256:d0ec0a31f97fa1d699f62b81bbe697d95b335f44f1c99fde8704dfc528e2102f", check)
        runner = (ROOT / "tools/ci/run_linux_check.sh").read_text()
        self.assertLess(runner.index('bootstrap_classic_tls.py" --verify-only'),
                        runner.index('cmake -S'))
        self.assertIn("export ATRINIK_CLASSIC_TLS_PREFIX=/opt/atrinik/tls", runner)
        gpu = (ROOT / "tools/ci/run_gpu_coverage.sh").read_text()
        self.assertEqual(gpu.count('"${tls_directory}:/opt/atrinik/tls:ro"'), 3)
        self.assertIn('"${source_root}/tools/ci/prepare_classic_tls.sh"', gpu)
        candidate = (ROOT / ".github/workflows/build-release-candidate.yml").read_text()
        self.assertIn("sudo python3 tools/ci/bootstrap_classic_tls.py --jobs 2", candidate)
        self.assertIn("ATRINIK_CLASSIC_TLS_PREFIX: /opt/atrinik/tls", candidate)
        windows = candidate[candidate.index("  client-windows:"):candidate.index("  server-image:")]
        self.assertNotIn("ATRINIK_CLASSIC_TLS_PREFIX", windows)

    def test_server_image_shares_recipe_and_copies_complete_root_owned_prefix(self) -> None:
        dockerfile = (ROOT / "server/Dockerfile").read_text()
        self.assertEqual(dockerfile.count("ubuntu@sha256:da6fc2be547864451aa253836dd926da33623312df4a9a243e35dc877c378a78"), 3)
        self.assertIn(" AS tls", dockerfile)
        self.assertIn("RUN python3 tools/ci/bootstrap_classic_tls.py --jobs 2", dockerfile)
        self.assertEqual(dockerfile.count("COPY --from=tls /opt/atrinik/tls /opt/atrinik/tls"), 2)
        self.assertIn("chown -R atrinik:atrinik /opt/atrinik/server /opt/atrinik/maps", dockerfile)
        self.assertNotIn("chown -R atrinik:atrinik /opt/atrinik\n", dockerfile)
        self.assertIn("libssl3t64 python3", dockerfile)
        for name in ("LD_LIBRARY_PATH", "OPENSSL_MODULES", "OPENSSL_CONF"):
            self.assertNotIn(name + "=", dockerfile)
        dockerignore = (ROOT / ".dockerignore").read_text()
        for name in ("bootstrap_classic_tls.py", "classic_tls.lock.json"):
            self.assertIn("!tools/ci/" + name, dockerignore)


if __name__ == "__main__":
    unittest.main()
