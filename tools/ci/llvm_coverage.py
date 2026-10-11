#!/usr/bin/env python3
"""Export native LLVM coverage for the complete Classic binary graph."""
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

LLVM_VERSION = "23.1.3"
PROFILE_DIRECTORY = "llvm-profiles"
RAW_MAGIC = 0xFF6C70726F667281  # LLVM little-endian, 64-bit frontend profile.


def run(command):
    result = subprocess.run(command, check=True, text=True, capture_output=True)
    # llvm-cov can exit successfully while warning about mismatched functions
    # or stale profiles. Such output is incomplete acceptance evidence.
    if result.stderr.strip():
        raise ValueError(f"{command[0]} reported diagnostics: {result.stderr.strip()}")
    return result.stdout


def verify_tool(tool):
    version = run([tool, "--version"])
    if not re.search(r"(?:[Vv]ersion |LLD )" + re.escape(LLVM_VERSION) + r"(?![\d.])", version):
        raise ValueError(f"{tool} must be LLVM {LLVM_VERSION}")


def profile_directory(build_dir):
    # Never follow a symlink when cleaning task-owned profile output.
    if build_dir.is_symlink() or (build_dir / PROFILE_DIRECTORY).is_symlink():
        raise ValueError("coverage build/profile directory must not be a symlink")
    if not build_dir.is_dir():
        raise ValueError(f"coverage build directory is unavailable: {build_dir}")
    return build_dir / PROFILE_DIRECTORY


def prepare(build_dir):
    directory = profile_directory(build_dir)
    if directory.exists():
        shutil.rmtree(directory)
    directory.mkdir()
    return str(directory.resolve() / "%m-%p.profraw")


def coverage_objects(build_dir):
    objects = []
    seen = set()
    for path in sorted(build_dir.rglob("*")):
        if path.is_symlink() or not path.is_file():
            continue
        relative = path.relative_to(build_dir)
        if "CMakeFiles" in relative.parts or PROFILE_DIRECTORY in relative.parts:
            continue
        # Executables and plugins both carry coverage maps; static libraries
        # are represented by their linked consumers. ELF magic prevents source,
        # archives, scripts or generated files from being mistaken for objects.
        with path.open("rb") as stream:
            if stream.read(4) != b"\x7fELF":
                continue
            if b"__llvm_covmap" in stream.read():
                # CTest runtime trees contain byte-for-byte plugin copies.
                # They represent the same maps and instrumentation identity.
                with path.open("rb") as binary:
                    digest = hashlib.file_digest(binary, "sha256").digest()
                if digest not in seen:
                    objects.append(path)
                    seen.add(digest)
    if not objects:
        raise ValueError("no instrumented executables or shared plugins found")
    return objects


