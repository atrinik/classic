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
IMAGE = {"Id": IMAGE_ID, "Os": "linux", "Architecture": "amd64", "Created": "today",
         "Config": {"User": "atrinik", "Entrypoint": ["atrinik-server-entrypoint"], "Volumes": None,
                    "Labels": {"org.opencontainers.image.revision": REVISION,
                               "org.opencontainers.image.version": "0.0.0"}}}
BENCHMARK = "\n".join("ATRINIK_CONTENT_BENCHMARK\t" + line for line in (
    "format\t1", "mode\toffline-authored-content", "iterations\t1", f"map\t{q.MAP}\t0\t10\t0\t20\t30"))


class DockerFixture:
    def __init__(self, fault=None):
        self.fault = fault
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
            output = json.dumps([IMAGE])
        elif args[1] == "create":
            cid = str(len(self.containers) + 1) * 64
            label = args[args.index("--label") + 1].split("=", 1)[1]
            self.containers[cid] = {
                "Id": cid, "Image": IMAGE_ID, "Config": {"User": "atrinik", "Labels": {q.OWNER_LABEL: label}},
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
            c["logs"] = BENCHMARK if c["benchmark"] else q.READY + f"\nBuild identity: revision={REVISION} dirty=false"
            if c["benchmark"] and self.fault == "missing-benchmark":
                c["logs"] = "startup only"
            if not c["benchmark"] and self.fault == "source-identity":
                c["logs"] = q.READY + "\nBuild identity: revision=unknown dirty=unknown"
            if not c["benchmark"] and self.fault == "early-exit":
                c["State"]["Running"] = False
            if not c["benchmark"] and self.fault == "stale-readiness" and self.starts == 3:
                c["logs"] = ""
        elif args[1] == "logs":
            output = self.containers[args[-1]]["logs"]
        elif args[1] == "exec":
            if args[3] == "cat":
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
        q.validate_image(IMAGE, IMAGE_ID, REVISION)
        for field, value in (("Id", "sha256:" + "c" * 64), ("Architecture", "arm64")):
            image = copy.deepcopy(IMAGE)
            image[field] = value
            with self.assertRaises(q.QualificationError):
                q.validate_image(image, IMAGE_ID, REVISION)
        for field, value in (("User", "root"), ("Entrypoint", ["sh"]), ("Volumes", {"/host": {}}),
                             ("Labels", {"org.opencontainers.image.revision": "c" * 40})):
            image = copy.deepcopy(IMAGE)
            image["Config"][field] = value
            with self.assertRaises(q.QualificationError):
                q.validate_image(image, IMAGE_ID, REVISION)

    def test_benchmark_requires_complete_exact_sample(self):
        self.assertEqual(q.validate_benchmark(BENCHMARK)["map"], q.MAP)
        for output in ("", BENCHMARK + "\n" + BENCHMARK, BENCHMARK.replace(q.MAP, "/other"),
                       BENCHMARK.replace("\t0\t10", "\t1\t10"), BENCHMARK.replace("\t30", "\tNaN"),
                       BENCHMARK.replace("format\t1", "format\t2")):
            with self.subTest(output=output), self.assertRaises(q.QualificationError):
                q.validate_benchmark(output)

    def exercise(self, fault=None):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        output = Path(temporary.name) / "evidence"
        fixture = DockerFixture(fault)
        qualifier = q.Qualification(IMAGE_ID, REVISION, output)
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
            qualifier = q.Qualification(IMAGE_ID, REVISION, Path(temporary))
            with patch.object(q.subprocess, "run", side_effect=fixture.run):
                cid = qualifier.create("fixture")
                fixture.containers[cid]["Config"]["Labels"][q.OWNER_LABEL] = "foreign"
                with self.assertRaisesRegex(q.QualificationError, "ownership"):
                    qualifier.cleanup()
        self.assertFalse(any(c[1] == "rm" for c in fixture.commands))
        self.assertFalse(qualifier.evidence["cleanup"][0]["removed"])

    def test_network_mount_and_image_drift_rejected(self):
        fixture = DockerFixture()
        with tempfile.TemporaryDirectory() as temporary:
            qualifier = q.Qualification(IMAGE_ID, REVISION, Path(temporary))
            with patch.object(q.subprocess, "run", side_effect=fixture.run):
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
