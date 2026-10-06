"""Exercise the package entry point without native builds or downloads."""
from __future__ import annotations

import os
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SourceProfilePackageTests(unittest.TestCase):
    def test_incomplete_or_release_qualification_fails_before_dependencies(self) -> None:
        for extra in (
            {"ATRINIK_PACKAGE_VERSION": "5.78.0", "ATRINIK_SOURCE_PROFILE_QUALIFICATION": "/missing"},
            {"ATRINIK_PACKAGE_VERSION": "0.0.0", "ATRINIK_SOURCE_PROFILE_QUALIFICATION": "/missing"},
            {"ATRINIK_PACKAGE_VERSION": "0.0.0", "ATRINIK_SOURCE_PROFILE_QUALIFICATION_SHA256": "0" * 64},
            {"ATRINIK_PACKAGE_VERSION": "0.0.0", "ATRINIK_SOURCE_PROFILE_QUALIFICATION": "/missing",
             "ATRINIK_SOURCE_PROFILE_QUALIFICATION_SHA256": "0" * 64,
             "ATRINIK_PROFILE_CONTENT_DIR": "/missing"},
        ):
            environment = {key: value for key, value in os.environ.items()
                           if not key.startswith(("ATRINIK_", "MXE_"))}
            environment.update(extra)
            with self.subTest(extra=extra):
                result = subprocess.run(["bash", str(ROOT / "tools/build-windows-package.sh")],
                                        cwd=ROOT, env=environment, capture_output=True, text=True,
                                        timeout=10, check=False)
                self.assertEqual(1, result.returncode)
                self.assertIn("source-profile qualification requires", result.stderr)
                self.assertNotIn("installed build dependency", result.stderr)


if __name__ == "__main__":
    unittest.main()
