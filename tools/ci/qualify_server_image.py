#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright 2026 The Atrinik Project
"""Qualify one freshly loaded final server image, without publication or host mounts."""

import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
OWNER_LABEL = "org.atrinik.server-image-qualification"
MAP = "/shattered_islands/world_0_70"
READY = "Server ready. Waiting for connections"
SAVED = "Server saves complete; releasing resources."
DIGEST = re.compile(r"sha256:[0-9a-f]{64}")
MANIFEST_TYPES = {"application/vnd.oci.image.manifest.v1+json",
                  "application/vnd.docker.distribution.manifest.v2+json"}


class QualificationError(RuntimeError):
    pass


def require(condition, message):
    if not condition:
        raise QualificationError(message)


def command(*args, timeout=30):
    result = subprocess.run(args, capture_output=True, text=True, timeout=timeout, check=False)
    require(result.returncode == 0, f"{args[0]} {args[1]} failed: {result.stderr[-4000:]}")
    return result.stdout


def validate_descriptor(descriptor, manifest_digest):
    require(isinstance(descriptor, dict), "Expected decoded manifest descriptor object")
    require(isinstance(descriptor.get("mediaType"), str) and descriptor["mediaType"] in MANIFEST_TYPES,
            "Expected a single image manifest, not an index or artifact")
    require(descriptor.get("digest") == manifest_digest, "Manifest descriptor digest mismatch")
    require(type(descriptor.get("size")) is int and descriptor["size"] > 0, "Invalid manifest descriptor size")
    require("artifactType" not in descriptor, "Artifact descriptors are not runtime images")
    if "platform" in descriptor:
        platform = descriptor["platform"]
        require(isinstance(platform, dict) and platform.get("os") == "linux"
                and platform.get("architecture") == "amd64", "Wrong descriptor platform")


def validate_build(build, image_tag, revision):
    require(isinstance(image_tag, str) and re.fullmatch(r"classic-server-qualification:[1-9][0-9]*-[1-9][0-9]*", image_tag),
            "Expected the owned run/attempt qualification tag")
    require(isinstance(build, dict) and build.get("source_revision") == revision
            and build.get("image_tag") == image_tag, "Build evidence source or tag mismatch")
    for name in ("config_digest", "manifest_digest"):
        require(isinstance(build.get(name), str) and DIGEST.fullmatch(build[name]), f"Invalid expected {name}")
    require(build["config_digest"] != build["manifest_digest"], "Config and manifest identities must be distinct")
    metadata = build.get("metadata")
    require(isinstance(metadata, dict), "Expected BuildKit metadata object")
    require(metadata.get("containerimage.config.digest") == build["config_digest"],
            "Build config output differs from metadata")
    require(metadata.get("containerimage.digest") == build["manifest_digest"],
            "Build manifest output differs from metadata")
    validate_descriptor(metadata.get("containerimage.descriptor"), build["manifest_digest"])


def validate_image(image, build, revision):
    require(isinstance(image, dict), "Expected Docker image object")
    descriptor = image.get("Descriptor")
    if descriptor is None:
        require(image.get("Id") == build["config_digest"], "Legacy Docker ID differs from expected config digest")
        identity_kind = "legacy-config"
    else:
        validate_descriptor(descriptor, build["manifest_digest"])
        expected = build["metadata"]["containerimage.descriptor"]
        require(all(descriptor[key] == expected[key] for key in ("digest", "mediaType", "size")),
                "Docker manifest descriptor differs from BuildKit metadata")
        require(image.get("Id") == build["manifest_digest"], "Docker ID differs from expected manifest digest")
        identity_kind = "manifest"
    require(image.get("Architecture") == "amd64" and image.get("Os") == "linux", "Wrong image platform")
    config = image.get("Config")
    require(isinstance(config, dict), "Expected Docker image configuration")
    require(config.get("User") == "atrinik", "Final image must default to the nonroot atrinik user")
    require(config.get("Entrypoint") == ["atrinik-server-entrypoint"], "Wrong final image entrypoint")
    require(not config.get("Volumes"), "Unexpected image volumes")
    labels = config.get("Labels") or {}
    require(labels.get("org.opencontainers.image.revision") == revision, "Image source revision mismatch")
    require(labels.get("org.opencontainers.image.version") == "0.0.0", "Qualification version must be 0.0.0")
    return identity_kind


