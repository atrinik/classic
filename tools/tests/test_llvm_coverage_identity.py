"""Regression evidence for complete LLVM 23 binary/profile associations."""
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later

import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

HELPER = Path(os.environ.get("ATRINIK_LLVM_COVERAGE_HELPER", Path(__file__).resolve().parents[1] / "ci" / "llvm_coverage.py"))
SPEC = importlib.util.spec_from_file_location("llvm_coverage_identity_helper", HELPER)
cov = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(cov)


def text_record(name="f", function_hash=91, values=(1, 0)):
    return (f"{name}\n# Func Hash:\n{function_hash}\n# Num Counters:\n{len(values)}\n"
            "# Counter Values:\n" + "\n".join(map(str, values)))


def name_hash(name):
    return int.from_bytes(hashlib.md5(name.encode()).digest()[:8], "little")


def mapping(filename):
    return {"filename": filename, "branches": [[1, 1, 1, 8, 0, 0, 0, 0, 4]],
            "expansions": [], "mcdc_records": [],
            "segments": [[1, 1, 0, True, True, False], [1, 8, 0, False, False, False]],
            "summary": {key: {"count": 2 if key == "branches" else 0 if key == "mcdc" else 1, "covered": 0, "percent": 0} for key in
                        ("branches", "functions", "instantiations", "lines", "regions", "mcdc")}}


def function(filename, name="f"):
    return {"branches": [[1, 1, 1, 8, 0, 0, 0, 0, 4]], "count": 0, "filenames": [filename],
            "mcdc_records": [], "name": name, "regions": [[1, 1, 1, 8, 0, 0, 0, 0]]}


def document(files, functions):
    return json.dumps({"type": "llvm.coverage.json.export", "version": "3.1.0",
                       "data": [{"files": files, "functions": functions, "totals": mapping("unused")["summary"]}]})


def lcov(filename, count=0):
    return (f"SF:{filename}\nFN:1,f\nFNDA:{count},f\nFNF:1\nFNH:{int(count > 0)}\n"
            f"DA:1,{count}\nLF:1\nLH:{int(count > 0)}\nBRDA:1,0,0,{count}\n"
            f"BRDA:1,0,1,0\nBRF:2\nBRH:{int(count > 0)}\nend_of_record\n")


