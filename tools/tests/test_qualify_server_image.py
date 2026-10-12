# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright 2026 The Atrinik Project
"""Final-image qualification rejects incomplete behavior and foreign resources."""

import copy
from datetime import datetime, timedelta, timezone
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("qualify_server_image", ROOT / "tools/ci/qualify_server_image.py")
q = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(q)
REVISION = "a" * 40
IMAGE_ID = "sha256:" + "b" * 64
MANIFEST_DIGEST = "sha256:" + "c" * 64
IMAGE_TAG = "classic-server-qualification:12345-1"
DESCRIPTOR = {"mediaType": "application/vnd.oci.image.manifest.v1+json", "digest": MANIFEST_DIGEST, "size": 1234}
BUILD = {"source_revision": REVISION, "image_tag": IMAGE_TAG, "config_digest": IMAGE_ID,
         "manifest_digest": MANIFEST_DIGEST, "metadata": {
             "containerimage.config.digest": IMAGE_ID, "containerimage.digest": MANIFEST_DIGEST,
             "containerimage.descriptor": DESCRIPTOR}}
IMAGE = {"Id": IMAGE_ID, "Os": "linux", "Architecture": "amd64", "Created": "today",
         "Config": {"User": "atrinik", "Entrypoint": ["atrinik-server-entrypoint"], "Volumes": None,
                    "Labels": {"org.opencontainers.image.revision": REVISION,
                               "org.opencontainers.image.version": "0.0.0"}}}
BENCHMARK = "\n".join("ATRINIK_CONTENT_BENCHMARK\t" + line for line in (
    "format\t1", "mode\toffline-authored-content", "iterations\t1", f"map\t{q.MAP}\t0\t10\t0\t20\t30"))


class DockerFixture:
    def __init__(self, fault=None, modern=False):
        self.fault = fault
        self.image = copy.deepcopy(IMAGE)
        if modern:
            self.image.update(Id=MANIFEST_DIGEST, Descriptor=copy.deepcopy(DESCRIPTOR))
        self.containers = {}
        self.commands = []
        self.starts = 0

    def run(self, args, **kwargs):
        args = list(args)
        self.commands.append(args)
        output = ""
        code = 0
        if args[0] == "git":
            output = REVISION if "rev-parse" in args else ""
        elif args[1:3] == ["image", "inspect"]:
            # On modern stores only the tag and manifest ID are inspectable.
            if args[-1] not in (IMAGE_TAG, self.image["Id"]):
                code = 1
            else:
                output = json.dumps([self.image])
        elif args[1] == "create":
            cid = str(len(self.containers) + 1) * 64
            label = args[args.index("--label") + 1].split("=", 1)[1]
            self.containers[cid] = {
                "Id": cid, "Image": self.image["Id"], "Config": {"User": "atrinik", "Labels": {q.OWNER_LABEL: label}},
                "Mounts": [], "HostConfig": {"NetworkMode": "none", "PortBindings": {},
                    "Privileged": False, "CapDrop": ["ALL"], "SecurityOpt": ["no-new-privileges"]},
                "State": {"Running": False, "Status": "created", "ExitCode": 0, "OOMKilled": False,
                          "StartedAt": "", "FinishedAt": ""},
                "benchmark": any(x.startswith("--content_benchmark=") for x in args), "logs": ""}
            output = cid
        elif args[1:3] == ["container", "inspect"]:
            output = json.dumps([self.containers[args[-1]]])
        elif args[1] == "start":
            c = self.containers[args[-1]]
            self.starts += 1
            c["State"].update(Running=not c["benchmark"], Status="exited" if c["benchmark"] else "running",
                StartedAt=(datetime.now(timezone.utc) - timedelta(seconds=2)).isoformat(), Health={"Status": "healthy"})
            c["logs"] = BENCHMARK if c["benchmark"] else q.READY
            if c["benchmark"] and self.fault == "missing-benchmark":
                c["logs"] = "startup only"
            if not c["benchmark"] and self.fault == "early-exit":
                c["State"]["Running"] = False
            if not c["benchmark"] and self.fault == "stale-readiness" and self.starts == 3:
                c["logs"] = ""
        elif args[1] == "logs":
            output = self.containers[args[-1]]["logs"]
        elif args[1] == "exec":
            if args[3] == "./atrinik-server":
                revision = "unknown" if self.fault == "source-identity" else REVISION
                output = f"This is Atrinik v0.0.0\nBuild identity: revision={revision} dirty=false\n"
            elif args[3] == "cat":
                output = "1" if self.fault == "stale-heartbeat" else str(int(datetime.now(timezone.utc).timestamp()))
            elif args[3] == "id":
                output = "0" if self.fault == "root" else "10001"
            elif args[3] == "python3" and self.fault == "provider":
                code = 1
            elif args[3] == "sha256sum":
                output = "fixture packaged hashes"
        elif args[1] == "stop":
            c = self.containers[args[-1]]
            c["State"].update(Running=False, Status="exited", ExitCode=137 if self.fault == "killed" else 0)
            if self.fault != "missing-save":
                c["logs"] += "\n" + q.SAVED
        elif args[1] == "rm":
            del self.containers[args[-1]]
        elif args[1] != "ps":
            raise AssertionError(args)
        return subprocess.CompletedProcess(args, code, output, "fixture error" if code else "")


