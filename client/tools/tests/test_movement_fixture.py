from __future__ import annotations

import hashlib
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ElementTree


CLIENT_ROOT = Path(__file__).resolve().parents[2]
FIXTURES = CLIENT_ROOT / "src/tests/fixtures/player_view"
DEPTHS = (-2, -1, 0, 1, 2)
MOVEMENT_STREAM_MAGIC = b"PVM1"
DENSE_RECORDS_PER_DEPTH = 17 * 17
EXPECTED_OBJECT_COUNTS = {-2: 16, -1: 23, 0: 201, 1: 24, 2: 13}
TRANSITION_MAP_NAME = "Movement transition"
TRANSITION_MAP_PATH = "/tests/player-view/movement-transition"
RESIZE_DELTA = (32, 24)
EXPECTED_STANDARD_CHECKPOINTS = {
    "movement-colored.xml": "d1c37190727b62a0f3f4fe9892dcf5179fc6e98a5ca08ab36bc75ce35f56f185",
    "movement-lighting-isolated.xml": "0c638194c31685bed8d4394f633aa3399b7f73de2b8a7f5ea9ed04b5ec0d3e78",
    "movement-colored-discrete.xml": "0c24f6c578651f9a674d02734bfb701198edd1d3ebbf81338cce9ca506d62119",
}


def movement_packets(path: Path) -> list[bytes]:
    data = bytes.fromhex(path.read_text(encoding="ascii"))
    if data[:4] != MOVEMENT_STREAM_MAGIC:
        raise ValueError("movement stream magic is invalid")
    count = data[4]
    cursor = 5
    packets = []
    for _ in range(count):
        size = struct.unpack_from(">I", data, cursor)[0]
        cursor += 4
        packets.append(data[cursor:cursor + size])
        cursor += size
    if cursor != len(data):
        raise ValueError("movement stream framing is invalid")
    return packets


def packet_levels(packet: bytes) -> list[tuple[int, bytes]]:
    cursor = 7
    levels = []
    for _ in range(packet[6]):
        depth, size = struct.unpack_from(">bI", packet, cursor)
        cursor += 5
        levels.append((depth, packet[cursor:cursor + size]))
        cursor += size
    if cursor != len(packet):
        raise ValueError("MAP level framing is invalid")
    return levels


def new_packet_header_end(packet: bytes) -> int:
    if not packet or packet[0] != 1:
        raise ValueError("reset packet is not MAP_UPDATE_CMD_NEW")
    cursor = 1
    for _ in range(3):
        cursor = packet.index(b"\0", cursor) + 1
    cursor += 2
    for _ in range(3):
        cursor = packet.index(b"\0", cursor) + 1
    return cursor + 5


def new_packet_geometry(packet: bytes) -> tuple[int, int, int, int]:
    header_end = new_packet_header_end(packet)
    return tuple(packet[header_end - 5:header_end - 1])


def new_packet_identity(packet: bytes) -> tuple[str, str]:
    if not packet or packet[0] != 1:
        raise ValueError("reset packet is not MAP_UPDATE_CMD_NEW")
    cursor = 1
    fields = []
    for _ in range(3):
        end = packet.index(b"\0", cursor)
        fields.append(packet[cursor:end].decode("utf-8"))
        cursor = end + 1
    cursor += 2
    for _ in range(3):
        end = packet.index(b"\0", cursor)
        fields.append(packet[cursor:end].decode("utf-8"))
        cursor = end + 1
    return fields[0], fields[5]


def new_packet_levels(packet: bytes) -> list[tuple[int, bytes]]:
    cursor = new_packet_header_end(packet) + 2
    count = packet[cursor]
    cursor += 1
    levels = []
    for _ in range(count):
        depth, size = struct.unpack_from(">bI", packet, cursor)
        cursor += 5
        levels.append((depth, packet[cursor:cursor + size]))
        cursor += size
    if cursor != len(packet):
        raise ValueError("NEW MAP level framing is invalid")
    return levels


