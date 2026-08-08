from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).resolve().parents[1] / "release" / "locked_inputs.py"
SPEC = importlib.util.spec_from_file_location("locked_inputs", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
locked_inputs = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(locked_inputs)


class LockedInputsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        for component, name in (("client", "sound"), ("server", "content")):
            path = self.root / component / "dependencies.lock.json"
            path.parent.mkdir(parents=True)
            path.write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "dependencies": [
                            {
                                "name": name,
                                "repository": f"atrinik/{name}",
                                "tag": "v1.0.0",
                                "commit": "a" * 40,
                                "url": f"https://github.com/atrinik/{name}/release.tar.gz",
                                "sha256": "b" * 64,
                                "destination": name,
                                "strip_components": 1,
                            }
                        ],
                    }
                ),
                encoding="utf-8",
            )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_inputs_record_coordinates_and_affected_artifacts(self) -> None:
        inputs = locked_inputs.load_locked_inputs("5.6.0", self.root)
        self.assertEqual([record["name"] for record in inputs], ["content", "sound"])
        content = inputs[0]
        self.assertIn(
            "atrinik-classic-server-5.6.0-windows-x86_64.zip",
            content["affects"],
        )
        self.assertIn("ghcr.io/atrinik/classic-server:5.6.0", content["affects"])

    def test_server_image_labels_include_every_provenance_coordinate(self) -> None:
        inputs = locked_inputs.load_locked_inputs("5.6.0", self.root)
        labels = locked_inputs.image_labels(inputs, "server")
        for field in ("repository", "tag", "commit", "url", "sha256"):
            self.assertTrue(
                any(label.startswith(f"org.atrinik.release.input.content.{field}=") for label in labels)
            )

    def test_expected_image_labels_bind_revision_version_and_inputs(self) -> None:
        inputs = locked_inputs.load_locked_inputs("5.6.0", self.root)
        labels = locked_inputs.expected_image_labels(
            inputs, "server", "5.6.0", "c" * 40
        )
        self.assertEqual(labels["org.opencontainers.image.revision"], "c" * 40)
        self.assertEqual(labels["org.opencontainers.image.version"], "5.6.0")
        self.assertEqual(
            labels["org.atrinik.release.input.content.commit"], "a" * 40
        )

    def test_image_attestations_require_slsa_and_spdx_structures(self) -> None:
        with mock.patch.object(
            locked_inputs,
            "inspect_attestation",
            side_effect=[
                {
                    "SLSA": {
                        "builder": {"id": "https://github.com/docker/build-push-action"},
                        "buildType": "https://mobyproject.org/buildkit@v1",
                        "materials": [],
                    }
                },
                {
                    "SPDX": {
                        "spdxVersion": "SPDX-2.3",
                        "SPDXID": "SPDXRef-DOCUMENT",
                        "packages": [],
                    }
                },
            ],
        ):
            locked_inputs.verify_attestations("example.invalid/image:5.6.0", "sha256:" + "a" * 64)

        with mock.patch.object(
            locked_inputs,
            "inspect_attestation",
            side_effect=[{"SLSA": {}}, {"SPDX": {}}],
        ):
            with self.assertRaises(locked_inputs.LockedInputError):
                locked_inputs.verify_attestations(
                    "example.invalid/image:5.6.0", "sha256:" + "a" * 64
                )

    def test_image_attestations_accept_slsa_v1_provenance(self) -> None:
        with mock.patch.object(
            locked_inputs,
            "inspect_attestation",
            side_effect=[
                {
                    "SLSA": {
                        "buildDefinition": {
                            "buildType": "https://mobyproject.org/buildkit@v1",
                            "resolvedDependencies": [],
                        },
                        "runDetails": {
                            "builder": {
                                "id": "https://github.com/atrinik/classic/actions/runs/1"
                            }
                        },
                    }
                },
                {
                    "SPDX": {
                        "spdxVersion": "SPDX-2.3",
                        "SPDXID": "SPDXRef-DOCUMENT",
                        "packages": [],
                    }
                },
            ],
        ):
            locked_inputs.verify_attestations(
                "example.invalid/image:5.6.1", "sha256:" + "a" * 64
            )

    def test_slsa_v1_requires_builder_and_resolved_dependencies(self) -> None:
        provenance = {
            "SLSA": {
                "buildDefinition": {
                    "buildType": "https://mobyproject.org/buildkit@v1"
                },
                "runDetails": {"builder": {"id": "builder"}},
            }
        }
        with mock.patch.object(
            locked_inputs,
            "inspect_attestation",
            side_effect=[provenance, {"SPDX": {}}],
        ):
            with self.assertRaises(locked_inputs.LockedInputError):
                locked_inputs.verify_attestations(
                    "example.invalid/image:5.6.1", "sha256:" + "a" * 64
                )


if __name__ == "__main__":
    unittest.main()