def validate_container(container, container_id, image_id, owner):
    require(container.get("Id") == container_id, "Container ID changed")
    require(container.get("Image") == image_id, "Container image changed")
    config = container["Config"]
    require((config.get("Labels") or {}).get(OWNER_LABEL) == owner, "Container ownership changed")
    require(config.get("User") == "atrinik", "Container user changed")
    host = container["HostConfig"]
    require(host.get("NetworkMode") == "none" and not host.get("PortBindings"), "Container network is not isolated")
    require(not container.get("Mounts") and not host.get("Privileged"), "Unexpected container mounts or privilege")
    require("ALL" in host.get("CapDrop", []) and "no-new-privileges" in host.get("SecurityOpt", []),
            "Container security restrictions changed")


def validate_benchmark(output):
    rows = [line.split("\t")[1:] for line in output.splitlines()
            if line.startswith("ATRINIK_CONTENT_BENCHMARK\t")]
    for expected in (["format", "1"], ["mode", "offline-authored-content"], ["iterations", "1"]):
        require(rows.count(expected) == 1, f"Missing or duplicate benchmark evidence: {expected[0]}")
    maps = [row for row in rows if row[0] == "map"]
    require(len(maps) == 1, "Expected exactly one completed map sample")
    row = maps[0]
    require(len(row) == 7 and row[1:3] == [MAP, "0"] and all(re.fullmatch(r"[0-9]+", x) for x in row[3:]),
            "Malformed or wrong map load/swap/reload evidence")
    return {"map": MAP, "sample": 0, "load_us": int(row[3]), "warm_us": int(row[4]),
            "swap_us": int(row[5]), "reload_us": int(row[6])}


