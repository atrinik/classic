"""Execute the production CMake runtime install rules without native builds."""

import os
import json
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]


class WindowsTrustNoticesTests(unittest.TestCase):
    def fixture(self, module, cares=True, notice=True, python=False):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        runtime = root / "toolchain/bin"
        runtime.mkdir(parents=True)
        (runtime / "libcurl-4.dll").write_bytes(b"fixture curl")
        (runtime / "unused.dll").write_bytes(b"unused dependency")
        if cares:
            (runtime / "libcares-2.dll").write_bytes(b"fixture c-ares")
        license_path = root / "toolchain/share/licenses/c-ares/LICENSE.md"
        license_path.parent.mkdir(parents=True)
        if notice:
            license_path.write_bytes(b"fixture complete MIT notice\n")
        llvm_notices = root / "toolchain/share/licenses/llvm-mingw"
        llvm_notices.mkdir(parents=True)
        for name in ("LICENSE.TXT", "COPYING", "COPYING.MinGW-w64-runtime.txt",
                     "COPYING.MinGW-w64.txt", "COPYING.winpthreads.txt", "COPYING.winstorecompat.txt"):
            (llvm_notices / name).write_bytes(b"fixture complete LLVM-MinGW notice\n")
        inventories = root / "toolchain/share/atrinik/windows-dependencies"
        inventories.mkdir(parents=True)
        (inventories / "dependencies.json").write_text(json.dumps({
            "schema_version": 1, "dependencies": [{"name": "fixture-curl", "version": "1",
                                                       "licenses": ["COPYING", "docs/NOTICE"]}]}))
        dlls = {"libcurl-4.dll": {"sha256": hashlib.sha256(b"fixture curl").hexdigest(),
                                     "owner": "fixture-curl", "source_sha256": "1" * 64, "imports": ["libcares-2.dll"] if cares else []}}
        if cares:
            dlls["libcares-2.dll"] = {"sha256": hashlib.sha256(b"fixture c-ares").hexdigest(),
                                      "owner": "fixture-curl", "source_sha256": "1" * 64, "imports": []}
        (inventories / "pe-closure.json").write_text(json.dumps({"schema_version": 2, "dlls": dlls,
            "notices": {"llvm-mingw": {"llvm-mingw/" + path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in llvm_notices.iterdir()},
                        "fixture-curl": {"fixture-curl/COPYING": hashlib.sha256(b"fixture dependency notice\n").hexdigest(),
                                          "fixture-curl/docs_NOTICE": hashlib.sha256(b"fixture dependency notice\n").hexdigest()}}}))
        notices = root / "toolchain/share/licenses/fixture-curl"
        notices.mkdir()
        for name in ("COPYING", "docs_NOTICE"):
            (notices / name).write_bytes(b"fixture dependency notice\n")
        python_runtime = root / "python"
        if python:
            python_runtime.mkdir()
            python_files = {"python314.dll": b"fixture Python", "_ssl.pyd": b"fixture extension",
                            "python314.zip": b"fixture stdlib", "LICENSE.txt": b"Python notice"}
            for name, data in python_files.items():
                (python_runtime / name).write_bytes(data)
            (inventories / "python-runtime.json").write_text(json.dumps({"schema_version": 1,
                "files": {name: {"sha256": hashlib.sha256(data).hexdigest()}
                          for name, data in python_files.items()}}))
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
            end = source.index('    install(DIRECTORY "${ATRINIK_SERVER_PACKAGE_ROOT}/maps"', start)
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
        inspector = root / "llvm-readobj"
        inspector.write_text('#!/usr/bin/env python3\nimport pathlib, sys\n'
                             'binary = pathlib.Path(sys.argv[-1])\n'
                             'print("Format: COFF-x86-64")\n'
                             'if binary.suffix == ".exe":\n'
                             '    print("Import {\\n  Name: libcurl-4.dll\\n}")\n'
                             f'elif binary.name == "libcurl-4.dll" and {cares!r}:\n'
                             '    print("Import {\\n  Name: libcares-2.dll\\n}")\n')
        inspector.chmod(0o755)
        (root / "fixture.exe").write_bytes(b"fixture executable")
        cmake = ('cmake_minimum_required(VERSION 3.21)\nproject(trust_fixture NONE)\n'
                 'set(INSTALL_SUBDIR_BIN ".")\nset(PACKAGE_TYPE zip)\n'
                 f'include("{ROOT}/cmake/InstallWindowsRuntime.cmake")\n'
                 f'set(ATRINIK_LLVM_READOBJ "{inspector}")\n'
                 'set(EXECUTABLE fixture-server)\n'
                 'foreach(target atrinik fixture-server plugin_arena plugin_python)\n'
                 '  add_executable(${target} IMPORTED)\n'
                 f'  set_target_properties(${{target}} PROPERTIES IMPORTED_LOCATION "{root}/fixture.exe")\n'
                 'endforeach()\n'
                 f'set(ATRINIK_LICENSE_FILE "{root}/LICENSE.md")\n'
                 f'set(ATRINIK_ATTRIBUTIONS_FILE "{root}/ATTRIBUTIONS.md")\n'
                 f'set(ATRINIK_WINDOWS_RUNTIME_DIR "{runtime}")\n' +
                 (f'set(ATRINIK_WINDOWS_PYTHON_RUNTIME_DIR "{python_runtime}")\n' if python else "") + rules +
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
                    self.assertFalse(any(n.endswith("/unused.dll") for n in names))
                    llvm_license = next(n for n in names if n.endswith("/licenses/llvm-mingw/LICENSE.TXT"))
                    self.assertEqual(archive.read(llvm_license), b"fixture complete LLVM-MinGW notice\n")
                    dependency_notice = next(n for n in names if n.endswith("/licenses/fixture-curl/docs_NOTICE"))
                    self.assertEqual(archive.read(dependency_notice), b"fixture dependency notice\n")
                    self.assertTrue(any(n.endswith("/licenses/windows-dependencies.json") for n in names))
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
            for command in ("python3", "git", "llvm-readobj"):
                script = bin_path / command
                script.write_text("#!/bin/sh\nexit 0\n")
                script.chmod(0o755)
            capture = root / "configure-args"
            script = bin_path / "cmake"
            script.write_text('#!/bin/sh\nprintf "%s\\n" "$@" > "$CAPTURE"\nexit 23\n')
            script.chmod(0o755)
            toolchain = root / "toolchain.cmake"
            toolchain.write_text("")
            runtime = root / "runtime"
            runtime.mkdir()
            shader = root / "shaders"
            shader.mkdir()
            env = dict(os.environ, PATH=f"{bin_path}:{os.environ['PATH']}", CAPTURE=str(capture),
                       ATRINIK_PACKAGE_VERSION="5.1.0", ATRINIK_WINDOWS_TARGET="x86_64-w64-mingw32", ATRINIK_WINDOWS_TOOLCHAIN_FILE=str(toolchain),
                       ATRINIK_WINDOWS_RUNTIME_DIR=str(runtime), ATRINIK_GPU_SHADER_DIRECTORY=str(shader))
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

    def test_unused_gcc_runtime_prefix_fails_package_install(self):
        for module in ("client", "server"):
            with self.subTest(module=module):
                root, result, _, _ = self.fixture(module, cares=False)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                (root / "toolchain/bin/libgcc_s_seh-1.dll").write_bytes(b"stale GCC")
                result = subprocess.run(
                    ["cmake", "--install", str(root / "build"), "--prefix", str(root / "installed")],
                    capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("GCC runtime DLL", " ".join((result.stdout + result.stderr).split()))

    def test_client_sound_verifier_uses_the_same_llvm_parser(self):
        root, result, _, _ = self.fixture("client", cares=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        inspector = root / "llvm-readobj"
        arguments = ["cmake", f"-DATRINIK_READOBJ={inspector}",
                     f"-DATRINIK_PYTHON={sys.executable}",
                     f"-DATRINIK_RUNTIME_HELPER={ROOT}/tools/ci/stage_windows_runtime.py",
                     f"-DATRINIK_CLIENT_EXECUTABLE={root}/fixture.exe", "-P",
                     str(ROOT / "client/cmake/verify_windows_client_sound.cmake")]
        result = subprocess.run(arguments, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not import required DLL", result.stdout + result.stderr)
        inspector.write_text('#!/usr/bin/env python3\n'
                             'print("Format: COFF-x86-64\\nImport {\\n  Name: SDL3_mixer.dll\\n}")\n')
        result = subprocess.run(arguments, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Windows client sound import verified", result.stdout + result.stderr)

    def test_missing_llvm_crt_notice_fails_configuration(self):
        root, result, _, _ = self.fixture("client", cares=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        (root / "toolchain/share/licenses/llvm-mingw/COPYING.MinGW-w64-runtime.txt").unlink()
        result = subprocess.run(["cmake", "-S", str(root), "-B", str(root / "build")],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("LLVM-MinGW package notice is missing", result.stdout + result.stderr)

    def test_dependency_inventory_rejects_missing_notices_and_unsafe_paths(self):
        cases = [
            {"schema_version": 2, "dependencies": [{"name": "fixture-curl", "licenses": ["COPYING"]}]},
            {"schema_version": 1, "dependencies": []},
            {"schema_version": 1, "dependencies": [{"name": "../escape", "licenses": ["COPYING"]}]},
            {"schema_version": 1, "dependencies": [{"name": "fixture-curl", "licenses": ["../COPYING"]}]},
            {"schema_version": 1, "dependencies": [{"name": "fixture-curl", "licenses": ["missing"]}]},
        ]
        for manifest in cases:
            with self.subTest(manifest=manifest):
                root, result, _, _ = self.fixture("client", cares=False)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                (root / "toolchain/share/atrinik/windows-dependencies/dependencies.json").write_text(json.dumps(manifest))
                result = subprocess.run(["cmake", "-S", str(root), "-B", str(root / "build")],
                                        capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def install_result(self, root):
        return subprocess.run(["cmake", "--install", str(root / "build"), "--prefix", str(root / "installed")],
                              capture_output=True, text=True)

    def test_pe_inventory_rejects_malformed_unowned_or_changed_dlls(self):
        for damage in ("malformed", "unowned", "bytes", "imports", "owner", "notice"):
            with self.subTest(damage=damage):
                root, result, _, _ = self.fixture("client", cares=False)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                inventory = root / "toolchain/share/atrinik/windows-dependencies/pe-closure.json"
                if damage == "malformed":
                    inventory.write_text("not JSON")
                elif damage == "unowned":
                    (root / "toolchain/bin/unowned.dll").write_bytes(b"unowned")
                    (root / "llvm-readobj").write_text('#!/usr/bin/env python3\nimport pathlib,sys\n'
                        'print("Format: COFF-x86-64")\n'
                        'if pathlib.Path(sys.argv[-1]).suffix == ".exe":\n'
                        '    print("Import {\\nName: unowned.dll\\n}")\n')
                elif damage == "bytes":
                    (root / "toolchain/bin/libcurl-4.dll").write_bytes(b"changed bytes")
                elif damage == "notice":
                    (root / "toolchain/share/licenses/fixture-curl/COPYING").write_bytes(b"changed notice")
                else:
                    manifest = json.loads(inventory.read_text())
                    if damage == "imports":
                        manifest["dlls"]["libcurl-4.dll"]["imports"] = ["kernel32.dll"]
                    else:
                        manifest["dlls"]["libcurl-4.dll"]["owner"] = "unowned"
                    inventory.write_text(json.dumps(manifest))
                result = self.install_result(root)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertFalse((root / "installed/unowned.dll").exists())

    def test_python_snapshot_ships_only_receipt_checked_files(self):
        root, result, _, _ = self.fixture("server", cares=False, python=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        with self.package(root, "server") as archive:
            for filename in ("python314.dll", "_ssl.pyd", "python314.zip", "LICENSE.txt"):
                name = next(n for n in archive.namelist() if n.endswith("/server/" + filename))
                self.assertEqual(archive.read(name), (root / "python" / filename).read_bytes())
            self.assertTrue(any(n.endswith("/licenses/windows-python-runtime.json") for n in archive.namelist()))

    def test_python_late_additions_nested_gcc_and_changed_bytes_cannot_ship(self):
        for damage in ("late-pyd", "nested-gcc", "changed", "symlink"):
            with self.subTest(damage=damage):
                root, result, _, _ = self.fixture("server", cares=False, python=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                runtime = root / "python"
                if damage == "late-pyd":
                    (runtime / "late.pyd").write_bytes(b"invalid PE")
                elif damage == "nested-gcc":
                    (runtime / "nested").mkdir()
                    (runtime / "nested/libgcc_s_seh-1.dll").write_bytes(b"GCC")
                elif damage == "changed":
                    (runtime / "_ssl.pyd").write_bytes(b"changed bytes")
                else:
                    (runtime / "_ssl.pyd").unlink()
                    (runtime / "_ssl.pyd").symlink_to(runtime / "python314.dll")
                result = self.install_result(root)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertFalse((root / "installed/server/late.pyd").exists())
                self.assertFalse((root / "installed/server/nested/libgcc_s_seh-1.dll").exists())

    def test_python_sealed_libffi_ignores_only_unreachable_sdk_duplicate(self):
        for reachable, missing_sdk in ((False, False), (True, False), (True, True)):
            with self.subTest(reachable=reachable, missing_sdk=missing_sdk):
                root, result, _, _ = self.fixture("server", cares=False, python=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                inventories = root / "toolchain/share/atrinik/windows-dependencies"
                target_bytes, python_bytes = b"LLVM SDK ffi", b"sealed CPython ffi"
                (root / "toolchain/bin/libffi-8.dll").write_bytes(target_bytes)
                (root / "python/libffi-8.dll").write_bytes(python_bytes)
                python_receipt = json.loads((inventories / "python-runtime.json").read_text())
                python_receipt["files"]["libffi-8.dll"] = {"sha256": hashlib.sha256(python_bytes).hexdigest()}
                (inventories / "python-runtime.json").write_text(json.dumps(python_receipt))
                producer_receipt = json.loads((inventories / "pe-closure.json").read_text())
                producer_receipt["dlls"]["libffi-8.dll"] = {
                    "owner": "fixture-curl", "source_sha256": "1" * 64,
                    "sha256": hashlib.sha256(target_bytes).hexdigest(), "imports": []}
                producer_receipt["dlls"]["libcurl-4.dll"]["imports"] = ["libffi-8.dll"] if reachable else []
                (inventories / "pe-closure.json").write_text(json.dumps(producer_receipt))
                inspector = root / "llvm-readobj"
                script = inspector.read_text()
                script += ('\nif binary.name == "_ssl.pyd":\n'
                           '    print("Import {\\nName: libffi-8.dll\\n}")\n')
                if reachable:
                    script += ('\nif binary.name == "libcurl-4.dll":\n'
                               '    print("Import {\\nName: libffi-8.dll\\n}")\n')
                inspector.write_text(script)
                if missing_sdk:
                    (root / "toolchain/bin/libffi-8.dll").unlink()
                if reachable:
                    result = self.install_result(root)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("reachable DLL provider conflict", " ".join((result.stdout + result.stderr).split()))
                else:
                    with self.package(root, "server") as archive:
                        filename = next(name for name in archive.namelist() if name.endswith("/server/libffi-8.dll"))
                        self.assertEqual(archive.read(filename), python_bytes)

    def test_producer_nested_modules_are_checked_without_flattening(self):
        for damage in (None, "bytes", "imports", "owner"):
            with self.subTest(damage=damage):
                root, result, _, _ = self.fixture("client", cares=False)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                module = root / "toolchain/lib/ossl-modules/legacy.dll"
                module.parent.mkdir(parents=True)
                module.write_bytes(b"verified legacy module")
                inventory = root / "toolchain/share/atrinik/windows-dependencies/pe-closure.json"
                manifest = json.loads(inventory.read_text())
                record = {"sha256": hashlib.sha256(module.read_bytes()).hexdigest(),
                          "owner": "fixture-curl", "source_sha256": "1" * 64, "imports": []}
                if damage == "owner":
                    record["owner"] = "unowned"
                elif damage == "imports":
                    record["imports"] = ["kernel32.dll"]
                elif damage == "bytes":
                    module.write_bytes(b"changed module")
                manifest["modules"] = {"lib/ossl-modules/legacy.dll": record}
                inventory.write_text(json.dumps(manifest))
                if damage is None:
                    with self.package(root, "client") as archive:
                        self.assertFalse(any(name.endswith("legacy.dll") for name in archive.namelist()))
                        metadata = next(name for name in archive.namelist() if name.endswith("windows-pe-closure.json"))
                        self.assertIn("modules", json.loads(archive.read(metadata)))
                else:
                    result = self.install_result(root)
                    self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
