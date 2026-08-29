from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


CLIENT_ROOT = Path(__file__).resolve().parents[2]


def load_module(name: str, path: Path):
    specification = importlib.util.spec_from_file_location(name, path)
    assert specification is not None and specification.loader is not None
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


embed = load_module("embed_gpu_shaders", CLIENT_ROOT / "tools/embed_gpu_shaders.py")
toolchain = load_module(
    "prepare_gpu_shader_toolchain",
    CLIENT_ROOT / "tools/prepare_gpu_shader_toolchain.py",
)


class GPUShaderBuildTests(unittest.TestCase):
    def write_cohort(self, directory: Path) -> Path:
        lines = []
        for name in embed.EXPECTED_NAMES:
            payload = f"generated {name}\n".encode("ascii")
            (directory / name).write_bytes(payload)
            digest = hashlib.sha256(payload).hexdigest()
            lines.append(f"{digest}  ./{name}\n")
        manifest = directory / "expected.sha256"
        manifest.write_text("".join(lines), encoding="ascii")
        return manifest

    def test_embed_requires_and_embeds_the_complete_locked_cohort(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = self.write_cohort(root)
            header = embed.header_bytes(root, manifest).decode("ascii")
            self.assertIn("gpu_shader_world_vertex_spv", header)
            self.assertIn("gpu_shader_final_fragment_dxil_size", header)
            self.assertTrue(header.endswith("#endif\n"))

            (root / embed.EXPECTED_NAMES[0]).write_bytes(b"changed")
            with self.assertRaisesRegex(embed.ShaderError, "digest mismatch"):
                embed.header_bytes(root, manifest)

    def test_manifest_rejects_missing_duplicate_and_unknown_entries(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = self.write_cohort(root)
            lines = manifest.read_text(encoding="ascii").splitlines()
            manifest.write_text("\n".join(lines[:-1]) + "\n", encoding="ascii")
            with self.assertRaisesRegex(embed.ShaderError, "membership mismatch"):
                embed.load_manifest(manifest)
            manifest.write_text("\n".join((*lines, lines[0])) + "\n", encoding="ascii")
            with self.assertRaisesRegex(embed.ShaderError, "duplicate"):
                embed.load_manifest(manifest)
            manifest.write_text(lines[0].replace("final_fragment", "unknown") + "\n",
                                encoding="ascii")
            with self.assertRaisesRegex(embed.ShaderError, "membership mismatch"):
                embed.load_manifest(manifest)

    def test_toolchain_lock_pins_governed_upstreams_and_selected_files(self) -> None:
        lock_path = CLIENT_ROOT / "shaders/toolchain.lock.json"
        lock = toolchain.load_lock(lock_path)
        dxc = lock["dxc"]
        spirv_cross = lock["spirv_cross"]
        self.assertEqual(dxc["tag"], "v1.9.2607")
        self.assertEqual(
            set(dxc["files"]),
            {
                "bin/dxc",
                "lib/libdxcompiler.so",
                "lib/libdxil.so",
                "LICENCE-MIT.txt",
                "LICENSE-LLVM.txt",
                "LICENSE-MS.txt",
            },
        )
        self.assertEqual(
            spirv_cross["commit"],
            "9c3c8e2cefdd8194b193bb8ed2fdff4d5527e382",
        )
        self.assertEqual(len(json.loads(lock_path.read_text())["dxc"]["sha256"]), 64)

    def test_cmake_generates_into_the_binary_tree_or_accepts_one_cohort(self) -> None:
        cmake = (CLIENT_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertNotIn("${CMAKE_CURRENT_SOURCE_DIR}/shaders/generated", cmake)
        self.assertIn("${CMAKE_CURRENT_BINARY_DIR}/shaders/generated", cmake)
        self.assertIn("add_custom_target(atrinik-gpu-shaders", cmake)
        self.assertIn("ATRINIK_GPU_SHADER_DIRECTORY", cmake)


if __name__ == "__main__":
    unittest.main()