class Qualification:
    def __init__(self, image_tag, build, revision, output):
        self.image_tag = image_tag
        self.build = build
        self.image_id = None
        self.revision = revision
        self.output = output
        self.owner = uuid.uuid4().hex
        self.containers = []
        self.evidence = {"schema_version": 1, "source_revision": revision, "image_tag": image_tag,
                         "owner": self.owner, "result": "failed", "containers": [], "cleanup": []}

    def write(self, name, value):
        (self.output / name).write_text(json.dumps(value, indent=2) + "\n")

    def resolve_image(self):
        validate_build(self.build, self.image_tag, self.revision)
        self.evidence.update(expected_config_digest=self.build["config_digest"],
                             expected_manifest_digest=self.build["manifest_digest"])
        images = json.loads(command("docker", "image", "inspect", self.image_tag))
        require(isinstance(images, list) and len(images) == 1, "Expected one image for the owned tag")
        image = images[0]
        identity_kind = validate_image(image, self.build, self.revision)
        self.image_id = image["Id"]
        self.evidence.update(image_id=self.image_id, image_identity_kind=identity_kind)
        self.write("image.json", {key: image[key] for key in
                                 ("Id", "Descriptor", "Architecture", "Os", "Created", "Config") if key in image})

    def inspect(self, container_id):
        image = json.loads(command("docker", "image", "inspect", self.image_id))[0]
        validate_image(image, self.build, self.revision)
        require(image["Id"] == self.image_id, "Resolved Docker image ID changed")
        container = json.loads(command("docker", "container", "inspect", container_id))[0]
        validate_container(container, container_id, self.image_id, self.owner)
        return container

    def create(self, purpose, *args):
        require(self.image_id is not None, "Resolve and validate the built image before creating containers")
        container_id = command(
            "docker", "create", "--name", f"classic-qualification-{self.owner}-{purpose}",
            "--label", f"{OWNER_LABEL}={self.owner}", "--network", "none", "--cap-drop", "ALL",
            "--security-opt", "no-new-privileges", "--cpus", "2", "--memory", "4g", "--pids-limit", "256",
            "--log-driver", "json-file", "--log-opt", "max-size=4m", "--log-opt", "max-file=1",
            "--env", "ATRINIK_NETWORK_STACK=ipv4=127.0.0.1", "--env", "ATRINIK_PORT_MAPPING=off",
            self.image_id, "--stun_server=off", "--metaserver_publish_origin=http://127.0.0.1:9",
            "--metaserver_rendezvous_origin=http://127.0.0.1:9/v1/classic", *args).strip()
        require(re.fullmatch(r"[0-9a-f]{64}", container_id), "Invalid created container ID")
        self.containers.append(container_id)
        self.inspect(container_id)
        self.evidence["containers"].append({"id": container_id, "purpose": purpose})
        return container_id

    def start(self, container_id):
        self.inspect(container_id)
        command("docker", "start", container_id)
        return self.inspect(container_id)["State"]["StartedAt"]

    def logs(self, container_id, name, since=None):
        self.inspect(container_id)
        args = ["docker", "logs", "--tail", "2000"]
        if since:
            args += ["--since", since]
        # Docker separates container stderr; retain both streams as evidence.
        result = subprocess.run([*args, container_id], capture_output=True, text=True, timeout=30)
        require(result.returncode == 0, "Cannot collect container logs")
        output = result.stdout + result.stderr
        (self.output / name).write_text(output[-1024 * 1024:])
        require(len(output) < 1024 * 1024 and len(output.splitlines()) < 2000,
                "Container logs exceed qualification evidence bounds")
        return output

    def exec(self, container_id, *args, timeout=60):
        self.inspect(container_id)
        return command("docker", "exec", container_id, *args, timeout=timeout)

    def stopped(self, container_id):
        state = self.inspect(container_id)["State"]
        require(not state["Running"] and state["Status"] == "exited" and state["ExitCode"] == 0
                and not state.get("OOMKilled") and not state.get("Error"), "Container did not exit cleanly")
        return {key: state[key] for key in ("Status", "ExitCode", "OOMKilled", "StartedAt", "FinishedAt")}

    def benchmark(self):
        container_id = self.create("content", f"--content_benchmark={MAP}", "--content_benchmark_iterations=1")
        self.start(container_id)
        deadline = time.monotonic() + 120
        while self.inspect(container_id)["State"]["Running"]:
            require(time.monotonic() < deadline, "Content behavior timed out")
            time.sleep(1)
        output = self.logs(container_id, "content.log")
        self.evidence["benchmark_exit"] = self.stopped(container_id)
        self.evidence["content_behavior"] = validate_benchmark(output)

    def lifecycle(self):
        container_id = self.create("lifecycle")
        self.evidence["lifecycle"] = []
        for iteration in range(2):
            started = self.start(container_id)
            deadline = time.monotonic() + 120
            while True:
                state = self.inspect(container_id)["State"]
                require(state["Running"], "Server exited before readiness")
                output = self.logs(container_id, f"runtime-{iteration}.log", started)
                if READY in output and state.get("Health", {}).get("Status") == "healthy":
                    break
                require(time.monotonic() < deadline, "Server readiness/health timed out")
                time.sleep(2)
            # Require a heartbeat from this start, not a healthy retained file.
            heartbeat = self.exec(container_id, "cat", "data/tmp/server-heartbeat").strip()
            require(heartbeat.isdecimal(), "Invalid heartbeat")
            require(int(heartbeat) > int(datetime.fromisoformat(started.replace("Z", "+00:00")).timestamp()),
                    "Heartbeat predates this server start")
            require(self.exec(container_id, "id", "-u").strip() == "10001", "Runtime UID is not 10001")
            if iteration == 0:
                version = self.exec(container_id, "./atrinik-server", "--version")
                (self.output / "server-version.txt").write_text(version)
                require(f"Build identity: revision={self.revision} dirty=false" in version
                        and "This is Atrinik v0.0.0" in version, "Packaged executable source identity differs")
                self.exec(container_id, "python3", "/usr/local/share/atrinik/runtime-provider.py", "verify-runtime",
                          "--server", "/opt/atrinik/server")
                # Hash only public packaged inputs; never copy generated keys or account state.
                hashes = self.exec(container_id, "sha256sum", "atrinik-server", "libplugin_arena.so",
                                   "libplugin_python.so", "/usr/local/share/atrinik/runtime-provider.lock.json",
                                   f"/opt/atrinik/maps{MAP}")
                (self.output / "runtime-hashes.txt").write_text(hashes)
                self.evidence["provider_verified"] = True
            self.inspect(container_id)
            command("docker", "stop", "--time", "30", container_id, timeout=45)
            state = self.stopped(container_id)
            output = self.logs(container_id, f"runtime-{iteration}.log", started)
            require(READY in output and SAVED in output, "Missing readiness or checked persistence shutdown evidence")
            self.evidence["lifecycle"].append({"iteration": iteration, "heartbeat": int(heartbeat), **state})

    def cleanup(self):
        errors = []
        for container_id in self.containers:
            try:
                self.logs(container_id, f"final-{container_id[:12]}.log")
            except (QualificationError, OSError, subprocess.SubprocessError) as error:
                errors.append(str(error))
            try:
                self.inspect(container_id)  # Fresh ID/image/source/owner fence before removal.
                command("docker", "rm", "--force", container_id)
                remaining = command("docker", "ps", "--all", "--quiet", "--no-trunc", "--filter", f"id={container_id}")
                require(not remaining.strip(), "Container remains after cleanup")
                self.evidence["cleanup"].append({"id": container_id, "removed": True})
            except (QualificationError, OSError, subprocess.SubprocessError) as error:
                errors.append(str(error))
                self.evidence["cleanup"].append({"id": container_id, "removed": False, "error": str(error)})
        require(not errors, "; ".join(errors))

    def run(self):
        self.output.mkdir(parents=True, exist_ok=False)
        try:
            require(re.fullmatch(r"[0-9a-f]{40}", self.revision), "Expected full source commit")
            require(command("git", "-C", str(ROOT), "rev-parse", "HEAD").strip() == self.revision, "Source HEAD differs")
            require(not command("git", "-C", str(ROOT), "status", "--porcelain", "--untracked-files=no").strip(),
                    "Tracked source is dirty")
            self.resolve_image()
            self.evidence["source_inputs"] = {
                name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
                for name in ("server/Dockerfile", "server/docker/runtime-provider.lock.json", "dependencies.bundle.json")}
            self.write("dependency-bundle.json", json.loads((ROOT / "dependencies.bundle.json").read_text()))
            self.evidence["sdk"] = (ROOT / "server/Dockerfile").read_text().split("FROM ", 1)[1].split()[0]
            self.benchmark()
            self.lifecycle()
            self.evidence["result"] = "passed"
        except (QualificationError, OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
            self.evidence["error"] = str(error)
            raise
        finally:
            try:
                self.cleanup()
            except (QualificationError, OSError, subprocess.SubprocessError) as error:
                self.evidence["result"] = "failed"
                self.evidence["cleanup_error"] = str(error)
                raise
            finally:
                self.write("qualification.json", self.evidence)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image-tag", required=True)
    parser.add_argument("--build-evidence", required=True, type=Path)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    build = json.loads(args.build_evidence.read_text())
    Qualification(args.image_tag, build, args.revision, args.output).run()


if __name__ == "__main__":
    main()
