from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).resolve().parents[1] / "ci" / "require_checks.py"
SPEC = importlib.util.spec_from_file_location("require_checks", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
require_checks = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(require_checks)


class RequireChecksTests(unittest.TestCase):
    def test_selected_component_must_succeed(self) -> None:
        require_checks.require_component("client", "true", "success")
        with self.assertRaisesRegex(require_checks.CheckResultError, "did not succeed"):
            require_checks.require_component("client", "true", "skipped")

    def test_unselected_component_may_be_skipped(self) -> None:
        require_checks.require_component("server", "false", "skipped")
        with self.assertRaisesRegex(
            require_checks.CheckResultError, "unexpected result"
        ):
            require_checks.require_component("server", "false", "success")

    def test_selected_native_windows_check_must_succeed(self) -> None:
        require_checks.require_component("native Windows", "true", "success")
        with self.assertRaisesRegex(require_checks.CheckResultError, "did not succeed"):
            require_checks.require_component("native Windows", "true", "skipped")

    def test_selected_integrated_check_must_succeed(self) -> None:
        require_checks.require_component("integrated", "true", "success")
        with self.assertRaisesRegex(require_checks.CheckResultError, "did not succeed"):
            require_checks.require_component("integrated", "true", "failure")

    def test_missing_or_malformed_classifier_output_fails_closed(self) -> None:
        for required in ("", "TRUE", "yes", "0"):
            with self.subTest(required=required):
                with self.assertRaisesRegex(
                    require_checks.CheckResultError, "exactly true or false"
                ):
                    require_checks.require_component("client", required, "skipped")

    def test_classifier_and_core_results_must_be_exact_success(self) -> None:
        for result in ("", "skipped", "failure", "cancelled"):
            with self.subTest(result=result):
                with self.assertRaises(require_checks.CheckResultError):
                    require_checks.require_success("core", result)


if __name__ == "__main__":
    unittest.main()