def binary_identity(path, readobj):
    """Read the pinned LLVM 23 ELF64 frontend instrumentation identity."""
    metadata = json.loads(run([readobj, "--elf-output-style=JSON", "--sections", "--symbols", str(path)]))
    if len(metadata) != 1 or metadata[0]["FileSummary"]["Format"] != "elf64-x86-64":
        raise ValueError(f"unsupported coverage binary format: {path}")
    sections = [entry["Section"] for entry in metadata[0]["Sections"]]

    def section(name):
        matches = [entry for entry in sections if entry["Name"]["Name"] == name]
        if len(matches) != 1:
            raise ValueError(f"missing or duplicate {name} section: {path}")
        return matches[0]

    with path.open("rb") as stream:
        header = stream.read(20)
        if header[:7] != b"\x7fELF\x02\x01\x01" or struct.unpack_from("<H", header, 18)[0] != 62:
            raise ValueError(f"unsupported coverage binary architecture: {path}")
        size = path.stat().st_size

        def read(offset, length):
            if offset < 0 or length <= 0 or offset + length > size:
                raise ValueError(f"invalid instrumentation section bounds: {path}")
            stream.seek(offset)
            return stream.read(length)

        versions = [entry["Symbol"] for entry in metadata[0]["Symbols"]
                    if entry["Symbol"]["Name"]["Name"] == "__llvm_profile_raw_version"]
        if len(versions) != 1 or versions[0]["Size"] != 8:
            raise ValueError(f"missing instrumentation version: {path}")
        version = versions[0]
        storage = section(version["Section"]["Name"])
        displacement = version["Value"] - storage["Address"]
        if not 0 <= displacement <= storage["Size"] - 8 or struct.unpack(
                "<Q", read(storage["Offset"] + displacement, 8))[0] != 11:
            raise ValueError(f"unsupported instrumentation version: {path}")
        data = section("__llvm_prf_data")
        if data["Offset"] % 8 or data["Size"] % 72:
            raise ValueError(f"invalid LLVM 23 instrumentation record sizing: {path}")
        content = read(data["Offset"], data["Size"])
        counter_section = section("__llvm_prf_cnts")
        read(counter_section["Offset"], counter_section["Size"])
        identities = []
        counter_ranges = []
        for offset in range(0, len(content), 72):
            name, function_hash = struct.unpack_from("<QQ", content, offset)
            counters = struct.unpack_from("<I", content, offset + 56)[0]
            # This helper supports the authored C frontend instrumentation,
            # without value profiling, offload counters or MC/DC bitmaps.
            if not counters or any(content[offset + 60:offset + 72]):
                raise ValueError(f"unsupported instrumentation record: {path}")
            counter_start = data["Address"] + offset + struct.unpack_from("<q", content, offset + 16)[0]
            counter_end = counter_start + counters * 8
            if (counter_start % 8 or counter_start < counter_section["Address"] or
                    counter_end > counter_section["Address"] + counter_section["Size"]):
                raise ValueError(f"invalid instrumentation counter bounds: {path}")
            counter_ranges.append((counter_start, counter_end))
            identities.append((name, function_hash, counters))
        counter_ranges.sort()
        if any(left[1] > right[0] for left, right in zip(counter_ranges, counter_ranges[1:])):
            raise ValueError(f"overlapping instrumentation counter records: {path}")
        # LLVM can retain multiple records with the same name/hash identity.
        # Their distinct counter ranges and multiplicity belong to the identity.
    return tuple(sorted(identities))


def profile_identity(text):
    identities = []
    for record in text.strip().split("\n\n"):
        match = re.fullmatch(r"([^\n]+)\n# Func Hash:\n([0-9]+)\n# Num Counters:\n([0-9]+)\n# Counter Values:\n([0-9\n]+)", record)
        if not match:
            raise ValueError("unsupported or malformed LLVM text profile")
        name, function_hash, count, values = match.groups()
        count = int(count)
        if not count or len(values.splitlines()) != count:
            raise ValueError("incomplete LLVM profile counters")
        name_hash = int.from_bytes(hashlib.md5(name.encode()).digest()[:8], "little")
        identities.append((name_hash, int(function_hash), count))
    return tuple(sorted(identities))


def executed_objects(objects, profiles, profdata, readobj):
    identities = {}
    for path in objects:
        identity = binary_identity(path, readobj)
        if identity in identities:
            raise ValueError("ambiguous instrumentation identity in different binaries")
        identities[identity] = path
    executed = set()
    for profile in profiles:
        # Including zero-count records distinguishes an unexecuted binary from
        # a stale/partial profile with a coincidentally matching entry point.
        identity = profile_identity(run([profdata, "show", "--all-functions", "--counts", "--text", str(profile)]))
        if identity not in identities:
            raise ValueError(f"profile does not match a complete current binary: {profile}")
        executed.add(identities[identity])
    return sorted(executed)


def source_prefixes(root, component):
    modules = ("client", "server", "libatrinik") if component == "all" else (component,)
    projects = {"client": "atrinik-client", "server": "atrinik-server", "libatrinik": "libatrinik"}
    # Generated release identity makes a flattened component root explicit. Check
    # it before directory names, so a scoped archive cannot impersonate another
    # component merely by containing a same-named subdirectory.
    if (root / "VERSION").exists() or (root / "SOURCE_REVISION").exists():
        try:
            version = (root / "VERSION").read_text().strip()
            revision = (root / "SOURCE_REVISION").read_text().strip()
            cmake = (root / "CMakeLists.txt").read_text()
        except OSError as error:
            raise ValueError("source root does not identify the requested Classic component") from error
        names = [quoted or plain for quoted, plain in re.findall(
            r'^\s*(?i:project)\s*\(\s*(?:"([^"]+)"|([\w-]+))(?=\s|\))', cmake, re.MULTILINE)]
        if (not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version) or
                not re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", revision) or len(names) != 1):
            raise ValueError("source root does not identify the requested Classic component")
        if names[0] != "atrinik-classic":
            if component == "all" or names[0] != projects[component]:
                raise ValueError("source root does not identify the requested Classic component")
            return [(component, root)]
    # A missing monorepo component must never widen the filter to the root.
    if component != "all" and not (root / component).is_dir():
        raise ValueError("source root does not identify the requested Classic component")
    return [(module, root / module) for module in modules]


