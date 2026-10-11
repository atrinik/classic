#!/usr/bin/env python3
"""Inspect a Classic AppImage with trusted host tools; never run its payload."""
from __future__ import annotations

import argparse
from collections.abc import Iterable
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TRUSTED_SOURCE_FILES = (
    "tools/ci/appimage/packaging.lock.json",
    "tools/ci/appimage/AppRun",
    "tools/ci/appimage/openssl.cnf",
    "client/ca-bundle.crt",
)
MAX_IMAGE = 2 * 1024**3
MAX_PAYLOAD = 4 * 1024**3
BUILD_PATHS = re.compile(rb"/(?:home|root|workspace|build|__w|work)/|/tmp/(?:source/|build[-/]|atrinik-appimage[^/]*?/)|/opt/atrinik-appimage/")
TEST_FEATURES = re.compile(rb"(?:__gcov|__asan|__ubsan|__tsan|__lsan|ATRINIK_[A-Z_]*TESTING|atrinik_live_movement_test|movement_fault_injection|--gpu-player-view|injected GPU conformance fault|--help-parser-test|--widget-priority-test|--sound-test)")
HOST_ONLY = re.compile(r"(?:ld-linux.*|lib(?:c|m|pthread|dl|rt|resolv|util|anl)\.so(?:\..*)?|lib(?:vulkan|GL|EGL|GLX|OpenGL|GLES).*|.*_dri\.so)")
VERSION = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+")
SOURCE_LIBRARIES = {
    "libSDL3.so.0": "SDL3", "libSDL3_image.so.0": "SDL3_image",
    "libSDL3_ttf.so.0": "SDL3_ttf", "libSDL3_mixer.so.0": "SDL3_mixer",
    "libssl.so.3": "openssl", "libcrypto.so.3": "openssl",
    "libcurl.so.4": "curl", "libcares.so.2": "c-ares",
}
OPENSSL_CONFIG = ["openssl_conf = openssl_init", "[openssl_init]", "providers = provider_sect",
                  "[provider_sect]", "default = default_sect", "legacy = legacy_sect",
                  "[default_sect]", "activate = 1", "[legacy_sect]", "activate = 1"]


@dataclass(frozen=True)
class Entry:
    kind: str
    mode: str
    size: int
    target: str | None = None


def _run_bytes(tool: str, *args: str) -> bytes:
    # PATH and loader variables from a candidate/build environment must not
    # choose the inspector or affect its own dynamic linking.
    executable = Path("/usr/bin") / tool
    if not executable.is_file():
        raise ValueError(f"trusted host tool is unavailable: {executable}")
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("LD_", "GIT_")) and k not in {"PYTHONPATH", "PYTHONHOME"}}
    env.update(PATH="/usr/bin:/bin", LC_ALL="C", TZ="UTC", GIT_NO_REPLACE_OBJECTS="1")
    try:
        result = subprocess.run([str(executable), *args], capture_output=True,
                                env=env, timeout=120, check=False)
    except (OSError, subprocess.TimeoutExpired, UnicodeError) as exc:
        raise ValueError(f"{tool} inspection failed: {exc}") from exc
    if result.returncode:
        raise ValueError(f"{tool} inspection failed: {result.stderr.decode(errors='replace').strip()[:1000]}")
    return result.stdout


def _run(tool: str, *args: str) -> str:
    try:
        return _run_bytes(tool, *args).decode("utf-8")
    except UnicodeError as exc:
        raise ValueError(f"{tool} inspection returned invalid text") from exc


def _safe_name(name: str) -> bool:
    return (bool(name) and not name.startswith("/") and "\\" not in name
            and all(part not in {"", ".", ".."} for part in name.split("/"))
            and not any(ord(char) < 32 or ord(char) == 127 for char in name))


def _link_destination(name: str, target: str) -> str:
    if not target or target.startswith("/") or "\\" in target or any(ord(c) < 32 or ord(c) == 127 for c in target):
        raise ValueError(f"unsafe AppImage symlink: {name}")
    parts = list(PurePosixPath(name).parent.parts)
    for part in target.split("/"):
        if part in {"", "."}:
            continue
        if part == "..":
            if not parts:
                raise ValueError(f"escaping AppImage symlink: {name}")
            parts.pop()
        else:
            parts.append(part)
    return "/".join(parts)


