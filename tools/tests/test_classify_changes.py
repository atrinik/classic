from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest


MODULE_PATH = Path(__file__).resolve().parents[1] / "ci" / "classify_changes.py"
SPEC = importlib.util.spec_from_file_location("classify_changes", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
classify_changes = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(classify_changes)


class ClassifyChangesTests(unittest.TestCase):
    def test_full_run_selects_every_native_component_and_language(self) -> None:
        result = classify_changes.classify([], full=True)
        self.assertTrue(result["client"])
        self.assertTrue(result["server"])
        self.assertTrue(result["windows"])
        self.assertEqual(result["codeql_languages"], "c-cpp,python,actions")
        self.assertEqual(result["codeql_paths"], ["."])

    def test_docs_only_pull_request_skips_expensive_jobs(self) -> None:
        result = classify_changes.classify(["docs/RELEASING.md"])
        self.assertFalse(result["client"])
        self.assertFalse(result["server"])
        self.assertFalse(result["windows"])
        self.assertFalse(result["codeql_run"])

    def test_component_change_selects_only_its_native_closure(self) -> None:
        result = classify_changes.classify(["client/src/client/main.c"])
        self.assertTrue(result["client"])
        self.assertFalse(result["server"])
        self.assertTrue(result["windows"])
        self.assertEqual(result["codeql_languages"], "c-cpp")
        self.assertEqual(result["codeql_paths"], ["client"])

    def test_shared_native_change_selects_both_components(self) -> None:
        result = classify_changes.classify(["protocol/schema/commands.jsonl"])
        self.assertTrue(result["client"])
        self.assertTrue(result["server"])
        self.assertTrue(result["windows"])
        self.assertEqual(result["codeql_languages"], "c-cpp")
        self.assertFalse(result["codeql_client"])
        self.assertFalse(result["codeql_server"])
        self.assertTrue(result["codeql_core_cpp"])

    def test_release_configuration_selects_both_native_components(self) -> None:
        result = classify_changes.classify([".releaserc.cjs"])
        self.assertTrue(result["client"])
        self.assertTrue(result["server"])
        self.assertFalse(result["windows"])

    def test_libatrinik_and_ci_contract_changes_select_windows_tests(self) -> None:
        for path in (
            "client/src/client/main.c",
            "libatrinik/path.c",
            "protocol/generated/c/commands.c",
            "tools/ci/classify_changes.py",
            ".github/workflows/check.yml",
        ):
            with self.subTest(path=path):
                self.assertTrue(classify_changes.classify([path])["windows"])

    def test_python_and_workflow_changes_select_matching_languages(self) -> None:
        result = classify_changes.classify(
            ["editor/tool.py", ".github/workflows/release.yml"]
        )
        self.assertTrue(result["client"])
        self.assertTrue(result["server"])
        self.assertEqual(result["codeql_languages"], "python,actions")
        self.assertEqual(
            result["codeql_paths"],
            ["."],
        )

    def test_deleted_python_path_uses_a_stable_existing_source_root(self) -> None:
        result = classify_changes.classify(["tools/removed/module.py"])
        self.assertEqual(result["codeql_languages"], "python")
        self.assertEqual(result["codeql_paths"], ["."])

    def test_unknown_python_root_falls_back_to_the_repository(self) -> None:
        result = classify_changes.classify(["removed-package/module.py"])
        self.assertEqual(result["codeql_paths"], ["."])

    def test_rename_out_reports_both_old_and_new_paths(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            subprocess.run(["git", "init", "-q", "-b", "main", root], check=True)
            subprocess.run(
                ["git", "-C", root, "config", "user.email", "test@example.invalid"],
                check=True,
            )
            subprocess.run(
                ["git", "-C", root, "config", "user.name", "CI Test"],
                check=True,
            )
            old = root / "client" / "removed.py"
            old.parent.mkdir()
            old.write_text("print('old')\n", encoding="utf-8")
            subprocess.run(["git", "-C", root, "add", "."], check=True)
            subprocess.run(["git", "-C", root, "commit", "-qm", "old"], check=True)
            base = subprocess.run(
                ["git", "-C", root, "rev-parse", "HEAD"],
                check=True,
                stdout=subprocess.PIPE,
                text=True,
            ).stdout.strip()
            new = root / "docs" / "removed.py"
            new.parent.mkdir()
            old.rename(new)
            subprocess.run(["git", "-C", root, "add", "-A"], check=True)
            subprocess.run(["git", "-C", root, "commit", "-qm", "moved"], check=True)
            head = subprocess.run(
                ["git", "-C", root, "rev-parse", "HEAD"],
                check=True,
                stdout=subprocess.PIPE,
                text=True,
            ).stdout.strip()
            paths = classify_changes.changed_paths(base, head, root)
            self.assertEqual(paths, ["client/removed.py", "docs/removed.py"])
            result = classify_changes.classify(paths)
            self.assertTrue(result["client"])
            self.assertTrue(result["codeql_client"])

    def test_config_uses_quoted_safe_paths(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "config.yml"
            classify_changes.write_codeql_config(path, ["client", "tools/a b.py"])
            self.assertEqual(
                path.read_text(encoding="utf-8"),
                'name: "Atrinik Classic path-aware analysis"\n'
                'paths:\n  - "client"\n  - "tools/a b.py"\n',
            )

    def test_outputs_create_a_single_language_codeql_matrix(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "output"
            config = Path(temporary) / "config.yml"
            result = classify_changes.classify([], full=True)
            classify_changes.write_outputs(output, result, config)
            matrix_line = next(
                line
                for line in output.read_text(encoding="utf-8").splitlines()
                if line.startswith("codeql_matrix=")
            )
            matrix = json.loads(matrix_line.removeprefix("codeql_matrix="))
            self.assertEqual(
                {entry["category"] for entry in matrix["include"]},
                {
                    "/partition:client/language:c-cpp",
                    "/partition:server/language:c-cpp",
                    "/partition:core/language:c-cpp",
                    "/partition:core/language:python",
                    "/partition:core/language:actions",
                },
            )

    def test_pull_request_matrix_has_only_changed_partitions(self) -> None:
        cases = {
            "client/main.c": {"/partition:client/language:c-cpp"},
            "server/main.c": {"/partition:server/language:c-cpp"},
            "libatrinik/socket.c": {"/partition:core/language:c-cpp"},
        }
        for changed, expected in cases.items():
            with self.subTest(changed=changed), tempfile.TemporaryDirectory() as temporary:
                output = Path(temporary) / "output"
                result = classify_changes.classify([changed])
                classify_changes.write_outputs(
                    output, result, Path(temporary) / "config.yml"
                )
                values = dict(
                    line.split("=", 1)
                    for line in output.read_text(encoding="utf-8").splitlines()
                )
                self.assertEqual(
                    values["windows"],
                    "true"
                    if changed.startswith(("client/", "libatrinik/"))
                    else "false",
                )
                matrix = json.loads(values["codeql_matrix"])
                self.assertEqual(
                    {entry["category"] for entry in matrix["include"]}, expected
                )

    def test_each_codeql_scope_has_invariant_nonoverlapping_paths(self) -> None:
        changed = [
            "client/main.c",
            "server/main.c",
            "libatrinik/socket.c",
            "tools/check.py",
            ".github/workflows/check.yml",
        ]
        expected = {
            "client": ["client"],
            "server": ["server"],
            "core-cpp": ["libatrinik", "protocol"],
            "python": ["."],
            "actions": [".github/workflows"],
        }
        for scope, paths in expected.items():
            with self.subTest(scope=scope):
                result = classify_changes.classify(changed, codeql_scope=scope)
                self.assertEqual(result["codeql_paths"], paths)

    def test_release_automation_change_selects_both_native_consumers(self) -> None:
        result = classify_changes.classify(["tools/release/package_sources.py"])
        self.assertTrue(result["client"])
        self.assertTrue(result["server"])

    def test_root_legal_files_select_both_packaging_consumers(self) -> None:
        for path in ("LICENSE.md", "ATTRIBUTIONS.md"):
            with self.subTest(path=path):
                result = classify_changes.classify([path])
                self.assertTrue(result["client"])
                self.assertTrue(result["server"])

    def test_outputs_create_a_successful_noop_matrix(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "output"
            config = Path(temporary) / "config.yml"
            result = classify_changes.classify(["README.md"])
            classify_changes.write_outputs(output, result, config)
            self.assertIn(
                '"category":"/noop","display":"No supported changes",'
                '"language":"none","run":false,"scope":"union"',
                output.read_text(encoding="utf-8"),
            )

    def test_unsafe_path_is_rejected(self) -> None:
        with self.assertRaises(classify_changes.ClassificationError):
            classify_changes.classify(["../escape.c"])


if __name__ == "__main__":
    unittest.main()