def production_path(filename, root, prefixes):
    path = Path(filename)
    if not path.is_absolute():
        path = root / path
    path = path.resolve()
    for module, prefix in prefixes:
        if path.is_relative_to(prefix):
            relative = path.relative_to(prefix)
            excluded = {"tests", "build", "dependencies", "_deps", "third_party", "third-party"}
            if path.suffix == ".c" and not excluded.intersection(relative.parts):
                if module == "libatrinik" or relative.parts[0] == "src":
                    return str(path)
            break
    return None


def filter_lcov(report, source_root, component):
    root = source_root.resolve()
    prefixes = source_prefixes(root, component)
    records = []
    for record in report.split("end_of_record"):
        match = re.search(r"^SF:(.+)$", record, re.MULTILINE)
        if not match:
            continue
        if production_path(match.group(1), root, prefixes):
            records.append(record.strip() + "\nend_of_record\n")
    if not records:
        raise ValueError("coverage export contains no intended production C sources")
    return "".join(records)


def object_arguments(cov, objects, *options):
    command = [cov, "export", *options, str(objects[0])]
    for path in objects[1:]:
        command += ["-object", str(path)]
    return command


def zero_maps(report, source_root, component):
    root = source_root.resolve()
    prefixes = source_prefixes(root, component)
    document = json.loads(report)
    if (set(document) != {"data", "type", "version"} or
            document["type"] != "llvm.coverage.json.export" or document["version"] != "3.1.0" or
            len(document["data"]) != 1):
        raise ValueError("unsupported LLVM coverage JSON schema")
    data = document["data"][0]
    if set(data) != {"files", "functions", "totals"}:
        raise ValueError("unsupported LLVM coverage JSON data")

    def counters(records, width, indices):
        for record in records:
            if len(record) != width or any(type(record[index]) is not int or record[index] != 0 for index in indices):
                raise ValueError("unsupported or nonzero LLVM baseline mapping")

    def summary(metrics):
        if set(metrics) != {"branches", "functions", "instantiations", "lines", "regions", "mcdc"}:
            raise ValueError("unsupported LLVM coverage summary")
        for metric in metrics.values():
            if (not {"count", "covered", "percent"} <= set(metric) <= {"count", "covered", "percent", "notcovered"} or
                    type(metric["count"]) is not int or metric["count"] < 0 or
                    type(metric["covered"]) is not int or metric["covered"] != 0 or
                    type(metric["percent"]) not in (int, float) or metric["percent"] != 0 or
                    type(metric.get("notcovered", metric["count"])) is not int or
                    metric.get("notcovered", metric["count"]) != metric["count"]):
                raise ValueError("unsupported or nonzero LLVM baseline summary")

    summary(data["totals"])

    def normalized(value):
        if isinstance(value, list):
            return [normalized(item) for item in value]
        if isinstance(value, dict):
            return {key: (str((root / item).resolve()) if key == "filename" else
                          [str((root / path).resolve()) for path in item] if key == "filenames" else
                          normalized(item)) for key, item in value.items()}
        return value

    maps = {}
    for entry in data["files"]:
        if set(entry) != {"filename", "branches", "expansions", "mcdc_records", "segments", "summary"} or entry["mcdc_records"]:
            raise ValueError("unsupported LLVM coverage file mapping")
        counters(entry["segments"], 6, (2,))
        counters(entry["branches"], 9, (4, 5))
        summary(entry["summary"])
        for expansion in entry["expansions"]:
            if set(expansion) != {"branches", "filenames", "source_region", "target_regions"}:
                raise ValueError("unsupported LLVM coverage expansion mapping")
            counters(expansion["branches"], 9, (4, 5))
            counters([expansion["source_region"], *expansion["target_regions"]], 8, (4,))
        filename = production_path(entry["filename"], root, prefixes)
        if filename:
            if filename in maps:
                raise ValueError("duplicate production source in LLVM JSON")
            maps[filename] = [normalized(entry), []]
    for function in data["functions"]:
        if (set(function) != {"branches", "count", "filenames", "mcdc_records", "name", "regions"} or
                function["count"] != 0 or function["mcdc_records"]):
            raise ValueError("unsupported or nonzero LLVM baseline function")
        counters(function["branches"], 9, (4, 5))
        counters(function["regions"], 8, (4,))
        descriptor = json.dumps(normalized(function), sort_keys=True)
        for filename in set(function["filenames"]):
            filename = production_path(filename, root, prefixes)
            if filename in maps:
                maps[filename][1].append(descriptor)
    for entry in maps.values():
        entry[1].sort()  # Preserve multiplicity, filename table ordering and IDs.
    return maps