def _listing(output: str) -> dict[str, Entry]:
    entries: dict[str, Entry] = {}
    pattern = re.compile(r"^([dl-][rwxstST-]{9})\s+(\d+)/(\d+)\s+(\d+)\s+\d{4}-\d\d-\d\d\s+\d\d:\d\d\s+(.*)$")
    total = 0
    root_seen = False
    for line in output.splitlines():
        if not line.strip():
            continue
        match = pattern.fullmatch(line)
        if match is None:
            raise ValueError("unsafe or unrecognized SquashFS member listing")
        mode, uid, gid, size, name = match.groups()
        if name == "squashfs-root" and mode[0] == "d":
            if (root_seen or int(uid) != 0 or int(gid) != 0 or any(c in mode for c in "sStT")
                    or mode[7] != "r" or mode[9] != "x"):
                raise ValueError("unsafe AppImage filesystem root")
            root_seen = True
            continue
        if not name.startswith("squashfs-root/"):
            raise ValueError("unexpected SquashFS root")
        name = name.removeprefix("squashfs-root/")
        target = None
        if mode[0] == "l":
            if " -> " not in name:
                raise ValueError("malformed SquashFS symlink")
            name, target = name.split(" -> ", 1)
        if (not _safe_name(name) or name in entries or int(uid) != 0 or int(gid) != 0
                or any(c in mode for c in "sStT")
                or mode[0] != "l" and (mode[7] != "r" or mode[0] == "d" and mode[9] != "x")):
            raise ValueError(f"unsafe AppImage member: {name}")
        entries[name] = Entry(mode[0], mode, int(size), target)
        total += int(size)
        if total > MAX_PAYLOAD or len(entries) > 200000:
            raise ValueError("AppImage payload exceeds inspection limits")
    for name in entries:
        for parent in PurePosixPath(name).parents:
            if str(parent) != "." and (str(parent) not in entries or entries[str(parent)].kind != "d"):
                raise ValueError(f"AppImage path has a non-directory ancestor: {name}")
    for name, entry in entries.items():
        if entry.kind == "l":
            seen = {name}
            destination = _link_destination(name, entry.target or "")
            while destination in entries and entries[destination].kind == "l":
                if destination in seen:
                    raise ValueError(f"cyclic AppImage symlink: {name}")
                seen.add(destination)
                destination = _link_destination(destination, entries[destination].target or "")
            if destination not in entries or entries[destination].kind != "-":
                raise ValueError(f"AppImage symlink must resolve to a payload file: {name}")
    return entries


def _runtime_offset(path: Path, lock: dict) -> int:
    runtime = lock.get("runtime", {})
    size = runtime.get("size")
    digest = runtime.get("sha256")
    md5_offset = runtime.get("digest_md5_offset")
    md5_size = runtime.get("digest_md5_size")
    if (type(size) is not int or not 64 <= size <= 16 * 1024**2
            or not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest)
            or type(md5_offset) is not int or type(md5_size) is not int or md5_size != 16
            or not 64 <= md5_offset <= size - md5_size
            or runtime.get("digest_policy") != "zero-field-md5-v1"):
        raise ValueError("trusted AppImage runtime lock is incomplete")
    with path.open("rb") as stream:
        prefix = stream.read(size)
        # appimagetool 1.9.1 writes this 16-byte field after constructing the
        # image. Its offset and original zero bytes are attested by the trusted
        # runtime lock. Never locate or resize a mutable section from candidate
        # ELF metadata: the normalized hash still binds every other byte,
        # including the section table identifying this exact field.
        normalized = prefix[:md5_offset] + bytes(md5_size) + prefix[md5_offset + md5_size:]
        if (len(prefix) != size or prefix[:6] != b"\x7fELF\x02\x01" or prefix[8:11] != b"AI\x02"
                or prefix[18:20] != b"\x3e\x00" or hashlib.sha256(normalized).hexdigest() != digest):
            raise ValueError("AppImage runtime differs from trusted x86_64 type-2 runtime")
        # This is format consistency, not an authenticity mechanism: the
        # normalized SHA-256 above authenticates the runtime, while the release
        # contract authenticates the entire artifact with SHA-256. The producer
        # replaces appimagetool 1.9.1's undefined skipped-buffer MD5 with this
        # deterministic full-file digest, treating only the locked field as
        # sixteen zero bytes. Other signature/key fields remain runtime-pinned.
        checksum = hashlib.md5(usedforsecurity=False)
        checksum.update(normalized)
        while chunk := stream.read(1024 * 1024):
            checksum.update(chunk)
        if prefix[md5_offset:md5_offset + md5_size] != checksum.digest():
            raise ValueError("AppImage runtime checksum differs from canonical full-file digest")
        stream.seek(size)
        # AppImage producers may align the SquashFS after the exact runtime.
        # Only zero padding is permitted; searching arbitrary payload bytes for
        # a magic value would let a forged prefix choose its own interpretation.
        padding = stream.read(1024 * 1024 + 4)
    offset = padding.find(b"hsqs")
    if offset < 0 or padding[:offset].strip(b"\0"):
        raise ValueError("AppImage has no safely aligned SquashFS payload")
    offset += size
    with path.open("rb") as stream:
        stream.seek(offset)
        superblock = stream.read(96)
        if len(superblock) != 96 or struct.unpack_from("<HH", superblock, 28) != (4, 0):
            raise ValueError("AppImage has an invalid SquashFS superblock")
        if struct.unpack_from("<Q", superblock, 56)[0] != 0xffffffffffffffff:
            raise ValueError("AppImage must not contain filesystem extended attributes")
        used = struct.unpack_from("<Q", superblock, 40)[0]
        remainder = path.stat().st_size - offset - used
        if used < 96 or not 0 <= remainder <= 4096:
            raise ValueError("AppImage has an invalid SquashFS extent")
        stream.seek(offset + used)
        if stream.read().strip(b"\0"):
            raise ValueError("AppImage contains data outside its SquashFS payload")
    return offset


