#!/usr/bin/env python3
"""Verify and summarize a bounded live movement route report."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import sys
import xml.etree.ElementTree as ET
from array import array
from collections.abc import Sequence
from pathlib import Path
from typing import NoReturn


MAX_ROUTE_BYTES = 8 * 1024 * 1024
MAX_REPORT_BYTES = 128 * 1024 * 1024
MAX_REPORT_LINES = 1_000_000
MAX_LINE_BYTES = 1024 * 1024
MAX_COUNTER = (1 << 64) - 1
HEX_40 = re.compile(r"[0-9a-f]{40}\Z")
HEX_64 = re.compile(r"[0-9a-f]{64}\Z")
MAP_PATH = re.compile(r"/(?:[^/\x00-\x1f]+/)*[^/\x00-\x1f]+\Z")
ROOT_ATTRIBUTES = {"version", "timeout-ms", "step-timeout-ms"}
CHECKPOINT_ATTRIBUTES = {"map", "x", "y", "direction"}
IDENTITY_FIELDS = {
    "type", "schema_version", "route_sha256", "route_checkpoints",
    "source_revision", "source_dirty", "gpu_backend", "gpu_device", "gpu_driver",
    "viewport_width", "viewport_height", "look_width", "look_height", "fps_limit",
    "smooth_lighting", "gpu_hardware_timing_available", "cpu_stage_names",
    "gpu_host_stage_names",
}
ARRIVAL_FIELDS = {
    "type", "index", "map_path", "x", "y", "publication_generation", "elapsed_us",
}
PRESENTATION_FIELDS = {
    "type", "index", "map_publication_generation", "gpu_published_generation",
    "elapsed_us",
}
FRAME_FIELDS = {
    "type", "sequence", "elapsed_us", "frame_us", "presented", "map_path", "x", "y",
    "map_publication_generation", "map_ready", "cpu_totals_us", "gpu_host_totals_ns",
    "gpu_invalidation_totals", "map_totals", "gpu_totals", "network", "assets",
}
TERMINAL_FIELDS = {
    "type", "status", "reason", "arrivals", "presented_checkpoints",
    "expected_checkpoints", "frames", "presented_frames", "elapsed_us",
}


class ReportError(ValueError):
    """The route or live report violates its closed contract."""


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ReportError(message)


def _integer(value: object, label: str, minimum: int = 0,
             maximum: int | None = MAX_COUNTER) -> int:
    _require(type(value) is int and value >= minimum, f"{label} must be an integer >= {minimum}")
    if maximum is not None:
        _require(value <= maximum, f"{label} must be <= {maximum}")
    return value


def _closed(record: dict, fields: set[str], label: str) -> None:
    missing = fields - set(record)
    unknown = set(record) - fields
    _require(not missing and not unknown,
             f"{label} fields are not closed "
             f"(missing={sorted(missing)}, unknown={sorted(unknown)})")


def _parse_decimal(value: str | None, label: str, minimum: int, maximum: int) -> int:
    _require(value is not None and re.fullmatch(r"0|[1-9][0-9]*", value) is not None,
             f"{label} must be a canonical decimal integer")
    _require(len(value) <= len(str(maximum)), f"{label} must be <= {maximum}")
    return _integer(int(value), label, minimum, maximum)


def read_route(path: Path) -> tuple[bytes, list[dict[str, object]], dict[str, int]]:
    try:
        with path.open("rb") as stream:
            contents = stream.read(MAX_ROUTE_BYTES + 1)
    except OSError as error:
        raise ReportError(f"cannot read route: {error}") from error
    _require(len(contents) <= MAX_ROUTE_BYTES, "route exceeds 8 MiB")
    _require(bool(contents), "route is empty")
    try:
        text = contents.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ReportError("route is not UTF-8") from error
    _require("<!" not in text, "route declarations, entities, and comments are forbidden")
    _require("&" not in text, "route entity references are forbidden")
    _require(re.search(r"(?:^|\s)xmlns(?::[^=\s]+)?\s*=", text) is None,
             "route namespaces are forbidden")
    stripped = text.lstrip("\ufeff \t\r\n")
    if stripped.startswith("<?xml"):
        _require(re.match(r"<\?xml[ \t\r\n]", stripped) is not None,
                 "route XML declaration is malformed")
        end = stripped.find("?>")
        _require(end >= 0, "route XML declaration is truncated")
        stripped = stripped[end + 2:]
    _require("<?" not in stripped, "route processing instructions are forbidden")
    try:
        root = ET.fromstring(contents)
    except ET.ParseError as error:
        raise ReportError(f"invalid route XML: {error}") from error
    _require(root.tag == "live-movement-route" and ":" not in root.tag,
             "route root must be live-movement-route without a namespace")
    _require(set(root.attrib) == ROOT_ATTRIBUTES, "route root attributes are not closed")
    _require(root.attrib["version"] == "1", "route version must be 1")
    timeout_ms = _parse_decimal(root.attrib["timeout-ms"], "route timeout-ms", 1, 3_600_000)
    step_timeout_ms = _parse_decimal(root.attrib["step-timeout-ms"],
                                     "route step-timeout-ms", 1, 60_000)
    _require(step_timeout_ms <= timeout_ms, "route step-timeout-ms exceeds timeout-ms")
    _require(root.text is None or not root.text.strip(), "route root cannot contain text")
    checkpoints: list[dict[str, object]] = []
    for index, child in enumerate(root):
        _require(child.tag == "checkpoint" and ":" not in child.tag,
                 "route may contain only checkpoint elements without namespaces")
        _require(set(child.attrib) == CHECKPOINT_ATTRIBUTES,
                 f"checkpoint {index} attributes are not closed")
        _require(len(child) == 0 and (child.text is None or not child.text.strip()) and
                 (child.tail is None or not child.tail.strip()),
                 f"checkpoint {index} must be empty")
        map_path = child.attrib["map"]
        _require(MAP_PATH.fullmatch(map_path) is not None,
                 f"checkpoint {index} map must be an absolute canonical map path")
        _require(all(part not in {".", ".."} for part in map_path.split("/")[1:]),
                 f"checkpoint {index} map must not contain dot segments")
        x = _parse_decimal(child.attrib["x"], f"checkpoint {index} x", 0, 255)
        y = _parse_decimal(child.attrib["y"], f"checkpoint {index} y", 0, 255)
        direction = _parse_decimal(child.attrib["direction"],
                                   f"checkpoint {index} direction", 0, 9)
        if index == 0:
            _require(direction == 0, "first checkpoint direction must be 0")
        else:
            _require(direction in {1, 2, 3, 4, 6, 7, 8, 9},
                     f"checkpoint {index} direction must be a keypad direction other than 5")
        checkpoints.append({"map_path": map_path, "x": x, "y": y, "direction": direction})
    _require(2 <= len(checkpoints) <= 50_000,
             "route must contain between 2 and 50000 checkpoints")
    return contents, checkpoints, {
        "timeout_us": timeout_ms * 1000,
        "step_timeout_us": step_timeout_ms * 1000,
    }


def _reject_constant(value: str) -> NoReturn:
    raise ReportError(f"non-finite JSON number is forbidden: {value}")


def _object(pairs: list[tuple[str, object]]) -> dict:
    result: dict[str, object] = {}
    for key, value in pairs:
        _require(key not in result, f"duplicate JSON member: {key}")
        result[key] = value
    return result


def _records(path: Path):
    try:
        stream = path.open("rb")
    except OSError as error:
        raise ReportError(f"cannot read report: {error}") from error
    total = 0
    with stream:
        line_number = 0
        while True:
            try:
                raw_line = stream.readline(MAX_LINE_BYTES + 1)
            except OSError as error:
                raise ReportError(f"cannot read report: {error}") from error
            if not raw_line:
                break
            line_number += 1
            _require(line_number <= MAX_REPORT_LINES, "report exceeds 1000000 lines")
            total += len(raw_line)
            _require(total <= MAX_REPORT_BYTES, "report exceeds 128 MiB")
            _require(len(raw_line) <= MAX_LINE_BYTES, f"report line {line_number} exceeds 1 MiB")
            _require(raw_line.strip(), f"report line {line_number} is blank")
            try:
                line = raw_line.decode("utf-8")
            except UnicodeDecodeError as error:
                raise ReportError(f"report line {line_number} is not UTF-8") from error
            try:
                value = json.loads(line, parse_constant=_reject_constant,
                                   object_pairs_hook=_object)
            except json.JSONDecodeError as error:
                raise ReportError(f"report line {line_number} is invalid JSON: {error}") from error
            _require(isinstance(value, dict), f"report line {line_number} must be an object")
            yield line_number, value


def _string(value: object, label: str, allow_empty: bool = False) -> str:
    _require(isinstance(value, str) and (allow_empty or bool(value)),
             f"{label} must be {'a' if allow_empty else 'a nonempty'} string")
    return value


def _counter_map(value: object, label: str) -> dict[str, int]:
    _require(isinstance(value, dict), f"{label} must be an object")
    result: dict[str, int] = {}
    for key, item in value.items():
        _require(isinstance(key, str) and bool(key), f"{label} has an invalid field name")
        result[key] = _integer(item, f"{label}.{key}")
    return result


def _network_health(value: object) -> dict[str, int | bool]:
    _require(isinstance(value, dict), "frame network must be an object")
    result: dict[str, int | bool] = {}
    for field in ("connected", "shutdown_pending"):
        _require(type(value.get(field)) is bool, f"frame network.{field} must be boolean")
        result[field] = value[field]
    for key, item in value.items():
        _require(isinstance(key, str) and bool(key), "frame network has an invalid field name")
        if key not in result:
            result[key] = _integer(item, f"frame network.{key}")
    return result


def _stage_names(value: object, label: str) -> list[str]:
    _require(isinstance(value, list), f"{label} must be an array")
    _require(bool(value), f"{label} must not be empty")
    _require(all(isinstance(item, str) and item for item in value),
             f"{label} entries must be nonempty strings")
    _require(len(set(value)) == len(value), f"{label} entries must be unique")
    return value


def _quantiles(values: Sequence[int]) -> dict[str, int]:
    _require(bool(values), "cannot summarize an empty measurement series")
    ordered = sorted(values)
    result: dict[str, int] = {}
    for percentile in (50, 90, 95, 99):
        result[f"p{percentile}"] = ordered[math.ceil(len(ordered) * percentile / 100) - 1]
    result["max"] = ordered[-1]
    return result


def _stage_summary(names: list[str], samples: list[array], unit: str) -> dict:
    result = {}
    for index, name in enumerate(names):
        values = samples[index]
        entry = _quantiles(values)
        entry[f"total_{unit}"] = sum(values)
        result[name] = entry
    return result


def _validate_identity(record: dict, route_digest: str, checkpoint_count: int) -> dict:
    _closed(record, IDENTITY_FIELDS, "identity")
    _require(record["type"] == "identity" and type(record["schema_version"]) is int and
             record["schema_version"] == 1,
             "first record must be schema-version-1 identity")
    _require(record["route_sha256"] == route_digest, "identity route SHA-256 does not match")
    _require(type(record["route_checkpoints"]) is int and
             record["route_checkpoints"] == checkpoint_count,
             "identity route checkpoint count does not match")
    _require(isinstance(record["source_revision"], str) and
             HEX_40.fullmatch(record["source_revision"]) is not None,
             "identity source_revision must be a full lowercase Git SHA")
    _require(type(record["source_dirty"]) is bool, "identity source_dirty must be boolean")
    for field in ("gpu_backend", "gpu_device", "gpu_driver"):
        _string(record[field], f"identity {field}")
    for field in ("viewport_width", "viewport_height", "look_width", "look_height", "fps_limit"):
        _integer(record[field], f"identity {field}", 1)
    _require(record["look_width"] <= record["viewport_width"] and
             record["look_height"] <= record["viewport_height"],
             "identity look dimensions exceed viewport")
    _require(record["gpu_hardware_timing_available"] is False,
             "schema 1 requires gpu_hardware_timing_available=false")
    _require(type(record["smooth_lighting"]) is bool,
             "identity smooth_lighting must be boolean")
    record["cpu_stage_names"] = _stage_names(record["cpu_stage_names"],
                                              "identity cpu_stage_names")
    record["gpu_host_stage_names"] = _stage_names(record["gpu_host_stage_names"],
                                                   "identity gpu_host_stage_names")
    return record


def verify(route_path: Path, report_path: Path) -> dict:
    route_bytes, checkpoints, route_limits = read_route(route_path)
    route_digest = hashlib.sha256(route_bytes).hexdigest()
    identity: dict | None = None
    terminal: dict | None = None
    arrivals = 0
    presentations = 0
    frame_count = 0
    presented_frames = 0
    last_elapsed = -1
    last_frame_elapsed = 0
    last_arrival_generation = 0
    last_gpu_generation = 0
    pending_arrival: dict | None = None
    presentation_frame_generation: int | None = None
    step_started_elapsed = 0
    frame_times = array("Q")
    presented_spacings = array("Q")
    first_presented_elapsed: int | None = None
    last_presented_elapsed: int | None = None
    cpu_deltas: list[array] = []
    gpu_host_deltas: list[array] = []
    previous_cpu: list[int] | None = None
    previous_gpu_host: list[int] | None = None
    previous_gpu_invalidations: list[int] | None = None
    previous_map_totals: dict[str, int] | None = None
    previous_gpu_totals: dict[str, int] | None = None
    network_maxima: dict[str, int | bool] = {}
    asset_maxima: dict[str, int] = {}
    covered_maps: list[str] = []
    covered_tiles: set[tuple[str, int, int]] = set()

    record_count = 0
    for line_number, record in _records(report_path):
        record_count += 1
        record_type = record.get("type")
        _require(terminal is None, f"record after terminal at line {line_number}")
        if record_count == 1:
            identity = _validate_identity(record, route_digest, len(checkpoints))
            cpu_deltas = [array("Q") for _ in identity["cpu_stage_names"]]
            gpu_host_deltas = [array("Q") for _ in identity["gpu_host_stage_names"]]
            continue
        _require(identity is not None, "report identity is missing")
        _require(record_type != "identity", f"duplicate identity at line {line_number}")

        if record_type == "frame":
            _closed(record, FRAME_FIELDS, "frame")
            frame_count += 1
            _require(type(record["sequence"]) is int and record["sequence"] == frame_count,
                     f"frame sequence is not contiguous at line {line_number}")
            elapsed = _integer(record["elapsed_us"], "frame elapsed_us", 1)
            frame_us = _integer(record["frame_us"], "frame frame_us", 1)
            _require(elapsed <= route_limits["timeout_us"], "report exceeded route timeout")
            _require(elapsed >= last_elapsed, "record elapsed_us decreased")
            _require((frame_count == 1 and elapsed >= frame_us) or
                     (frame_count > 1 and elapsed > last_frame_elapsed and
                      elapsed - last_frame_elapsed >= frame_us),
                     "frame elapsed_us is inconsistent with frame_us")
            last_elapsed = elapsed
            last_frame_elapsed = elapsed
            _require(type(record["presented"]) is bool, "frame presented must be boolean")
            _require(type(record["map_ready"]) is bool, "frame map_ready must be boolean")
            map_path = _string(record["map_path"], "frame map_path", allow_empty=True)
            if map_path:
                _require(MAP_PATH.fullmatch(map_path) is not None, "frame map_path is invalid")
            x = _integer(record["x"], "frame x", 0, 255)
            y = _integer(record["y"], "frame y", 0, 255)
            generation = _integer(record["map_publication_generation"],
                                  "frame map_publication_generation")
            cpu = record["cpu_totals_us"]
            gpu_host = record["gpu_host_totals_ns"]
            _require(isinstance(cpu, list) and len(cpu) == len(identity["cpu_stage_names"]),
                     "frame cpu_totals_us length does not match identity")
            _require(isinstance(gpu_host, list) and
                     len(gpu_host) == len(identity["gpu_host_stage_names"]),
                     "frame gpu_host_totals_ns length does not match identity")
            cpu = [_integer(item, "frame cpu total") for item in cpu]
            gpu_host = [_integer(item, "frame GPU host total") for item in gpu_host]
            if previous_cpu is None:
                cpu_delta = cpu
                gpu_host_delta = gpu_host
            else:
                _require(all(current >= previous for current, previous in zip(cpu, previous_cpu)),
                         "frame CPU cumulative counter reset")
                _require(all(current >= previous for current, previous in
                             zip(gpu_host, previous_gpu_host or [])),
                         "frame GPU host cumulative counter reset")
                cpu_delta = [current - previous for current, previous in zip(cpu, previous_cpu)]
                gpu_host_delta = [current - previous for current, previous in
                                  zip(gpu_host, previous_gpu_host or [])]
            for values, delta in ((cpu_deltas, cpu_delta),
                                  (gpu_host_deltas, gpu_host_delta)):
                for samples, item in zip(values, delta):
                    samples.append(item)
            previous_cpu = cpu
            previous_gpu_host = gpu_host
            gpu_invalidations = record["gpu_invalidation_totals"]
            _require(isinstance(gpu_invalidations, list) and len(gpu_invalidations) == 11,
                     "frame gpu_invalidation_totals must contain 11 counters")
            gpu_invalidations = [
                _integer(item, "frame GPU invalidation total") for item in gpu_invalidations
            ]
            if previous_gpu_invalidations is not None:
                _require(all(current >= previous for current, previous in
                             zip(gpu_invalidations, previous_gpu_invalidations)),
                         "frame GPU invalidation cumulative counter reset")
            previous_gpu_invalidations = gpu_invalidations
            map_totals = _counter_map(record["map_totals"], "frame map_totals")
            gpu_totals = _counter_map(record["gpu_totals"], "frame gpu_totals")
            for current, previous, label in ((map_totals, previous_map_totals, "map"),
                                              (gpu_totals, previous_gpu_totals, "GPU")):
                if previous is not None:
                    _require(set(current) == set(previous),
                             f"frame {label} cumulative counter fields changed")
                    _require(all(current[key] >= previous[key] for key in current),
                             f"frame {label} cumulative counter reset")
            previous_map_totals = map_totals
            previous_gpu_totals = gpu_totals
            network = _network_health(record["network"])
            for key, value in network.items():
                if type(value) is bool:
                    network_maxima[key] = bool(network_maxima.get(key, False)) or value
                else:
                    previous_maximum = network_maxima.get(key, 0)
                    _require(type(previous_maximum) is int,
                             f"frame network.{key} changed type")
                    network_maxima[key] = max(previous_maximum, value)
            assets = _counter_map(record["assets"], "frame assets")
            for key, value in assets.items():
                asset_maxima[key] = max(asset_maxima.get(key, 0), value)
            frame_times.append(frame_us)
            if record["presented"]:
                presented_frames += 1
                if first_presented_elapsed is None:
                    first_presented_elapsed = elapsed
                if last_presented_elapsed is not None:
                    presented_spacings.append(elapsed - last_presented_elapsed)
                last_presented_elapsed = elapsed
                if pending_arrival is not None:
                    qualifying_frame = (
                        record["map_ready"] and map_path == pending_arrival["map_path"] and
                        x == pending_arrival["x"] and y == pending_arrival["y"] and
                        generation >= pending_arrival["publication_generation"]
                    )
                    presentation_frame_generation = generation if qualifying_frame else None
            continue

        elapsed = _integer(record.get("elapsed_us"), f"{record_type} elapsed_us")
        _require(elapsed <= route_limits["timeout_us"], "report exceeded route timeout")
        _require(elapsed >= last_elapsed, "record elapsed_us decreased")
        last_elapsed = elapsed
        if record_type == "arrival":
            _closed(record, ARRIVAL_FIELDS, "arrival")
            _require(pending_arrival is None,
                     "arrival occurred before prior checkpoint presentation")
            _require(arrivals < len(checkpoints), "report contains too many arrivals")
            _require(type(record["index"]) is int and record["index"] == arrivals,
                     "arrival index is not contiguous")
            expected = checkpoints[arrivals]
            _integer(record["x"], "arrival x", 0, 255)
            _integer(record["y"], "arrival y", 0, 255)
            _require(record["map_path"] == expected["map_path"] and
                     record["x"] == expected["x"] and record["y"] == expected["y"],
                     f"arrival {arrivals} does not match its route checkpoint")
            generation = _integer(record["publication_generation"],
                                  "arrival publication_generation", 1)
            _require(generation > last_arrival_generation,
                     "arrival publication_generation is not fresh")
            last_arrival_generation = generation
            pending_arrival = record
            presentation_frame_generation = None
            arrivals += 1
        elif record_type == "checkpoint_presented":
            _closed(record, PRESENTATION_FIELDS, "checkpoint_presented")
            _require(pending_arrival is not None, "checkpoint presentation has no pending arrival")
            _require(type(record["index"]) is int and
                     record["index"] == presentations == pending_arrival["index"],
                     "checkpoint presentation index is not contiguous")
            presentation_map_generation = _integer(
                record["map_publication_generation"],
                "checkpoint map_publication_generation", 1)
            _require(presentation_frame_generation is not None,
                     "checkpoint presentation lacks a preceding matching presented frame")
            _require(presentation_map_generation == presentation_frame_generation,
                     "checkpoint presentation generation does not match its presented frame")
            generation = _integer(record["gpu_published_generation"],
                                  "checkpoint gpu_published_generation", 1)
            _require(generation > last_gpu_generation,
                     "checkpoint GPU published generation is not fresh")
            _require(elapsed - step_started_elapsed <= route_limits["step_timeout_us"],
                     "checkpoint presentation exceeded route step timeout")
            last_gpu_generation = generation
            covered_maps.append(pending_arrival["map_path"])
            covered_tiles.add((pending_arrival["map_path"], pending_arrival["x"],
                               pending_arrival["y"]))
            pending_arrival = None
            presentation_frame_generation = None
            step_started_elapsed = elapsed
            presentations += 1
        elif record_type == "terminal":
            _closed(record, TERMINAL_FIELDS, "terminal")
            _require(isinstance(record["status"], str) and
                     record["status"] in {"success", "failure"},
                     "terminal status is invalid")
            _string(record["reason"], "terminal reason", allow_empty=True)
            for field in ("arrivals", "presented_checkpoints", "expected_checkpoints",
                          "frames", "presented_frames"):
                _integer(record[field], f"terminal {field}")
            terminal = record
        else:
            raise ReportError(f"unknown record type at line {line_number}: {record_type!r}")

    _require(identity is not None, "report is empty or missing identity")
    _require(terminal is not None, "report is truncated: terminal record is missing")
    _require(terminal["status"] == "success", f"terminal reported failure: {terminal['reason']}")
    expected_count = len(checkpoints)
    _require(pending_arrival is None and arrivals == presentations == expected_count,
             "report did not present every route checkpoint")
    expected_terminal = {
        "arrivals": arrivals,
        "presented_checkpoints": presentations,
        "expected_checkpoints": expected_count,
        "frames": frame_count,
        "presented_frames": presented_frames,
    }
    _require(all(terminal[field] == value for field, value in expected_terminal.items()),
             "terminal counts do not match observed report records")
    _require(terminal["elapsed_us"] == last_elapsed,
             "terminal elapsed_us does not match the report duration")
    _require(frame_count > 0 and presented_frames > 0, "successful report has no presented frames")
    _require(bool(presented_spacings),
             "successful report needs at least two presented frames for FPS evidence")
    _require(first_presented_elapsed is not None and last_presented_elapsed is not None,
             "presented frame timing is incomplete")
    duration_us = last_presented_elapsed - first_presented_elapsed
    _require(duration_us > 0, "presented frame duration is not positive")
    unique_maps = list(dict.fromkeys(covered_maps))
    return {
        "status": "success",
        "route_sha256": route_digest,
        "source_revision": identity["source_revision"],
        "source_dirty": identity["source_dirty"],
        "gpu_backend": identity["gpu_backend"],
        "gpu_device": identity["gpu_device"],
        "gpu_driver": identity["gpu_driver"],
        "settings": {
            field: identity[field] for field in
            ("viewport_width", "viewport_height", "look_width", "look_height", "fps_limit",
             "smooth_lighting", "gpu_hardware_timing_available")
        },
        "arrivals": arrivals,
        "presented_checkpoints": presentations,
        "expected_checkpoints": expected_count,
        "frames": frame_count,
        "presented_frames": presented_frames,
        "elapsed_us": terminal["elapsed_us"],
        "coverage": {
            "maps": unique_maps,
            "unique_maps": len(unique_maps),
            "unique_tiles": len(covered_tiles),
            "checkpoints": presentations,
        },
        "frame_us": _quantiles(frame_times),
        "presented_frame_spacing_us": _quantiles(presented_spacings),
        "presented_fps": presented_frames * 1_000_000.0 / terminal["elapsed_us"],
        "presented_interval_fps": (presented_frames - 1) * 1_000_000.0 / duration_us,
        "cpu_stages": _stage_summary(identity["cpu_stage_names"], cpu_deltas, "us"),
        "gpu_host_stages": _stage_summary(identity["gpu_host_stage_names"],
                                           gpu_host_deltas, "ns"),
        "gpu_invalidation_totals": previous_gpu_invalidations,
        "network_maxima": network_maxima,
        "asset_maxima": asset_maxima,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("route", type=Path)
    parser.add_argument("report", type=Path)
    parser.add_argument("--output", type=Path, metavar="SUMMARY")
    arguments = parser.parse_args(argv)
    try:
        summary = verify(arguments.route, arguments.report)
        rendered = json.dumps(summary, sort_keys=True, separators=(",", ":")) + "\n"
        if arguments.output is not None:
            arguments.output.write_text(rendered, encoding="utf-8")
        sys.stdout.write(rendered)
    except (OSError, ReportError) as error:
        print(f"verify_live_movement.py: error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
