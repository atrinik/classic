from __future__ import annotations

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ENTRYPOINT = (
    Path(__file__).resolve().parents[2]
    / "server"
    / "docker"
    / "server-entrypoint.sh"
)


class ServerContainerEntrypointTests(unittest.TestCase):
    def run_entrypoint(self, maps_path: str | None = None, admin_socket: str | None = None) -> list[str]:
        with tempfile.TemporaryDirectory() as temporary:
            server = Path(temporary)
            (server / "install_data").mkdir()
            (server / "data").mkdir(mode=0o700)
            executable = server / "atrinik-server"
            executable.write_text(
                "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$ATRINIK_TEST_ARGS\"\n",
                encoding="utf-8",
            )
            executable.chmod(0o755)
            arguments = server / "arguments"
            environment = os.environ.copy()
            environment["ATRINIK_TEST_ARGS"] = str(arguments)
            environment.pop("ATRINIK_ADMIN_SHUTDOWN_SOCKET", None)
            if admin_socket is not None:
                environment["ATRINIK_ADMIN_SHUTDOWN_SOCKET"] = admin_socket
            if maps_path is not None:
                environment["ATRINIK_MAPS_PATH"] = maps_path

            subprocess.run(
                ["/bin/sh", ENTRYPOINT, "--stun_server=off"],
                cwd=server,
                env=environment,
                check=True,
            )
            return arguments.read_text(encoding="utf-8").splitlines()

    def test_rejects_unprovisioned_data_before_initialization(self) -> None:
        for kind in ("missing", "0755", "symlink", "wrong-owner"):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as temporary:
                server = Path(temporary)
                defaults = server / "install_data"
                defaults.mkdir()
                (defaults / "private-default").write_text("untouched", encoding="utf-8")
                executable = server / "atrinik-server"
                executable.write_text("#!/bin/sh\ntouch listener-started\n", encoding="utf-8")
                executable.chmod(0o755)
                data = server / "data"
                environment = os.environ.copy()
                environment.pop("ATRINIK_JOIN_PASSWORD", None)
                environment.pop("ATRINIK_JOIN_PASSWORD_FILE", None)
                if kind == "symlink":
                    target = server / "target"
                    target.mkdir(mode=0o700)
                    data.symlink_to(target, target_is_directory=True)
                elif kind != "missing":
                    data.mkdir(mode=0o755 if kind == "0755" else 0o700)
                    if kind == "wrong-owner":
                        # Simulate the effective UID of another container service.
                        commands = server / "commands"
                        commands.mkdir()
                        command = commands / "id"
                        command.write_text("#!/bin/sh\necho 999999\n", encoding="utf-8")
                        command.chmod(0o755)
                        environment["PATH"] = str(commands) + os.pathsep + environment["PATH"]
                result = subprocess.run(
                    ["/bin/sh", ENTRYPOINT], cwd=server, env=environment,
                    text=True, capture_output=True, check=False,
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Data provisioning error:", result.stderr)
                self.assertIn("0700", result.stderr)
                self.assertFalse((server / "listener-started").exists())
                self.assertFalse((data / ".atrinik-initialized").exists())
                self.assertFalse((data / "private-default").exists())
                if kind == "0755":
                    self.assertEqual(data.stat().st_mode & 0o777, 0o755)
                if kind == "symlink":
                    self.assertTrue(data.is_symlink())

    def test_compose_uses_monorepo_build_context_and_service_identity(self) -> None:
        repository = ENTRYPOINT.parents[2]
        compose = repository / "server" / "compose.server.yaml"
        # These paths are resolved from the Compose file's directory; the image
        # copies root LICENSE.md and sibling protocol/libatrinik/server sources.
        text = compose.read_text(encoding="utf-8")
        self.assertIn("      context: ..\n      dockerfile: server/Dockerfile\n", text)
        self.assertTrue((compose.parent / ".." / "server/Dockerfile").is_file())
        self.assertIn('user: "${LOCAL_UID:-10001}:${LOCAL_GID:-10001}"', text)

    def test_admin_socket_opt_in(self) -> None:
        self.assertFalse(any(arg.startswith("--admin_shutdown_socket=") for arg in self.run_entrypoint()))
        self.assertIn("--admin_shutdown_socket=/run/atrinik-admin/server.sock",
                      self.run_entrypoint(admin_socket="/run/atrinik-admin/server.sock"))

    def test_uses_packaged_maps_directory(self) -> None:
        self.assertIn("--mapspath=/opt/atrinik/maps", self.run_entrypoint())

    def test_allows_maps_directory_override(self) -> None:
        self.assertIn(
            "--mapspath=/srv/atrinik/maps",
            self.run_entrypoint("/srv/atrinik/maps"),
        )


if __name__ == "__main__":
    unittest.main()