def _json(path: Path) -> dict:
    def pairs(items: list[tuple[str, object]]) -> dict:
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError(f"duplicate JSON key: {key}")
            result[key] = value
        return result
    try:
        if path.stat().st_size > 4 * 1024**2:
            raise ValueError("AppImage metadata exceeds inspection limits")
        result = json.loads(path.read_text(), object_pairs_hook=pairs)
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError(f"invalid AppImage metadata: {path.name}") from exc
    if not isinstance(result, dict):
        raise ValueError("AppImage metadata must be an object")
    return result


def _elf(path: Path, *, executable: bool = False) -> tuple[set[str], str | None]:
    if path.stat().st_size > 256 * 1024**2:
        raise ValueError(f"AppImage ELF exceeds inspection limits: {path.name}")
    data = path.read_bytes()
    if len(data) < 64 or data[:6] != b"\x7fELF\x02\x01" or data[18:20] != b"\x3e\x00" or data[16:18] not in {b"\x02\0", b"\x03\0"}:
        raise ValueError(f"AppImage ELF must be little-endian x86_64: {path.name}")
    if BUILD_PATHS.search(data):
        raise ValueError(f"AppImage ELF contains an absolute build path: {path.name}")
    if TEST_FEATURES.search(data):
        raise ValueError(f"AppImage ELF contains test instrumentation: {path.name}")
    dynamic = _run("readelf", "--wide", "--dynamic", str(path))
    needed = set(re.findall(r"\(NEEDED\).*Shared library: \[([^\]]+)\]", dynamic))
    sonames = re.findall(r"\(SONAME\).*Library soname: \[([^\]]+)\]", dynamic)
    if len(sonames) > 1 or any(not _safe_name(n) or "/" in n for n in needed | set(sonames)):
        raise ValueError(f"unsafe ELF library identity: {path.name}")
    for search_path in re.findall(r"\((?:RPATH|RUNPATH)\).*Library (?:rpath|runpath): \[([^\]]*)\]", dynamic):
        for item in search_path.split(":"):
            if item not in {"$ORIGIN", "${ORIGIN}", "$ORIGIN/../lib", "${ORIGIN}/../lib", "$ORIGIN/..", "${ORIGIN}/.."}:
                raise ValueError(f"unsafe ELF library search path: {path.name}")
    versions = _run("readelf", "--wide", "--version-info", str(path))
    for major, minor in re.findall(r"\bGLIBC_(\d+)\.(\d+)(?:\.\d+)?\b", versions):
        if (int(major), int(minor)) > (2, 39):
            raise ValueError(f"AppImage requires glibc newer than 2.39: {path.name}")
    if "GLIBC_PRIVATE" in versions:
        raise ValueError(f"AppImage requires private glibc symbols: {path.name}")
    symbols = _run("readelf", "--wide", "--symbols", str(path))
    if TEST_FEATURES.search(symbols.encode()):
        raise ValueError(f"AppImage ELF contains test instrumentation: {path.name}")
    program = _run("readelf", "--wide", "--program-headers", str(path))
    interpreters = re.findall(r"\[Requesting program interpreter: ([^\]]+)\]", program)
    if (executable and interpreters != ["/lib64/ld-linux-x86-64.so.2"]
            or not executable and (interpreters or data[16:18] != b"\x03\0")):
        raise ValueError(f"AppImage ELF has an unexpected program interpreter: {path.name}")
    return needed, sonames[0] if sonames else None


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while data := stream.read(1024 * 1024):
            digest.update(data)
    return digest.hexdigest()


