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
