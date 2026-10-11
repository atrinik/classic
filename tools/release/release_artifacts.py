#!/usr/bin/env python3
"""Verify a closed candidate against its immutable source release contract."""
from __future__ import annotations

import argparse
import ast
import io
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import tarfile
import tempfile

from locked_inputs import load_locked_inputs

from finalize_artifacts import build_spdx, sha256

ROOT = Path(__file__).resolve().parents[2]
MODULES = ("client", "server", "editor", "libatrinik", "protocol")


def expected_names(version: str, schema: int) -> set[str]:
    if type(schema) is not int or schema not in (1, 2, 3):
        raise RuntimeError("unsupported release artifact schema")
    names = {f"atrinik-classic-{version}.tar.gz"}
    names.update(f"atrinik-classic-{module}-{version}.tar.gz" for module in MODULES)
    names.update({f"atrinik-classic-client-{version}-windows-x86_64.zip",
                  f"atrinik-classic-server-{version}-windows-x86_64.zip",
                  f"atrinik_classic_protocol-{version}-py3-none-any.whl",
                  f"atrinik-classic-{version}.spdx.json", "release-manifest.json", "SHA256SUMS"})
    if schema == 2:
        names.add(f"atrinik-classic-client-{version}-linux-amd64.deb")
    if schema == 3:
        names.add(f"atrinik-classic-client-{version}-linux-x86_64.AppImage")
    return names


def git_value(root: Path, *args: str, preserve_whitespace: bool = False) -> str:
    result = subprocess.run(["git", "-C", str(root), *args], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError("cannot read immutable source contract: " + result.stderr.strip())
    return result.stdout if preserve_whitespace else result.stdout.strip()


def source_schema(root: Path, revision: str) -> int:
    if re.fullmatch(r"[0-9a-f]{40}", revision) is None:
        raise RuntimeError("revision must be a full lowercase commit ID")
    source = git_value(root, "show", f"{revision}:tools/release/finalize_artifacts.py")
    tree = ast.parse(source)
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == "RELEASE_ARTIFACT_SCHEMA" for t in node.targets):
            if isinstance(node.value, ast.Constant) and type(node.value.value) is int and node.value.value in (1, 2, 3):
                return node.value.value
            raise RuntimeError("unsupported source release artifact contract")
    # Historical releases predate the explicit constant. Recognize their exact
    # schema literal in the authoritative generator, never in candidate metadata.
    for node in tree.body:
        if isinstance(node, ast.FunctionDef) and node.name == "build_release_manifest":
            for returned in ast.walk(node):
                if isinstance(returned, ast.Return) and isinstance(returned.value, ast.Dict):
                    for key, value in zip(returned.value.keys, returned.value.values):
                        if isinstance(key, ast.Constant) and key.value == "schema_version" and isinstance(value, ast.Constant) and value.value == 1:
                            return 1
    raise RuntimeError("unrecognized source release artifact contract")


def deb_tar(path: Path, option: str) -> tarfile.TarFile:
    result = subprocess.run(["dpkg-deb", option, str(path)], capture_output=True)
    if result.returncode:
        raise RuntimeError(f"invalid Debian package {path.name}: {result.stderr.decode(errors='replace').strip()}")
    return tarfile.open(fileobj=io.BytesIO(result.stdout), mode="r:")


def safe_deb_members(archive: tarfile.TarFile) -> dict[str, tarfile.TarInfo]:
    entries = {}
    for member in archive:
        name = member.name.removeprefix("./").removesuffix("/")
        if name in ("", ".") and member.isdir():
            continue
        parts = name.split("/")
        if (PurePosixPath(name).is_absolute() or any(p in ("", ".", "..") for p in parts)
                or "\\" in name or any(ord(c) < 32 for c in name)
                or not (member.isfile() or member.isdir()) or member.mode & 0o6000
                or member.uid != 0 or member.gid != 0 or name in entries):
            raise RuntimeError(f"unsafe Debian package member: {member.name}")
        entries[name] = member
    for name, member in entries.items():
        if any(parent in entries and not entries[parent].isdir()
               for parent in (str(p) for p in PurePosixPath(name).parents) if parent != "."):
            raise RuntimeError(f"Debian package path collision: {name}")
    return entries