class IdentityTests(unittest.TestCase):
    def test_every_native_coverage_entrypoint_separates_internal_names(self):
        root = Path(__file__).resolve().parents[2]
        for name in ("CMakeLists.txt", "client/CMakeLists.txt", "server/CMakeLists.txt",
                     "libatrinik/CMakeLists.txt", "libatrinik/pathfinding/CMakeLists.txt"):
            with self.subTest(entrypoint=name):
                cmake = (root / name).read_text()
                self.assertEqual(cmake.count("-funique-internal-linkage-names"), 1)
                self.assertRegex(cmake, r"-fprofile-update=atomic\s+-funique-internal-linkage-names")

    def test_complete_profile_includes_zero_functions_and_zero_counters(self):
        report = text_record() + "\n\n" + text_record("unused", 123, (0, 0, 0)) + "\n"
        self.assertEqual(cov.profile_identity(report), tuple(sorted([
            (name_hash("f"), 91, 2), (name_hash("unused"), 123, 3)])))

    def test_malformed_text_profiles_fail_closed(self):
        good = text_record()
        bad = ["", good.rsplit("\n", 1)[0], good + "\n2", good.replace("# Num Counters:\n2", "# Num Counters:\n0"),
               good.replace("# Func Hash:", "# Function hash:"),
               good.replace("91", "0x5b"), good.replace("\n1\n0", "\n-1\n0")]
        for report in bad:
            with self.subTest(report=report), self.assertRaises(ValueError):
                cov.profile_identity(report)

    def test_profile_record_multiplicity_is_part_of_identity(self):
        record = text_record()
        identity = (name_hash("f"), 91, 2)
        self.assertEqual(cov.profile_identity(record + "\n\n" + record), (identity, identity))
        with patch.object(cov, "binary_identity", return_value=(identity, identity)), patch.object(cov, "run", return_value=record), self.assertRaises(ValueError):
            cov.executed_objects([Path("a")], [Path("raw")], "profdata", "readobj")

    def test_complete_unique_association_and_repeated_process_profiles(self):
        a, b = Path("a"), Path("b")
        ia, ib = cov.profile_identity(text_record()), cov.profile_identity(text_record("main", 3, (0,)))
        with patch.object(cov, "binary_identity", side_effect=lambda p, _: {a: ia, b: ib}[p]), patch.object(cov, "run", return_value=text_record()):
            self.assertEqual(cov.executed_objects([a, b], [Path("p1"), Path("p2")], "profdata", "readobj"), [a])

    def test_foreign_partial_and_ambiguous_associations_rejected(self):
        a, b = Path("a"), Path("b")
        full = cov.profile_identity(text_record() + "\n\n" + text_record("unused", 2, (0,)))
        for report in (text_record(), text_record("foreign", 8, (1,))):
            with self.subTest(report=report), patch.object(cov, "binary_identity", return_value=full), patch.object(cov, "run", return_value=report), self.assertRaises(ValueError):
                cov.executed_objects([a], [Path("raw")], "profdata", "readobj")
        with patch.object(cov, "binary_identity", return_value=full), self.assertRaises(ValueError):
            cov.executed_objects([a, b], [], "profdata", "readobj")

    def test_duplicate_byte_artifacts_are_deduplicated(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name, data in (("a", b"\x7fELF__llvm_covmap one"), ("b", b"\x7fELF__llvm_covmap one"), ("c", b"\x7fELF__llvm_covmap two")):
                (root / name).write_bytes(data)
            self.assertEqual(cov.coverage_objects(root), [root / "a", root / "c"])


class BinaryIdentityTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "fixture"
        self.bytes = bytearray(512)
        self.bytes[:7] = b"\x7fELF\x02\x01\x01"
        struct.pack_into("<H", self.bytes, 18, 62)
        struct.pack_into("<Q", self.bytes, 64, 11)
        for index, (name, h, counters) in enumerate((("f", 91, 2), ("unused", 123, 1))):
            offset = 128 + 72 * index
            struct.pack_into("<QQq", self.bytes, offset, name_hash(name), h, 0x2000 + 16 * index - (0x1000 + 72 * index))
            struct.pack_into("<I", self.bytes, offset + 56, counters)
        sections = [{"Name": {"Name": name}, "Offset": offset, "Size": size, "Address": address}
                    for name, offset, size, address in ((".data", 64, 8, 0x3000), ("__llvm_prf_data", 128, 144, 0x1000), ("__llvm_prf_cnts", 320, 24, 0x2000))]
        self.metadata = [{"FileSummary": {"Format": "elf64-x86-64"}, "Sections": [{"Section": s} for s in sections],
                          "Symbols": [{"Symbol": {"Name": {"Name": "__llvm_profile_raw_version"}, "Size": 8, "Value": 0x3000, "Section": {"Name": ".data"}}}]}]

    def identity(self):
        self.path.write_bytes(self.bytes)
        with patch.object(cov, "run", return_value=json.dumps(self.metadata)):
            return cov.binary_identity(self.path, "readobj")

    def test_real_record_bytes_with_relative_counters(self):
        self.assertEqual(self.identity(), tuple(sorted([(name_hash("f"), 91, 2), (name_hash("unused"), 123, 1)])))

    def test_duplicate_identity_preserves_distinct_ranges_and_section_padding(self):
        struct.pack_into("<QQ", self.bytes, 200, name_hash("f"), 91)
        struct.pack_into("<I", self.bytes, 256, 2)
        self.metadata[0]["Sections"][2]["Section"]["Size"] = 40
        identity = (name_hash("f"), 91, 2)
        self.assertEqual(self.identity(), (identity, identity))

    def test_bad_format_version_record_and_counter_bounds(self):
        mutations = [lambda: self.bytes.__setitem__(4, 1),
                     lambda: struct.pack_into("<H", self.bytes, 18, 183),
                     lambda: struct.pack_into("<Q", self.bytes, 64, 10),
                     lambda: self.metadata[0]["FileSummary"].update(Format="coff-x86-64"),
                     lambda: self.metadata[0]["Sections"][1]["Section"].update(Size=143),
                     lambda: self.metadata[0]["Sections"][1]["Section"].update(Offset=129),
                     lambda: self.metadata[0]["Sections"][2]["Section"].update(Offset=500),
                     lambda: struct.pack_into("<I", self.bytes, 184, 0),
                     lambda: self.bytes.__setitem__(188, 1),
                     lambda: struct.pack_into("<q", self.bytes, 144, 0x1001),
                     lambda: struct.pack_into("<I", self.bytes, 184, 4),
                     lambda: struct.pack_into("<q", self.bytes, 216, 0x1000 - 72),
                     lambda: struct.pack_into("<QQ", self.bytes, 200, name_hash("f"), 91)]
        original_bytes, original_metadata = bytes(self.bytes), copy.deepcopy(self.metadata)
        for index, mutate in enumerate(mutations):
            self.bytes = bytearray(original_bytes)
            self.metadata = copy.deepcopy(original_metadata)
            mutate()
            # A duplicate identity needs the same counter cardinality too.
            if index == len(mutations) - 1:
                struct.pack_into("<I", self.bytes, 256, 2)
            with self.subTest(index=index), self.assertRaises(ValueError):
                self.identity()


class CompleteReportTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "client" / "src").mkdir(parents=True)
        self.shared = str(self.root / "client/src/shared.c")
        self.unique = str(self.root / "client/src/unrun.c")
        self.associated = document([mapping(self.shared)], [function(self.shared)])
        self.baseline = document([mapping(self.shared), mapping(self.unique)], [function(self.shared), function(self.unique, "unrun")])
        self.measured = lcov(self.shared, 7)

    def complete(self, baseline=None, associated=None, measured=None, zero=None):
        responses = [measured if measured is not None else self.measured,
                     zero if zero is not None else lcov(self.shared) + lcov(self.unique),
                     baseline if baseline is not None else self.baseline,
                     associated if associated is not None else self.associated]
        with patch.object(cov, "run", side_effect=responses):
            return cov.complete_report("cov", [Path("test"), Path("unrun")], [Path("test")], Path("merged"), self.root, "client")

    def test_measured_records_preserved_and_unique_files_zero(self):
        result = self.complete()
        self.assertIn(self.measured, result)
        self.assertIn(lcov(self.unique), result)
        self.assertEqual(result.count("SF:"), 2)

    def test_json_selectors_follow_complete_unfiltered_production_inventory(self):
        with patch.object(cov, "run", side_effect=[self.measured, lcov(self.shared) + lcov(self.unique),
                                                   self.baseline, self.associated]) as run:
            cov.complete_report("cov", [Path("test"), Path("unrun")], [Path("test")], Path("merged"), self.root, "client")
        commands = [call.args[0] for call in run.call_args_list]
        self.assertNotIn(self.shared, commands[0])
        self.assertNotIn(self.shared, commands[1])
        self.assertEqual(commands[2][-2:], sorted([self.shared, self.unique]))
        self.assertEqual(commands[3][-2:], sorted([self.shared, self.unique]))

    def test_normalized_filename_tables_match(self):
        relative = document([mapping("client/src/shared.c")], [function("client/src/shared.c")])
        self.assertIn(self.measured, self.complete(associated=relative))

    def test_outer_expansion_order_is_independent_of_object_discovery(self):
        baseline = json.loads(self.baseline)
        associated = json.loads(self.associated)
        expansions = [{"filenames": [self.shared, "client/src/header.h"],
                       "branches": [[line, 1, line, 8, 0, 0, 1, 0, 4],
                                    [line, 2, line, 7, 0, 0, 1, 0, 4]],
                       "source_region": [line, 1, line, 8, 0, 0, 1, 1],
                       "target_regions": [[line, 1, line, 8, 0, 1, 0, 0],
                                          [line, 2, line, 7, 0, 1, 0, 0]]}
                      for line in (1, 2)]
        baseline["data"][0]["files"][0]["expansions"] = expansions
        associated["data"][0]["files"][0]["expansions"] = list(reversed(expansions))
        self.assertIn(self.measured, self.complete(baseline=json.dumps(baseline), associated=json.dumps(associated)))
        for change in ("coordinate", "duplicate", "remove", "filenames", "branches", "regions"):
            altered = copy.deepcopy(baseline)
            entries = altered["data"][0]["files"][0]["expansions"]
            if change == "coordinate":
                entries[0]["source_region"][1] += 1
            elif change == "duplicate":
                entries.append(copy.deepcopy(entries[0]))
            elif change == "remove":
                entries.pop()
            else:
                key = {"filenames": "filenames", "branches": "branches", "regions": "target_regions"}[change]
                entries[0][key].reverse()
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.complete(baseline=json.dumps(altered), associated=json.dumps(associated))

    def test_changed_shared_segments_branches_expansions_and_functions_rejected(self):
        for field in ("segments", "branches", "expansions", "functions"):
            baseline = json.loads(self.baseline)
            data = baseline["data"][0]
            if field == "functions":
                data[field].append(function(self.shared, "additional_static_inline"))
            elif field == "expansions":
                data["files"][0][field].append({"filenames": [self.shared], "source_region": [1, 1, 1, 8, 0, 0, 0, 1], "target_regions": []})
            else:
                data["files"][0][field][0][1] += 1
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.complete(baseline=json.dumps(baseline))

    def test_malformed_measured_lcov_rejected(self):
        for measured in (f"SF:{self.shared}\nend_of_record\n",
                         self.measured.removesuffix("end_of_record\n"),
                         self.measured.replace("DA:1,7", "DA:1,-1"),
                         self.measured.replace("LH:1", "LH:2"),
                         self.measured.replace("BRH:1", "BRH:3"),
                         self.measured.replace("FNH:1", "FNH:2"),
                         self.measured.replace("FNF:1", "FNF:1\nFNF:1"),
                         self.measured.replace("FN:1,f", f"SF:{self.unique}\nFN:1,f")):
            with self.subTest(measured=measured), self.assertRaises(ValueError):
                self.complete(measured=measured)

    def test_zero_baseline_lcov_must_have_only_zero_counts(self):
        with self.assertRaises(ValueError):
            self.complete(zero=lcov(self.shared) + lcov(self.unique, 77))

    def test_function_summaries_preserve_llvm_instantiation_grouping(self):
        measured = self.measured.replace("FN:1,f", "FN:1,f\nFN:1,g").replace("FNDA:7,f", "FNDA:7,f\nFNDA:0,g")
        self.assertIn(measured, self.complete(measured=measured))

    def test_unknown_schema_mcdc_and_duplicate_sources_rejected(self):
        for change in ("version", "file-key", "mcdc", "duplicate-json", "nonzero-function", "missing-summary"):
            baseline = json.loads(self.baseline)
            if change == "version":
                baseline["version"] = "4.0.0"
            elif change == "file-key":
                baseline["data"][0]["files"][0]["unknown"] = []
            elif change == "mcdc":
                baseline["data"][0]["files"][0]["mcdc_records"] = [[1]]
            elif change == "duplicate-json":
                baseline["data"][0]["files"].append(copy.deepcopy(baseline["data"][0]["files"][0]))
            elif change == "missing-summary":
                baseline["data"][0]["files"][0]["summary"].pop("mcdc")
            else:
                baseline["data"][0]["functions"][0]["count"] = 1
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.complete(baseline=json.dumps(baseline))
        with self.assertRaises(ValueError):
            self.complete(measured=self.measured + self.measured)
        with self.assertRaises(ValueError):
            self.complete(zero=lcov(self.shared))

    def test_nonzero_or_truncated_empty_profile_mapping_rejected(self):
        for field in ("segments", "branches", "summary", "regions"):
            baseline = json.loads(self.baseline)
            data = baseline["data"][0]
            if field == "regions":
                data["functions"][0][field][0][4] = 1
            elif field == "summary":
                data["files"][0][field]["lines"]["covered"] = 1
            else:
                index = 2 if field == "segments" else 4
                data["files"][0][field][0][index] = 1
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.complete(baseline=json.dumps(baseline))
        baseline = json.loads(self.baseline)
        baseline["data"][0]["files"][0]["segments"][0].pop()
        with self.assertRaises(ValueError):
            self.complete(baseline=json.dumps(baseline))