def dense_records(payload: bytes) -> tuple[list[dict[str, object]], int]:
    """Decode the generator's bounded record subset from one level payload."""
    cursor = 0
    records: list[dict[str, object]] = []
    while len(records) < DENSE_RECORDS_PER_DEPTH:
        if len(payload) - cursor < 6:
            raise ValueError("dense fixture record is truncated")
        mask, scalar, layer_count = struct.unpack_from(">HHB", payload, cursor)
        cursor += 5
        layers = []
        for _ in range(layer_count):
            if len(payload) - cursor < 5:
                raise ValueError("dense fixture layer is truncated")
            layer, face, object_flags, flags = struct.unpack_from(">BHBB", payload, cursor)
            cursor += 5
            if object_flags != 0 or flags != 0:
                raise ValueError("dense fixture layer has unsupported flags")
            layers.append((layer, face))

        ext_flags = payload[cursor]
        cursor += 1
        rgb = None
        if ext_flags == 0x2:
            if len(payload) - cursor < 7 or payload[cursor] != 0x1:
                raise ValueError("dense fixture RGB radiance is malformed")
            rgb = struct.unpack_from(">HHH", payload, cursor + 1)
            cursor += 7
        elif ext_flags != 0:
            raise ValueError("dense fixture has unsupported extended flags")

        records.append({"mask": mask, "scalar": scalar, "layers": layers, "rgb": rgb})
    return records, cursor