def validate_deb(path: Path, version: str) -> None:
    with deb_tar(path, "--ctrl-tarfile") as archive:
        entries = safe_deb_members(archive)
        if set(entries) - {"control", "md5sums"} or "control" not in entries or not entries["control"].isfile():
            raise RuntimeError("Debian package has unexpected control files or maintainer scripts")
        stream = archive.extractfile(entries["control"])
        assert stream is not None
        fields = {}
        for line in stream.read().decode("utf-8").splitlines():
            if line.startswith((" ", "\t")):
                continue
            if ":" not in line:
                raise RuntimeError("malformed Debian control")
            key, value = line.split(":", 1)
            if key in fields:
                raise RuntimeError("duplicate Debian control field")
            fields[key] = value.strip()
        for key, expected in (("Package", "atrinik"), ("Version", version), ("Architecture", "amd64")):
            if fields.get(key) != expected:
                raise RuntimeError(f"Debian package has wrong {key}")
        dependencies = fields.get("Depends", "").split(",")
        for package, minimum in (("libsdl3-0", "3.4"), ("libssl3t64", "3.5"),
                                 ("libsdl3-image0", "3.2"), ("libsdl3-ttf0", "3.2"),
                                 ("libsdl3-mixer0", "3.2.4")):
            matches = [re.fullmatch(rf"\s*{package}(?::amd64)?\s*\(>=\s*([^\s)]+)\)\s*", d) for d in dependencies]
            versions = [m.group(1) for m in matches if m is not None]
            # CPack combines authored runtime floors with generated shlibdeps.
            # Comma-separated duplicates are conjunctive: one sufficient lower
            # bound preserves the floor even when another bound is weaker.
            if not any(subprocess.run(
                ["dpkg", "--compare-versions", bound, "ge", minimum],
                capture_output=True,
            ).returncode == 0 for bound in versions):
                raise RuntimeError(f"Debian package requires {package} >= {minimum}")
        for package in ("ca-certificates", "libvulkan1"):
            if not any(re.fullmatch(rf"\s*{package}(?::amd64)?(?:\s*\([^)]*\))?\s*", d) for d in dependencies):
                raise RuntimeError(f"Debian package requires {package}")
    with deb_tar(path, "--fsys-tarfile") as archive:
        entries = safe_deb_members(archive)
        allowed = ("usr/games", "usr/share/games/atrinik", "usr/share/doc/atrinik", "usr/share/applications", "usr/share/icons", "usr/share/pixmaps")
        for name, member in entries.items():
            if (member.isdir() and member.mode & 0o005 != 0o005
                    or member.isfile() and not member.mode & 0o004):
                raise RuntimeError(f"Debian payload is inaccessible to ordinary users: {name}")
            if not any(name == root or name.startswith(root + "/") or member.isdir() and root.startswith(name + "/") for root in allowed):
                raise RuntimeError(f"unexpected Debian payload path: {name}")
            if member.isfile() and (".so" in PurePosixPath(name).name or name.endswith((".a", ".dll", ".exe"))):
                raise RuntimeError(f"private library in Debian package: {name}")
        required = ("usr/games/atrinik", "usr/share/applications/atrinik.desktop",
                    "usr/share/doc/atrinik/LICENSE.md", "usr/share/doc/atrinik/ATTRIBUTIONS.md",
                    "usr/share/games/atrinik/client.cfg")
        for name in required:
            if name not in entries or not entries[name].isfile() or entries[name].size == 0:
                raise RuntimeError(f"Debian package missing {name}")
        executable = archive.extractfile(entries["usr/games/atrinik"])
        assert executable is not None
        header = executable.read(20)
        if len(header) != 20 or header[:6] != b"\x7fELF\x02\x01" or header[18:20] != b"\x3e\x00":
            raise RuntimeError("Debian client must be a Linux amd64 ELF binary")
        desktop = archive.extractfile(entries["usr/share/applications/atrinik.desktop"])
        assert desktop is not None
        if "Exec=/usr/games/atrinik" not in desktop.read().decode("utf-8").splitlines():
            raise RuntimeError("Debian desktop entry must launch /usr/games/atrinik")
        if not entries["usr/games/atrinik"].mode & 0o001:
            raise RuntimeError("Debian client is not executable")
        for root in ("usr/share/games/atrinik/sound", "usr/share/games/atrinik/fonts", "usr/share/games/atrinik/textures", "usr/share/doc/atrinik"):
            if not any(name.startswith(root + "/") and member.isfile() and member.size > 0 for name, member in entries.items()):
                raise RuntimeError(f"Debian package missing payload {root}")


def validate_source_metadata(manifest: dict, source_root: Path, revision: str, version: str, schema: int) -> None:
    epoch = int(git_value(source_root, "show", "-s", "--format=%ct", revision))
    if type(manifest.get("source_epoch")) is not int or manifest["source_epoch"] != epoch:
        raise RuntimeError("candidate source epoch differs")
    descriptor = json.loads(git_value(source_root, "show", f"{revision}:dependencies.bundle.json"))
    if manifest.get("dependency_bundle") != descriptor:
        raise RuntimeError("candidate dependency bundle differs from source")
    with tempfile.TemporaryDirectory(prefix="atrinik-release-contract-") as temporary:
        root = Path(temporary)
        for component in ("client", "server"):
            path = root / component / "dependencies.lock.json"
            path.parent.mkdir()
            path.write_text(git_value(source_root, "show", f"{revision}:{component}/dependencies.lock.json"))
        inputs = load_locked_inputs(version, root, schema)
    if manifest.get("locked_inputs") != inputs:
        raise RuntimeError("candidate locked inputs differ from source")
    if schema == 3:
        packaging_lock = json.loads(git_value(source_root, "show", f"{revision}:tools/ci/appimage/packaging.lock.json"))
        if manifest.get("appimage_packaging_lock") != packaging_lock:
            raise RuntimeError("candidate AppImage packaging lock differs from source")