@unittest.skipUnless(os.environ.get("ATRINIK_LLVM_COVERAGE_NATIVE_TEST") == "1", "explicit LLVM 23 SDK fixture opt-in required")
class NativeLLVM23Tests(unittest.TestCase):
    """Compile only tiny disposable fixtures in the caller's approved SDK."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.src = self.root / "client/src"
        self.src.mkdir(parents=True)
        self.tests = self.root / "client/tests"
        self.tests.mkdir()
        self.build = self.root / "build"
        self.build.mkdir()
        self.clang = os.environ.get("CC", "clang")
        self.profdata = os.environ.get("LLVM_PROFDATA", "llvm-profdata")
        self.llvmcov = os.environ.get("LLVM_COV", "llvm-cov")
        self.readobj = os.environ.get("LLVM_READOBJ", "llvm-readobj")
        for tool in (self.clang, self.profdata, self.llvmcov, self.readobj):
            cov.verify_tool(tool)
        self.common = self.src / "common.c"
        self.common.write_text("#define INNER(x) ((x) ? 3 : 5)\n#define OUTER(x) INNER((x) > 1)\nstatic inline int inc(int x) { return x + 1; }\nint common(int x) { if (x > 0) return OUTER(inc(x)); return x ? 8 : 9; }\n")
        self.test_main = self.tests / "test.c"
        self.test_main.write_text("int common(int); int main(void) { return common(2) == 3 ? 0 : 1; }\n")
        self.bench_main = self.tests / "benchmark.c"
        self.bench_main.write_text("int common(int); int main(void) { int n = 0; for (int i = 0; i < 4; ++i) n += common(i); return n == 0; }\n")
        self.unrun_main = self.src / "application.c"
        self.unrun_main.write_text("int main(void) { volatile int x = 0; return x ? 1 : 0; }\n")
        self.plugin_src = self.src / "plugin.c"
        self.plugin_src.write_text("int plugin(int x) { return x > 0 ? 1 : 0; }\n")
        self.zero_src = self.src / "zero.c"
        self.zero_src.write_text("int never_called(int x) { return x ? 1 : 0; }\n")
        self.zero_main = self.tests / "zero.c"
        self.zero_main.write_text("int main(void) { return 0; }\n")
        self.test = self.compile("test", [self.test_main, self.common])
        self.zero_test = self.compile("zero-test", [self.zero_main, self.zero_src])
        self.compile("benchmark", [self.bench_main, self.common])
        self.compile("application", [self.unrun_main])
        self.compile("plugin.so", [self.plugin_src], ["-shared", "-fPIC"])
        pattern = cov.prepare(self.build)
        subprocess.run([str(self.test)], check=True, env={**os.environ, "LLVM_PROFILE_FILE": pattern})
        subprocess.run([str(self.zero_test)], check=True, env={**os.environ, "LLVM_PROFILE_FILE": pattern})
        self.profiles = sorted((self.build / cov.PROFILE_DIRECTORY).glob("*.profraw"))

    def compile(self, name, sources, extra=()):
        output = self.build / name
        command = [self.clang, "-std=c17", "-Werror", "-O0", "-fprofile-instr-generate", "-fcoverage-mapping", "-fprofile-update=atomic", "-funique-internal-linkage-names",
                   "--rtlib=compiler-rt", "--unwindlib=libunwind", "-fuse-ld=lld", *extra, *map(str, sources), "-o", str(output)]
        subprocess.run(command, check=True, capture_output=True, text=True)
        return output

    def test_native_complete_graph_preserves_measured_macro_and_branch_counts(self):
        objects = cov.coverage_objects(self.build)
        executed = cov.executed_objects(objects, self.profiles, self.profdata, self.readobj)
        self.assertEqual(executed, [self.test, self.zero_test])
        merged = self.build / "merged.profdata"
        cov.run([self.profdata, "merge", "-o", str(merged), *map(str, self.profiles)])
        measured = cov.lcov_records(cov.run(cov.object_arguments(self.llvmcov, executed, "-format=lcov", f"-instr-profile={merged}")), self.root, "client")
        result = cov.complete_report(self.llvmcov, objects, executed, merged, self.root, "client")
        records = cov.lcov_records(result, self.root, "client")
        self.assertEqual(records[str(self.common)], measured[str(self.common)])
        self.assertIn("BRDA:", measured[str(self.common)])
        self.assertEqual(records[str(self.zero_src)], measured[str(self.zero_src)])
        self.assertIn("FNH:0", records[str(self.zero_src)])
        for source in (self.unrun_main, self.plugin_src):
            self.assertIn(str(source), records)
            self.assertIn("FNH:0", records[str(source)])
            self.assertIn("LH:0", records[str(source)])

    def test_native_source_selectors_preserve_complete_production_zero_maps(self):
        objects = cov.coverage_objects(self.build)
        unfiltered = cov.run(cov.object_arguments(self.llvmcov, objects, "-empty-profile"))
        sources = sorted(cov.lcov_records(cov.run(cov.object_arguments(self.llvmcov, objects,
                             "-empty-profile", "-format=lcov")), self.root, "client", zero=True))
        filtered = cov.run(cov.object_arguments(self.llvmcov, objects, "-empty-profile") + sources)
        self.assertEqual(cov.zero_maps(unfiltered, self.root, "client"),
                         cov.zero_maps(filtered, self.root, "client"))
        # Source selection reduces file rendering, never the global functions
        # used to compare shared inline, branch and expansion mappings.
        self.assertEqual(json.loads(unfiltered)["data"][0]["functions"],
                         json.loads(filtered)["data"][0]["functions"])

    def test_native_unexecuted_additional_same_source_mapping_rejected(self):
        self.common.write_text(self.common.read_text() +
                               "#ifdef EXTRA\nint extra(int x) { return x ? 7 : 8; }\n#endif\n")
        self.compile("benchmark", [self.bench_main, self.common], ["-DEXTRA"])
        objects = cov.coverage_objects(self.build)
        executed = cov.executed_objects(objects, self.profiles, self.profdata, self.readobj)
        merged = self.build / "merged.profdata"
        cov.run([self.profdata, "merge", "-o", str(merged), *map(str, self.profiles)])
        with self.assertRaises(ValueError):
            cov.complete_report(self.llvmcov, objects, executed, merged, self.root, "client")

    def test_native_rebuilt_incompatible_binary_rejects_old_profile(self):
        self.common.write_text("int common(int x) { if (x > 0) { if (x > 1) return 3; return 4; } return 9; }\n")
        self.compile("test", [self.test_main, self.common])
        with self.assertRaises(ValueError):
            cov.executed_objects(cov.coverage_objects(self.build), self.profiles, self.profdata, self.readobj)

    def test_native_renamed_text_indexed_and_truncated_raw_profiles_rejected(self):
        original = self.profiles[0].read_bytes()
        text = cov.run([self.profdata, "show", "--all-functions", "--counts", "--text", str(self.profiles[0])]).encode()
        indexed = self.root / "indexed.profdata"
        cov.run([self.profdata, "merge", "-o", str(indexed), str(self.profiles[0])])
        for data in (text, indexed.read_bytes(), original[:16]):
            self.profiles[0].write_bytes(data)
            output = self.root / "coverage.lcov"
            output.write_text("stale")
            with self.subTest(format=data[:16]), self.assertRaises((ValueError, subprocess.CalledProcessError)):
                cov.export(self.build, self.root, "client", str(output), compiler=self.clang)
            self.assertFalse(output.exists())
        self.profiles[0].write_bytes(original)

    def test_native_unique_internal_names_preserve_unused_inline_mapping(self):
        header = self.tests / "shared.h"
        header.write_text("static inline int shared(int x) { return x ? 1 : 0; }\n")
        sources = []
        for unit, body in (("a", "return 0;"), ("b", "return shared(0);")):
            folder = self.tests / unit
            folder.mkdir()
            source = folder / "same.c"
            source.write_text('#include "../shared.h"\nint main(void) { ' + body + ' }\n')
            sources.append(source)
        for unique in (False, True):
            directory = self.root / ("unique" if unique else "original")
            directory.mkdir()
            binaries = [self.compile(f"{directory.name}-{index}", [source],
                        [] if unique else ["-fno-unique-internal-linkage-names"])
                        for index, source in enumerate(sources)]
            for binary in binaries:
                subprocess.run([str(binary)], check=True, env={**os.environ,
                               "LLVM_PROFILE_FILE": str(directory / "%m-%p.profraw")})
            merged = directory / "coverage.profdata"
            cov.run([self.profdata, "merge", "-o", str(merged), *map(str, directory.glob("*.profraw"))])
            command = cov.object_arguments(self.llvmcov, binaries, "-format=lcov", f"-instr-profile={merged}")
            if not unique:
                with self.assertRaisesRegex(ValueError, "mismatched data"):
                    cov.run(command)
                continue
            report = cov.run(command)
            self.assertRegex(report, r"FNDA:0,.*shared.*__uniq")
            self.assertRegex(report, r"FNDA:1,.*shared.*__uniq")
            profiles = cov.run([self.profdata, "show", "--all-functions", "--counts", "--text", str(merged)])
            self.assertIn("# Counter Values:\n1\n0", profiles)
            baseline = json.loads(cov.run(cov.object_arguments(self.llvmcov, binaries, "-empty-profile")))
            shared = [f for f in baseline["data"][0]["functions"] if "shared" in f["name"]]
            self.assertEqual(len(shared), 2)
            self.assertEqual(len({f["name"] for f in shared}), 2)


if __name__ == "__main__":
    unittest.main()