class QualificationTests(unittest.TestCase):
    def test_image_identity_platform_user_entrypoint_and_volumes(self):
        q.validate_image(IMAGE, BUILD, REVISION)
        for field, value in (("Id", "sha256:" + "c" * 64), ("Architecture", "arm64")):
            image = copy.deepcopy(IMAGE)
            image[field] = value
            with self.assertRaises(q.QualificationError):
                q.validate_image(image, BUILD, REVISION)
        for field, value in (("User", "root"), ("Entrypoint", ["sh"]), ("Volumes", {"/host": {}}),
                             ("Labels", {"org.opencontainers.image.revision": "c" * 40})):
            image = copy.deepcopy(IMAGE)
            image["Config"][field] = value
            with self.assertRaises(q.QualificationError):
                q.validate_image(image, BUILD, REVISION)

    def test_supported_image_identity_shapes_are_distinct(self):
        q.validate_build(BUILD, IMAGE_TAG, REVISION)
        for null_descriptor in (False, True):
            image = copy.deepcopy(IMAGE)
            if null_descriptor:
                image["Descriptor"] = None
            self.assertEqual(q.validate_image(image, BUILD, REVISION), "legacy-config")
        for media_type in q.MANIFEST_TYPES:
            with self.subTest(media_type=media_type):
                build = copy.deepcopy(BUILD)
                build["metadata"]["containerimage.descriptor"]["mediaType"] = media_type
                image = copy.deepcopy(IMAGE)
                image.update(Id=MANIFEST_DIGEST, Descriptor=copy.deepcopy(build["metadata"]["containerimage.descriptor"]))
                self.assertEqual(q.validate_image(image, build, REVISION), "manifest")
                image["Descriptor"]["platform"] = {"os": "linux", "architecture": "amd64"}
                self.assertEqual(q.validate_image(image, build, REVISION), "manifest")

    def test_build_identity_requires_matching_outputs_source_tag_and_descriptor(self):
        changes = [
            ("config_digest", "sha256:" + "d" * 64), ("manifest_digest", "sha256:" + "d" * 64),
            ("config_digest", "not-a-digest"), ("manifest_digest", None),
            ("source_revision", "d" * 40), ("image_tag", "classic-server-qualification:12345-2"),
            ("metadata", None),
        ]
        for key, value in changes:
            with self.subTest(key=key, value=value):
                build = copy.deepcopy(BUILD)
                build[key] = value
                with self.assertRaises(q.QualificationError):
                    q.validate_build(build, IMAGE_TAG, REVISION)
        for descriptor in (None, "base64-not-decoded", {}, {**DESCRIPTOR, "size": True},
                           {**DESCRIPTOR, "size": 0}, {**DESCRIPTOR, "size": -1},
                           {**DESCRIPTOR, "mediaType": "application/vnd.oci.image.index.v1+json"},
                           {**DESCRIPTOR, "mediaType": []},
                           {**DESCRIPTOR, "artifactType": "application/example"},
                           {**DESCRIPTOR, "platform": {"os": "linux", "architecture": "arm64"}}):
            build = copy.deepcopy(BUILD)
            build["metadata"]["containerimage.descriptor"] = descriptor
            with self.subTest(descriptor=descriptor), self.assertRaises(q.QualificationError):
                q.validate_build(build, IMAGE_TAG, REVISION)
        for tag in ("ghcr.io/atrinik/classic-server:development", "classic-server-qualification:12345-0", "sha256:abcd"):
            with self.subTest(tag=tag), self.assertRaises(q.QualificationError):
                q.validate_build(BUILD, tag, REVISION)

    def test_modern_rewritten_unknown_index_and_mixed_identities_fail_closed(self):
        modern = copy.deepcopy(IMAGE)
        modern.update(Id=MANIFEST_DIGEST, Descriptor=copy.deepcopy(DESCRIPTOR))
        variants = []
        for value in (None, False, {}, [], {**DESCRIPTOR, "digest": "sha256:" + "d" * 64},
                      {**DESCRIPTOR, "size": DESCRIPTOR["size"] + 1}, {**DESCRIPTOR, "size": True},
                      {**DESCRIPTOR, "mediaType": "application/vnd.docker.distribution.manifest.v2+json"},
                      {**DESCRIPTOR, "mediaType": "application/vnd.oci.image.index.v1+json"},
                      {**DESCRIPTOR, "platform": {"os": "windows", "architecture": "amd64"}}):
            image = copy.deepcopy(modern)
            image["Descriptor"] = value
            variants.append(image)
        for image_id in (IMAGE_ID, "sha256:" + "d" * 64, "not-a-digest"):
            image = copy.deepcopy(modern)
            image["Id"] = image_id
            variants.append(image)
        absent = copy.deepcopy(modern)
        del absent["Descriptor"]
        variants.append(absent)
        for image in variants:
            with self.subTest(image=image), self.assertRaises(q.QualificationError):
                q.validate_image(image, BUILD, REVISION)

    def test_modern_runtime_uses_manifest_id_without_inventing_config_observation(self):
        fixture, evidence = self.exercise(modern=True)
        self.assertEqual(evidence["expected_config_digest"], IMAGE_ID)
        self.assertEqual(evidence["expected_manifest_digest"], MANIFEST_DIGEST)
        self.assertEqual(evidence["image_id"], MANIFEST_DIGEST)
        self.assertEqual(evidence["image_identity_kind"], "manifest")
        inspections = [command[-1] for command in fixture.commands if command[1:3] == ["image", "inspect"]]
        self.assertEqual(inspections[0], IMAGE_TAG)
        self.assertTrue(all(value == MANIFEST_DIGEST for value in inspections[1:]))
        for command in fixture.commands:
            if command[1] == "create":
                self.assertIn(MANIFEST_DIGEST, command)
                self.assertNotIn(IMAGE_TAG, command)
                self.assertNotIn(IMAGE_ID, command)

    def test_rewritten_tag_fails_before_any_container_creation(self):
        fixture = DockerFixture(modern=True)
        fixture.image["Id"] = "sha256:" + "d" * 64
        with tempfile.TemporaryDirectory() as temporary:
            qualifier = q.Qualification(IMAGE_TAG, copy.deepcopy(BUILD), REVISION, Path(temporary) / "evidence")
            with patch.object(q.subprocess, "run", side_effect=fixture.run):
                with self.assertRaises(q.QualificationError):
                    qualifier.run()
            evidence = json.loads((qualifier.output / "qualification.json").read_text())
            self.assertEqual(evidence["result"], "failed")
            self.assertNotIn("image_id", evidence)
        self.assertFalse(any(command[1] == "create" for command in fixture.commands))

    def test_benchmark_requires_complete_exact_sample(self):
        self.assertEqual(q.validate_benchmark(BENCHMARK)["map"], q.MAP)
        for output in ("", BENCHMARK + "\n" + BENCHMARK, BENCHMARK.replace(q.MAP, "/other"),
                       BENCHMARK.replace("\t0\t10", "\t1\t10"), BENCHMARK.replace("\t30", "\tNaN"),
                       BENCHMARK.replace("format\t1", "format\t2")):
            with self.subTest(output=output), self.assertRaises(q.QualificationError):
                q.validate_benchmark(output)

    def exercise(self, fault=None, modern=False):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        output = Path(temporary.name) / "evidence"
        fixture = DockerFixture(fault, modern)
        qualifier = q.Qualification(IMAGE_TAG, copy.deepcopy(BUILD), REVISION, output)
        # Advancing test clock makes missing readiness hit a bounded deadline.
        with patch.object(q.subprocess, "run", side_effect=fixture.run), \
                patch.object(q.time, "sleep"), patch.object(q.time, "monotonic", side_effect=range(0, 10000, 61)):
            if fault:
                with self.assertRaises(q.QualificationError):
                    qualifier.run()
            else:
                qualifier.run()
        evidence = json.loads((output / "qualification.json").read_text())
        self.assertEqual(evidence["result"], "failed" if fault else "passed")
        self.assertFalse(fixture.containers)
        self.assertTrue(all(row["removed"] for row in evidence["cleanup"]))
        return fixture, evidence

    def test_full_behavior_two_starts_and_checked_cleanup(self):
        fixture, evidence = self.exercise()
        self.assertEqual(len(evidence["lifecycle"]), 2)
        self.assertEqual(fixture.starts, 3)
        for command in fixture.commands:
            if command[1] == "create":
                self.assertNotIn("--volume", command)
                self.assertNotIn("--publish", command)
                self.assertIn("none", command)
        self.assertGreaterEqual(sum("--since" in c for c in fixture.commands), 4)

    def test_behavior_provider_readiness_heartbeat_and_shutdown_fail_closed(self):
        for fault in ("missing-benchmark", "early-exit", "stale-readiness", "stale-heartbeat", "root",
                      "provider", "killed", "missing-save", "source-identity"):
            with self.subTest(fault=fault):
                self.exercise(fault)

    def test_foreign_container_is_never_removed(self):
        fixture = DockerFixture()
        with tempfile.TemporaryDirectory() as temporary:
            qualifier = q.Qualification(IMAGE_TAG, copy.deepcopy(BUILD), REVISION, Path(temporary))
            with patch.object(q.subprocess, "run", side_effect=fixture.run):
                qualifier.resolve_image()
                cid = qualifier.create("fixture")
                fixture.containers[cid]["Config"]["Labels"][q.OWNER_LABEL] = "foreign"
                with self.assertRaisesRegex(q.QualificationError, "ownership"):
                    qualifier.cleanup()
        self.assertFalse(any(c[1] == "rm" for c in fixture.commands))
        self.assertFalse(qualifier.evidence["cleanup"][0]["removed"])

    def test_modern_descriptor_drift_blocks_cleanup(self):
        fixture = DockerFixture(modern=True)
        with tempfile.TemporaryDirectory() as temporary:
            qualifier = q.Qualification(IMAGE_TAG, copy.deepcopy(BUILD), REVISION, Path(temporary))
            with patch.object(q.subprocess, "run", side_effect=fixture.run):
                qualifier.resolve_image()
                qualifier.create("fixture")
                fixture.image["Descriptor"]["size"] += 1
                with self.assertRaisesRegex(q.QualificationError, "descriptor differs"):
                    qualifier.cleanup()
        self.assertFalse(any(command[1] == "rm" for command in fixture.commands))
        self.assertFalse(qualifier.evidence["cleanup"][0]["removed"])

    def test_network_mount_and_image_drift_rejected(self):
        fixture = DockerFixture()
        with tempfile.TemporaryDirectory() as temporary:
            qualifier = q.Qualification(IMAGE_TAG, copy.deepcopy(BUILD), REVISION, Path(temporary))
            with patch.object(q.subprocess, "run", side_effect=fixture.run):
                qualifier.resolve_image()
                cid = qualifier.create("fixture")
                original = copy.deepcopy(fixture.containers[cid])
                for field, value in (("Image", "sha256:" + "c" * 64), ("Mounts", [{"Source": "/host"}])):
                    fixture.containers[cid] = copy.deepcopy(original)
                    fixture.containers[cid][field] = value
                    with self.assertRaises(q.QualificationError):
                        qualifier.inspect(cid)
                fixture.containers[cid] = copy.deepcopy(original)
                fixture.containers[cid]["HostConfig"]["NetworkMode"] = "host"
                with self.assertRaises(q.QualificationError):
                    qualifier.inspect(cid)


if __name__ == "__main__":
    unittest.main()