def validate_appimage_metadata(directory: Path, manifest: dict, source_root: Path,
                               revision: str, version: str) -> None:
    # The helper's current checkout cannot stand in for a historical source.
    # Give the package validator only the trusted files from this exact commit.
    from appimage import TRUSTED_SOURCE_FILES, read_appimage_inventory
    with tempfile.TemporaryDirectory(prefix="atrinik-appimage-contract-") as temporary:
        root = Path(temporary)
        for relative in TRUSTED_SOURCE_FILES:
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(git_value(source_root, "show", f"{revision}:{relative}", preserve_whitespace=True))
        inventory = read_appimage_inventory(
            directory / f"atrinik-classic-client-{version}-linux-x86_64.AppImage",
            version, revision=revision, source_root=root,
        )
    if manifest.get("appimage_inventory") != inventory:
        raise RuntimeError("candidate AppImage inventory differs from validated package")
    sbom_name = f"atrinik-classic-{version}.spdx.json"
    paths = sorted(path for path in directory.iterdir()
                   if path.name not in ("SHA256SUMS", "release-manifest.json", sbom_name))
    expected = build_spdx(paths, version, revision, manifest["source_epoch"],
                          manifest["locked_inputs"], inventory, manifest["appimage_packaging_lock"])
    if json.loads((directory / sbom_name).read_text()) != expected:
        raise RuntimeError("candidate SPDX differs from AppImage and locked inputs")


def validate_candidate(directory: Path, tag: str, revision: str, source_root: Path = ROOT) -> dict[str, tuple[int, str]]:
    if re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+", tag) is None:
        raise RuntimeError("tag must be vMAJOR.MINOR.PATCH")
    version = tag[1:]
    if tag != "v0.0.0" and git_value(source_root, "rev-parse", "--verify", f"{tag}^{{commit}}") != revision:
        raise RuntimeError("candidate revision differs from immutable tag")
    schema = source_schema(source_root, revision)
    manifest = json.loads((directory / "release-manifest.json").read_text())
    if not isinstance(manifest, dict) or type(manifest.get("schema_version")) is not int or manifest.get("schema_version") != schema or manifest.get("tag") != tag or manifest.get("version") != version or manifest.get("revision") != revision:
        raise RuntimeError("candidate manifest differs from immutable source contract")
    validate_source_metadata(manifest, source_root, revision, version, schema)
    expected = expected_names(version, schema)
    entries = list(directory.iterdir())
    if any(not path.is_file() or path.is_symlink() for path in entries) or {p.name for p in entries} != expected:
        raise RuntimeError("candidate artifact set differs from source contract")
    indexed = expected - {"release-manifest.json", "SHA256SUMS"}
    artifacts = manifest.get("artifacts")
    if not isinstance(artifacts, list):
        raise RuntimeError("invalid manifest artifacts")
    records = {}
    for artifact in artifacts:
        if not isinstance(artifact, dict) or not isinstance(artifact.get("name"), str) or artifact["name"] in records:
            raise RuntimeError("invalid or duplicate manifest artifact")
        records[artifact["name"]] = artifact
    if set(records) != indexed:
        raise RuntimeError("manifest artifact set differs from source contract")
    actual = {path.name: (path.stat().st_size, f"sha256:{sha256(path)}") for path in entries}
    for name, record in records.items():
        size, digest = actual[name]
        if type(record.get("size")) is not int or record["size"] != size or record.get("sha256") != digest[7:]:
            raise RuntimeError(f"candidate artifact hash/size mismatch: {name}")
    checksums = {}
    for line in (directory / "SHA256SUMS").read_text().splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  ([^/\\]+)", line)
        if match is None or match[2] in checksums:
            raise RuntimeError("invalid or duplicate checksum record")
        checksums[match[2]] = match[1]
    if set(checksums) != expected - {"SHA256SUMS"} or any(checksums[name] != actual[name][1][7:] for name in checksums):
        raise RuntimeError("candidate checksums differ")
    if schema == 2:
        validate_deb(directory / f"atrinik-classic-client-{version}-linux-amd64.deb", version)
    if schema == 3:
        validate_appimage_metadata(directory, manifest, source_root, revision, version)
    return actual


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--directory", type=Path)
    parser.add_argument("--tag")
    parser.add_argument("--revision", required=True)
    parser.add_argument("--source-root", type=Path, default=Path.cwd())
    parser.add_argument("--print-source-schema", action="store_true")
    args = parser.parse_args()
    if args.print_source_schema:
        if args.directory is not None or args.tag is not None:
            parser.error("--print-source-schema accepts only --revision and --source-root")
        try:
            print(source_schema(args.source_root, args.revision))
        except (OSError, ValueError, RuntimeError) as error:
            parser.exit(1, f"release source contract validation failed: {error}\n")
        return 0
    if args.directory is None or args.tag is None:
        parser.error("candidate validation requires --directory and --tag")
    try:
        assets = validate_candidate(args.directory, args.tag, args.revision, args.source_root)
    except (OSError, ValueError, RuntimeError, tarfile.TarError) as error:
        parser.exit(1, f"release candidate validation failed: {error}\n")
    print(f"verified {len(assets)} release assets")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
