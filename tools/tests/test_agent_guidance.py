from __future__ import annotations

from datetime import datetime, timezone
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


class AgentGuidanceTests(unittest.TestCase):
    def test_copyright_header_contract_is_complete(self) -> None:
        guide = " ".join(
            (ROOT / "AGENTS.md").read_text(encoding="utf-8").split()
        )
        contributing = " ".join(
            (ROOT / "CONTRIBUTING.md").read_text(encoding="utf-8").split()
        )
        native_skill = " ".join(
            (
                ROOT / ".agents/skills/classic-native-change/SKILL.md"
            ).read_text(encoding="utf-8").split()
        )

        for marker in {
            "On touch, refresh existing copyright terminal years",
            "blanket holders",
            "`CONTRIBUTING.md`",
            "preserve precise attribution",
        }:
            with self.subTest(surface="AGENTS.md", marker=marker):
                self.assertIn(marker, guide)

        for marker in {
            "Use `The Atrinik Project` as the exact collective holder",
            "migrate prospectively",
            "retain its original start year",
            "current calendar year",
            "Crossfire, Daimonin and other upstream notices",
            "SPDX identifiers",
            "authoritative generator or template",
            "a separate legal and attribution surface",
        }:
            with self.subTest(surface="CONTRIBUTING.md", marker=marker):
                self.assertIn(marker, contributing)

        current_year = datetime.now(timezone.utc).year
        for example in {
            f"Copyright 2021-{current_year} The Atrinik Project",
            f"Copyright {current_year} The Atrinik Project",
            f"Copyright 2024-{current_year} The Atrinik Project",
            (
                f"Copyright (C) 2009-{current_year} Zoey Rose and "
                "Atrinik Development Team"
            ),
        }:
            with self.subTest(example=example):
                self.assertIn(example, contributing)

        for marker in {
            "root `CONTRIBUTING.md` copyright-header contract",
            "preserve named, mixed, and upstream attribution",
            "update generated headers only at their source",
        }:
            with self.subTest(surface="classic-native-change", marker=marker):
                self.assertIn(marker, native_skill)


if __name__ == "__main__":
    unittest.main()