def lcov_records(report, source_root, component, zero=False):
    root = source_root.resolve()
    prefixes = source_prefixes(root, component)
    records = {}
    pending = []
    patterns = (r"TN:.*", r"SF:.+", r"FN:[1-9][0-9]*,.+", r"FNDA:[0-9]+,.+",
                r"(?:FNF|FNH|LF|LH|BRF|BRH):[0-9]+", r"DA:[1-9][0-9]*,[0-9]+",
                r"BRDA:[1-9][0-9]*,[0-9]+,[0-9]+,(?:[0-9]+|-)")
    for line in report.splitlines():
        if not line:
            continue
        if line != "end_of_record":
            if not any(re.fullmatch(pattern, line) for pattern in patterns):
                raise ValueError("unsupported or malformed LLVM LCOV record")
            pending.append(line)
            continue
        sources = [line[3:] for line in pending if line.startswith("SF:")]
        if len(sources) != 1:
            raise ValueError("missing or duplicate source in LLVM LCOV record")
        summaries = {}
        declarations, functions, lines, branches = [], [], {}, {}
        for entry in pending:
            key, value = entry.split(":", 1)
            if key in {"FNF", "FNH", "LF", "LH", "BRF", "BRH"}:
                if key in summaries:
                    raise ValueError("duplicate LLVM LCOV summary")
                summaries[key] = int(value)
            elif key == "FN":
                declarations.append(value.split(",", 1)[1])
            elif key == "FNDA":
                count, name = value.split(",", 1)
                functions.append(name)
                if zero and int(count):
                    raise ValueError("nonzero LLVM LCOV baseline function")
            elif key in {"DA", "BRDA"}:
                fields = value.split(",")
                identity = tuple(fields[:-1])
                target = lines if key == "DA" else branches
                if identity in target:
                    raise ValueError("duplicate LLVM LCOV counter")
                count = 0 if fields[-1] == "-" else int(fields[-1])
                target[identity] = count
                if zero and count:
                    raise ValueError("nonzero LLVM LCOV baseline counter")
        if (set(summaries) != {"FNF", "FNH", "LF", "LH", "BRF", "BRH"} or
                sorted(declarations) != sorted(functions) or
                (summaries["FNF"] and not declarations) or
                summaries["FNH"] > summaries["FNF"] or
                summaries["LH"] > summaries["LF"] or summaries["BRH"] > summaries["BRF"] or
                (summaries["LF"] and not lines) or (summaries["BRF"] and not branches) or
                (zero and any(summaries[key] for key in ("FNH", "LH", "BRH")))):
            raise ValueError("incomplete or inconsistent LLVM LCOV summaries")
        # LLVM summaries group source functions/lines/branches differently from
        # instantiated macro records. Preserve them; do not count flat rows.
        filename = production_path(sources[0], root, prefixes)
        if filename:
            if filename in records:
                raise ValueError("duplicate production source in LLVM LCOV")
            records[filename] = "\n".join(pending) + "\nend_of_record\n"
        pending = []
    if pending:
        raise ValueError("unterminated LLVM LCOV record")
    return records


