#!/usr/bin/env python3
"""Exercise Debian packaging with an existing system ELF, without a client build."""

import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/build-linux-package.sh"


class LinuxPackageContractTests(unittest.TestCase):
    def test_production_and_offline_boundaries(self):
        script = SCRIPT.read_text()
        self.assertIn("-DBUILD_TESTING=OFF", script)
        self.assertNotIn("-DBUILD_TESTING=ON", script)
        self.assertIn("--refresh --offline", script)
        self.assertIn("-DFETCHCONTENT_FULLY_DISCONNECTED=ON", script)
        self.assertIn('build_parallelism=${CMAKE_BUILD_PARALLEL_LEVEL:-$(nproc)}', script)
        self.assertIn('--parallel "${build_parallelism}"', script)
        self.assertIn("umask 022", script)
        self.assertIn('mktemp -d "${output_directory}/.atrinik-linux-package.XXXXXX"', script)
        self.assertIn('-G DEB -B "${staging_directory}"', script)
        for component in ("ATRINIK_PROTOCOL", "LIBATRINIK"):
            self.assertIn(f"-DFETCHCONTENT_SOURCE_DIR_{component}=", script)
        for marker in ("'--gpu-player-view'", "'injected GPU conformance fault'"):
            self.assertIn(marker, script)
        self.assertIn("python3 tools/verify_gpu_fixture_provenance.py", script)
        sources = (ROOT / "src/cmake.txt").read_text()
        self.assertIn(
            "if (BUILD_TESTING)\n\tlist(APPEND SOURCES src/client/gpu_player_view.c)\nendif ()",
            sources,
        )
        cmake = (ROOT / "CMakeLists.txt").read_text()
        for dependency in ("SDL3 3.4", "SDL3_image 3.2", "SDL3_ttf 3.2",
                           "SDL3_mixer 3.2.4", "OpenSSL 3.5"):
            self.assertIn(f"find_package({dependency}", cmake)

    def test_invalid_inputs_fail_before_build(self):
        with tempfile.TemporaryDirectory() as directory:
            environment = dict(os.environ)
            for variable in ("ATRINIK_PACKAGE_VERSION", "ATRINIK_DEPENDENCY_DOWNLOADS",
                             "ATRINIK_GPU_SHADER_DIRECTORY"):
                environment.pop(variable, None)
            for version in ("", "5.1", "5.1.0;bad", "../5.1.0"):
                environment["ATRINIK_PACKAGE_VERSION"] = version
                result = subprocess.run(["bash", str(SCRIPT)], cwd=directory,
                                        env=environment, capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("must be MAJOR.MINOR.PATCH", result.stderr)
            environment["ATRINIK_PACKAGE_VERSION"] = "5.1.0"
            for parallelism in ("0", "-1", "2.0", "2;bad", "two"):
                environment["CMAKE_BUILD_PARALLEL_LEVEL"] = parallelism
                result = subprocess.run(["bash", str(SCRIPT)], cwd=directory,
                                        env=environment, capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("CMAKE_BUILD_PARALLEL_LEVEL must be a positive integer",
                              result.stderr)
            environment["CMAKE_BUILD_PARALLEL_LEVEL"] = "2"
            result = subprocess.run(["bash", str(SCRIPT)], cwd=directory,
                                    env=environment, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("regular staged directory", result.stderr)
            link = Path(directory) / "linked-stage"
            link.symlink_to(directory, target_is_directory=True)
            for variable in ("ATRINIK_DEPENDENCY_DOWNLOADS", "ATRINIK_GPU_SHADER_DIRECTORY"):
                environment["ATRINIK_DEPENDENCY_DOWNLOADS"] = directory
                environment["ATRINIK_GPU_SHADER_DIRECTORY"] = directory
                environment[variable] = str(link)
                result = subprocess.run(["bash", str(SCRIPT)], cwd=directory,
                                        env=environment, capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(f"{variable} does not identify a regular staged directory",
                              result.stderr)
            environment["ATRINIK_DEPENDENCY_DOWNLOADS"] = directory
            environment["ATRINIK_GPU_SHADER_DIRECTORY"] = directory
            result = subprocess.run(["bash", str(SCRIPT)], cwd=directory,
                                    env=environment, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Required sibling source is missing", result.stderr)
            self.assertFalse((Path(directory) / "build").exists())

    @unittest.skipUnless(all(shutil.which(tool) for tool in
                            ("cmake", "cpack", "dpkg-deb", "dpkg-shlibdeps", "file")),
                         "Debian packaging tools are required")
    def test_real_cpack_metadata_and_runtime_payload(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source"
            source.mkdir()
            cmake = (ROOT / "CMakeLists.txt").read_text()
            installer = cmake.split("# Installer.\n", 1)[1].split(
                "# Configure the .h file", 1)[0]
            # Reuse every authored install/CPack rule, substituting only the ELF
            # target so the fixture never compiles application code on the host.
            installer = installer.replace(
                "install(TARGETS atrinik DESTINATION ${INSTALL_SUBDIR_BIN}\n"
                "        COMPONENT AtrinikClient)",
                'install(PROGRAMS "${CMAKE_CURRENT_SOURCE_DIR}/fixture-elf" DESTINATION ${INSTALL_SUBDIR_BIN}\n'
                '        RENAME atrinik COMPONENT AtrinikClient)',
            )
            prefix = '''cmake_minimum_required(VERSION 3.21)
project(package-fixture LANGUAGES NONE)
set(PACKAGE_TYPE deb)
set(PACKAGE_VERSION 5.1.0)
set(PACKAGE_VERSION_MAJOR 5)
set(PACKAGE_VERSION_MINOR 1)
set(PACKAGE_VERSION_PATCH 0)
set(ATRINIK_LICENSE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.md")
set(ATRINIK_ATTRIBUTIONS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/ATTRIBUTIONS.md")
# A sibling development install must never enter the runtime package.
install(FILES LICENSE.md DESTINATION include/atrinik)
'''
            (source / "CMakeLists.txt").write_text(prefix + installer)
            shutil.copyfile("/bin/true", source / "fixture-elf")
            for name in ("textures", "cache", "data", "fonts", "gfx_user", "settings",
                         "sound", "srv_files"):
                (source / name).mkdir()
                (source / name / "fixture.txt").write_text(name)
            for name in ("INSTALL", "README.md", "LICENSE.md", "ATTRIBUTIONS.md",
                         "client.cfg", "textures/icon.png"):
                (source / name).write_text("fixture\n")
            shutil.copyfile(ROOT / "atrinik.desktop", source / "atrinik.desktop")
            (source / "data/discord-application-id").write_text("excluded")
            build = Path(directory) / "build"
            def run(*arguments):
                result = subprocess.run(arguments, capture_output=True, text=True,
                                        umask=0o077)
                self.assertEqual(
                    result.returncode, 0,
                    f"Command {arguments!r} failed with exit {result.returncode}:\n"
                    f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}",
                )
                return result
            run("cmake", "-S", str(source), "-B", str(build),
                "-DCMAKE_INSTALL_PREFIX=/usr", "-DCMAKE_SYSTEM_PROCESSOR=x86_64")
            config = (build / "CPackConfig.cmake").read_text()
            self.assertIn('set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS "ON")', config)
            run("cpack", "--config", str(build / "CPackConfig.cmake"),
                "-B", directory)
            package = Path(directory) / "atrinik-classic-client-5.1.0-linux-amd64.deb"
            self.assertTrue(package.is_file())
            for field, expected in (("Package", "atrinik"), ("Version", "5.1.0"),
                                    ("Architecture", "amd64")):
                self.assertEqual(run("dpkg-deb", "-f", str(package), field).stdout.strip(),
                                 expected)
            dependencies = run("dpkg-deb", "-f", str(package), "Depends").stdout
            for dependency in ("libsdl3-0 (>= 3.4)", "libsdl3-image0 (>= 3.2)",
                               "libsdl3-ttf0 (>= 3.2)", "libsdl3-mixer0 (>= 3.2.4)",
                               "libssl3t64 (>= 3.5)", "ca-certificates", "libvulkan1",
                               "libc6 (>="):
                self.assertIn(dependency, dependencies)
            self.assertNotIn("-dev", dependencies)
            payload = Path(directory) / "payload"
            run("dpkg-deb", "-x", str(package), str(payload))
            required = ("usr/games/atrinik", "usr/share/applications/atrinik.desktop",
                        "usr/share/pixmaps/atrinik.png", "usr/share/doc/atrinik/LICENSE.md",
                        "usr/share/doc/atrinik/ATTRIBUTIONS.md",
                        "usr/share/games/atrinik/client.cfg",
                        "usr/share/games/atrinik/sound/fixture.txt")
            for path in required:
                self.assertTrue((payload / path).is_file(), path)
            self.assertIn("Exec=/usr/games/atrinik\n",
                          (payload / "usr/share/applications/atrinik.desktop").read_text())
            self.assertEqual((payload / "usr/share/games/atrinik/client.cfg").read_bytes(),
                             (source / "client.cfg").read_bytes())
            self.assertFalse((payload / "usr/include").exists())
            self.assertFalse((payload / "usr/lib").exists())
            for path in payload.rglob("*"):
                if path.is_dir():
                    self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o755,
                                     str(path.relative_to(payload)))
                elif path.is_file():
                    self.assertTrue(path.stat().st_mode & stat.S_IROTH,
                                    str(path.relative_to(payload)))
            self.assertTrue((payload / "usr/games/atrinik").stat().st_mode & stat.S_IXOTH)
            self.assertFalse((payload / "usr/share/games/atrinik/data/discord-application-id").exists())
            self.assertEqual((payload / "usr/games/atrinik").read_bytes(),
                             Path("/bin/true").read_bytes())
            for name, argument, message in (
                ("prefix", "-DCMAKE_INSTALL_PREFIX=/opt", "CMAKE_INSTALL_PREFIX=/usr"),
                ("architecture", "-DCMAKE_SYSTEM_PROCESSOR=aarch64", "Linux amd64"),
            ):
                result = subprocess.run(["cmake", "-S", str(source), "-B",
                                         str(Path(directory) / name),
                                         "-DCMAKE_SYSTEM_NAME=Linux",
                                         "-DCMAKE_SYSTEM_PROCESSOR=x86_64",
                                         "-DCMAKE_INSTALL_PREFIX=/usr", argument],
                                        capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)


if __name__ == "__main__":
    unittest.main()
