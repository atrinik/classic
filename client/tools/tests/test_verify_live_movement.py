#!/usr/bin/env python3

from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path


MODULE_PATH = Path(__file__).resolve().parents[1] / "verify_live_movement.py"
SPEC = importlib.util.spec_from_file_location("verify_live_movement", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
verifier = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(verifier)


ROUTE = b"""<live-movement-route version="1" timeout-ms="60000" step-timeout-ms="5000">
  <checkpoint map="/brynknot/city" x="10" y="20" direction="0"/>
  <checkpoint map="/brynknot/wilderness" x="11" y="21" direction="6"/>
</live-movement-route>
"""


def identity() -> dict:
    return {
        "type": "identity",
        "schema_version": 1,
        "route_sha256": hashlib.sha256(ROUTE).hexdigest(),
        "route_checkpoints": 2,
        "source_revision": "a" * 40,
        "source_dirty": False,
        "gpu_backend": "vulkan",
        "gpu_device": "Test GPU",
        "gpu_driver": "1.2.3",
        "viewport_width": 1280,
        "viewport_height": 720,
        "look_width": 17,
        "look_height": 17,
        "fps_limit": 60,
        "smooth_lighting": True,
        "started_utc_us": 1_800_000_000_000_000,
        "gpu_hardware_timing_available": False,
        "cpu_stage_names": ["map", "ui"],
        "gpu_host_stage_names": ["submit"],
    }


def frame(sequence: int, elapsed_us: int, map_path: str, x: int, y: int,
          generation: int, primary_generation: int | None = None) -> dict:
    if primary_generation is None:
        primary_generation = generation
    return {
        "type": "frame",
        "sequence": sequence,
        "elapsed_us": elapsed_us,
        "frame_us": 100,
        "presented": True,
        "map_path": map_path,
        "x": x,
        "y": y,
        "map_publication_generation": generation,
        "gpu_primary_publication_generation": primary_generation,
        "map_ready": True,
        "cpu_totals_us": [sequence * 10, sequence * 20],
        "gpu_host_totals_ns": [sequence * 100],
        "gpu_invalidation_totals": [sequence * value for value in range(1, 11)],
        "map_totals": {
            "primary_map_draws": sequence,
            "compiled_render_commands": sequence * 2,
            "reused_render_commands": sequence * 3,
            "living_commands": sequence * 4,
            "animation_draws": sequence * 5,
            "level_draws": sequence * 6,
            "render_commands": sequence * 7,
        },
        "gpu_totals": {
            "map_submissions": sequence,
            "map_completions": sequence,
            "map_dropped_updates": 0,
            "map_merged_updates": sequence - 1,
            "map_full_redraws": sequence,
            "map_retained_frames": sequence - 1,
            "source_upload_count": sequence,
            "source_upload_bytes": sequence * 100,
            "instance_upload_count": sequence,
            "instance_upload_bytes": sequence * 10,
            "map_queue_age_total_ns": sequence * 1000,
            "map_frame_latency_total_ns": sequence * 2000,
            "resource_creations": sequence,
        },
        "network": {"connected": True, "shutdown_pending": False,
                    "main_service_gap_us": 10,
                    "main_service_gap_max_us": sequence * 10,
                    "queue_depth": 3 - sequence,
                    "queue_oldest_age_us": sequence,
                    "queue_processing_total_us": sequence * 10,
                    "queue_budget_yields_total": sequence - 1,
                    "keepalive_tx_total": sequence,
                    "keepalive_rx_total": sequence,
                    "keepalive_timeout_total": 0,
                    "keepalive_last_rtt_us": 50},
        "assets": {"installed_total": sequence * 10, "pending": sequence - 1,
                   "admitted": sequence, "unprepared": 0},
        "world_time": {"valid": True, "game_seconds": sequence * 60,
                       "light_keyframe_valid": True,
                       "light_keyframe_generation": sequence},
    }


def good_records() -> list[dict]:
    return [
        identity(),
        {"type": "arrival", "index": 0, "map_path": "/brynknot/city", "x": 10,
         "y": 20, "publication_generation": 1, "elapsed_us": 10},
        frame(1, 100, "/brynknot/city", 10, 20, 1),
        {"type": "checkpoint_presented", "index": 0, "map_publication_generation": 1,
         "gpu_published_generation": 1, "elapsed_us": 101},
        {"type": "arrival", "index": 1, "map_path": "/brynknot/wilderness", "x": 11,
         "y": 21, "publication_generation": 2, "elapsed_us": 102},
        frame(2, 200, "/brynknot/wilderness", 11, 21, 2),
        {"type": "checkpoint_presented", "index": 1, "map_publication_generation": 2,
         "gpu_published_generation": 2, "elapsed_us": 201},
        frame(3, 1_000_201, "/brynknot/wilderness", 11, 21, 2),
        {"type": "terminal", "status": "success", "reason": "", "arrivals": 2,
         "presented_checkpoints": 2, "expected_checkpoints": 2, "frames": 3,
         "presented_frames": 3, "elapsed_us": 1_000_201},
    ]


def png(width: int = 2, height: int = 1) -> bytes:
    def chunk(kind: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff))

    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    pixels = b"".join(b"\0" + b"\0\0\0\xff" * width for _ in range(height))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
            chunk(b"IDAT", zlib.compress(pixels)) + chunk(b"IEND", b""))


