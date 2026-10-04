from __future__ import annotations

import json
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

        for marker in {
            "On touch, refresh existing Atrinik-owned copyright terminal years",
            "blanket holders",
            "`CONTRIBUTING.md`",
            "preserve precise attribution",
        }:
            with self.subTest(surface="AGENTS.md", marker=marker):
                self.assertIn(marker, guide)

        for marker in {
            "Use `The Atrinik Project` as the exact collective holder",
            "already predominates in modern MIT source headers",
            "exact blanket format is `Copyright START[-END] The Atrinik Project`",
            "Only the surrounding comment delimiters vary by file format",
            "omit `(C)`, `(c)`, `©`, commas, and trailing punctuation",
            "migrate prospectively",
            "each existing Atrinik-owned copyright notice",
            "retain its original start year",
            "current calendar year",
            "Crossfire, Daimonin and other upstream notices",
            "Leave upstream and third-party notice years unchanged",
            "SPDX identifiers",
            "authoritative generator or template",
            "a separate legal and attribution surface",
        }:
            with self.subTest(surface="CONTRIBUTING.md", marker=marker):
                self.assertIn(marker, contributing)

        for example in {
            "Copyright 2021-2026 The Atrinik Project",
            "Copyright 2026 The Atrinik Project",
            "Copyright 2024-2026 The Atrinik Project",
            "Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team",
        }:
            with self.subTest(example=example):
                self.assertIn(example, contributing)

    def test_external_skill_provider_contract_is_pinned_offline(self) -> None:
        provider = json.loads((ROOT / ".agents/skill-provider.json").read_text(encoding="utf-8"))
        self.assertEqual(provider["schema_version"], 1)
        self.assertEqual(provider["repository"], "https://github.com/atrinik/agent-skills")
        self.assertRegex(provider["revision"], r"^[0-9a-f]{40}$")
        self.assertNotEqual(provider["revision"], "0" * 40)
        self.assertEqual(provider["marketplace"], "atrinik")
        self.assertEqual(provider["plugin"], "atrinik-development")
        self.assertEqual(provider["path"], "plugins/atrinik-development")
        self.assertEqual(provider["required_skills"], [
            "classic-native-change", "classic-protocol-change", "classic-runtime",
        ])
        guide = (ROOT / "AGENTS.md").read_text(encoding="utf-8")
        for skill in provider["required_skills"]:
            with self.subTest(skill=skill):
                self.assertIn(f"`{skill}`", guide)
                self.assertNotIn(f".agents/skills/{skill}", guide)
        self.assertIn(".agents/skill-provider.json", guide)
        # Provider-content validation belongs to its repository; this consumer
        # verifies only its own immutable reference without fetching a plugin.


if __name__ == "__main__":
    unittest.main()