def _identity(data: bytes, version: str, revision: str) -> None:
    """Require the retained client record in one mapped read-only ELF section."""
    if len(data) < 64 or data[:6] != b"\x7fELF\x02\x01" or data[18:20] != b"\x3e\0":
        raise ValueError("AppImage identity must belong to an x86_64 ELF")
    section_offset = struct.unpack_from("<Q", data, 40)[0]
    section_size, section_count, strings_index = struct.unpack_from("<HHH", data, 58)
    if (section_size != 64 or not 0 < strings_index < section_count or section_offset < 64
            or section_offset + section_size * section_count > len(data)):
        raise ValueError("AppImage identity has a malformed ELF section table")
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, section_offset + index * section_size)
                for index in range(section_count)]
    strings_section = sections[strings_index]
    start, size = strings_section[4:6]
    if strings_section[1] != 3 or start < 64 or size == 0 or start + size > len(data):
        raise ValueError("AppImage identity has a malformed section-name table")
    strings = data[start:start + size]
    matches = []
    for section in sections:
        name_offset = section[0]
        if name_offset >= len(strings) or b"\0" not in strings[name_offset:]:
            raise ValueError("AppImage identity has a malformed ELF section name")
        if strings[name_offset:].split(b"\0", 1)[0] == b".atrinik.identity":
            matches.append(section)
    if len(matches) != 1:
        raise ValueError("AppImage client must have exactly one .atrinik.identity section")
    section = matches[0]
    start, size = section[4:6]
    expected = b"ATRINIK_APPIMAGE_IDENTITY_V1\0version=" + version.encode("ascii") + b"\0revision=" + revision.encode("ascii") + b"\0"
    if (section[1] != 1 or section[2] != 2 or start < 64 or start + size > len(data)
            or data[start:start + size] != expected):
        raise ValueError("AppImage client identity record differs from trusted source version or revision")
    program_offset = struct.unpack_from("<Q", data, 32)[0]
    program_size, program_count = struct.unpack_from("<HH", data, 54)
    if program_size != 56 or program_count == 0 or program_offset + program_size * program_count > len(data):
        raise ValueError("AppImage identity has a malformed ELF program table")
    mapped = False
    for index in range(program_count):
        segment = struct.unpack_from("<IIQQQQQQ", data, program_offset + index * program_size)
        kind, flags, offset, address, _, file_size, memory_size, _ = segment
        if (kind == 1 and offset <= start and start + size <= offset + file_size
                and section[3] == address + start - offset):
            if not flags & 4 or flags & 2 or file_size > memory_size or offset + file_size > len(data):
                raise ValueError("AppImage client identity is not mapped read-only")
            mapped = True
    if not mapped:
        raise ValueError("AppImage client identity is not allocated in a read-only load segment")


