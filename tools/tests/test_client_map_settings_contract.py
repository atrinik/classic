from __future__ import annotations

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]


class ClientMapSettingsContractTests(unittest.TestCase):
    def macro(self, name: str) -> int:
        header = (ROOT / "client" / "src" / "include" / "map.h").read_text(
            encoding="utf-8"
        )
        match = re.search(rf"^#define {re.escape(name)} (\d+)$", header, re.MULTILINE)
        self.assertIsNotNone(match, f"missing numeric {name} macro")
        assert match is not None
        return int(match.group(1))

    def setting(self, name: str) -> dict[str, str]:
        lines = (ROOT / "client" / "data" / "settings.txt").read_text(
            encoding="utf-8"
        ).splitlines()
        start = lines.index(f"\tsetting {name}") + 1
        result: dict[str, str] = {}
        for line in lines[start:]:
            if line == "\tend":
                return result
            key, _, value = line.strip().partition(" ")
            result[key] = value
        self.fail(f"unterminated {name} setting")

    def test_user_map_settings_match_the_protocol_bounds(self) -> None:
        minimum = self.macro("MAP_LOOK_SIZE_MIN")
        default = self.macro("MAP_LOOK_SIZE_DEFAULT")
        maximum = self.macro("MAP_LOOK_SIZE_MAX")

        self.assertLessEqual(minimum, default)
        self.assertLessEqual(default, maximum)
        for name in ("Map width", "Map height"):
            with self.subTest(name=name):
                setting = self.setting(name)
                self.assertEqual(setting["type"], "range")
                self.assertEqual(setting["range"], f"{minimum} - {maximum}")
                self.assertEqual(setting["default"], str(default))

    def test_handshake_converts_both_settings_to_wire_sizes(self) -> None:
        main = (ROOT / "client" / "src" / "client" / "main.c").read_text(
            encoding="utf-8"
        )
        converted = re.findall(
            r"MAP_LOOK_TO_WIRE_SIZE\s*\(\s*setting_get_int\s*"
            r"\(\s*OPT_CAT_MAP,\s*(OPT_MAP_(?:WIDTH|HEIGHT))\s*\)\s*\)",
            main,
            re.DOTALL,
        )
        self.assertCountEqual(converted, ["OPT_MAP_WIDTH", "OPT_MAP_HEIGHT"])

        commands = (
            ROOT / "client" / "src" / "client" / "commands.c"
        ).read_text(encoding="utf-8")
        self.assertEqual(commands.count("x > MAP_WIRE_SIZE_MAX"), 1)
        self.assertEqual(commands.count("y > MAP_WIRE_SIZE_MAX"), 1)
        self.assertEqual(commands.count("MAP_WIRE_TO_LOOK_SIZE(x)"), 1)
        self.assertEqual(commands.count("MAP_WIRE_TO_LOOK_SIZE(y)"), 1)


if __name__ == "__main__":
    unittest.main()
