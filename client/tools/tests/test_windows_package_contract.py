#!/usr/bin/env python3

"""Windows package production/test separation contract."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


class WindowsPackageContractTests(unittest.TestCase):
    def test_package_build_excludes_test_only_renderer(self) -> None:
        script = (ROOT / "tools" / "build-windows-package.sh").read_text(encoding="utf-8")
        sources = (ROOT / "src" / "cmake.txt").read_text(encoding="utf-8")
        self.assertIn("-DBUILD_TESTING=OFF", script)
        self.assertNotIn("-DBUILD_TESTING=ON", script)
        self.assertIn("'--gpu-player-view'", script)
        self.assertIn("'injected GPU conformance fault'", script)
        self.assertIn("python3 tools/verify_gpu_fixture_provenance.py", script)
        self.assertIn(
            "if (BUILD_TESTING)\n\tlist(APPEND SOURCES src/client/gpu_player_view.c)\nendif ()",
            sources,
        )
        production_sources = sources.partition("if (BUILD_TESTING)")[0]
        self.assertNotIn("gpu_player_view.c", production_sources)


if __name__ == "__main__":
    unittest.main()