class MovementFixtureTests(unittest.TestCase):
    def test_generated_five_depth_snapshot_and_delta_are_pinned(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            temporary_path = Path(temporary)
            generated_snapshot = temporary_path / "snapshot.hex"
            generated_delta = temporary_path / "delta.hex"
            generated_static_delta = temporary_path / "static-delta.hex"
            generated_transition = temporary_path / "transition.hex"
            subprocess.run(
                [
                    sys.executable,
                    str(CLIENT_ROOT / "tools/generate_movement_five_depth.py"),
                    str(FIXTURES / "colored-scene.map2.hex"),
                    str(generated_snapshot),
                ],
                check=True,
            )
            subprocess.run(
                [
                    sys.executable,
                    str(CLIENT_ROOT / "tools/generate_movement_five_depth.py"),
                    str(FIXTURES / "colored-scene.map2.hex"),
                    str(generated_transition),
                    "--transition",
                ],
                check=True,
            )
            subprocess.run(
                [
                    sys.executable,
                    str(CLIENT_ROOT / "tools/generate_movement_delta.py"),
                    str(generated_delta),
                ],
                check=True,
            )
            subprocess.run(
                [
                    sys.executable,
                    str(CLIENT_ROOT / "tools/generate_movement_delta.py"),
                    str(generated_static_delta),
                    "--static-radiance",
                ],
                check=True,
            )
            for generated, pinned in (
                (generated_snapshot, FIXTURES / "movement-colored-five-depth.map2.hex"),
                (generated_delta, FIXTURES / "movement-colored-delta.map2.hex"),
                (
                    generated_static_delta,
                    FIXTURES / "movement-lighting-static-delta.map2.hex",
                ),
                (generated_transition, FIXTURES / "movement-colored-transition.map2.hex"),
            ):
                self.assertEqual(
                    hashlib.sha256(generated.read_bytes()).digest(),
                    hashlib.sha256(pinned.read_bytes()).digest(),
                )

    def test_lifecycle_inputs_have_distinct_pinned_identities(self) -> None:
        reset_path = FIXTURES / "movement-colored-five-depth.map2.hex"
        transition_path = FIXTURES / "movement-colored-transition.map2.hex"
        reset = bytes.fromhex(reset_path.read_text(encoding="ascii"))
        transition = bytes.fromhex(transition_path.read_text(encoding="ascii"))

        self.assertEqual(new_packet_identity(reset), ("Offline equivalence", ""))
        self.assertEqual(
            new_packet_identity(transition),
            (TRANSITION_MAP_NAME, TRANSITION_MAP_PATH),
        )
        self.assertNotEqual(reset, transition)
        self.assertEqual(new_packet_geometry(reset), new_packet_geometry(transition))
        self.assertEqual(new_packet_levels(reset), new_packet_levels(transition))

        for manifest_name in (
            "movement-colored.xml",
            "movement-lighting-isolated.xml",
            "movement-colored-discrete.xml",
        ):
            root = ElementTree.parse(FIXTURES / manifest_name).getroot()
            self.assertEqual(
                root.attrib["expected-standard-checkpoint-sha256"],
                EXPECTED_STANDARD_CHECKPOINTS[manifest_name],
            )
            resize_delta = (
                int(root.attrib["resize-width-delta"]),
                int(root.attrib["resize-height-delta"]),
            )
            self.assertEqual(resize_delta, RESIZE_DELTA)
            standard = (int(root.attrib["viewport-width"]),
                        int(root.attrib["viewport-height"]))
            self.assertEqual(tuple(a + b for a, b in zip(standard, resize_delta)), (352, 264))
            self.assertEqual(
                tuple(a + b for a, b in zip((1920, 1080), resize_delta)),
                (1952, 1104),
            )
            expected_inputs = {
                "snapshot": reset_path,
                "next-snapshot": FIXTURES
                / (
                    "movement-lighting-static-delta.map2.hex"
                    if manifest_name == "movement-lighting-isolated.xml"
                    else "movement-colored-delta.map2.hex"
                ),
                "transition-snapshot": transition_path,
            }
            for attribute, path in expected_inputs.items():
                self.assertEqual(root.attrib[attribute], path.relative_to(CLIENT_ROOT).as_posix())
                self.assertEqual(
                    root.attrib[f"{attribute}-sha256"],
                    hashlib.sha256(path.read_bytes()).hexdigest(),
                )

    def test_movement_stream_is_closed_and_uses_only_same_map_updates(self) -> None:
        snapshot = bytes.fromhex(
            (FIXTURES / "movement-colored-five-depth.map2.hex").read_text(encoding="ascii")
        )
        _, _, *origin_values = new_packet_geometry(snapshot)
        origin = tuple(origin_values)
        packets = movement_packets(FIXTURES / "movement-colored-delta.map2.hex")

        self.assertEqual(snapshot[0], 1)
        self.assertEqual(len(packets), 5)
        self.assertEqual([packet[0] for packet in packets], [0] * len(packets))
        coordinates = [(packet[1], packet[2]) for packet in packets]
        self.assertEqual(coordinates, [(11, 10), origin, (10, 11), origin, origin])

        first_step = (coordinates[0][0] - origin[0], coordinates[0][1] - origin[1])
        turn_step = (coordinates[2][0] - origin[0], coordinates[2][1] - origin[1])
        self.assertEqual(abs(first_step[0]) + abs(first_step[1]), 1)
        self.assertEqual(abs(turn_step[0]) + abs(turn_step[1]), 1)
        self.assertEqual(first_step[0] * turn_step[0] + first_step[1] * turn_step[1], 0)
        for end in range(4, 9, 4):
            replay = (coordinates[:4] * 2)[:end]
            self.assertEqual(replay[-1], origin)

    def test_active_packets_cover_every_depth_with_clear_light_and_object_deltas(self) -> None:
        packets = movement_packets(FIXTURES / "movement-colored-delta.map2.hex")
        expected_cells = (
            ((20, 10), (20, 11)),
            ((0, 10), (0, 9)),
            ((10, 20), (11, 20)),
            ((10, 0), (9, 0)),
        )
        for packet_index, packet in enumerate(packets):
            self.assertEqual(packet[3:6], b"\0\0\0")
            levels = packet_levels(packet)
            self.assertEqual([depth for depth, _ in levels], list(DEPTHS))
            for _, payload in levels:
                if packet_index == 4:
                    self.assertEqual(payload, b"")
                    continue
                self.assertEqual(len(payload), 20)
                clear_mask, update_mask = struct.unpack_from(">HH", payload, 0)
                clear_cell, update_cell = expected_cells[packet_index]
                self.assertEqual(((clear_mask >> 11) & 0x1F, (clear_mask >> 6) & 0x1F),
                                 clear_cell)
                self.assertEqual(clear_mask & 0x3F, 0x2)
                self.assertEqual(((update_mask >> 11) & 0x1F, (update_mask >> 6) & 0x1F),
                                 update_cell)
                self.assertEqual(update_mask & 0x3F, 0x4)
                self.assertEqual(payload[6], 1)
                self.assertEqual(payload[7], 0)
                self.assertEqual(struct.unpack_from(">H", payload, 8)[0], packet_index + 1)
                self.assertEqual(payload[12:14], b"\x02\x01")

    def test_isolated_active_packets_move_without_changing_radiance(self) -> None:
        packets = movement_packets(FIXTURES / "movement-lighting-static-delta.map2.hex")
        coordinates = [(packet[1], packet[2]) for packet in packets]
        self.assertEqual(coordinates, [(11, 10), (10, 10), (10, 11), (10, 10), (10, 10)])
        for packet in packets:
            levels = packet_levels(packet)
            self.assertEqual([depth for depth, _ in levels], list(DEPTHS))
            self.assertTrue(all(payload == b"" for _, payload in levels))

    def test_lighting_work_delta_wraps_queue_drain_and_primary_draw(self) -> None:
        source = (CLIENT_ROOT / "src/client/player_view.c").read_text(encoding="utf-8")
        function = source[source.index("player_view_movement_draw(") :]
        started = function.index("lighting_benchmark_timings_get(&lighting_before_tick);")
        drained = function.index("client_commands_drain_with_clock(")
        drawn = function.index("map_draw_map(surface);")
        finished = function.index("lighting_benchmark_timings_get(&lighting_after_draw);")
        elapsed = function.index("player_view_lighting_elapsed(&lighting_after_draw)")
        minimap = function.index("map_draw_map(local_minimap_surface);")
        timed_frame = function[function.index("uint64_t frame_started") : minimap]
        self.assertNotIn("lighting_benchmark_statistics_get", timed_frame)
        self.assertLess(started, drained)
        self.assertLess(drained, drawn)
        self.assertLess(drawn, finished)
        self.assertLess(finished, elapsed)
        self.assertLess(elapsed, minimap)

    def test_snapshot_is_dense_representative_and_sparsely_colored(self) -> None:
        source = bytes.fromhex(
            (FIXTURES / "colored-scene.map2.hex").read_text(encoding="ascii")
        )
        snapshot = bytes.fromhex(
            (FIXTURES / "movement-colored-five-depth.map2.hex").read_text(encoding="ascii")
        )
        self.assertEqual(new_packet_geometry(snapshot), (21, 21, 10, 10))

        source_payloads = [payload for _, payload in new_packet_levels(source)]
        levels = new_packet_levels(snapshot)
        self.assertEqual([depth for depth, _ in levels], list(DEPTHS))
        expected_coordinates = {
            (x, y) for y in range(2, 19) for x in range(2, 19)
        }
        observed_layers = set()
        observed_faces = set()
        for index, (depth, payload) in enumerate(levels):
            records, dense_size = dense_records(payload)
            preserved = payload[dense_size:]
            self.assertEqual(preserved, source_payloads[index % len(source_payloads)])
            coordinates = set()
            object_count = 0
            colored_count = 0
            for record in records:
                mask = record["mask"]
                scalar = record["scalar"]
                coordinates.add(((mask >> 11) & 0x1F, (mask >> 6) & 0x1F))
                self.assertEqual(mask & 0x3F, 0x4)
                self.assertIn(scalar, range(0x0700, 0x0880))
                layers = record["layers"]
                object_count += len(layers)
                for layer, face in layers:
                    observed_layers.add(layer)
                    observed_faces.add(face)
                if record["rgb"] is not None:
                    colored_count += 1
                    self.assertTrue(any(channel != scalar for channel in record["rgb"]))
            self.assertEqual(coordinates, expected_coordinates)
            self.assertEqual(object_count, EXPECTED_OBJECT_COUNTS[depth])
            self.assertEqual(colored_count, 3)

        self.assertEqual(observed_layers, {0, 2, 4, 6})
        self.assertEqual(observed_faces, {1, 2, 3, 4, 6, 7})


if __name__ == "__main__":
    unittest.main()