def system_license_notices(root: Path | str, lock: dict, packages: Iterable[str]) -> list[dict]:
    """Inventory locked notice texts without inferring their legal scope."""
    root = Path(root)
    package_names = list(packages)
    if any(not isinstance(name, str) or not re.fullmatch(r"[a-z0-9][a-z0-9+.-]*", name)
           for name in package_names):
        raise ValueError("invalid AppImage system notice package")
    records = []
    total = 0
    for package in sorted(set(package_names)):
        license_id = lock.get("system_package_licenses", {}).get(package)
        if not isinstance(license_id, str) or not re.fullmatch(r"LicenseRef-[A-Za-z0-9.-]+", license_id):
            raise ValueError(f"AppImage package has no source-bound notice LicenseRef: {package}")
        common = lock.get("system_package_notice_files", {}).get(package)
        if (not isinstance(common, list) or any(not isinstance(name, str)
                or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9+_.-]*", name) for name in common)
                or common != sorted(set(common))):
            raise ValueError(f"invalid common-license names in trusted AppImage lock: {package}")
        paths = [(f"usr/share/doc/atrinik/licenses/system/{package}/copyright",
                  lock.get("system_package_copyright_sha256", {}).get(package))]
        paths.extend((f"usr/share/doc/atrinik/licenses/system/common-licenses/{name}",
                      lock.get("system_common_license_sha256", {}).get(name)) for name in common)
        files, texts = [], []
        for relative, expected in paths:
            path = root / relative
            try:
                identity = path.lstat()
                if not stat.S_ISREG(identity.st_mode) or not 0 < identity.st_size <= 4 * 1024**2:
                    raise ValueError(f"AppImage notice must be a bounded regular file: {relative}")
                data = path.read_bytes()
            except OSError as exc:
                raise ValueError(f"AppImage missing required notice: {relative}") from exc
            digest = hashlib.sha256(data).hexdigest()
            if digest != expected:
                raise ValueError(f"AppImage notice differs from trusted lock: {relative}")
            try:
                text = data.decode("utf-8")
            except UnicodeError as exc:
                raise ValueError(f"AppImage notice is not exact UTF-8 text: {relative}") from exc
            files.append({"path": relative, "sha256": digest})
            texts.append(f"===== {relative} =====\n" + text + "\n")
        text = "".join(texts)
        total += len(text.encode("utf-8"))
        if total > 4 * 1024**2:
            raise ValueError("AppImage system notice inventory exceeds metadata limits")
        records.append({"package": package, "license_id": license_id, "text": text,
                        "sha256": hashlib.sha256(text.encode("utf-8")).hexdigest(), "files": files})
    return records


