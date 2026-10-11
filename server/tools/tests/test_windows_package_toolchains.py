"""Run the package helper with recorded build phases and disposable outputs."""
from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

DRIVER = r'''
import json, os, pathlib, shutil, sys
name = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
with open(os.environ["FIXTURE_CALLS"], "a") as output:
    output.write(json.dumps({"tool": name, "args": args,
                            "path": os.environ["PATH"],
                            "toolchain": os.environ.get("CMAKE_TOOLCHAIN_FILE"),
                            "cc": os.environ.get("CC"), "cxx": os.environ.get("CXX")}) + "\n")
if name == "git":
    print(os.environ["FIXTURE_REPOSITORY"])
elif name == "cmake":
    if args[0] == "-E":
        action, paths = args[1], args[2:]
        if action == "remove_directory":
            shutil.rmtree(paths[0], ignore_errors=True)
        elif action == "make_directory":
            for path in paths:
                pathlib.Path(path).mkdir(parents=True, exist_ok=True)
        elif action == "copy_directory":
            shutil.copytree(paths[0], paths[1], dirs_exist_ok=True)
        elif action == "copy":
            for path in paths[:-1]:
                shutil.copy(path, paths[-1])
        else:
            raise RuntimeError(action)
    elif args[0] == "-S":
        pathlib.Path(args[args.index("-B") + 1]).mkdir(parents=True, exist_ok=True)
    elif args[0] == "--build" and args[1].endswith("windows-region-generator"):
        build = pathlib.Path(args[1])
        (build / "atrinik-server").write_text(pathlib.Path(sys.argv[0]).read_text())
        (build / "atrinik-server").chmod(0o755)
        for plugin in ("libplugin_arena.so", "libplugin_python.so"):
            (build / plugin).write_bytes(b"fixture plugin")
elif name == "atrinik-server":
    assets = next(arg.split("=", 1)[1] for arg in args if arg.startswith("--assetspath="))
    pathlib.Path(assets, "client-maps").mkdir(parents=True)
elif name == "cpack":
    destination = pathlib.Path(args[args.index("-B") + 1])
    (destination / "atrinik-classic-server-0.0.0-windows-x86_64.zip").write_bytes(b"fixture package")
'''


class WindowsPackageToolchainTests(unittest.TestCase):
    def test_native_worldmaker_does_not_inherit_cross_compiler_selection(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            repository = Path(temporary)
            server = repository / "server"
            binaries = repository / "host-bin"
            mingw = repository / "llvm-mingw/bin"
            binaries.mkdir()
            mingw.mkdir(parents=True)
            for tool in ("cmake", "cpack", "python3", "git", "ccache", "llvm-readobj"):
                executable = binaries / tool
                executable.write_text(f"#!{sys.executable}\n" + DRIVER)
                executable.chmod(0o755)
            for module in ("protocol", "libatrinik"):
                (repository / module).mkdir()
                (repository / module / "CMakeLists.txt").touch()
            for directory in ("runtime/content/maps", "runtime/content/lib",
                              "runtime/content/attribution", "install_data", "resources"):
                (server / directory).mkdir(parents=True)
            for name in ("runtime/content/compatibility.json", "runtime/content/manifest.json",
                         "server.cfg", "permissions.cfg"):
                (server / name).write_text("fixture\n")
            toolchain = repository / "mingw.cmake"
            toolchain.touch()
            cache = repository / "dependency-cache"
            cache.mkdir()
            log = repository / "calls.jsonl"
            environment = {key: value for key, value in os.environ.items()
                           if not key.startswith(("ATRINIK_", "MXE_"))}
            inherited_path = f"{mingw}:{binaries}:{os.environ['PATH']}"
            environment.update({
                "PATH": inherited_path, "CC": str(mingw / "clang"),
                "CXX": str(mingw / "clang++"), "FIXTURE_CALLS": str(log),
                "CMAKE_TOOLCHAIN_FILE": str(toolchain),
                "FIXTURE_REPOSITORY": str(repository), "ATRINIK_PACKAGE_VERSION": "0.0.0",
                "ATRINIK_WINDOWS_TOOLCHAIN_FILE": str(toolchain),
                "ATRINIK_WINDOWS_TARGET": "x86_64-w64-mingw32",
                "ATRINIK_WINDOWS_RUNTIME_DIR": str(mingw),
                "ATRINIK_WINDOWS_PYTHON_INCLUDE_DIR": str(mingw),
                "ATRINIK_WINDOWS_PYTHON_LIBRARY": str(toolchain),
                "ATRINIK_WINDOWS_PYTHON_RUNTIME_DIR": str(mingw),
                "ATRINIK_DEPENDENCY_CACHE_DIR": str(cache),
            })
            result = subprocess.run(["bash", str(ROOT / "tools/build-windows-package.sh")],
                                    cwd=server, env=environment, capture_output=True, text=True,
                                    timeout=20, check=False)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            calls = [json.loads(line) for line in log.read_text().splitlines()]
            configurations = [call for call in calls if call["tool"] == "cmake"
                              and call["args"][0] == "-S"]
            self.assertEqual(len(configurations), 2)
            native, cross = configurations
            native_prefix = "/opt/llvm-23.1.3/bin"
            self.assertEqual(native["path"], f"{native_prefix}:{inherited_path}")
            self.assertEqual(native["cc"], f"{native_prefix}/clang")
            self.assertEqual(native["cxx"], f"{native_prefix}/clang++")
            native_toolchain = ROOT.parent / "cmake/toolchains/llvm.cmake"
            self.assertEqual(Path(native["toolchain"]).resolve(), native_toolchain)
            self.assertIn(f"-DCMAKE_TOOLCHAIN_FILE={native['toolchain']}", native["args"])
            for variable, tool in (("C_COMPILER", "clang"), ("LINKER", "ld.lld"),
                                   ("AR", "llvm-ar"), ("READELF", "llvm-readelf")):
                self.assertIn(f"-DCMAKE_{variable}={native_prefix}/{tool}", native["args"])
            self.assertEqual(cross["path"], inherited_path)
            self.assertEqual(cross["cc"], str(mingw / "clang"))
            self.assertEqual(cross["toolchain"], str(toolchain))
            self.assertIn(f"-DCMAKE_TOOLCHAIN_FILE={toolchain}", cross["args"])
            self.assertFalse(any(arg.startswith("-DCMAKE_C_COMPILER=") for arg in cross["args"]))
            for configuration in configurations:
                for argument in ("-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=OFF",
                                 f"-DFETCHCONTENT_SOURCE_DIR_ATRINIK_PROTOCOL={repository}/protocol",
                                 f"-DFETCHCONTENT_SOURCE_DIR_LIBATRINIK={repository}/libatrinik",
                                 f"-DATRINIK_DEPENDENCY_CACHE_DIR={cache}"):
                    self.assertIn(argument, configuration["args"])
            native_build = next(call for call in calls if call["tool"] == "cmake"
                                and call["args"][:2] == ["--build", "build/windows-region-generator"])
            self.assertEqual(native_build["path"], native["path"])
            self.assertIn("plugin_python", native_build["args"])
            worldmaker = next(call for call in calls if call["tool"] == "atrinik-server")
            self.assertEqual(worldmaker["path"], native["path"])
            self.assertIn("--worldmaker", worldmaker["args"])
            cross_build = next(call for call in calls if call["tool"] == "cmake"
                               and call["args"][:2] == ["--build", "build/windows-release"])
            self.assertEqual(cross_build["path"], inherited_path)


if __name__ == "__main__":
    unittest.main()
