#!/usr/bin/env python3
"""Load immutable runtime inputs consumed by classic release artifacts."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[2]
COMMIT_RE = re.compile(r"[0-9a-f]{40}")
DIGEST_RE = re.compile(r"[0-9a-f]{64}")
VERSION_RE = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+")
LOCKS = (
    ("client", Path("client/dependencies.lock.json")),
    ("server", Path("server/dependencies.lock.json")),
)


class LockedInputError(RuntimeError):
    """Raised when runtime dependency evidence is incomplete or malformed."""


def affected_artifacts(component: str, version: str) -> list[str]:
    if component == "client":
        return [f"atrinik-classic-client-{version}-windows-x86_64.zip"]
    if component == "server":
        return [
            f"atrinik-classic-server-{version}-windows-x86_64.zip",
            f"ghcr.io/atrinik/classic-server:{version}",
        ]
    raise LockedInputError(f"unsupported runtime-input component: {component}")


def load_locked_inputs(version: str, root: Path = ROOT) -> list[dict[str, object]]:
    if VERSION_RE.fullmatch(version) is None:
        raise LockedInputError("release input version must be MAJOR.MINOR.PATCH")
    records: list[dict[str, object]] = []
    names: set[str] = set()
    for component, relative_path in LOCKS:
        value = json.loads((root / relative_path).read_text(encoding="utf-8"))
        if value.get("schema_version") != 1 or not isinstance(
            value.get("dependencies"), list
        ):
            raise LockedInputError(f"unsupported dependency lock: {relative_path}")
        for dependency in value["dependencies"]:
            if not isinstance(dependency, dict):
                raise LockedInputError(f"invalid dependency in {relative_path}")
            required = {
                "name": dependency.get("name"),
                "repository": dependency.get("repository"),
                "tag": dependency.get("tag"),
                "commit": dependency.get("commit"),
                "url": dependency.get("url"),
                "sha256": dependency.get("sha256"),
                "destination": dependency.get("destination"),
                "strip_components": dependency.get("strip_components"),
            }
            if not all(isinstance(required[key], str) and required[key] for key in (
                "name",
                "repository",
                "tag",
                "commit",
                "url",
                "sha256",
                "destination",
            )) or not isinstance(required["strip_components"], int):
                raise LockedInputError(f"incomplete dependency in {relative_path}")
            name = str(required["name"])
            if name in names:
                raise LockedInputError(f"duplicate runtime input name: {name}")
            names.add(name)
            if COMMIT_RE.fullmatch(str(required["commit"])) is None:
                raise LockedInputError(f"invalid {name} commit")
            if DIGEST_RE.fullmatch(str(required["sha256"])) is None:
                raise LockedInputError(f"invalid {name} SHA-256")
            if not str(required["tag"]).startswith("v") or not str(
                required["url"]
            ).startswith("https://github.com/"):
                raise LockedInputError(f"invalid {name} release coordinates")
            records.append(
                {
                    **required,
                    "component": component,
                    "lock": relative_path.as_posix(),
                    "affects": affected_artifacts(component, version),
                }
            )
    return sorted(records, key=lambda record: str(record["name"]))


def image_labels(inputs: list[dict[str, object]], component: str) -> list[str]:
    labels = []
    for record in inputs:
        if record["component"] != component:
            continue
        prefix = f"org.atrinik.release.input.{record['name']}"
        for field in ("repository", "tag", "commit", "url", "sha256"):
            labels.append(f"{prefix}.{field}={record[field]}")
    return labels


def expected_image_labels(
    inputs: list[dict[str, object]], component: str, version: str, revision: str
) -> dict[str, str]:
    labels = {
        "org.opencontainers.image.licenses": "GPL-2.0-or-later",
        "org.opencontainers.image.revision": revision,
        "org.opencontainers.image.source": "https://github.com/atrinik/classic",
        "org.opencontainers.image.title": "Atrinik Classic Server",
        "org.opencontainers.image.version": version,
    }
    labels.update(line.split("=", 1) for line in image_labels(inputs, component))
    return labels


def inspect_attestation(image: str, digest: str, field: str) -> object:
    result = subprocess.run(
        [
            "docker",
            "buildx",
            "imagetools",
            "inspect",
            f"{image}@{digest}",
            "--format",
            f"{{{{json .{field}}}}}",
        ],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode:
        raise LockedInputError(
            f"cannot inspect image {field.lower()}: "
            + (result.stderr.strip() or result.stdout.strip())
        )
    try:
        value = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise LockedInputError(f"image {field.lower()} is not JSON") from error
    if value in (None, {}, []):
        raise LockedInputError(f"image has no {field.lower()} attestation")
    return value


def nested_objects(value: object) -> list[dict[str, object]]:
    objects: list[dict[str, object]] = []
    if isinstance(value, dict):
        objects.append(value)
        for child in value.values():
            objects.extend(nested_objects(child))
    elif isinstance(value, list):
        for child in value:
            objects.extend(nested_objects(child))
    return objects


def verify_attestations(image: str, digest: str) -> None:
    provenance = inspect_attestation(image, digest, "Provenance")
    if not any(
        isinstance(item.get("builder"), dict)
        and isinstance(item.get("buildType"), str)
        and bool(item["buildType"])
        and isinstance(item.get("materials"), list)
        for item in nested_objects(provenance)
    ):
        raise LockedInputError("image provenance is not a SLSA predicate")

    sbom = inspect_attestation(image, digest, "SBOM")
    if not any(
        isinstance(item.get("spdxVersion"), str)
        and str(item["spdxVersion"]).startswith("SPDX-2.")
        and item.get("SPDXID") == "SPDXRef-DOCUMENT"
        and isinstance(item.get("packages"), list)
        for item in nested_objects(sbom)
    ):
        raise LockedInputError("image SBOM is not an SPDX document")


def verify_image(
    image: str,
    inputs: list[dict[str, object]],
    component: str,
    version: str,
    revision: str,
    digest: str,
) -> None:
    result = subprocess.run(
        ["docker", "image", "inspect", f"{image}@{digest}"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode:
        raise LockedInputError(
            "cannot inspect existing image: "
            + (result.stderr.strip() or result.stdout.strip())
        )
    value = json.loads(result.stdout)
    if not isinstance(value, list) or len(value) != 1:
        raise LockedInputError("docker returned invalid image metadata")
    repo_digests = value[0].get("RepoDigests") if isinstance(value[0], dict) else None
    if not isinstance(repo_digests, list) or not any(
        isinstance(item, str) and item.endswith(f"@{digest}") for item in repo_digests
    ):
        raise LockedInputError("existing image does not match the audited digest")
    config = value[0].get("Config") if isinstance(value[0], dict) else None
    actual = config.get("Labels") if isinstance(config, dict) else None
    if not isinstance(actual, dict):
        raise LockedInputError("existing image has no labels")
    for key, expected in expected_image_labels(
        inputs, component, version, revision
    ).items():
        if actual.get(key) != expected:
            raise LockedInputError(f"existing image has the wrong {key} label")
    verify_attestations(image, digest)


def write_multiline_output(path: Path, name: str, lines: list[str]) -> None:
    delimiter = "ATRINIK_RELEASE_INPUTS"
    if any(delimiter in line or "\r" in line for line in lines):
        raise LockedInputError("unsafe release-input label")
    with path.open("a", encoding="utf-8") as stream:
        stream.write(f"{name}<<{delimiter}\n")
        stream.write("\n".join(lines) + "\n")
        stream.write(f"{delimiter}\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--component", choices=("client", "server"), required=True)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--github-output", type=Path)
    action.add_argument("--verify-image")
    parser.add_argument("--revision", default="")
    parser.add_argument("--digest", default="")
    arguments = parser.parse_args()
    try:
        inputs = load_locked_inputs(arguments.version)
        if arguments.github_output is not None:
            write_multiline_output(
                arguments.github_output,
                "labels",
                image_labels(inputs, arguments.component),
            )
        else:
            if COMMIT_RE.fullmatch(arguments.revision) is None:
                raise LockedInputError("--revision must be a full commit ID")
            if re.fullmatch(r"sha256:[0-9a-f]{64}", arguments.digest) is None:
                raise LockedInputError("--digest must be an immutable SHA-256")
            verify_image(
                arguments.verify_image,
                inputs,
                arguments.component,
                arguments.version,
                arguments.revision,
                arguments.digest,
            )
    except (OSError, ValueError, LockedInputError) as error:
        parser.exit(1, f"locked release inputs: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
