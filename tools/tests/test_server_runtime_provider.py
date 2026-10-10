# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright 2026 The Atrinik Project
"""Bounded executable tests for the standalone Docker provider closure."""

import copy
import hashlib
import importlib.util
import json
import os
import shlex
import sys
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("runtime_provider", ROOT / "server/docker/runtime-provider.py")
provider = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(provider)
LOCK = json.loads((ROOT / "server/docker/runtime-provider.lock.json").read_text())


class ServerRuntimeProviderTests(unittest.TestCase):
    def fixture(self):
        temporary = tempfile.TemporaryDirectory(dir=os.environ.get("ATRINIK_TEST_TMPDIR"))
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        prefix = root / "source"
        lock = copy.deepcopy(LOCK)
        for row in lock["files"]:
            path = prefix / provider.relative_path(row["path"])
            path.parent.mkdir(parents=True, exist_ok=True)
            if "symlink" in row:
                path.symlink_to(row["symlink"])
                continue
            if path.name == "classic-curl-toolchain.json":
                contents = json.dumps({"resolver": "c-ares", "threaded_resolver": False,
                                      "cares": {"version": "1.34.6"},
                                      "linux": {"curl_version": "8.18.0", "openssl_version": "3.5.5"}}).encode()
            elif path.name == "curl_config.h":
                contents = b"#define USE_ARES 1\n/* #undef USE_THREADS_POSIX */\n"
            else:
                contents = ("fixture " + row["path"]).encode()
            path.write_bytes(contents)
            path.chmod(int(row["mode"], 8))
            row["sha256"] = hashlib.sha256(contents).hexdigest()
            row["bytes"] = len(contents)
        return root, prefix, lock

    def test_stage_preserves_exact_files_and_complete_internal_links(self):
        root, prefix, lock = self.fixture()
        output = root / "staged"
        (prefix / "bin").mkdir()
        (prefix / "bin/compiler").write_text("must not ship")
        provider.stage(lock, prefix, output)
        actual = {str(p.relative_to(output)) for p in output.rglob("*") if p.is_file() or p.is_symlink()}
        expected = {str(provider.relative_path(r["path"])) for r in lock["files"]}
        self.assertEqual(actual, expected)
        for row in lock["files"]:
            src = prefix / provider.relative_path(row["path"])
            dest = output / provider.relative_path(row["path"])
            if src.is_symlink():
                self.assertEqual(os.readlink(src), os.readlink(dest))
                self.assertTrue(dest.resolve(strict=True).is_file())
            else:
                self.assertEqual(src.read_bytes(), dest.read_bytes())
        with self.assertRaisesRegex(provider.ProviderError, "already exists"):
            provider.stage(lock, prefix, output)

    def test_missing_changed_or_linked_payload_fails_before_copy(self):
        mutations = ("missing-license", "changed-library", "broken-link", "escape-link", "symlink-parent")
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                root, prefix, lock = self.fixture()
                license_file = prefix / "share/licenses/c-ares/LICENSE.md"
                library = prefix / "lib/libcurl.so.4.8.0"
                link = prefix / "lib/libcares.so.2"
                if mutation == "missing-license":
                    license_file.unlink()
                elif mutation == "changed-library":
                    library.write_bytes(b"different provider")
                elif mutation in ("broken-link", "escape-link"):
                    link.unlink()
                    link.symlink_to("missing.so" if mutation == "broken-link" else "../../external.so")
                else:
                    (prefix / "lib").rename(root / "external")
                    (prefix / "lib").symlink_to(root / "external")
                output = root / "staged"
                with self.assertRaises(provider.ProviderError):
                    provider.stage(lock, prefix, output)
                self.assertFalse(output.exists())

    def test_incomplete_or_unbounded_lock_fails(self):
        for name in ("/usr/local/lib/libcares.so.2.19.5", "/usr/local/share/licenses/c-ares/LICENSE.md"):
            lock = copy.deepcopy(LOCK)
            lock["files"] = [row for row in lock["files"] if row["path"] != name]
            with self.assertRaises(provider.ProviderError):
                provider.validate_lock(lock)
        lock = copy.deepcopy(LOCK)
        lock["files"].append(dict(lock["files"][0], path="/usr/local/bin/gcc"))
        with self.assertRaisesRegex(provider.ProviderError, "only locked runtime"):
            provider.validate_lock(lock)
        for target in ("/usr/local/lib/libcurl.so.4.8.0", "../../libcurl.so.4.8.0", "unknown.so"):
            lock = copy.deepcopy(LOCK)
            row = next(row for row in lock["files"] if row["path"].endswith("/libcurl.so.4"))
            row["symlink"] = target
            with self.assertRaises(provider.ProviderError):
                provider.validate_lock(lock)

    def test_resolver_policy_rejects_threaded_or_missing_ares(self):
        for content in ("#define USE_THREADS_POSIX 1\n#define USE_ARES 1\n", "/* #undef USE_ARES */\n"):
            _, prefix, _ = self.fixture()
            (prefix / "share/atrinik/curl/curl_config.h").write_text(content)
            with self.assertRaises(provider.ProviderError):
                provider.verify_resolver(prefix)
        _, prefix, _ = self.fixture()
        contract = prefix / "share/atrinik/classic-curl-toolchain.json"
        values = json.loads(contract.read_text())
        values["threaded_resolver"] = True
        contract.write_text(json.dumps(values))
        with self.assertRaisesRegex(provider.ProviderError, "Unqualified"):
            provider.verify_resolver(prefix)

    def test_package_checks_detect_indirect_omission_ssl_drift_and_shadow_curl(self):
        observed = {r["Package"]: (r["Version"], r["Architecture"]) for r in LOCK["packages"]}
        provider.verify_packages(LOCK, observed)
        for name in ("libunistring5", "libbrotli1", "libssl3t64", "libpython3.14",
                     "bsdutils", "login", "mount", "util-linux", "libuuid1"):
            changed = dict(observed)
            changed[name] = ("wrong-version", "amd64")
            with self.assertRaisesRegex(provider.ProviderError, "Runtime package mismatch"):
                provider.verify_packages(LOCK, changed)
            lock = copy.deepcopy(LOCK)
            lock["packages"] = [r for r in lock["packages"] if r["Package"] != name]
            with self.assertRaisesRegex(provider.ProviderError, "Missing direct or indirect"):
                provider.validate_lock(lock)
        observed["libcurl4t64"] = ("8.18.0", "amd64")
        with self.assertRaisesRegex(provider.ProviderError, "distro libcurl"):
            provider.verify_packages(LOCK, observed)

    def test_actual_loader_parser_rejects_unresolved_and_empty_evidence(self):
        expected = dict(LOCK["curl_distro_sonames"], **{"libcurl.so.4": "/usr/local/lib/libcurl.so.4",
                                                     "libcares.so.2": "/usr/local/lib/libcares.so.2"})
        output = "\n".join(f"{name} => {path} (0x1234)" for name, path in expected.items() if name != "loader")
        output += f"\n {expected['loader']} (0x1234)\n"
        self.assertEqual(provider.parse_ldd(output), expected)
        for missing in ("libunistring.so.5", "libbrotlicommon.so.1", "libssl.so.3"):
            broken = output.replace(f"{missing} => {expected[missing]} (0x1234)", f"{missing} => not found")
            with self.assertRaisesRegex(provider.ProviderError, "Unresolved ELF"):
                provider.parse_ldd(broken)
        with self.assertRaisesRegex(provider.ProviderError, "No dynamic"):
            provider.parse_ldd("statically linked\n")
        with tempfile.TemporaryDirectory(dir=os.environ.get("ATRINIK_TEST_TMPDIR")) as temporary:
            target = Path(temporary) / "fixture-elf"
            target.write_text("loader output fixture; not a native qualification")
            with patch.object(provider.subprocess, "check_output", return_value=output), \
                    patch.object(Path, "resolve", lambda path, strict=False: path):
                provider.verify_elf(LOCK, [target])
            shadow = output.replace("/usr/local/lib/libcurl.so.4", "/usr/lib/x86_64-linux-gnu/libcurl.so.4")
            with patch.object(provider.subprocess, "check_output", return_value=shadow), \
                    patch.object(Path, "resolve", lambda path, strict=False: path):
                with self.assertRaisesRegex(provider.ProviderError, "Unexpected ELF provider"):
                    provider.verify_elf(LOCK, [target])
            indirect = "\n".join(line for line in output.splitlines() if "libunistring.so.5" not in line)
            with patch.object(provider.subprocess, "check_output", return_value=indirect), \
                    patch.object(Path, "resolve", lambda path, strict=False: path):
                with self.assertRaisesRegex(provider.ProviderError, "Incomplete"):
                    provider.verify_elf(LOCK, [target])

    def test_dependency_free_python_modules_retain_all_loader_checks(self):
        expected = dict(LOCK["curl_distro_sonames"], **{
            "libcurl.so.4": "/usr/local/lib/libcurl.so.4",
            "libcares.so.2": "/usr/local/lib/libcares.so.2"})
        linked = "\n".join(f"{name} => {path} (0x1234)" for name, path in expected.items() if name != "loader")
        linked += f"\n {expected['loader']} (0x1234)\n"
        with tempfile.TemporaryDirectory(dir=os.environ.get("ATRINIK_TEST_TMPDIR")) as temporary:
            root = Path(temporary)
            modules = root / "lib-dynload"
            modules.mkdir()
            module = modules / "_asyncio.cpython-314-x86_64-linux-gnu.so"
            module.touch()
            core = root / "atrinik-server"
            core.touch()
            with patch.object(provider, "PYTHON_DYNLOAD", modules), \
                    patch.object(Path, "resolve", lambda path, strict=False: path):
                with patch.object(provider.subprocess, "check_output", side_effect=[linked, "\tstatically linked\n"]):
                    provider.verify_elf(LOCK, [core, module])
                for output in ("", "unknown output", "statically linked\nlibc.so.6 => not found",
                               "statically linked\nunexpected extra line"):
                    with self.subTest(output=output), \
                            patch.object(provider.subprocess, "check_output", return_value=output):
                        with self.assertRaisesRegex(provider.ProviderError, "Runtime ELF target .*_asyncio"):
                            provider.verify_elf(LOCK, [module])
                with patch.object(provider.subprocess, "check_output", return_value="statically linked"):
                    with self.assertRaisesRegex(provider.ProviderError, "Incomplete curl/c-ares"):
                        provider.verify_elf(LOCK, [module])
                    for name in ("atrinik-server", "atrinik-access-status", "libcurl.so.4", "libplugin_arena.so"):
                        target = root / name
                        target.touch()
                        with self.subTest(target=name), self.assertRaisesRegex(provider.ProviderError, "No dynamic ELF"):
                            provider.verify_elf(LOCK, [target])
                with patch.object(provider.subprocess, "check_output", side_effect=subprocess.CalledProcessError(
                        1, ["ldd", str(module)], output="statically linked")):
                    with self.assertRaisesRegex(provider.ProviderError, "Runtime ELF target .*_asyncio.*exit 1"):
                        provider.verify_elf(LOCK, [module])
                module.unlink()
                with self.assertRaisesRegex(provider.ProviderError, "Missing runtime ELF target"):
                    provider.verify_elf(LOCK, [module])

    def test_complete_package_graph_rejects_indirect_omissions(self):
        roots = ["fixture-server", "fixture-python"]
        dependencies = {"fixture-server": ["fixture-gd", "fixture-libc"],
                        "fixture-python": ["fixture-ffi", "fixture-libc"],
                        "fixture-gd": ["fixture-png"], "fixture-png": ["fixture-libc"],
                        "fixture-ffi": ["fixture-libc"], "fixture-libc": []}
        packages = [{"Package": name} for name in dependencies]
        provider.verify_package_closure(roots, dependencies, packages)
        for missing in ("fixture-png", "fixture-ffi"):
            changed = {name: deps for name, deps in dependencies.items() if name != missing}
            with self.assertRaisesRegex(provider.ProviderError, "Incomplete|Missing indirect"):
                provider.verify_package_closure(roots, changed, [row for row in packages if row["Package"] != missing])
            with self.assertRaisesRegex(provider.ProviderError, "Incomplete"):
                provider.verify_package_closure(roots, changed, packages)
        with self.assertRaisesRegex(provider.ProviderError, "Unrelated"):
            provider.verify_package_closure(roots, dict(dependencies, compiler=[]), packages + [{"Package": "compiler"}])

    def test_locked_actual_dpkg_graph_covers_every_indirect_package(self):
        roots = set(LOCK["package_roots"])
        indirect = {row["Package"] for row in LOCK["packages"]} - roots
        self.assertTrue({"libpng16-16t64", "libffi8", "libtinfo6"}.issubset(indirect))
        for missing in indirect:
            with self.subTest(missing=missing):
                lock = copy.deepcopy(LOCK)
                lock["packages"] = [row for row in lock["packages"] if row["Package"] != missing]
                with self.assertRaises(provider.ProviderError):
                    provider.validate_lock(lock)
        lock = copy.deepcopy(LOCK)
        # Removing an actual edge while retaining the recorded dpkg selection
        # must fail even if another path still reaches this shared dependency.
        name = next(name for name, deps in lock["package_dependencies"].items() if deps)
        lock["package_dependencies"][name].pop()
        with self.assertRaisesRegex(provider.ProviderError, "edge mismatch|Unrelated"):
            provider.validate_lock(lock)

    def test_package_cli_emits_exact_version_arguments(self):
        result = subprocess.run(["python3", str(ROOT / "server/docker/runtime-provider.py"), "packages"],
                                capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout.splitlines(), [f"{r['Package']}={r['Version']}" for r in LOCK["packages"]])
        self.assertFalse(any(line.startswith("libcurl4t64=") for line in result.stdout.splitlines()))

    def test_runtime_base_utilities_require_complete_matching_package_versions(self):
        for name in ("bsdutils", "login", "mount", "util-linux", "libblkid1",
                     "libmount1", "libsmartcols1", "libuuid1"):
            with self.subTest(missing=name):
                lock = copy.deepcopy(LOCK)
                lock["packages"] = [row for row in lock["packages"] if row["Package"] != name]
                with self.assertRaises(provider.ProviderError):
                    provider.validate_lock(lock)
        for name in ("libuuid1", "libblkid1", "libmount1", "libsmartcols1", "libpam-modules-bin"):
            with self.subTest(changed=name):
                lock = copy.deepcopy(LOCK)
                next(row for row in lock["packages"] if row["Package"] == name)["Version"] += ".2"
                with self.assertRaisesRegex(provider.ProviderError, "Unsatisfied locked dpkg dependency"):
                    provider.validate_lock(lock)

    def test_dependency_selection_rejects_wrong_alternative_and_version(self):
        versions = {"fixture-a": "1:2.3-1", "fixture-b": "3.0"}
        provider.verify_dependency_selection("fixture", {
            "requirement": "fixture-a (>= 1:2.0) | fixture-b", "selected": "fixture-a"}, versions)
        for requirement, selected in (("fixture-a (>= 1:2.4)", "fixture-a"),
                                      ("fixture-b", "fixture-a"),
                                      ("fixture-a (invalid 2.3)", "fixture-a")):
            with self.subTest(requirement=requirement), self.assertRaises(provider.ProviderError):
                provider.verify_dependency_selection("fixture", {
                    "requirement": requirement, "selected": selected}, versions)

    def run_docker_apt_acquisition(self, failed_phase=None):
        # Execute the Dockerfile's real update/install chain with only apt-get
        # replaced. This checks option propagation through xargs and shell
        # failure handling without network or package changes on the test host.
        docker = (ROOT / "server/Dockerfile").read_text().replace("\\\n", " ")
        start = docker.index("    && apt-get ") + len("    && ")
        end = docker.index("    && rm -rf /var/lib/apt/lists/*", start)
        acquisition = docker[start:end].strip()
        with tempfile.TemporaryDirectory(dir=os.environ.get("ATRINIK_TEST_TMPDIR")) as temporary:
            root = Path(temporary)
            log = root / "apt.jsonl"
            lock = root / "packages.lock"
            lock.write_text("fixture-package=1.2.3\n")
            stub = root / "apt-get"
            stub.write_text(
                "#!" + sys.executable + "\n"
                "import json, os, sys\n"
                "with open(os.environ['APT_TEST_LOG'], 'a') as stream:\n"
                "    stream.write(json.dumps(sys.argv[1:]) + '\\n')\n"
                "sys.exit(100 if os.environ.get('APT_TEST_FAIL') in sys.argv[1:] else 0)\n")
            stub.chmod(0o755)
            acquisition = acquisition.replace("/tmp/runtime-provider-packages.lock", shlex.quote(str(lock)))
            env = dict(os.environ, PATH=str(root) + os.pathsep + os.environ["PATH"],
                       APT_TEST_LOG=str(log), APT_TEST_FAIL=failed_phase or "")
            result = subprocess.run(["/bin/sh", "-c", acquisition], env=env,
                                    capture_output=True, text=True, timeout=10)
            calls = [json.loads(line) for line in log.read_text().splitlines()]
        return result, calls

    def test_docker_apt_uses_explicit_public_trust_for_both_acquisitions(self):
        result, calls = self.run_docker_apt_acquisition()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(calls), 2)
        for args in calls:
            self.assertEqual(args[:2], ["-o", "Acquire::https::CAInfo=/etc/ssl/certs/ca-certificates.crt"])
        self.assertEqual(calls[0][2:], ["-o", "APT::Update::Error-Mode=any", "update"])
        self.assertEqual(calls[1][2:], ["install", "-y", "--no-install-recommends", "fixture-package=1.2.3"])

    def test_docker_apt_failed_refresh_never_installs_from_stale_indexes(self):
        result, calls = self.run_docker_apt_acquisition("update")
        self.assertEqual(result.returncode, 100)
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0][-1], "update")

    def test_docker_apt_install_failure_propagates(self):
        result, calls = self.run_docker_apt_acquisition("install")
        self.assertEqual(result.returncode, 123)  # xargs propagates child failure.
        self.assertEqual(len(calls), 2)

    def test_docker_uses_pinned_images_and_checks_without_tls_bypass(self):
        docker = (ROOT / "server/Dockerfile").read_text()
        self.assertIn("FROM " + LOCK["build_image"] + " AS build", docker)
        # Dependabot owns the runtime image pin in Dockerfile. Validate an
        # immutable Ubuntu reference without duplicating its current digest.
        self.assertRegex(docker, r"(?m)^FROM ubuntu:[^\s@]+@sha256:[0-9a-f]{64}$")
        self.assertIn("https://snapshot.ubuntu.com/ubuntu/" + LOCK["snapshot"] + "/", docker)
        self.assertIn("RUN --network=none python3 tools/dependencies.py", docker)
        self.assertIn("COPY --from=build /opt/runtime-provider/ /usr/local/", docker)
        self.assertNotIn("COPY --from=build /usr/local/", docker)
        for forbidden in ("Verify-Peer=false", "Verify-Host=false", "--allow-unauthenticated",
                          "--allow-downgrades", "trusted=yes", "libcurl4t64"):
            self.assertNotIn(forbidden, docker)
        self.assertIn("runtime-provider.py verify-runtime", docker)
        self.assertIn("chown -R atrinik:atrinik maps server/data server/assets/data", docker)
        self.assertIn("USER atrinik", docker)


if __name__ == "__main__":
    unittest.main()