def _payload(root: Path, entries: dict[str, Entry], version: str, lock: dict,
             lock_digest: str, source_root: Path, revision: str | None,
             source_date_epoch: int | None, squashfs_epoch: int) -> dict:
    allowed = ("usr/bin", "usr/lib", "usr/share/games/atrinik", "usr/share/doc/atrinik",
               "usr/share/atrinik", "usr/share/alsa", "usr/share/applications", "usr/share/pixmaps")
    top_files = {"AppRun", "atrinik.desktop", "atrinik.png", ".DirIcon"}
    for name, entry in entries.items():
        if name not in top_files and not any(name == prefix or name.startswith(prefix + "/") or entry.kind == "d" and prefix.startswith(name + "/") for prefix in allowed):
            raise ValueError(f"unexpected AppImage payload path: {name}")
    def required(name: str) -> Path:
        entry = entries.get(name)
        if entry is None or entry.kind != "-" or entry.size == 0:
            raise ValueError(f"AppImage missing required file: {name}")
        return root / name
    launcher = required("AppRun")
    if launcher.read_bytes() != (source_root / "tools/ci/appimage/AppRun").read_bytes() or entries["AppRun"].mode[9] != "x":
        raise ValueError("AppImage launcher differs from trusted launcher")
    desktop = required("atrinik.desktop").read_text().splitlines()
    fields = {}
    section = False
    for line in desktop:
        if not line or line.startswith("#"):
            continue
        if line == "[Desktop Entry]" and not section:
            section = True
            continue
        if not section or "=" not in line or line.startswith("["):
            raise ValueError("AppImage desktop entry has unexpected sections")
        key, value = line.split("=", 1)
        if key in fields or key in {"Path", "Actions", "DBusActivatable"}:
            raise ValueError("AppImage desktop entry has unsafe or duplicate keys")
        fields[key] = value
    if (fields.get("Exec") != "atrinik" or fields.get("Icon") != "atrinik"
            or fields.get("Type") != "Application" or fields.get("TryExec", "atrinik") != "atrinik"):
        raise ValueError("AppImage desktop entry must launch atrinik")
    for name, reference in (("usr/share/applications/atrinik.desktop", "atrinik.desktop"),
                            ("usr/share/pixmaps/atrinik.png", "atrinik.png")):
        if name in entries and required(name).read_bytes() != required(reference).read_bytes():
            raise ValueError(f"AppImage installed desktop integration differs: {name}")
    for prefix in ("usr/share/applications/", "usr/share/pixmaps/"):
        if any(name.startswith(prefix) and name not in {"usr/share/applications/atrinik.desktop", "usr/share/pixmaps/atrinik.png"} for name in entries):
            raise ValueError("AppImage has unexpected desktop integration files")
    with required("atrinik.png").open("rb") as icon:
        if icon.read(8) != b"\x89PNG\r\n\x1a\n":
            raise ValueError("AppImage icon must be PNG")
    for notice in lock.get("notices", []):
        if not isinstance(notice, str) or not _safe_name(notice) or not notice.startswith("usr/share/doc/atrinik/"):
            raise ValueError("invalid notice in trusted AppImage lock")
        required(notice)
    required("usr/share/doc/atrinik/LICENSE.md")
    config_path = required("usr/share/atrinik/openssl.cnf")
    config = config_path.read_text().splitlines()
    if [line.strip() for line in config if line.strip() and not line.lstrip().startswith("#")] != OPENSSL_CONFIG:
        raise ValueError("AppImage OpenSSL configuration differs from trusted provider contract")
    if config_path.read_bytes() != (source_root / "tools/ci/appimage/openssl.cnf").read_bytes():
        raise ValueError("AppImage OpenSSL configuration differs from trusted source")
    if required("usr/share/atrinik/ca-bundle.crt").read_bytes() != (source_root / "client/ca-bundle.crt").read_bytes():
        raise ValueError("AppImage certificate trust differs from trusted source")
    if _sha256(required("usr/share/atrinik/packaging.lock.json")) != lock_digest:
        raise ValueError("AppImage embedded packaging lock differs from trusted source")
    required("usr/share/games/atrinik/client.cfg")
    for directory in ("data", "sound", "fonts", "textures"):
        prefix = f"usr/share/games/atrinik/{directory}/"
        if not any(name.startswith(prefix) and entry.kind == "-" and entry.size for name, entry in entries.items()):
            raise ValueError(f"AppImage missing game data: {directory}")
    metadata = _json(required("usr/share/atrinik/appimage-manifest.json"))
    if (type(metadata.get("schema")) is not int or metadata.get("schema") != 1 or metadata.get("version") != version
            or metadata.get("lock_sha256") != lock_digest
            or metadata.get("build_features") != {"testing": False, "coverage": False, "sanitizers": False}
            or any(type(v) is not bool for v in metadata.get("build_features", {}).values())):
        raise ValueError("AppImage manifest differs from trusted build contract")
    if (not isinstance(metadata.get("revision"), str)
            or not re.fullmatch(r"[0-9a-f]{40}", metadata["revision"])
            or revision is not None and metadata["revision"] != revision):
        raise ValueError("AppImage manifest has wrong source revision")
    epoch = metadata.get("source_date_epoch")
    if (type(epoch) is not int or not 0 <= epoch <= 0xffffffff
            or source_date_epoch is not None and epoch != source_date_epoch
            or epoch != squashfs_epoch):
        raise ValueError("AppImage source epoch differs from trusted source or SquashFS")
    native_inputs = {key: lock.get(key, {}) for key in ("sources", "tools", "runtime")}
    if metadata.get("native_inputs") != native_inputs:
        raise ValueError("AppImage native inputs differ from trusted lock")
    file_inventory = {name: _sha256(root / name)
                      for name, entry in sorted(entries.items())
                      if entry.kind == "-" and name != "usr/share/atrinik/appimage-manifest.json"}
    if metadata.get("files") != file_inventory:
        raise ValueError("AppImage file inventory differs from payload")
    client = required("usr/bin/atrinik")
    if entries["usr/bin/atrinik"].mode[9] != "x":
        raise ValueError("AppImage client is not executable")
    if client.stat().st_size > 256 * 1024**2:
        raise ValueError("AppImage client exceeds inspection limits")
    data = client.read_bytes()
    _identity(data, version, metadata["revision"])
    if b"Welcome to Atrinik version %s" not in data:
        raise ValueError("AppImage client has wrong Atrinik application identity")
    dependencies: dict[str, set[str]] = {}
    providers: dict[str, str] = {}
    client_needed, _ = _elf(client, executable=True)
    if not client_needed:
        raise ValueError("AppImage client has no dynamic application-library dependencies")
    dependencies["usr/bin/atrinik"] = client_needed
    for name, entry in entries.items():
        if entry.kind != "-":
            continue
        file = root / name
        if name == "usr/lib/ossl-modules/legacy.so":
            needed, soname = _elf(file)
            if soname is not None:
                raise ValueError("AppImage OpenSSL provider has unexpected SONAME")
            dependencies[name] = needed
        elif name.startswith("usr/lib/"):
            if PurePosixPath(name).parent != PurePosixPath("usr/lib") or ".so" not in file.name or HOST_ONLY.fullmatch(file.name):
                raise ValueError(f"forbidden AppImage bundled library: {name}")
            needed, soname = _elf(file)
            if soname is None or HOST_ONLY.fullmatch(soname) or soname in providers:
                raise ValueError(f"invalid or duplicate AppImage library SONAME: {name}")
            providers[soname] = name
            dependencies[name] = needed
        elif name == "usr/bin/openssl":
            if entry.mode[9] != "x":
                raise ValueError("AppImage openssl inspector is not executable")
            dependencies[name], _ = _elf(file, executable=True)
        elif name != "usr/bin/atrinik":
            if name.startswith("usr/bin/"):
                raise ValueError(f"unexpected AppImage program: {name}")
            with file.open("rb") as stream:
                if stream.read(4) == b"\x7fELF":
                    raise ValueError(f"unexpected AppImage executable: {name}")
    for soname, name in providers.items():
        link = entries.get(f"usr/lib/{soname}")
        if link is None or (root / "usr/lib" / soname).resolve() != (root / name).resolve():
            raise ValueError(f"AppImage library SONAME is not loadable: {soname}")
    required("usr/lib/ossl-modules/legacy.so")
    host = set(lock.get("host_libraries", []))
    for libraries in dependencies.values():
        missing = libraries - providers.keys() - host
        if missing:
            raise ValueError("AppImage library closure is incomplete: " + ", ".join(sorted(missing)))
    for soname in set(lock.get("required_libraries", [])) | set(lock.get("dlopen_libraries", [])):
        if soname not in providers:
            raise ValueError(f"AppImage missing required bundled library: {soname}")
    if metadata.get("bundled_libraries") != sorted(providers):
        raise ValueError("AppImage bundled-library manifest differs from ELF payload")
    records = metadata.get("bundled_library_records")
    if not isinstance(records, list) or len(records) != len(providers):
        raise ValueError("AppImage library provenance records are incomplete")
    sources = {item["name"]: item for item in lock.get("sources", [])}
    expected_records = []
    system_packages = set()
    for soname, name in sorted(providers.items()):
        if soname in SOURCE_LIBRARIES:
            source_name = SOURCE_LIBRARIES[soname]
            source = sources.get(source_name)
            if source is None:
                raise ValueError(f"AppImage library has no trusted source: {soname}")
            version_name, license_name = source.get("version"), source.get("license")
        else:
            package = lock.get("system_library_packages", {}).get(soname)
            version_name = lock.get("system_package_versions", {}).get(package)
            if not package or not version_name:
                raise ValueError(f"AppImage library has no trusted system package: {soname}")
            source_name = "deb:" + package
            system_packages.add(package)
            license_name = lock.get("system_package_licenses", {}).get(package)
            copyright_digest = lock.get("system_package_copyright_sha256", {}).get(package)
            notice = required(f"usr/share/doc/atrinik/licenses/system/{package}/copyright")
            if _sha256(notice) != copyright_digest:
                raise ValueError(f"AppImage system copyright differs from trusted lock: {package}")
        if not isinstance(version_name, str) or not version_name or not isinstance(license_name, str) or not license_name:
            raise ValueError(f"AppImage library has incomplete source provenance: {soname}")
        if re.search(r"\b(?:NOASSERTION|NONE)\b", license_name):
            raise ValueError(f"AppImage library has no actual license expression: {soname}")
        expected_records.append({"name": soname, "path": name, "version": version_name,
                                 "sha256": file_inventory[name], "license": license_name,
                                 "source": source_name})
    if records != expected_records:
        raise ValueError("AppImage library provenance differs from trusted lock or ELF payload")
    if any(name.startswith("usr/share/alsa/") and entry.kind == "-" for name, entry in entries.items()):
        system_packages.add("libasound2-data")
    if metadata.get("system_license_notices") != system_license_notices(root, lock, system_packages):
        raise ValueError("AppImage system notice inventory differs from trusted texts")
    return metadata


