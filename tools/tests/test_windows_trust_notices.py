"""Execute the production CMake runtime install rules without native builds."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]


class WindowsTrustNoticesTests(unittest.TestCase):
    def fixture(self, module, cares=True, notice=True):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        runtime = root / "toolchain/bin"
        runtime.mkdir(parents=True)
        (runtime / "libcurl-4.dll").write_bytes(b"fixture curl")
        if cares:
            (runtime / "libcares-2.dll").write_bytes(b"fixture c-ares")
        license_path = root / "toolchain/share/licenses/c-ares/LICENSE.md"
        license_path.parent.mkdir(parents=True)
        if notice:
            license_path.write_bytes(b"fixture complete MIT notice\n")
        source = (ROOT / module / "CMakeLists.txt").read_text()
        if module == "client":
            start = source.index("    if (ATRINIK_WINDOWS_RUNTIME_DIR)", source.index("# Installer."))
            end = source.index('    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/timidity.cfg")', start)
            rules = source[start:end]
            # Execute the authoritative cache default as well as the install block.
            start = source.index("    set(ATRINIK_CA_BUNDLE", source.index("# Installer."))
            end = source.index("\n\n", start)
            rules = source[start:end] + "\n" + rules
        else:
            start = source.index("    file(GLOB ATRINIK_WINDOWS_RUNTIME_DLLS", source.index("set(ATRINIK_WINDOWS_RUNTIME_DIR"))
            end = source.index('    install(DIRECTORY "${ATRINIK_WINDOWS_PYTHON_RUNTIME_DIR}/"', start)
            rules = source[start:end]
            # Server always installs its authored bundle directly.
            start = source.index("    install(FILES\n        README.md", end)
            end = source.index("\n\n", start)
            self.assertIn("\n        ca-bundle.crt\n", source[start:end])
            rules += source[start:end]
            for filename in ("README.md", "LICENSE.md", "ATTRIBUTIONS.md", "permissions.cfg",
                             "server.bat", "server.cfg", "server-custom.cfg.example"):
                (root / filename).write_text("fixture file\n")
        ca = ROOT / module / "ca-bundle.crt"
        (root / "ca-bundle.crt").write_bytes(ca.read_bytes())
        system_ca = root / "system-ca.crt"
        system_ca.write_bytes(b"different system CA fixture")
        cmake = ('cmake_minimum_required(VERSION 3.21)\nproject(trust_fixture NONE)\n'
                 'set(INSTALL_SUBDIR_BIN ".")\nset(PACKAGE_TYPE zip)\n'
                 f'set(ATRINIK_LICENSE_FILE "{root}/LICENSE.md")\n'
                 f'set(ATRINIK_ATTRIBUTIONS_FILE "{root}/ATTRIBUTIONS.md")\n'
                 f'set(ATRINIK_WINDOWS_RUNTIME_DIR "{runtime}")\n' + rules +
                 '\nset(CPACK_GENERATOR ZIP)\nset(CPACK_PACKAGE_FILE_NAME fixture)\ninclude(CPack)\n')
        (root / "CMakeLists.txt").write_text(cmake)
        arguments = ["cmake", "-S", str(root), "-B", str(root / "build")]
        result = subprocess.run(arguments, capture_output=True, text=True)
        return root, result, ca, system_ca

    def package(self, root, module):
        subprocess.run(["cmake", "--install", str(root / "build"), "--prefix", str(root / "installed")],
                       capture_output=True, text=True, check=True)
        subprocess.run(["cpack", "--config", str(root / "build/CPackConfig.cmake"), "-B", str(root / "packages")],
                       capture_output=True, text=True, check=True)
        return zipfile.ZipFile(root / "packages/fixture.zip")

    def test_both_packages_include_exact_source_ca_and_notice(self):
        for module in ("client", "server"):
            with self.subTest(module=module):
                root, result, ca, system_ca = self.fixture(module)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                with self.package(root, module) as archive:
                    names = archive.namelist()
                    notices = [n for n in names if n.endswith("/c-ares-LICENSE.md")]
                    bundles = [n for n in names if n.endswith("/ca-bundle.crt")]
                    self.assertEqual(len(notices), 1)
                    self.assertEqual(len(bundles), 1)
                    self.assertEqual(archive.read(notices[0]), b"fixture complete MIT notice\n")
                    self.assertEqual(archive.read(bundles[0]), ca.read_bytes())
                    self.assertNotEqual(archive.read(bundles[0]), system_ca.read_bytes())
                    cares_dlls = [n for n in names if n.endswith("/libcares-2.dll")]
                    self.assertEqual(len(cares_dlls), 1)
                    self.assertEqual(Path(notices[0]).parent, Path(cares_dlls[0]).parent)

    def test_cares_missing_or_directory_notice_fails_closed(self):
        for module in ("client", "server"):
            for directory in (False, True):
                with self.subTest(module=module, directory=directory):
                    root, result, _, _ = self.fixture(module, notice=False)
                    if directory:
                        (root / "toolchain/share/licenses/c-ares/LICENSE.md").mkdir()
                        result = subprocess.run(["cmake", "-S", str(root), "-B", str(root / "build")],
                                                capture_output=True, text=True)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("c-ares runtime requires ATRINIK_WINDOWS_CARES_LICENSE", result.stdout + result.stderr)

    def test_old_threaded_toolchain_does_not_require_notice(self):
        for module in ("client", "server"):
            with self.subTest(module=module):
                root, result, _, _ = self.fixture(module, cares=False, notice=False)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                with self.package(root, module) as archive:
                    self.assertFalse(any(n.endswith("c-ares-LICENSE.md") for n in archive.namelist()))

    def test_explicit_license_override(self):
        for module in ("client", "server"):
            with self.subTest(module=module):
                root, result, _, _ = self.fixture(module)
                moved = root / "alternate-MIT.md"
                (root / "toolchain/share/licenses/c-ares/LICENSE.md").rename(moved)
                result = subprocess.run(["cmake", "-S", str(root), "-B", str(root / "build"),
                                         f"-DATRINIK_WINDOWS_CARES_LICENSE={moved}"], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                with self.package(root, module) as archive:
                    notice = next(n for n in archive.namelist() if n.endswith("/c-ares-LICENSE.md"))
                    self.assertEqual(archive.read(notice), moved.read_bytes())

    def test_client_helper_passes_authored_ca_even_with_system_ca(self):
        # Run the actual packaging helper until its configure call. All preceding
        # dependency commands are fixture stubs; configure captures its arguments.
        helper = ROOT / "client/tools/build-windows-package.sh"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            client = root / "client"
            client.mkdir()
            (client / "tools").mkdir()
            (client / "ca-bundle.crt").write_bytes((ROOT / "client/ca-bundle.crt").read_bytes())
            (client / "tools/dependencies.py").write_text("")
            bin_path = root / "bin"
            bin_path.mkdir()
            for command in ("python3", "git"):
                script = bin_path / command
                script.write_text("#!/bin/sh\nexit 0\n")
                script.chmod(0o755)
            capture = root / "configure-args"
            script = bin_path / "fixture-cmake"
            script.write_text('#!/bin/sh\nprintf "%s\\n" "$@" > "$CAPTURE"\nexit 23\n')
            script.chmod(0o755)
            toolchain = root / "toolchain.cmake"
            toolchain.write_text("")
            runtime = root / "runtime"
            runtime.mkdir()
            shader = root / "shaders"
            shader.mkdir()
            env = dict(os.environ, PATH=f"{bin_path}:{os.environ['PATH']}", CAPTURE=str(capture),
                       ATRINIK_PACKAGE_VERSION="5.1.0", MXE_TARGET="fixture", MXE_TOOLCHAIN_FILE=str(toolchain),
                       MXE_RUNTIME_DIR=str(runtime), ATRINIK_GPU_SHADER_DIRECTORY=str(shader))
            result = subprocess.run(["bash", str(helper)], cwd=client, env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 23, result.stdout + result.stderr)
            arguments = capture.read_text().splitlines()
            ca_argument = next(arg for arg in arguments if arg.startswith("-DATRINIK_CA_BUNDLE="))
            self.assertEqual(ca_argument, f"-DATRINIK_CA_BUNDLE={client}/ca-bundle.crt")

    def test_standalone_client_ca_override_remains_available(self):
        root, result, _, system_ca = self.fixture("client", cares=False)
        result = subprocess.run(["cmake", "-S", str(root), "-B", str(root / "build"),
                                 f"-DATRINIK_CA_BUNDLE={system_ca}"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        with self.package(root, "client") as archive:
            name = next(n for n in archive.namelist() if n.endswith("/ca-bundle.crt"))
            self.assertEqual(archive.read(name), system_ca.read_bytes())


if __name__ == "__main__":
    unittest.main()
