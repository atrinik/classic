"""Adversarial checks use real SquashFS fixtures and host ELF files, no compiler."""
from __future__ import annotations

import hashlib
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

SPEC = importlib.util.spec_from_file_location("atrinik_appimage", Path(__file__).resolve().parents[1] / "release/appimage.py")
appimage = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = appimage
SPEC.loader.exec_module(appimage)


class ListingTests(unittest.TestCase):
    def row(self, name, mode="-rw-r--r--", target="", owner="0/0"):
        return f"{mode} {owner} 1 2026-10-10 00:00 squashfs-root/{name}{target}\n"

    def test_unsafe_member_names_and_types(self):
        for name in ("../outside", "/absolute", "a/../outside", "a//outside", "a\\outside", "a\nextra"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                appimage._listing(self.row(name))
        for mode in ("brw-r--r--", "crw-r--r--", "prw-r--r--", "-rwsr-xr-x", "-rw-r-----"):
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                appimage._listing(self.row("unsafe", mode))
        with self.assertRaises(ValueError):
            appimage._listing(self.row("unowned", owner="1000/1000"))

    def test_duplicate_and_nondirectory_ancestors(self):
        for listing in (self.row("a") * 2, self.row("a") + self.row("a/b"), self.row("a/b")):
            with self.assertRaises(ValueError):
                appimage._listing(listing)

    def test_escaping_dangling_cyclic_and_directory_links(self):
        cases = (
            self.row("link", "lrwxrwxrwx", " -> /etc/passwd"),
            self.row("link", "lrwxrwxrwx", " -> ../outside"),
            self.row("link", "lrwxrwxrwx", " -> absent"),
            self.row("a", "lrwxrwxrwx", " -> b") + self.row("b", "lrwxrwxrwx", " -> a"),
            self.row("dir", "drwxr-xr-x") + self.row("link", "lrwxrwxrwx", " -> dir"),
        )
        for listing in cases:
            with self.subTest(listing=listing), self.assertRaises(ValueError):
                appimage._listing(listing)

    def test_safe_relative_library_link(self):
        listing = self.row("usr", "drwxr-xr-x") + self.row("usr/lib", "drwxr-xr-x")
        listing += self.row("usr/lib/libtest.so.1.2")
        listing += self.row("usr/lib/libtest.so.1", "lrwxrwxrwx", " -> libtest.so.1.2")
        self.assertEqual(len(appimage._listing(listing)), 4)

    def test_unsafe_filesystem_root_rejected(self):
        for row in ("drwxr-xr-x 1000/1000 1 2026-10-10 00:00 squashfs-root\n",
                    "drwxr-sr-x 0/0 1 2026-10-10 00:00 squashfs-root\n",
                    "drwxr-x--- 0/0 1 2026-10-10 00:00 squashfs-root\n"):
            with self.subTest(row=row), self.assertRaisesRegex(ValueError, "filesystem root"):
                appimage._listing(row)


@unittest.skipUnless(all(Path("/usr/bin", tool).is_file() for tool in ("readelf", "unsquashfs", "mksquashfs"))
                     and Path("/usr/bin/true").is_file()
                     and Path("/usr/lib/x86_64-linux-gnu/libzstd.so.1").is_file()
                     and Path("/usr/lib/x86_64-linux-gnu/ossl-modules/legacy.so").is_file(),
                     "real SquashFS fixtures require trusted inspectors and host zstd ELF")
class AppImageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="atrinik-appimage-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.payload = self.root / "AppDir"
        self.source = self.root / "source"
        self.version = "5.17.0"
        self.revision = "a" * 40
        runtime = bytearray(128)
        runtime[:6] = b"\x7fELF\x02\x01"
        runtime[8:11] = b"AI\x02"
        runtime[18:20] = b"\x3e\x00"
        self.runtime = bytes(runtime)
        self.lock = {
            "schema": 1,
            "runtime": {"version": "fixture", "size": len(runtime),
                        "digest_md5_offset": 80, "digest_md5_size": 16,
                        "digest_policy": "zero-field-md5-v1",
                        "sha256": hashlib.sha256(runtime).hexdigest()},
            "sources": [], "tools": {},
            "system_library_packages": {"libzstd.so.1": "libzstd1"},
            "system_package_versions": {"libzstd1": "fixture-version"},
            "system_package_licenses": {"libzstd1": "LicenseRef-Debian-libzstd1"},
            "system_package_copyright_sha256": {
                "libzstd1": hashlib.sha256(b"fixture system notice\n").hexdigest()},
            "system_package_notice_files": {"libzstd1": ["BSD"]},
            "system_common_license_sha256": {"BSD": hashlib.sha256(b"fixture common BSD notice\n").hexdigest()},
            "host_libraries": ["libc.so.6", "libm.so.6", "ld-linux-x86-64.so.2", "libcrypto.so.3"],
            "required_libraries": ["libzstd.so.1"], "dlopen_libraries": [],
            "notices": ["usr/share/doc/atrinik/LICENSE.md", "usr/share/doc/atrinik/ATTRIBUTIONS.md"],
        }
        self.lock_path = self.source / "tools/ci/appimage/packaging.lock.json"
        self.lock_path.parent.mkdir(parents=True)
        self.lock_path.write_text(json.dumps(self.lock, sort_keys=True))
        self.launcher = b"#!/bin/sh\necho 'fixture launcher must never execute'\nexit 93\n"
        (self.lock_path.parent / "AppRun").write_bytes(self.launcher)
        self.write("AppRun", self.launcher, executable=True)
        self.write("atrinik.desktop", "[Desktop Entry]\nType=Application\nExec=atrinik\nIcon=atrinik\nName=Atrinik\n")
        self.write("atrinik.png", b"\x89PNG\r\n\x1a\nfixture")
        self.client_revision = self.revision
        self.write("usr/bin/atrinik", self.client_elf(), executable=True)
        library = Path("/usr/lib/x86_64-linux-gnu/libzstd.so.1").resolve()
        self.library_name = "usr/lib/" + library.name
        self.write(self.library_name, library.read_bytes(), executable=True)
        self.write("usr/lib/ossl-modules/legacy.so", Path("/usr/lib/x86_64-linux-gnu/ossl-modules/legacy.so").read_bytes())
        self.write("usr/share/atrinik/openssl.cnf", "\n".join(appimage.OPENSSL_CONFIG) + "\n")
        self.write("usr/share/atrinik/ca-bundle.crt", "fixture certificate trust bundle\n")
        (self.lock_path.parent / "openssl.cnf").write_bytes((self.payload / "usr/share/atrinik/openssl.cnf").read_bytes())
        (self.source / "client").mkdir()
        (self.source / "client/ca-bundle.crt").write_bytes((self.payload / "usr/share/atrinik/ca-bundle.crt").read_bytes())
        (self.payload / "usr/lib/libzstd.so.1").symlink_to(library.name)
        (self.payload / ".DirIcon").symlink_to("atrinik.png")
        for name in self.lock["notices"]:
            self.write(name, "fixture copyright and license\n")
        self.write("usr/share/doc/atrinik/licenses/system/libzstd1/copyright", "fixture system notice\n")
        self.write("usr/share/doc/atrinik/licenses/system/common-licenses/BSD", "fixture common BSD notice\n")
        self.write("usr/share/games/atrinik/client.cfg", "fixture config\n")
        for directory in ("data", "sound", "fonts", "textures"):
            self.write(f"usr/share/games/atrinik/{directory}/fixture", "fixture\n")
        self.metadata = {"schema": 1, "version": self.version, "revision": self.revision,
                         "source_date_epoch": 1791648000,
                         "lock_sha256": hashlib.sha256(self.lock_path.read_bytes()).hexdigest(),
                         "build_features": {"testing": False, "coverage": False, "sanitizers": False},
                         "bundled_libraries": ["libzstd.so.1"],
                         "bundled_library_records": [{"name": "libzstd.so.1", "path": self.library_name,
                             "version": "fixture-version", "license": "LicenseRef-Debian-libzstd1", "source": "deb:libzstd1",
                             "sha256": hashlib.sha256(library.read_bytes()).hexdigest()}],
                         "native_inputs": {key: self.lock[key] for key in ("sources", "tools", "runtime")},
                         "system_license_notices": appimage.system_license_notices(self.payload, self.lock, ["libzstd1"])}

    def write(self, name, data, executable=False):
        path = self.payload / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data.encode() if isinstance(data, str) else data)
        path.chmod(0o755 if executable else 0o644)
        for parent in path.parents:
            if parent == self.payload.parent:
                break
            parent.chmod(0o755)
        return path

    def seal(self, data):
        data = bytearray(data)
        offset = self.lock["runtime"]["digest_md5_offset"]
        data[offset:offset + 16] = bytes(16)
        data[offset:offset + 16] = hashlib.md5(data, usedforsecurity=False).digest()
        return bytes(data)

    def client_elf(self, *, version=None, revision=None, flags=2, section_type=1,
                   duplicate=False, mapped=True, load_flags=4):
        """Append the producer's retained section to a host ELF, no compiler."""
        data = bytearray(Path("/usr/bin/true").read_bytes())
        section_offset = struct.unpack_from("<Q", data, 40)[0]
        section_count, strings_index = struct.unpack_from("<HH", data, 60)
        sections = [list(struct.unpack_from("<IIQQQQIIQQ", data, section_offset + index * 64))
                    for index in range(section_count)]
        strings_section = sections[strings_index]
        strings = bytes(data[strings_section[4]:strings_section[4] + strings_section[5]])
        program_offset = struct.unpack_from("<Q", data, 32)[0]
        program_count = struct.unpack_from("<H", data, 56)[0]
        programs = [list(struct.unpack_from("<IIQQQQQQ", data, program_offset + index * 56))
                    for index in range(program_count)]
        data.extend(b"\0Welcome to Atrinik version %s\0")
        start = (len(data) + 4095) // 4096 * 4096
        data.extend(bytes(start - len(data)))
        record = (b"ATRINIK_APPIMAGE_IDENTITY_V1\0version=" + (version or self.version).encode()
                  + b"\0revision=" + (revision or self.revision).encode() + b"\0")
        data.extend(record)
        address = 0x10000000
        section = [len(strings), section_type, flags, address if mapped else address + 1,
                   start, len(record), 0, 0, 1, 0]
        sections.append(section)
        if duplicate:
            sections.append(list(section))
        strings_section[4] = len(data)
        strings += b".atrinik.identity\0"
        strings_section[5] = len(strings)
        data.extend(strings)
        data.extend(bytes((-len(data)) % 8))
        section_offset = len(data)
        for entry in sections:
            data.extend(struct.pack("<IIQQQQIIQQ", *entry))
        program_offset = len(data)
        count = len(programs) + 1
        file_size = program_offset + count * 56 - start
        for entry in programs:
            if entry[0] == 6:  # Keep PT_PHDR consistent with the relocated table.
                entry[:] = [6, 4, program_offset, address + program_offset - start,
                            address + program_offset - start, count * 56, count * 56, 8]
        programs.append([1, load_flags, start, address, address, file_size, file_size, 4096])
        for entry in programs:
            data.extend(struct.pack("<IIQQQQQQ", *entry))
        struct.pack_into("<Q", data, 32, program_offset)
        struct.pack_into("<Q", data, 40, section_offset)
        struct.pack_into("<H", data, 56, count)
        struct.pack_into("<H", data, 60, len(sections))
        return bytes(data)

    def image(self, *, update_files=True, update_lock=True, padding=b"", filesystem_epoch=None):
        if self.metadata["revision"] != self.client_revision:
            self.write("usr/bin/atrinik", self.client_elf(revision=self.metadata["revision"]), executable=True)
            self.client_revision = self.metadata["revision"]
        if update_lock:
            self.write("usr/share/atrinik/packaging.lock.json", self.lock_path.read_bytes())
        if update_files:
            self.metadata["files"] = {
                path.relative_to(self.payload).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                for path in sorted(self.payload.rglob("*"))
                if path.is_file() and not path.is_symlink()
                and path.relative_to(self.payload).as_posix() != "usr/share/atrinik/appimage-manifest.json"
            }
        self.write("usr/share/atrinik/appimage-manifest.json", json.dumps(self.metadata, sort_keys=True))
        filesystem = self.root / "payload.squashfs"
        epoch = self.metadata["source_date_epoch"] if filesystem_epoch is None else filesystem_epoch
        result = subprocess.run(["/usr/bin/mksquashfs", str(self.payload), str(filesystem),
                                 "-noappend", "-all-root", "-no-xattrs", "-processors", "1",
                                 "-mkfs-time", str(epoch), "-all-time", str(epoch)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        candidate = self.root / "fixture.AppImage"
        candidate.write_bytes(self.seal(self.runtime + padding + filesystem.read_bytes()))
        return candidate

    def validate(self, path=None, **kwargs):
        return appimage.validate_appimage(path or self.image(), self.version,
                                         revision=self.revision, source_root=self.source, **kwargs)

    def test_valid_real_squashfs_without_payload_execution(self):
        image = self.image(padding=b"\0" * 4096)
        with mock.patch.object(appimage, "_run", wraps=appimage._run) as run:
            self.assertIsNone(self.validate(image))
            self.assertTrue(all(call.args[0] in {"unsquashfs", "readelf"} for call in run.call_args_list))
        inventory = appimage.read_appimage_inventory(image, self.version, revision=self.revision, source_root=self.source)
        self.assertEqual(inventory, self.metadata)

    def test_runtime_tampering_wrong_type_and_nonzero_padding(self):
        for offset in (8, 18, 40):
            image = self.image()
            data = bytearray(image.read_bytes())
            data[offset] ^= 1
            image.write_bytes(data)
            with self.subTest(offset=offset), self.assertRaisesRegex(ValueError, "runtime"):
                self.validate(image)
        with self.assertRaisesRegex(ValueError, "SquashFS"):
            self.validate(self.image(padding=b"untrusted-prefix"))

    def test_trusted_runtime_md5_field_requires_canonical_fullimage_digest(self):
        image = self.image()
        data = bytearray(image.read_bytes())
        self.assertNotEqual(data[80:96], bytes(16))
        self.validate(image)
        data[80:96] = bytes(range(16))
        image.write_bytes(data)
        with self.assertRaisesRegex(ValueError, "checksum differs"):
            self.validate(image)
        for offset in (79, 96):
            changed = bytearray(data)
            changed[offset] ^= 1
            image.write_bytes(changed)
            with self.subTest(offset=offset), self.assertRaisesRegex(ValueError, "runtime"):
                self.validate(image)
        changed = bytearray(self.image().read_bytes())
        changed[-1] ^= 1
        image.write_bytes(changed)
        with self.assertRaisesRegex(ValueError, "checksum differs"):
            self.validate(image)

    def test_runtime_mutable_field_bounds_come_only_from_trusted_lock(self):
        image = self.image()
        for field, value in (("digest_md5_offset", 8), ("digest_md5_offset", 127),
                             ("digest_md5_offset", True), ("digest_md5_size", 17),
                             ("digest_md5_size", "16"), ("digest_policy", "upstream")):
            lock = dict(self.lock, runtime=dict(self.lock["runtime"]))
            lock["runtime"][field] = value
            with self.subTest(field=field, value=value), self.assertRaisesRegex(ValueError, "runtime lock"):
                appimage._runtime_offset(image, lock)

    def test_candidate_symlink_and_truncated_squashfs(self):
        candidate = self.image()
        link = self.root / "candidate-link"
        link.symlink_to(candidate)
        with self.assertRaisesRegex(ValueError, "safely read"):
            self.validate(link)
        candidate.write_bytes(self.seal(self.runtime + b"hsqsbroken"))
        with self.assertRaisesRegex(ValueError, "SquashFS"):
            self.validate(candidate)

    def test_fifo_candidate_is_rejected_without_blocking(self):
        candidate = self.root / "fifo.AppImage"
        os.mkfifo(candidate)
        script = ("import sys; sys.path.insert(0, sys.argv[1]); "
                  "from appimage import validate_appimage; "
                  "validate_appimage(sys.argv[2], sys.argv[3], source_root=sys.argv[4])")
        result = subprocess.run([sys.executable, "-c", script,
                                 str(Path(appimage.__file__).parent), str(candidate),
                                 self.version, str(self.source)], capture_output=True,
                                text=True, timeout=3)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("bounded regular file", result.stderr)

    def test_escape_rejected_before_extraction(self):
        (self.payload / "escape").symlink_to("../../outside")
        with mock.patch.object(appimage, "_run", wraps=appimage._run) as run:
            with self.assertRaisesRegex(ValueError, "escaping"):
                self.validate()
        self.assertEqual([call.args[0] for call in run.call_args_list], ["unsquashfs"])

    def test_missing_layout_notice_and_launcher(self):
        for name in ("usr/share/doc/atrinik/ATTRIBUTIONS.md", "usr/share/games/atrinik/textures/fixture", "atrinik.png"):
            file = self.payload / name
            data = file.read_bytes()
            file.unlink()
            with self.subTest(name=name), self.assertRaises(ValueError):
                self.validate()
            self.write(name, data)
        self.write("AppRun", b"#!/bin/sh\nmalicious\n", executable=True)
        with self.assertRaisesRegex(ValueError, "launcher"):
            self.validate()

    def test_desktop_duplicate_exec_rejected(self):
        self.write("atrinik.desktop", "[Desktop Entry]\nType=Application\nExec=atrinik\nIcon=atrinik\nExec=/bin/sh\n")
        with self.assertRaisesRegex(ValueError, "duplicate"):
            self.validate()

    def test_manifest_cannot_authenticate_itself(self):
        for field, value in (("lock_sha256", "0" * 64), ("revision", "b" * 40),
                             ("native_inputs", {}), ("bundled_libraries", []),
                             ("version", "5.16.0"), ("schema", True),
                             ("build_features", {"testing": 0, "coverage": False, "sanitizers": False})):
            original = self.metadata[field]
            self.metadata[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.validate()
            self.metadata[field] = original

    def test_file_inventory_and_duplicate_json_keys(self):
        image = self.image()
        self.metadata["files"]["atrinik.png"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "file inventory"):
            self.validate(self.image(update_files=False))
        duplicate = self.root / "duplicate.json"
        duplicate.write_text('{"schema":1,"schema":1}')
        with self.assertRaisesRegex(ValueError, "duplicate JSON"):
            appimage._json(duplicate)

    def test_wrong_elf_architecture_version_build_path_and_testing(self):
        client = self.payload / "usr/bin/atrinik"
        original = client.read_bytes()
        cases = (original[:18] + b"\xb7\0" + original[20:],
                 self.client_elf(version="9.99.0"), self.client_elf(version="0.0.0"),
                 original + b"\0/home/builder/atrinik/source.c\0", original + b"\0__gcov_init\0")
        for data in cases:
            self.write("usr/bin/atrinik", data, executable=True)
            with self.subTest(tail=data[-40:]), self.assertRaises(ValueError):
                self.validate()

    def test_known_testing_markers_are_rejected_and_production_routes_allowed(self):
        client = self.payload / "usr/bin/atrinik"
        original = client.read_bytes()
        for marker in (b"--gpu-player-view", b"injected GPU conformance fault",
                       b"--help-parser-test", b"--widget-priority-test", b"--sound-test"):
            client.write_bytes(original + b"\0" + marker + b"\0")
            with self.subTest(marker=marker), self.assertRaisesRegex(ValueError, "test instrumentation"):
                self.validate()
        client.write_bytes(original + b"\0--live-movement-route\0")
        self.validate()

    def test_identity_uses_one_exact_mapped_readonly_section(self):
        client = self.payload / "usr/bin/atrinik"
        self.assertNotIn(b"\0" + self.version.encode() + b"\0", client.read_bytes())
        self.validate()
        cases = (Path("/usr/bin/true").read_bytes(), self.client_elf(duplicate=True),
                 self.client_elf(version="0.0.0"), self.client_elf(revision="b" * 40),
                 self.client_elf(flags=3), self.client_elf(flags=0),
                 self.client_elf(section_type=8), self.client_elf(mapped=False),
                 self.client_elf(load_flags=6))
        for data in cases:
            client.write_bytes(data)
            with self.subTest(size=len(data)), self.assertRaisesRegex(ValueError, "identity"):
                self.validate()
        data = bytearray(self.client_elf())
        section_offset = struct.unpack_from("<Q", data, 40)[0]
        count = struct.unpack_from("<H", data, 60)[0]
        struct.pack_into("<Q", data, section_offset + (count - 1) * 64 + 24, len(data) + 1)
        client.write_bytes(data)
        with self.assertRaisesRegex(ValueError, "identity"):
            self.validate()

    def test_missing_library_closure_and_dlopen_requirement(self):
        original = self.lock["host_libraries"]
        self.lock["host_libraries"] = []
        self.lock_path.write_text(json.dumps(self.lock, sort_keys=True))
        self.metadata["lock_sha256"] = hashlib.sha256(self.lock_path.read_bytes()).hexdigest()
        with self.assertRaisesRegex(ValueError, "closure"):
            self.validate()
        self.lock["host_libraries"] = original
        self.lock["dlopen_libraries"] = ["libmissing.so.1"]
        self.lock_path.write_text(json.dumps(self.lock, sort_keys=True))
        self.metadata["lock_sha256"] = hashlib.sha256(self.lock_path.read_bytes()).hexdigest()
        with self.assertRaisesRegex(ValueError, "required bundled library"):
            self.validate()

    def test_host_glibc_and_unexpected_program_rejected(self):
        self.write("usr/lib/libc.so.6", Path("/usr/lib/x86_64-linux-gnu/libc.so.6").read_bytes())
        with self.assertRaisesRegex(ValueError, "forbidden"):
            self.validate()
        (self.payload / "usr/lib/libc.so.6").unlink()
        self.write("usr/bin/extra", Path("/usr/bin/true").read_bytes(), executable=True)
        with self.assertRaisesRegex(ValueError, "unexpected.*program"):
            self.validate()

    def test_glibc_floor_rpath_and_instrumentation_inspectors(self):
        elf = self.payload / "usr/bin/atrinik"
        for dynamic, versions, symbols, expected in (
            ("", "Name: GLIBC_2.40", "", "glibc newer"),
            ("", "Name: GLIBC_PRIVATE", "", "private glibc"),
            ("(RUNPATH) Library runpath: [/tmp/build/lib]", "", "", "search path"),
            ("(NEEDED) Shared library: [../liboutside.so]", "", "", "library identity"),
            ("", "", "__asan_init", "test instrumentation"),
        ):
            with self.subTest(expected=expected), mock.patch.object(appimage, "_run", side_effect=[dynamic, versions, symbols]):
                with self.assertRaisesRegex(ValueError, expected):
                    appimage._elf(elf, executable=True)

    def test_library_provenance_is_bound_to_locked_package_and_file(self):
        record = self.metadata["bundled_library_records"][0]
        for field, value in (("source", "deb:unrelated"), ("version", "unlocked-version"),
                             ("license", "MIT"), ("sha256", "0" * 64),
                             ("path", "usr/bin/atrinik"), ("name", "libother.so.1")):
            original = record[field]
            record[field] = value
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, "provenance"):
                self.validate()
            record[field] = original
        (self.payload / "usr/share/doc/atrinik/licenses/system/libzstd1/copyright").unlink()
        with self.assertRaisesRegex(ValueError, "required file"):
            self.validate()

    def test_extended_attributes_and_trailing_payload_rejected(self):
        for mutation in ("xattrs", "trailing"):
            image = self.image()
            data = bytearray(image.read_bytes())
            if mutation == "xattrs":
                data[len(self.runtime) + 56:len(self.runtime) + 64] = b"\0" * 8
            else:
                data += b"unexpected tail"
            image.write_bytes(self.seal(data))
            with self.subTest(mutation=mutation), self.assertRaisesRegex(ValueError, "attributes|outside"):
                self.validate(image)

    def test_native_library_source_provenance(self):
        self.lock["sources"] = [{"name": "FixtureSDL", "version": "3.4.2", "license": "Zlib"}]
        self.lock_path.write_text(json.dumps(self.lock, sort_keys=True))
        self.metadata["lock_sha256"] = hashlib.sha256(self.lock_path.read_bytes()).hexdigest()
        self.metadata["native_inputs"]["sources"] = self.lock["sources"]
        record = self.metadata["bundled_library_records"][0]
        record.update(source="FixtureSDL", version="3.4.2", license="Zlib")
        self.metadata["system_license_notices"] = []
        with mock.patch.dict(appimage.SOURCE_LIBRARIES, {"libzstd.so.1": "FixtureSDL"}):
            self.validate()
            record["version"] = "0.0.0"
            with self.assertRaisesRegex(ValueError, "provenance"):
                self.validate()

    def test_system_license_and_notice_are_bound_to_trusted_lock(self):
        notice = self.payload / "usr/share/doc/atrinik/licenses/system/libzstd1/copyright"
        notice.write_text("substituted copyright and license\n")
        with self.assertRaisesRegex(ValueError, "system copyright"):
            self.validate()
        notice.write_text("fixture system notice\n")
        self.metadata["bundled_library_records"][0]["license"] = "NOASSERTION"
        with self.assertRaisesRegex(ValueError, "provenance"):
            self.validate()
        self.lock["system_package_licenses"]["libzstd1"] = "NOASSERTION"
        self.lock_path.write_text(json.dumps(self.lock, sort_keys=True))
        self.metadata["lock_sha256"] = hashlib.sha256(self.lock_path.read_bytes()).hexdigest()
        with self.assertRaisesRegex(ValueError, "actual license expression"):
            self.validate()

    def test_full_system_notice_records_reject_self_consistent_forgery(self):
        record = self.metadata["system_license_notices"][0]
        original = dict(record)
        for field, value in (("text", "invented notice text\n"), ("license_id", "LicenseRef-Unrelated"),
                             ("files", []), ("package", "unrelated"), ("sha256", "0" * 64)):
            record[field] = value
            if field == "text":
                record["sha256"] = hashlib.sha256(value.encode()).hexdigest()
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, "notice inventory"):
                self.validate()
            record.clear()
            record.update(original)
        notice = self.payload / "usr/share/doc/atrinik/licenses/system/common-licenses/BSD"
        notice.write_text("invented common license\n")
        with self.assertRaisesRegex(ValueError, "notice differs"):
            self.validate()

    def test_notice_helper_rejects_traversal_symlinks_and_missing_common_text(self):
        path = self.payload / "usr/share/doc/atrinik/licenses/system/common-licenses/BSD"
        original = path.read_bytes()
        path.unlink()
        with self.assertRaisesRegex(ValueError, "missing required notice"):
            appimage.system_license_notices(self.payload, self.lock, ["libzstd1"])
        path.symlink_to("../libzstd1/copyright")
        with self.assertRaisesRegex(ValueError, "bounded regular file"):
            appimage.system_license_notices(self.payload, self.lock, ["libzstd1"])
        path.unlink()
        path.write_bytes(original)
        self.lock["system_package_notice_files"]["libzstd1"] = ["../../outside"]
        with self.assertRaisesRegex(ValueError, "common-license names"):
            appimage.system_license_notices(self.payload, self.lock, ["libzstd1"])

    def test_openssl_provider_config_and_nested_modules(self):
        config = self.payload / "usr/share/atrinik/openssl.cnf"
        original = config.read_bytes()
        config.write_bytes(original + b".include /tmp/untrusted-config\n")
        with self.assertRaisesRegex(ValueError, "configuration"):
            self.validate()
        config.write_bytes(original)
        provider = self.payload / "usr/lib/ossl-modules/legacy.so"
        original_provider = provider.read_bytes()
        provider.write_bytes((self.payload / self.library_name).read_bytes())
        with self.assertRaisesRegex(ValueError, "provider.*SONAME"):
            self.validate()
        provider.write_bytes(original_provider)
        self.write("usr/lib/ossl-modules/unexpected.so", original_provider)
        with self.assertRaisesRegex(ValueError, "forbidden.*library"):
            self.validate()

    def test_actual_program_interpreter_rejected(self):
        client = self.payload / "usr/bin/atrinik"
        client.write_bytes(client.read_bytes().replace(b"/lib64/ld-linux-x86-64.so.2", b"/evilx/ld-linux-x86-64.so.2"))
        with self.assertRaisesRegex(ValueError, "program interpreter"):
            self.validate()

    def test_tls_trust_and_embedded_lock_cannot_authenticate_themselves(self):
        self.write("usr/share/atrinik/ca-bundle.crt", "malicious certificate trust\n")
        with self.assertRaisesRegex(ValueError, "certificate trust"):
            self.validate()
        self.write("usr/share/atrinik/ca-bundle.crt", (self.source / "client/ca-bundle.crt").read_bytes())
        self.image()
        self.write("usr/share/atrinik/packaging.lock.json", "{}\n")
        with self.assertRaisesRegex(ValueError, "embedded packaging lock"):
            self.validate(self.image(update_lock=False))

    def test_cli_reads_exact_source_blobs_and_rejects_head_drift(self):
        def git(*args):
            return subprocess.run(["/usr/bin/git", "-C", str(self.source), *args],
                                  capture_output=True, check=True, text=True).stdout.strip()
        git("init", "--quiet")
        (self.source / "fixture-extra.txt").write_text("shared resource contract\n")
        git("add", ".")
        git("-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
            "-c", "commit.gpgsign=false", "commit", "--quiet", "-m", "fixture")
        self.revision = git("rev-parse", "HEAD")
        self.metadata["revision"] = self.revision
        self.metadata["source_date_epoch"] = int(git("show", "-s", "--format=%ct", "HEAD"))
        candidate = self.image()
        # Mutable source bytes cannot replace the trusted revision's contract.
        self.lock_path.write_text("{}\n")
        args = ["appimage.py", str(candidate), self.version, "--revision", self.revision,
                "--source-root", str(self.source)]
        validate = appimage.validate_appimage
        def verify_shared_resources(path, version, *, revision, source_root, source_date_epoch):
            self.assertEqual((source_root / "fixture-extra.txt").read_text(), "shared resource contract\n")
            self.assertEqual(source_date_epoch, self.metadata["source_date_epoch"])
            validate(path, version, revision=revision, source_root=source_root, source_date_epoch=source_date_epoch)
        resources = appimage.TRUSTED_SOURCE_FILES + ("fixture-extra.txt",)
        with (mock.patch.object(sys, "argv", args), contextlib.redirect_stdout(io.StringIO()),
              mock.patch.object(appimage, "TRUSTED_SOURCE_FILES", resources),
              mock.patch.object(appimage, "validate_appimage", side_effect=verify_shared_resources)):
            self.assertEqual(appimage.main(), 0)
        args[4] = "f" * 40
        with mock.patch.object(sys, "argv", args), contextlib.redirect_stderr(io.StringIO()) as error:
            with self.assertRaises(SystemExit) as exited:
                appimage.main()
        self.assertEqual(exited.exception.code, 1)
        self.assertIn("HEAD differs", error.getvalue())
        args[4] = self.revision
        args.extend(["--source-date-epoch", str(self.metadata["source_date_epoch"] + 1)])
        with mock.patch.object(sys, "argv", args), contextlib.redirect_stderr(io.StringIO()) as error:
            with self.assertRaises(SystemExit):
                appimage.main()
        self.assertIn("source epoch differs", error.getvalue())

    def test_source_epoch_matches_trusted_commit_and_squashfs(self):
        epoch = self.metadata["source_date_epoch"]
        image = self.image()
        self.validate(image, source_date_epoch=epoch)
        for expected in (epoch + 1, -1, 2**32, True):
            with self.subTest(expected=expected), self.assertRaisesRegex(ValueError, "source epoch"):
                self.validate(image, source_date_epoch=expected)
        data = bytearray(image.read_bytes())
        offset = len(self.runtime) + 8
        data[offset:offset + 4] = (epoch + 1).to_bytes(4, "little")
        image.write_bytes(self.seal(data))
        with self.assertRaisesRegex(ValueError, "source epoch"):
            self.validate(image)
        for value in (True, -1, 2**32, str(epoch)):
            self.metadata["source_date_epoch"] = value
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, "source epoch"):
                self.validate(self.image(filesystem_epoch=epoch))


if __name__ == "__main__":
    unittest.main()