def read_appimage_inventory(path: Path | str, version: str, *, revision: str | None = None,
                            source_root: Path | str | None = None,
                            source_date_epoch: int | None = None) -> dict:
    """Validate once and return metadata bound to the trusted source checkout."""
    if VERSION.fullmatch(version) is None:
        raise ValueError("AppImage version must be MAJOR.MINOR.PATCH")
    if revision is not None and re.fullmatch(r"[0-9a-f]{40}", revision) is None:
        raise ValueError("AppImage revision must be a full lowercase commit ID")
    if source_date_epoch is not None and (type(source_date_epoch) is not int or not 0 <= source_date_epoch <= 0xffffffff):
        raise ValueError("AppImage expected source epoch must be a uint32 integer")
    source = Path(source_root) if source_root is not None else ROOT
    lock_path = source / "tools/ci/appimage/packaging.lock.json"
    lock = _json(lock_path)
    lock_digest = hashlib.sha256(lock_path.read_bytes()).hexdigest()
    with tempfile.TemporaryDirectory(prefix="atrinik-appimage-inspect-") as temporary:
        snapshot = Path(temporary) / "candidate.AppImage"
        try:
            descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
            with os.fdopen(descriptor, "rb") as image_stream:
                identity = os.fstat(image_stream.fileno())
                if not stat.S_ISREG(identity.st_mode) or identity.st_size > MAX_IMAGE:
                    raise ValueError("AppImage must be a bounded regular file")
                with snapshot.open("wb") as target:
                    copied = 0
                    while data := image_stream.read(1024 * 1024):
                        copied += len(data)
                        if copied > MAX_IMAGE:
                            raise ValueError("AppImage exceeds inspection limits")
                        target.write(data)
        except OSError as exc:
            raise ValueError("cannot safely read AppImage") from exc
        offset = _runtime_offset(snapshot, lock)
        with snapshot.open("rb") as stream:
            stream.seek(offset + 8)
            squashfs_epoch = struct.unpack("<I", stream.read(4))[0]
        offset = str(offset)
        entries = _listing(_run("unsquashfs", "-lln", "-o", offset, str(snapshot)))
        destination = Path(temporary) / "payload"
        _run("unsquashfs", "-no-xattrs", "-processors", "1", "-o", offset,
             "-d", str(destination), str(snapshot))
        return _payload(destination, entries, version, lock, lock_digest, source, revision,
                        source_date_epoch, squashfs_epoch)


