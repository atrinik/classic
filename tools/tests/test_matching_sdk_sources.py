"""Execute the consumers' real SDK selection with offline CMake fixtures."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class MatchingSDKSourceTests(unittest.TestCase):
    def configure(self, component, layout, override=False, provided=False):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            consumer = root / component
            consumer.mkdir()
            protocol = consumer / "dependencies/protocol"
            protocol.mkdir(parents=True)
            header = protocol / "generated/c/include/atrinik/protocol/game_commands.h"
            header.parent.mkdir(parents=True)
            header.write_text("#define ATRINIK_PROTOCOL_VERSION 1082U\n")
            (protocol / "CMakeLists.txt").write_text(
                "add_library(protocol INTERFACE)\n"
                "add_library(Atrinik::Protocol ALIAS protocol)\n")
            library = root / "explicit" if override else (
                root / "libatrinik" if layout == "sibling" else
                consumer / "dependencies/libatrinik")
            if layout != "missing":
                library.mkdir(parents=True)
                (library / "CMakeLists.txt").write_text(
                    "add_library(core INTERFACE)\n"
                    "add_library(Atrinik::Core ALIAS core)\n"
                    "set_property(GLOBAL PROPERTY sdk_source \"${CMAKE_CURRENT_SOURCE_DIR}\")\n")
            source = (ROOT / component / "CMakeLists.txt").read_text()
            start = source.index("if (NOT FETCHCONTENT_SOURCE_DIR_ATRINIK_PROTOCOL)")
            end = source.index("\nset(EXECUTABLE", start) if component == "client" else source.index("\ninclude(cmake/pcpnatpmp.cmake)", start)
            selection = source[start:end]
            supplied = (
                "add_library(core INTERFACE)\nadd_library(Atrinik::Core ALIAS core)\n"
                if provided else ""
            )
            (consumer / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.21)\n"
                "project(sdk_selection NONE)\ninclude(FetchContent)\n" + supplied + selection +
                '\nget_property(selected GLOBAL PROPERTY sdk_source)\n'
                'file(WRITE "${CMAKE_BINARY_DIR}/selected.txt" "${selected}")\n')
            build = root / "build"
            arguments = ["cmake", "-Werror=dev", "-S", str(consumer), "-B", str(build),
                         "-DFETCHCONTENT_FULLY_DISCONNECTED=ON"]
            if override:
                arguments.append("-DFETCHCONTENT_SOURCE_DIR_LIBATRINIK=" + str(library))
            result = subprocess.run(arguments, capture_output=True, text=True)
            selected = (build / "selected.txt").read_text() if (build / "selected.txt").exists() else None
            return result, selected, str(library)

    def test_sibling_embedded_and_explicit_sources_are_selected_without_downloads(self):
        for component in ("client", "server"):
            for layout, override in (("sibling", False), ("embedded", False), ("embedded", True)):
                with self.subTest(component=component, layout=layout, override=override):
                    result, selected, library = self.configure(component, layout, override)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertEqual(selected, library)

    def test_missing_and_invalid_explicit_sources_fail_before_dependency_acquisition(self):
        for component in ("client", "server"):
            for override in (False, True):
                with self.subTest(component=component, override=override):
                    result, selected, _ = self.configure(component, "missing", override)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("Matching Classic libatrinik source is required", result.stderr)
                    self.assertNotIn("Performing download", result.stdout + result.stderr)
                    self.assertIsNone(selected)

    def test_parent_provided_sdk_target_requires_no_library_source(self):
        for component in ("client", "server"):
            with self.subTest(component=component):
                result, selected, _ = self.configure(component, "missing", provided=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(selected, "")



if __name__ == "__main__":
    unittest.main()
