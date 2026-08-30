from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile


MODULE_PATH = Path(__file__).resolve().parents[1] / "release" / "compose_windows_review_bundle.py"
SPEC = importlib.util.spec_from_file_location("compose_windows_review_bundle", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
bundle = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bundle)


class ComposeWindowsReviewBundleTests(unittest.TestCase):
    revision = "a" * 40

    def make_zip(self, path: Path, members: dict[str, bytes]) -> None:
        with zipfile.ZipFile(path, "w") as archive:
            for name, data in members.items():
                archive.writestr(name, data)

    def packages(self, root: Path) -> tuple[Path, Path]:
        client = root / "client.zip"
        server = root / "server.zip"
        revision = self.revision.encode()
        self.make_zip(
            client,
            {
                "client/atrinik.exe": b"client " + revision,
                "client/SDL3.dll": b"shared",
                "client/libffi-8.dll": b"client-superset",
                "client/ca-bundle.crt": b"client-certs",
                "client/README.md": b"client readme",
            },
        )
        self.make_zip(
            server,
            {
                "package/server/atrinik-server.exe": b"server " + revision,
                "package/server/python.exe": b"python",
                "package/server/SDL3.dll": b"shared",
                "package/server/libffi-8.dll": b"server",
                "package/server/ca-bundle.crt": b"server-certs",
                "package/server/install_data/maps/emergency": b"map",
                "package/server/README.md": b"server readme",
                "package/server/server.bat": b"legacy launcher",
            },
        )
        return client, server

    def test_composes_flat_deterministic_review_bundle(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            client, server = self.packages(root)
            first = root / "first.zip"
            second = root / "second.zip"
            bundle.compose(client, server, first, self.revision)
            bundle.compose(client, server, second, self.revision)
            self.assertEqual(first.read_bytes(), second.read_bytes())

            with zipfile.ZipFile(first) as archive:
                names = archive.namelist()
                prefix = f"atrinik-classic-issue-477-windows-one-click-{self.revision[:7]}/"
                self.assertTrue(all(name.startswith(prefix) for name in names))
                relative = [name.removeprefix(prefix) for name in names]
                self.assertNotIn("client", {Path(name).parts[0] for name in relative})
                self.assertNotIn("server", {Path(name).parts[0] for name in relative})
                self.assertEqual(
                    [name for name in relative if name.endswith(".bat")],
                    ["run-review.bat"],
                )
                self.assertEqual(archive.read(prefix + "libffi-8.dll"), b"client-superset")
                powershell = archive.read(prefix + "run-review.ps1").decode()
                for token in (
                    "server-data-stage-",
                    "Get-NetUDPEndpoint -LocalPort 1731",
                    "OwningProcess -eq $Server.Id",
                    "--connect=127.0.0.1",
                    '"--reconnect"',
                ):
                    self.assertIn(token, powershell)
                manifest = json.loads(archive.read(prefix + "BUNDLE-MANIFEST.json"))
                self.assertEqual(manifest["revision"], self.revision)
                self.assertEqual(manifest["udp_port"], 1731)

    def test_rejects_path_traversal(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            client, server = self.packages(root)
            self.make_zip(client, {"client/../atrinik.exe": self.revision.encode()})
            with self.assertRaisesRegex(bundle.BundleError, "unsafe ZIP member"):
                bundle.compose(client, server, root / "out.zip", self.revision)

    def test_rejects_different_unapproved_collision(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            client, server = self.packages(root)
            self.make_zip(
                server,
                {
                    "package/server/atrinik-server.exe": b"server "
                    + self.revision.encode(),
                    "package/server/python.exe": b"python",
                    "package/server/SDL3.dll": b"different",
                    "package/server/install_data/maps/emergency": b"map",
                },
            )
            with self.assertRaises(bundle.BundleError):
                bundle.compose(client, server, root / "out.zip", self.revision)

    def test_rejects_production_test_marker(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            client, server = self.packages(root)
            with zipfile.ZipFile(client, "w") as archive:
                archive.writestr(
                    "client/atrinik.exe",
                    self.revision.encode() + b" --gpu-player-view",
                )
            with self.assertRaisesRegex(bundle.BundleError, "test marker"):
                bundle.compose(client, server, root / "out.zip", self.revision)


if __name__ == "__main__":
    unittest.main()