def validate_appimage(path: Path | str, version: str, *, revision: str | None = None,
                     source_root: Path | str | None = None,
                     source_date_epoch: int | None = None) -> None:
    """Raise ValueError unless PATH satisfies the trusted source build contract."""
    try:
        read_appimage_inventory(path, version, revision=revision, source_root=source_root,
                               source_date_epoch=source_date_epoch)
    except (OSError, UnicodeError) as exc:
        raise ValueError(f"cannot inspect AppImage: {exc}") from exc


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path)
    parser.add_argument("version")
    parser.add_argument("--revision", help="full expected source commit")
    parser.add_argument("--source-root", type=Path, help="trusted source checkout at the expected commit")
    parser.add_argument("--source-date-epoch", type=int, help="expected source commit timestamp")
    args = parser.parse_args()
    try:
        if args.source_root is not None or args.revision is not None:
            if args.revision is None or re.fullmatch(r"[0-9a-f]{40}", args.revision) is None:
                raise ValueError("--source-root requires a full --revision")
            source = (args.source_root or ROOT).resolve()
            if _run("git", "-C", str(source), "rev-parse", "--verify", "HEAD").strip() != args.revision:
                raise ValueError("trusted source checkout HEAD differs from expected revision")
            if Path(_run("git", "-C", str(source), "rev-parse", "--show-toplevel").strip()).resolve() != source:
                raise ValueError("trusted source root is not the checkout root")
            epoch = int(_run("git", "-C", str(source), "show", "-s", "--format=%ct", args.revision).strip())
            if args.source_date_epoch is not None and args.source_date_epoch != epoch:
                raise ValueError("expected source epoch differs from trusted source commit")
            with tempfile.TemporaryDirectory(prefix="atrinik-appimage-source-") as temporary:
                immutable = Path(temporary)
                for name in TRUSTED_SOURCE_FILES:
                    destination = immutable / name
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    destination.write_bytes(_run_bytes("git", "-C", str(source), "show", f"{args.revision}:{name}"))
                validate_appimage(args.path, args.version, revision=args.revision, source_root=immutable,
                                 source_date_epoch=epoch)
        else:
            validate_appimage(args.path, args.version, source_date_epoch=args.source_date_epoch)
    except (ValueError, OSError, UnicodeError) as exc:
        parser.exit(1, f"invalid AppImage: {exc}\n")
    print(f"validated AppImage: {args.path.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