def capture_event(kind: str, path: Path, contents: bytes, source: dict,
                  elapsed_us: int) -> dict:
    return {
        "type": "capture",
        "kind": kind,
        "path": str(path),
        "sha256": hashlib.sha256(contents).hexdigest(),
        "width": 2,
        "height": 1,
        "size_bytes": len(contents),
        "map_path": source["map_path"],
        "x": source["x"],
        "y": source["y"],
        "map_publication_generation": source["map_publication_generation"],
        "gpu_primary_publication_generation": source["gpu_primary_publication_generation"],
        "game_time_valid": source["world_time"]["valid"],
        "game_seconds": source["world_time"]["game_seconds"],
        "elapsed_us": elapsed_us,
    }


class VerifyLiveMovementTests(unittest.TestCase):
    def verify(self, records: list[dict], route: bytes = ROUTE) -> dict:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            route_path = root / "route.xml"
            report_path = root / "report.jsonl"
            route_path.write_bytes(route)
            report_path.write_text("".join(json.dumps(record, allow_nan=True) + "\n"
                                           for record in records), encoding="utf-8")
            return verifier.verify(route_path, report_path)

    def assert_rejected(self, records: list[dict], message: str) -> None:
        with self.assertRaisesRegex(verifier.ReportError, message):
            self.verify(records)

    def verify_capture(self, mutate=None) -> dict:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            route_path = root / "route.xml"
            report_path = root / "report.jsonl"
            initial_path = root / "initial.png"
            final_path = root / "final.png"
            contents = png()
            initial_path.write_bytes(contents)
            final_path.write_bytes(contents)
            records = good_records()
            records[0]["capture_paths"] = {
                "initial": str(initial_path), "final": str(final_path),
            }
            initial = capture_event("initial", initial_path, contents, records[2], 100)
            final = capture_event("final", final_path, contents, records[5], 201)
            records.insert(3, initial)
            records.insert(8, final)
            if mutate is not None:
                mutate(records, root)
            route_path.write_bytes(ROUTE)
            report_path.write_text("".join(json.dumps(record) + "\n" for record in records),
                                   encoding="utf-8")
            return verifier.verify(route_path, report_path)

    def test_accepts_and_aggregates_complete_report(self) -> None:
        summary = self.verify(good_records())
        self.assertEqual(summary["status"], "success")
        self.assertEqual(summary["identity"], identity())
        self.assertEqual(summary["coverage"], {
            "maps": ["/brynknot/city", "/brynknot/wilderness"],
            "unique_maps": 2,
            "unique_tiles": 2,
            "checkpoints": 2,
        })
        self.assertEqual(summary["frame_us"],
                         {"p50": 100, "p90": 100, "p95": 100, "p99": 100, "max": 100})
        self.assertEqual(summary["presented_frame_spacing_us"]["p50"], 100)
        self.assertEqual(summary["cpu_stages"]["map"]["total_us"], 30)
        self.assertEqual(summary["gpu_host_stages"]["submit"]["total_ns"], 300)
        self.assertEqual(summary["network_maxima"]["queue_depth"], 2)
        self.assertEqual(summary["network_maxima"]["connected"], True)
        self.assertEqual(summary["asset_maxima"], {
            "installed_total": 30, "pending": 2, "admitted": 3, "unprepared": 0,
        })
        self.assertEqual(summary["gpu_invalidation_totals"], list(range(3, 33, 3)))
        self.assertEqual(summary["world_time_first"]["game_seconds"], 60)
        self.assertEqual(summary["world_time_last"]["game_seconds"], 180)
        self.assertEqual(summary["movement_frames"], 1)
        self.assertEqual(summary["movement_presented_frames"], 1)
        self.assertEqual(summary["movement_elapsed_us"], 100)
        self.assertEqual(summary["movement_frame_us"]["p95"], 100)
        self.assertEqual(summary["movement_presented_fps"], 10_000.0)
        self.assertEqual(summary["movement_cpu_stages"]["map"]["total_us"], 10)
        self.assertEqual(summary["movement_gpu_host_stages"]["submit"]["total_ns"], 100)
        self.assertEqual(summary["per_map_frames"], {"/brynknot/wilderness": 1})
        self.assertEqual(summary["per_map_frame_us"]["/brynknot/wilderness"]["max"], 100)
        self.assertNotIn("captures", summary)

    def test_accepts_paired_capture_artifacts(self) -> None:
        summary = self.verify_capture()
        self.assertEqual(set(summary["captures"]), {"initial", "final"})
        self.assertEqual(summary["captures"]["initial"]["width"], 2)
        self.assertNotIn("type", summary["captures"]["initial"])
        self.assertNotIn("kind", summary["captures"]["initial"])

    def test_rejects_missing_duplicate_and_unrequested_captures(self) -> None:
        def missing(records, _root):
            records.pop(8)

        with self.assertRaisesRegex(verifier.ReportError, "missing a capture callback"):
            self.verify_capture(missing)

        def duplicate(records, _root):
            records.insert(4, copy.deepcopy(records[3]))

        with self.assertRaisesRegex(verifier.ReportError, "duplicate initial capture"):
            self.verify_capture(duplicate)

        def unrequested(records, _root):
            del records[0]["capture_paths"]

        with self.assertRaisesRegex(verifier.ReportError, "forbidden without"):
            self.verify_capture(unrequested)

    def test_rejects_capture_path_and_file_identity_failures(self) -> None:
        def outside(records, root):
            records[0]["capture_paths"]["initial"] = str(root.parent / "initial.png")

        with self.assertRaisesRegex(verifier.ReportError, "report parent directory"):
            self.verify_capture(outside)

        def duplicate_path(records, _root):
            records[0]["capture_paths"]["final"] = records[0]["capture_paths"]["initial"]

        with self.assertRaisesRegex(verifier.ReportError, "must be distinct"):
            self.verify_capture(duplicate_path)

        def alias_path(records, root):
            alias = str(root) + "/./initial.png"
            records[0]["capture_paths"]["final"] = alias
            records[8]["path"] = alias

        with self.assertRaisesRegex(verifier.ReportError, "must be canonical"):
            self.verify_capture(alias_path)

        cases = (
            ("sha256", "f" * 64, "SHA-256"),
            ("size_bytes", len(png()) + 1, "size does not match"),
            ("width", 3, "dimensions do not match"),
        )
        for field, value, message in cases:
            with self.subTest(field=field):
                def invalid(records, _root, field=field, value=value):
                    records[3][field] = value

                with self.assertRaisesRegex(verifier.ReportError, message):
                    self.verify_capture(invalid)

        def symlink(records, root):
            target = root / "target.png"
            target.write_bytes(png())
            initial = root / "initial.png"
            initial.unlink()
            initial.symlink_to(target)

        with self.assertRaisesRegex(verifier.ReportError, "must not be a symlink"):
            self.verify_capture(symlink)

    def test_rejects_capture_source_mismatch_and_wrong_order(self) -> None:
        def source_mismatch(records, _root):
            records[3]["x"] += 1

        with self.assertRaisesRegex(verifier.ReportError, "does not match its presented frame"):
            self.verify_capture(source_mismatch)

        def wrong_order(records, _root):
            initial = records.pop(3)
            initial["elapsed_us"] = 102
            records.insert(5, initial)

        with self.assertRaisesRegex(verifier.ReportError, "outside its checkpoint"):
            self.verify_capture(wrong_order)

    def test_cli_writes_same_stable_summary_as_stdout(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            route_path = root / "route.xml"
            report_path = root / "report.jsonl"
            output_path = root / "summary.json"
            route_path.write_bytes(ROUTE)
            report_path.write_text("".join(json.dumps(record) + "\n"
                                           for record in good_records()), encoding="utf-8")
            result = subprocess.run(
                [sys.executable, "-I", str(MODULE_PATH), str(route_path), str(report_path),
                 "--output", str(output_path)],
                check=True, capture_output=True, text=True,
            )
            self.assertEqual(result.stdout, output_path.read_text(encoding="utf-8"))
            self.assertEqual(json.loads(result.stdout)["source_revision"], "a" * 40)

    def test_accepts_uncapped_fps_and_rejects_unknown_limit(self) -> None:
        records = good_records()
        records[0]["fps_limit"] = 0
        summary = self.verify(records)
        self.assertEqual(summary["settings"]["fps_limit"], 0)
        records = good_records()
        records[0]["fps_limit"] = 75
        self.assert_rejected(records, "fps_limit is unsupported")

    def test_rejects_truncated_report(self) -> None:
        self.assert_rejected(good_records()[:-1], "truncated")

    def test_rejects_duplicate_json_members(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            route_path = root / "route.xml"
            report_path = root / "report.jsonl"
            route_path.write_bytes(ROUTE)
            report_path.write_text('{"type":"identity","type":"identity"}\n', encoding="utf-8")
            with self.assertRaisesRegex(verifier.ReportError, "duplicate JSON member"):
                verifier.verify(route_path, report_path)

    def test_rejects_unknown_route_attributes_and_namespace(self) -> None:
        route = ROUTE.replace(b'timeout-ms="60000"', b'timeout-ms="60000" surprise="1"')
        with self.assertRaisesRegex(verifier.ReportError, "root attributes"):
            self.verify(good_records(), route)
        route = ROUTE.replace(b'version="1"', b'version="1" xmlns:x="urn:test"')
        with self.assertRaisesRegex(verifier.ReportError, "namespace"):
            self.verify(good_records(), route)
        route = b'<?xml-stylesheet href="unused"?>\n' + ROUTE
        with self.assertRaisesRegex(verifier.ReportError, "declaration"):
            self.verify(good_records(), route)
        route = ROUTE.replace(b'timeout-ms="60000"',
                              b'timeout-ms="' + b"9" * 5000 + b'"')
        with self.assertRaisesRegex(verifier.ReportError, "timeout-ms must be <="):
            self.verify(good_records(), route)

    def test_rejects_arrival_before_prior_presentation(self) -> None:
        records = good_records()
        records.insert(3, copy.deepcopy(records[4]))
        records[3]["elapsed_us"] = 101
        records[4]["elapsed_us"] = 102
        self.assert_rejected(records, "before prior checkpoint presentation")

    def test_rejects_fake_arrival_coordinates(self) -> None:
        records = good_records()
        records[1]["x"] = 12
        self.assert_rejected(records, "does not match")

    def test_rejects_unchanged_arrival_or_gpu_generation(self) -> None:
        records = good_records()
        records[4]["publication_generation"] = 1
        self.assert_rejected(records, "publication_generation is not fresh")
        records = good_records()
        records[5]["gpu_primary_publication_generation"] = 1
        records[6]["gpu_published_generation"] = 1
        self.assert_rejected(records, "GPU published generation is not fresh")

    def test_rejects_presentation_without_matching_presented_frame(self) -> None:
        records = good_records()
        records[2]["presented"] = False
        self.assert_rejected(records, "lacks a preceding matching presented frame")

    def test_accepts_newer_qualifying_frame_generation(self) -> None:
        records = good_records()
        records[2]["map_publication_generation"] = 3
        records[2]["gpu_primary_publication_generation"] = 3
        records[3]["map_publication_generation"] = 3
        records[3]["gpu_published_generation"] = 3
        records[4]["publication_generation"] = 4
        records[5]["map_publication_generation"] = 5
        records[5]["gpu_primary_publication_generation"] = 5
        records[6]["map_publication_generation"] = 5
        records[6]["gpu_published_generation"] = 5
        summary = self.verify(records)
        self.assertEqual(summary["presented_checkpoints"], 2)

    def test_rejects_presentation_generation_not_from_qualifying_frame(self) -> None:
        records = good_records()
        records[2]["map_publication_generation"] = 3
        records[3]["map_publication_generation"] = 2
        self.assert_rejected(records, "does not match its presented frame")

    def test_rejects_auxiliary_gpu_publication_as_checkpoint_proof(self) -> None:
        records = good_records()
        records[2]["gpu_primary_publication_generation"] = 7
        self.assert_rejected(records, "does not match its presented primary frame")

    def test_accepts_equal_event_timestamps(self) -> None:
        records = good_records()
        records[3]["elapsed_us"] = records[2]["elapsed_us"]
        records[4]["elapsed_us"] = records[2]["elapsed_us"]
        records[6]["elapsed_us"] = records[5]["elapsed_us"]
        records[8]["elapsed_us"] = records[7]["elapsed_us"]
        summary = self.verify(records)
        self.assertEqual(summary["elapsed_us"], 1_000_201)

    def test_rejects_nonfinite_number(self) -> None:
        records = good_records()
        records[2]["frame_us"] = float("nan")
        self.assert_rejected(records, "non-finite")

    def test_rejects_numeric_type_spoofing_and_unbounded_counter(self) -> None:
        records = good_records()
        records[1]["x"] = 10.0
        self.assert_rejected(records, "arrival x")
        records = good_records()
        records[3]["map_publication_generation"] = True
        self.assert_rejected(records, "map_publication_generation")
        records = good_records()
        records[-1]["elapsed_us"] = 1 << 80
        self.assert_rejected(records, "must be <=")
        records = good_records()
        records[0]["started_utc_us"] = 0
        self.assert_rejected(records, "started_utc_us")

    def test_rejects_invalid_terminal_status_type(self) -> None:
        records = good_records()
        records[-1]["status"] = []
        self.assert_rejected(records, "terminal status")

    def test_rejects_route_and_step_timeout_violations(self) -> None:
        records = good_records()
        records[-1]["elapsed_us"] = 60_000_001
        self.assert_rejected(records, "route timeout")
        records = good_records()
        records[2]["elapsed_us"] = 5_000_010
        records[3]["elapsed_us"] = 5_000_011
        self.assert_rejected(records, "step timeout")

    def test_login_time_uses_global_not_initial_step_deadline(self) -> None:
        records = good_records()
        offset = 20_000_000
        for record in records[1:]:
            record["elapsed_us"] += offset
        summary = self.verify(records)
        self.assertEqual(summary["presented_checkpoints"], 2)

    def test_movement_metrics_exclude_login_and_post_route_drain(self) -> None:
        records = good_records()
        records[2]["elapsed_us"] = 100_000
        records[2]["frame_us"] = 100_000
        records[3]["elapsed_us"] = 100_001
        records[4]["elapsed_us"] = 100_002
        records[5]["elapsed_us"] = 100_100
        records[6]["elapsed_us"] = 100_101
        records[7]["elapsed_us"] = 1_100_101
        records[8]["elapsed_us"] = 1_100_101
        summary = self.verify(records)
        self.assertEqual(summary["frame_us"]["max"], 100_000)
        self.assertEqual(summary["movement_frame_us"]["max"], 100)
        self.assertEqual(summary["movement_frames"], 1)

    def test_rejects_zero_duration_movement_window(self) -> None:
        records = good_records()
        records[3]["elapsed_us"] = 200
        records[4]["elapsed_us"] = 200
        records[5]["elapsed_us"] = 200
        records[6]["elapsed_us"] = 200
        records[7]["elapsed_us"] = 1_000_200
        records[8]["elapsed_us"] = 1_000_200
        self.assert_rejected(records, "no positive-duration presented movement evidence")

    def test_rejects_unhealthy_network_after_arrival(self) -> None:
        records = good_records()
        records[2]["network"]["connected"] = False
        self.assert_rejected(records, "disconnected after first arrival")
        records = good_records()
        records[7]["network"]["shutdown_pending"] = True
        self.assert_rejected(records, "shutdown is pending")

    def test_rejects_invalid_world_time_schema(self) -> None:
        records = good_records()
        records[2]["world_time"]["valid"] = 1
        self.assert_rejected(records, "world_time.valid must be boolean")
        records = good_records()
        records[2]["world_time"]["extra"] = 0
        self.assert_rejected(records, "world_time fields are not closed")

    def test_rejects_missing_nested_metric_fields(self) -> None:
        for group in ("map_totals", "gpu_totals", "network", "assets"):
            with self.subTest(group=group):
                records = good_records()
                records[2][group].pop(next(iter(records[2][group])))
                self.assert_rejected(records, f"frame {group} fields are not closed")

    def test_rejects_nested_cumulative_counter_resets(self) -> None:
        for field in ("main_service_gap_max_us", "queue_processing_total_us",
                      "queue_budget_yields_total", "keepalive_tx_total",
                      "keepalive_rx_total", "keepalive_timeout_total"):
            with self.subTest(group="network", field=field):
                records = good_records()
                records[2]["network"][field] = 1
                records[5]["network"][field] = 0
                self.assert_rejected(records, "network cumulative counter reset")
        records = good_records()
        records[5]["assets"]["installed_total"] = 0
        self.assert_rejected(records, "asset cumulative counter reset")

    def test_rejects_success_without_completed_post_route_drain(self) -> None:
        records = good_records()
        records.pop(7)
        records[-1]["frames"] = 2
        records[-1]["presented_frames"] = 2
        self.assert_rejected(records, "lacks a presented frame after the final checkpoint")
        records = good_records()
        records[7]["elapsed_us"] = 1_000_200
        records[-1]["elapsed_us"] = 1_000_200
        self.assert_rejected(records, "drain completed")

    def test_rejects_oversized_report_line_before_json_decode(self) -> None:
        original = verifier.MAX_LINE_BYTES
        verifier.MAX_LINE_BYTES = 100
        self.addCleanup(setattr, verifier, "MAX_LINE_BYTES", original)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            route_path = root / "route.xml"
            report_path = root / "report.jsonl"
            route_path.write_bytes(ROUTE)
            report_path.write_bytes(b"{" + b" " * 100 + b"}\n")
            with self.assertRaisesRegex(verifier.ReportError, "exceeds 1 MiB"):
                verifier.verify(route_path, report_path)

    def test_rejects_nonmonotonic_frame_elapsed_and_counter_reset(self) -> None:
        records = good_records()
        records[5]["elapsed_us"] = 199
        self.assert_rejected(records, "inconsistent with frame_us")
        records = good_records()
        records[5]["cpu_totals_us"][0] = 9
        self.assert_rejected(records, "CPU cumulative counter reset")
        records = good_records()
        records[5]["gpu_invalidation_totals"][0] = 0
        self.assert_rejected(records, "GPU invalidation cumulative counter reset")

    def test_rejects_unknown_record_and_false_success_counts(self) -> None:
        records = good_records()
        records.insert(-1, {"type": "mystery", "elapsed_us": 1_000_201})
        self.assert_rejected(records, "unknown record type")
        records = good_records()
        records[-1]["arrivals"] = 1
        self.assert_rejected(records, "terminal counts")

    def test_rejects_identity_route_digest_and_checkpoint_count_mismatch(self) -> None:
        records = good_records()
        records[0]["route_sha256"] = "0" * 64
        self.assert_rejected(records, "route SHA-256")
        records = good_records()
        records[0]["route_checkpoints"] = 3
        self.assert_rejected(records, "checkpoint count")


if __name__ == "__main__":
    unittest.main()