def complete_report(cov, objects, executed, merged, source_root, component):
    measured = lcov_records(run(object_arguments(cov, executed, "-format=lcov", f"-instr-profile={merged}")), source_root, component)
    baseline = zero_maps(run(object_arguments(cov, objects, "-empty-profile")), source_root, component)
    associated = zero_maps(run(object_arguments(cov, executed, "-empty-profile")), source_root, component)
    zero_records = lcov_records(run(object_arguments(cov, objects, "-empty-profile", "-format=lcov")), source_root, component, zero=True)
    if not measured or set(measured) != set(associated) or set(zero_records) != set(baseline):
        raise ValueError("coverage export contains no intended production C sources or inconsistent file sets")
    # LCOV branch IDs are ordinal within a line, including macro expansions.
    # Never merge ordinal IDs from different map sets. Keep LLVM's measured
    # records intact, and append only entire source files from its zero baseline.
    for filename, mapping in associated.items():
        if baseline.get(filename) != mapping:
            raise ValueError(f"unexecuted binary adds mappings to a measured source; execute that binary: {filename}")
    records = {**zero_records, **measured}
    if set(records) != set(baseline):
        raise ValueError("incomplete production coverage file set")
    return "".join(records[name] for name in sorted(records))


def export(build_dir, source_root, component, output, compiler=None):
    """Collect measured counts plus authentic LLVM zero-hit file baselines.

    The caller owns freshness: prepare, run its tests successfully, then export.
    Structural identity detects incompatible/partial supplied profiles, but
    cannot date unchanged instrumentation or prove a wholly absent execution.
    Objects without a supplied profile contribute only explicitly zero counts.
    """
    if output != "-":
        # A failed collection must never leave an older report to be uploaded.
        Path(output).unlink(missing_ok=True)
    directory = profile_directory(build_dir)
    profiles = sorted(directory.glob("*.profraw"))
    if not profiles or any(path.is_symlink() or path.stat().st_size == 0 for path in profiles):
        raise ValueError("missing, empty or symlinked LLVM raw profiles")
    for profile in profiles:
        with profile.open("rb") as stream:
            header = stream.read(16)
        if len(header) != 16 or struct.unpack("<QQ", header) != (RAW_MAGIC, 11):
            raise ValueError(f"unsupported or malformed LLVM 23 raw profile header: {profile}")
    if compiler is None:
        cache = build_dir / "CMakeCache.txt"
        match = re.search(r"^CMAKE_C_COMPILER:(?:FILEPATH|STRING)=(.+)$", cache.read_text(), re.MULTILINE)
        if not match:
            raise ValueError("coverage build has no recorded C compiler")
        compiler = match.group(1)
    profdata = os.environ.get("LLVM_PROFDATA", "llvm-profdata")
    cov = os.environ.get("LLVM_COV", "llvm-cov")
    readobj = os.environ.get("LLVM_READOBJ", "llvm-readobj")
    for tool in (compiler, profdata, cov, readobj):
        verify_tool(tool)
    objects = coverage_objects(build_dir)
    executed = executed_objects(objects, profiles, profdata, readobj)
    merged = directory / "coverage.profdata"
    # Zero-count records retain legitimate hash variants from different linked
    # consumers; sparse merging would mistake those variants for stale data.
    run([profdata, "merge", "-o", str(merged), *map(str, profiles)])
    report = complete_report(cov, objects, executed, merged, source_root, component)
    if output == "-":
        sys.stdout.write(report)
    else:
        Path(output).write_text(report)
    print(f"LLVM coverage: {len(profiles)} profiles, {len(executed)} measured/{len(objects)} objects, {component} -> {output}", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    reset = commands.add_parser("prepare", help="clear only this build's raw profiles before its tests")
    reset.add_argument("--build-dir", required=True, type=Path)
    emit = commands.add_parser("export", help="merge profiles and export filtered LCOV")
    emit.add_argument("--build-dir", required=True, type=Path)
    emit.add_argument("--source-root", required=True, type=Path)
    emit.add_argument("--component", required=True, choices=("client", "server", "libatrinik", "all"))
    emit.add_argument("--output", required=True)
    emit.add_argument("--compiler", help="exact compiler when the build does not use CMake")
    args = parser.parse_args()
    try:
        if args.command == "prepare":
            print(prepare(args.build_dir))
        else:
            export(args.build_dir, args.source_root, args.component, args.output, args.compiler)
    except (OSError, ValueError, KeyError, TypeError, struct.error, subprocess.CalledProcessError) as error:
        print(f"LLVM coverage failed: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError) and error.stderr:
            print(error.stderr, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
