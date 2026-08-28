from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
import sys
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).resolve().parents[1] / "verify_movement_fault_injection.py"
SPEC = importlib.util.spec_from_file_location("verify_movement_fault_injection", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
fault_verifier = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(fault_verifier)


def result(returncode: int, stdout: str = "", stderr: str = "") -> subprocess.CompletedProcess[str]:
    return subprocess.CompletedProcess([], returncode, stdout, stderr)


class MovementFaultInjectionTests(unittest.TestCase):
    @mock.patch.object(fault_verifier, "verify_cursor")
    @mock.patch.object(fault_verifier, "verify_movement_fault")
    @mock.patch.object(fault_verifier, "verify_movement")
    @mock.patch.object(fault_verifier, "verify_frozen")
    def test_main_accepts_brynknot_viewport(
        self,
        verify_frozen: mock.Mock,
        verify_movement: mock.Mock,
        verify_movement_fault: mock.Mock,
        verify_cursor: mock.Mock,
    ) -> None:
        with mock.patch.object(fault_verifier, "require_legacy_player_view"), mock.patch.object(
            sys,
            "argv",
            [
                "verify_movement_fault_injection.py",
                "client",
                "frozen.xml",
                "movement.xml",
                "brynknot",
            ],
        ):
            self.assertEqual(fault_verifier.main(), 0)

        verify_frozen.assert_called_once_with(Path("client"), Path("frozen.xml"))
        verify_movement.assert_called_once_with(
            Path("client"), Path("movement.xml"), "brynknot"
        )
        self.assertTrue(verify_movement_fault.call_args_list)
        self.assertTrue(
            all(call.args[-1] == "brynknot" for call in verify_movement_fault.call_args_list)
        )
        verify_cursor.assert_called_once_with(Path("client"), Path("movement.xml"))

    def test_gpu_only_revision_rejects_retired_entrypoint(self) -> None:
        with self.assertRaisesRegex(SystemExit, "unavailable on GPU-only revisions"):
            fault_verifier.require_legacy_player_view()

    @mock.patch.object(fault_verifier.subprocess, "run")
    def test_invoke_scopes_fault_to_child_process(self, run: mock.Mock) -> None:
        run.return_value = result(0)
        fault_verifier.invoke(Path("atrinik"), ["--mode", Path("fixture.xml")])

        arguments = run.call_args.args[0]
        options = run.call_args.kwargs
        self.assertEqual(arguments, ["atrinik", "--mode", "fixture.xml"])
        self.assertEqual(options["env"]["ATRINIK_MOVEMENT_FAULT"], "mutable-rle")
        self.assertTrue(options["capture_output"])
        self.assertFalse(options["check"])
        self.assertEqual(options["timeout"], 20)

        fault_verifier.invoke(
            Path("atrinik"), ["--mode", Path("fixture.xml")], "sprite-cache-clock"
        )
        self.assertEqual(
            run.call_args.kwargs["env"]["ATRINIK_MOVEMENT_FAULT"],
            "sprite-cache-clock",
        )

    @mock.patch.object(fault_verifier, "invoke")
    def test_frozen_benchmark_must_succeed_without_fault_diagnostic(
        self, invoke: mock.Mock
    ) -> None:
        invoke.return_value = result(
            0,
            "player-view-benchmark\tstandard\t101\t12345\n",
        )
        fault_verifier.verify_frozen(Path("atrinik"), Path("frozen.xml"))

        invoke.return_value = result(1, stderr="unexpected failure")
        with self.assertRaisesRegex(SystemExit, "unexpectedly consumed movement fault"):
            fault_verifier.verify_frozen(Path("atrinik"), Path("frozen.xml"))

        invoke.return_value = result(
            0,
            "player-view-benchmark\tstandard\t101\t12345\n",
            fault_verifier.FAULT_DIAGNOSTIC,
        )
        with self.assertRaisesRegex(SystemExit, "activated the movement-only fault"):
            fault_verifier.verify_frozen(Path("atrinik"), Path("frozen.xml"))

    @mock.patch.object(fault_verifier, "invoke")
    def test_movement_requires_specific_injection_detection_failure(
        self, invoke: mock.Mock
    ) -> None:
        invoke.return_value = result(1, stderr=fault_verifier.FAULT_DIAGNOSTIC + "\n")
        fault_verifier.verify_movement(Path("atrinik"), Path("movement.xml"))

        invoke.return_value = result(0, stdout="{}\n")
        with self.assertRaisesRegex(SystemExit, "did not reject"):
            fault_verifier.verify_movement(Path("atrinik"), Path("movement.xml"))

        invoke.return_value = result(1, stderr="unrelated failure\n")
        with self.assertRaisesRegex(SystemExit, "without proving"):
            fault_verifier.verify_movement(Path("atrinik"), Path("movement.xml"))

        invoke.return_value = result(
            1,
            stdout='{"benchmark":"player-view-movement"}\n',
            stderr=fault_verifier.FAULT_DIAGNOSTIC + "\n",
        )
        with self.assertRaisesRegex(SystemExit, "emitted a consumable JSON record"):
            fault_verifier.verify_movement(Path("atrinik"), Path("movement.xml"))

        invoke.return_value = result(
            1,
            stdout="[timestamp] expected renderer diagnostic\n",
            stderr=fault_verifier.FAULT_DIAGNOSTIC + "\n",
        )
        fault_verifier.verify_movement(Path("atrinik"), Path("movement.xml"))

    @mock.patch.object(fault_verifier, "invoke")
    def test_sprite_cache_clock_requires_specific_detection(
        self, invoke: mock.Mock
    ) -> None:
        invoke.return_value = result(
            1, stderr=fault_verifier.CLOCK_FAULT_DIAGNOSTIC + "\n"
        )
        fault_verifier.verify_movement_fault(
            Path("atrinik"),
            Path("movement.xml"),
            fault_verifier.CLOCK_FAULT,
            fault_verifier.CLOCK_FAULT_DIAGNOSTIC,
        )
        self.assertEqual(invoke.call_args.args[2], fault_verifier.CLOCK_FAULT)

        invoke.return_value = result(1, stderr="unrelated failure\n")
        with self.assertRaisesRegex(SystemExit, "sprite-cache-clock"):
            fault_verifier.verify_movement_fault(
                Path("atrinik"),
                Path("movement.xml"),
                fault_verifier.CLOCK_FAULT,
                fault_verifier.CLOCK_FAULT_DIAGNOSTIC,
            )


if __name__ == "__main__":
    unittest.main()
