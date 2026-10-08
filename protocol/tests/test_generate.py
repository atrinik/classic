from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location("protocol_generate", ROOT / "tools/generate.py")
assert SPEC is not None and SPEC.loader is not None
GENERATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GENERATE)


class GenerateTests(unittest.TestCase):
    def test_repository_schema_is_valid(self) -> None:
        schema = GENERATE.load_schema(ROOT / "schema/game-commands.json")
        self.assertEqual(schema["protocol_version"], 1082)
        self.assertEqual(schema["item_name_size"], 128)
        self.assertEqual(schema["item_extra_message_size"], 256)
        self.assertEqual(schema["player_status"]["max_statuses"], 48)
        self.assertEqual(len(schema["client_to_server"]), 26)
        self.assertEqual(len(schema["server_to_client"]), 33)

    def test_combined_access_and_exploration_command_ids(self) -> None:
        schema = GENERATE.load_schema(ROOT / "schema/game-commands.json")
        expected = {
            "client_to_server": {
                "ACCESS_AUTH": 23,
                "ACCESS_ADMIN": 24,
                "REGION_EXPLORATION": 25,
            },
            "server_to_client": {
                "ACCESS_RESULT": 29,
                "ACCESS_ADMIN_RESULT": 30,
                "ACCESS_POLICY": 31,
                "REGION_EXPLORATION": 32,
            },
        }
        for direction, commands in expected.items():
            actual = {command["symbol"]: command["id"] for command in schema[direction]}
            for symbol, command_id in commands.items():
                with self.subTest(direction=direction, symbol=symbol):
                    self.assertEqual(actual[symbol], command_id)

    def test_repository_outputs_are_current(self) -> None:
        schema = GENERATE.load_schema(ROOT / "schema/game-commands.json")
        self.assertTrue(GENERATE.update_outputs(GENERATE.expected_outputs(ROOT, schema), True))

    def test_duplicate_json_key_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "schema.json"
            path.write_text('{"schema_version":1,"schema_version":1}', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "duplicate JSON key"):
                GENERATE.load_schema(path)

    def test_noncontiguous_command_id_is_rejected(self) -> None:
        data = json.loads((ROOT / "schema/game-commands.json").read_text(encoding="utf-8"))
        data["client_to_server"][1]["id"] = 9
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "schema.json"
            path.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "contiguous value 1"):
                GENERATE.load_schema(path)

    def test_invalid_item_name_size_is_rejected(self) -> None:
        data = json.loads((ROOT / "schema/game-commands.json").read_text(encoding="utf-8"))
        data["item_name_size"] = 1
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "schema.json"
            path.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "item_name_size is outside"):
                GENERATE.load_schema(path)

    def test_invalid_item_extra_message_size_is_rejected(self) -> None:
        data = json.loads((ROOT / "schema/game-commands.json").read_text(encoding="utf-8"))
        data["item_extra_message_size"] = 1
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "schema.json"
            path.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "item_extra_message_size is outside"):
                GENERATE.load_schema(path)

    def test_duplicate_symbol_is_rejected(self) -> None:
        data = json.loads((ROOT / "schema/game-commands.json").read_text(encoding="utf-8"))
        data["server_to_client"][1]["symbol"] = data["server_to_client"][0]["symbol"]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "schema.json"
            path.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "duplicate server_to_client symbol"):
                GENERATE.load_schema(path)

    def test_invalid_player_status_operation_is_rejected(self) -> None:
        data = json.loads((ROOT / "schema/game-commands.json").read_text(encoding="utf-8"))
        data["player_status"]["operations"][1]["id"] = 9
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "schema.json"
            path.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "contiguous value 1"):
                GENERATE.load_schema(path)

    def test_oversized_player_status_snapshot_is_rejected(self) -> None:
        data = json.loads((ROOT / "schema/game-commands.json").read_text(encoding="utf-8"))
        data["player_status"]["max_statuses"] = 49
        data["player_status"]["tooltip_size"] = 65535
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "schema.json"
            path.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "maximum packet payload"):
                GENERATE.load_schema(path)


if __name__ == "__main__":
    unittest.main()
