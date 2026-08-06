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
        schema = GENERATE.load_schema(ROOT / "schema/legacy-commands.json")
        self.assertEqual(schema["protocol_version"], 1072)
        self.assertEqual(len(schema["client_to_server"]), 24)
        self.assertEqual(len(schema["server_to_client"]), 29)

    def test_repository_outputs_are_current(self) -> None:
        schema = GENERATE.load_schema(ROOT / "schema/legacy-commands.json")
        self.assertTrue(GENERATE.update_outputs(GENERATE.expected_outputs(ROOT, schema), True))

    def test_duplicate_json_key_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "schema.json"
            path.write_text('{"schema_version":1,"schema_version":1}', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "duplicate JSON key"):
                GENERATE.load_schema(path)

    def test_noncontiguous_command_id_is_rejected(self) -> None:
        data = json.loads((ROOT / "schema/legacy-commands.json").read_text(encoding="utf-8"))
        data["client_to_server"][1]["id"] = 9
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "schema.json"
            path.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "contiguous value 1"):
                GENERATE.load_schema(path)

    def test_duplicate_symbol_is_rejected(self) -> None:
        data = json.loads((ROOT / "schema/legacy-commands.json").read_text(encoding="utf-8"))
        data["server_to_client"][1]["symbol"] = data["server_to_client"][0]["symbol"]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "schema.json"
            path.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "duplicate server_to_client symbol"):
                GENERATE.load_schema(path)


if __name__ == "__main__":
    unittest.main()
