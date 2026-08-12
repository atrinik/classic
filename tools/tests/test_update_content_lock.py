from __future__ import annotations

from io import BytesIO
import hashlib
import importlib.util
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "update_content_lock", ROOT / "tools" / "release" / "update_content_lock.py"
)
assert SPEC is not None and SPEC.loader is not None
UPDATER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(UPDATER)


def manifest(version: str = "2.14.0", commit: str = "a" * 40) -> dict[str, object]:
    payload = b"content\n"
    digest = hashlib.sha256(payload).hexdigest()
    file_entry = {
        "path": "attribution/maps/COPYING",
        "sha256": digest,
        "size": len(payload),
    }
    return {
        "schema_version": 2,
        "target": "classic",
        "source": {
            "repository": "atrinik/content",
            "branch": "main",
            "commit": commit,
        },
        "release_version": version,
        "content_format": "classic-ads-v1",
        "artifact_format": "atrinik-classic-runtime-content-v1",
        "compatible_classic_releases": ">=5.10.1 <6.0.0",
        "consumers": ["classic/client", "classic/editor", "classic/server"],
        "replacement_ready": False,
        "replacement_toolkit_package": False,
        "license_files": [],
        "files": [file_entry],
    }


def runtime_archive(path: Path, value: dict[str, object], *, unsafe: str | None = None) -> None:
    root = f"atrinik-content-{value['release_version']}-classic-runtime"
    payload = b"content\n"
    value["license_files"] = list(value["files"])
    with tarfile.open(path, "w:gz") as archive:
        for name, data in (
            (f"{root}/attribution/maps/COPYING", payload),
            (f"{root}/manifest.json", json.dumps(value).encode()),
        ):
            info = tarfile.TarInfo(name)
            info.size = len(data)
            archive.addfile(info, BytesIO(data))
        if unsafe:
            info = tarfile.TarInfo(unsafe)
            info.type = tarfile.SYMTYPE
            info.linkname = "target"
            archive.addfile(info)


class FakeAPI(UPDATER.GitHubAPI):
    def __init__(self, pages: list[object] | None = None) -> None:
        self.pages = pages or []

    def get(self, endpoint: str) -> object:
        page = int(endpoint.rsplit("=", 1)[1])
        return self.pages[page - 1]


class UpdateContentLockTests(unittest.TestCase):
    def test_semantic_versions_are_canonical(self) -> None:
        self.assertEqual(UPDATER.semantic_version("v1.8.6"), (1, 8, 6))
        for invalid in ("1.8.6", "v1.08.6", "v1.8", "v1.8.6-rc.1"):
            with self.subTest(invalid=invalid), self.assertRaises(UPDATER.UpdateError):
                UPDATER.semantic_version(invalid)

    def test_release_discovery_is_paginated_and_bounded(self) -> None:
        first = [{"tag_name": f"v1.0.{index}"} for index in range(100)]
        api = FakeAPI([first, [{"tag_name": "v1.1.0"}]])
        self.assertEqual(len(api.releases("atrinik/content")), 101)
        with mock.patch.object(UPDATER, "MAX_RELEASE_PAGES", 2):
            with self.assertRaisesRegex(UPDATER.UpdateError, "pagination"):
                FakeAPI([first, first]).releases("atrinik/content")

    def test_release_discovery_rejects_malformed_pages(self) -> None:
        with self.assertRaisesRegex(UPDATER.UpdateError, "array"):
            FakeAPI([{}]).releases("atrinik/content")
        with self.assertRaisesRegex(UPDATER.UpdateError, "non-object"):
            FakeAPI([["bad"]]).releases("atrinik/content")

    def test_checksums_require_one_strict_runtime_entry(self) -> None:
        name = "atrinik-content-2.14.0-classic-runtime.tar.gz"
        digest = "a" * 64
        self.assertEqual(UPDATER.parse_checksums(f"{digest}  {name}\n".encode(), name), digest)
        for data in (
            b"",
            f"{digest} *{name}\n".encode(),
            f"{digest}  {name}\n{digest}  {name}\n".encode(),
            f"{digest.upper()}  {name}\n".encode(),
        ):
            with self.subTest(data=data), self.assertRaises(UPDATER.UpdateError):
                UPDATER.parse_checksums(data, name)

    def test_assets_must_be_unique_and_canonical(self) -> None:
        duplicate = {
            "assets": [
                {"name": "SHA256SUMS"},
                {"name": "SHA256SUMS"},
            ]
        }
        with self.assertRaisesRegex(UPDATER.UpdateError, "duplicate asset"):
            UPDATER.asset_map(duplicate)
        asset = {"browser_download_url": "https://example.invalid/file", "size": 10}
        with self.assertRaisesRegex(UPDATER.UpdateError, "canonical"):
            UPDATER.asset_url(asset, "https://github.com/file")

    def test_candidate_verifies_assets_checksums_archive_and_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            archive_path = Path(directory) / "source.tar.gz"
            runtime_archive(archive_path, manifest())
            archive_data = archive_path.read_bytes()
            digest = hashlib.sha256(archive_data).hexdigest()
            runtime_name = "atrinik-content-2.14.0-classic-runtime.tar.gz"
            base = "https://github.com/atrinik/content/releases/download/v2.14.0"
            runtime_url = f"{base}/{runtime_name}"
            checksum_url = f"{base}/SHA256SUMS"
            checksum_data = f"{digest}  {runtime_name}\n".encode()
            release = {
                "tag_name": "v2.14.0",
                "assets": [
                    {"name": runtime_name, "browser_download_url": runtime_url, "size": len(archive_data)},
                    {"name": "SHA256SUMS", "browser_download_url": checksum_url, "size": len(checksum_data)},
                ],
            }
            downloads = {runtime_url: archive_data, checksum_url: checksum_data}

            def downloader(url: str, output: BytesIO, maximum: int) -> tuple[str, int]:
                data = downloads[url]
                self.assertLessEqual(len(data), maximum)
                output.write(data)
                return hashlib.sha256(data).hexdigest(), len(data)

            api = mock.Mock()
            api.tag_commit.return_value = "a" * 40
            candidate = UPDATER.verify_candidate(
                release, api, Path(directory), (5, 15, 4), downloader
            )
            self.assertEqual(candidate["sha256"], digest)
            self.assertEqual(candidate["commit"], "a" * 40)

            bad_checksums = f"{'f' * 64}  {runtime_name}\n".encode()
            downloads[checksum_url] = bad_checksums
            release["assets"][1]["size"] = len(bad_checksums)
            for generated in Path(directory).glob("v2.14.0-*"):
                generated.unlink()
            with self.assertRaisesRegex(UPDATER.UpdateError, "differs from SHA256SUMS"):
                UPDATER.verify_candidate(
                    release, api, Path(directory), (5, 15, 4), downloader
                )

    def test_archive_and_manifest_contract_passes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "runtime.tar.gz"
            runtime_archive(path, manifest())
            UPDATER.verify_archive(
                path, version="2.14.0", commit="a" * 40,
                classic_version=(5, 15, 4),
            )

    def test_archive_rejects_unsafe_type(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "runtime.tar.gz"
            runtime_archive(
                path, manifest(),
                unsafe="atrinik-content-2.14.0-classic-runtime/maps/link",
            )
            with self.assertRaisesRegex(UPDATER.UpdateError, "unsafe member type"):
                UPDATER.verify_archive(
                    path, version="2.14.0", commit="a" * 40,
                    classic_version=(5, 15, 4),
                )

    def test_archive_rejects_traversal_and_wrong_root(self) -> None:
        for name in ("../escape", "wrong-root/file"):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "runtime.tar.gz"
                with tarfile.open(path, "w:gz") as archive:
                    info = tarfile.TarInfo(name)
                    info.size = 1
                    archive.addfile(info, BytesIO(b"x"))
                with self.assertRaises(UPDATER.UpdateError):
                    UPDATER.verify_archive(
                        path, version="2.14.0", commit="a" * 40,
                        classic_version=(5, 15, 4),
                    )

    def test_archive_rejects_file_directory_collisions(self) -> None:
        root = "atrinik-content-2.14.0-classic-runtime"
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "runtime.tar.gz"
            with tarfile.open(path, "w:gz") as archive:
                parent = tarfile.TarInfo(f"{root}/maps")
                parent.size = 1
                archive.addfile(parent, BytesIO(b"x"))
                child = tarfile.TarInfo(f"{root}/maps/file")
                child.size = 1
                archive.addfile(child, BytesIO(b"x"))
            with self.assertRaisesRegex(UPDATER.UpdateError, "collision"):
                UPDATER.verify_archive(
                    path, version="2.14.0", commit="a" * 40,
                    classic_version=(5, 15, 4),
                )

    def test_manifest_rejects_digest_and_source_mismatch(self) -> None:
        for mutation, message in (
            (lambda value: value["files"][0].update(sha256="b" * 64), "exactly match"),
            (lambda value: value["source"].update(commit="b" * 40), "source coordinate"),
            (lambda value: value["source"].update(branch="1.x"), "source coordinate"),
            (lambda value: value.update(target="replacement"), "target"),
            (lambda value: value.update(replacement_ready=True), "replacement_ready"),
            (lambda value: value.update(consumers=["classic/server"]), "consumers"),
        ):
            with self.subTest(message=message), tempfile.TemporaryDirectory() as directory:
                value = manifest()
                mutation(value)
                path = Path(directory) / "runtime.tar.gz"
                runtime_archive(path, value)
                with self.assertRaisesRegex(UPDATER.UpdateError, message):
                    UPDATER.verify_archive(
                        path, version="2.14.0", commit="a" * 40,
                        classic_version=(5, 15, 4),
                    )

    def test_manifest_requires_compatible_classic_release(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "runtime.tar.gz"
            runtime_archive(path, manifest())
            with self.assertRaisesRegex(UPDATER.UpdateError, "does not satisfy"):
                UPDATER.verify_archive(
                    path, version="2.14.0", commit="a" * 40,
                    classic_version=(6, 0, 0),
                )

    def test_manifest_requires_complete_files_and_license_attributions(self) -> None:
        for mutation, message in (
            (lambda value: value.update(files=[]), "files must be non-empty"),
            (lambda value: value.update(license_files=[]), "license_files must be non-empty"),
        ):
            with self.subTest(message=message):
                value = manifest()
                value["license_files"] = list(value["files"])
                mutation(value)
                with self.assertRaisesRegex(UPDATER.UpdateError, message):
                    UPDATER.validate_manifest(
                        json.dumps(value).encode(),
                        {"attribution/maps/COPYING": (hashlib.sha256(b"content\n").hexdigest(), 8)},
                        version="2.14.0", commit="a" * 40,
                        classic_version=(5, 15, 4),
                    )

    def test_tag_peeling_is_bounded_and_rejects_cycles(self) -> None:
        api = mock.Mock()
        api.get.side_effect = [
            {"object": {"type": "tag", "sha": "a" * 40}},
            {"object": {"type": "tag", "sha": "a" * 40}},
        ]
        with self.assertRaisesRegex(UPDATER.UpdateError, "cyclic"):
            UPDATER.GitHubAPI.tag_commit(api, "atrinik/content", "v1.8.6")

    def test_lock_update_changes_only_the_coordinate_and_is_idempotent(self) -> None:
        root = ROOT
        value = {
            "schema_version": 1,
            "dependencies": [
                {
                    "name": "content", "repository": "atrinik/content",
                    "tag": "v1.2.0", "commit": "a" * 40,
                    "url": "https://old", "sha256": "b" * 64,
                    "destination": "runtime/content", "strip_components": 1,
                },
                {"name": "resources", "sentinel": [1, 2, 3]},
            ],
        }
        content = value["dependencies"][0]
        resources_before = json.dumps(value["dependencies"][1], sort_keys=True)
        selected = {
            "tag": "v1.8.6", "commit": "c" * 40,
            "url": "https://new", "sha256": "d" * 64,
        }
        with mock.patch.object(UPDATER, "validate_lock_with_server") as validate:
            evidence = UPDATER.update_lock(root, value, content, selected, apply=False)
            self.assertTrue(evidence["changed"])
            validate.assert_called_once()
            self.assertEqual(json.dumps(value["dependencies"][1], sort_keys=True), resources_before)
            again = UPDATER.update_lock(root, value, content, selected, apply=False)
            self.assertFalse(again["changed"])

    def test_lock_update_is_atomic_and_preserves_unrelated_records(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "server").mkdir()
            value = {
                "schema_version": 1,
                "dependencies": [
                    {
                        "name": "content", "repository": "atrinik/content",
                        "tag": "v1.2.0", "commit": "a" * 40,
                        "url": "https://old", "sha256": "b" * 64,
                        "destination": "runtime/content", "strip_components": 1,
                    },
                    {"name": "resources", "sentinel": {"nested": True}},
                ],
            }
            lock_path = root / UPDATER.LOCK_PATH
            lock_path.write_text(json.dumps(value, indent=2) + "\n")
            content = value["dependencies"][0]
            resources = json.loads(json.dumps(value["dependencies"][1]))
            selected = {
                "tag": "v1.8.6", "commit": "c" * 40,
                "url": "https://new", "sha256": "d" * 64,
            }
            with mock.patch.object(UPDATER, "validate_lock_with_server"):
                UPDATER.update_lock(root, value, content, selected, apply=True)
            written = json.loads(lock_path.read_text())
            self.assertEqual(written["dependencies"][1], resources)
            self.assertEqual(written["dependencies"][0]["tag"], "v1.8.6")
            self.assertEqual(list(lock_path.parent.glob(".dependencies-lock-*")), [])

    def test_current_coordinate_detects_tag_reuse(self) -> None:
        current = {
            "tag": "v1.2.0", "commit": "a" * 40,
            "url": "https://github.com/atrinik/content/releases/download/v1.2.0/atrinik-content-1.2.0-runtime.tar.gz",
            "sha256": "b" * 64,
        }
        release = {"tag_name": "v1.2.0", "draft": False, "published_at": "now", "assets": []}
        api = mock.Mock()
        api.tag_commit.return_value = "c" * 40
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(UPDATER.UpdateError, "tag was reused"):
                UPDATER.verify_current_coordinate(
                    current, [release], api, Path(directory), mock.Mock()
                )

    def test_current_coordinate_detects_digest_drift(self) -> None:
        current = {
            "tag": "v1.2.0", "commit": "a" * 40,
            "url": "https://github.com/atrinik/content/releases/download/v1.2.0/atrinik-content-1.2.0-runtime.tar.gz",
            "sha256": "b" * 64,
        }
        release = {"tag_name": "v1.2.0", "draft": False, "published_at": "now"}
        api = mock.Mock()
        api.tag_commit.return_value = "a" * 40
        with tempfile.TemporaryDirectory() as directory:
            with mock.patch.object(
                UPDATER, "checksum_for_release",
                return_value=(
                    "c" * 64,
                    {},
                    "https://github.com/atrinik/content/releases/download/v1.2.0/atrinik-content-1.2.0-runtime.tar.gz",
                ),
            ):
                with self.assertRaisesRegex(UPDATER.UpdateError, "digest differs"):
                    UPDATER.verify_current_coordinate(
                        current, [release], api, Path(directory), mock.Mock()
                    )

    def test_current_main_coordinate_uses_the_classic_target_asset(self) -> None:
        url = (
            "https://github.com/atrinik/content/releases/download/v2.14.0/"
            "atrinik-content-2.14.0-classic-runtime.tar.gz"
        )
        current = {
            "tag": "v2.14.0",
            "commit": "a" * 40,
            "url": url,
            "sha256": "b" * 64,
        }
        release = {
            "tag_name": "v2.14.0",
            "draft": False,
            "published_at": "now",
        }
        api = mock.Mock()
        api.tag_commit.return_value = "a" * 40
        with tempfile.TemporaryDirectory() as directory:
            with mock.patch.object(
                UPDATER,
                "checksum_for_release",
                return_value=("b" * 64, {}, url),
            ) as checksums:
                version, legacy = UPDATER.verify_current_coordinate(
                    current, [release], api, Path(directory), mock.Mock()
                )
        self.assertEqual(version, (2, 14, 0))
        self.assertFalse(legacy)
        self.assertTrue(checksums.call_args.kwargs["classic_target"])

    def test_generic_main_runtime_cannot_claim_the_legacy_migration_gate(self) -> None:
        current = {
            "tag": "v2.14.0",
            "commit": "a" * 40,
            "url": (
                "https://github.com/atrinik/content/releases/download/v2.14.0/"
                "atrinik-content-2.14.0-runtime.tar.gz"
            ),
            "sha256": "b" * 64,
        }
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(
                UPDATER.UpdateError, "canonical coordinates"
            ):
                UPDATER.verify_current_coordinate(
                    current, [], mock.Mock(), Path(directory), mock.Mock()
                )

    def test_noncanonical_lock_format_is_not_rewritten(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "lock.json"
            path.write_text('{"dependencies": []}\n')
            with self.assertRaisesRegex(UPDATER.UpdateError, "formatting"):
                UPDATER.load_lock(path)

    def test_highest_passing_candidate_wins_and_failures_are_evidence(self) -> None:
        root = ROOT
        releases = [
            {"tag_name": "v2.15.0", "draft": False, "published_at": "now"},
            {"tag_name": "v2.14.0", "draft": False, "published_at": "now"},
        ]
        api = mock.Mock()
        api.releases.return_value = releases
        api.compare.return_value = "ahead"
        lock = {"dependencies": []}
        current = {"tag": "v1.2.0", "commit": "a" * 40, "url": "old", "sha256": "b" * 64}

        def verify(release: dict[str, object], *_args: object, **_kwargs: object) -> dict[str, object]:
            if release["tag_name"] == "v2.15.0":
                raise UPDATER.UpdateError("bad archive")
            return {
                "tag": "v2.14.0", "version": [2, 14, 0], "commit": "c" * 40,
                "url": "new", "sha256": "d" * 64,
            }

        mutation = {"changed": True, "old": {}, "new": {}}
        with (
            mock.patch.object(UPDATER, "load_lock", return_value=(lock, current)),
            mock.patch.object(UPDATER, "current_classic_version", return_value=(5, 15, 4)),
            mock.patch.object(
                UPDATER,
                "verify_current_coordinate",
                return_value=((1, 2, 0), True),
            ),
            mock.patch.object(UPDATER, "verify_candidate", side_effect=verify),
            mock.patch.object(UPDATER, "update_lock", return_value=mutation) as update,
        ):
            evidence = UPDATER.execute(root, apply=False, api=api)
        self.assertTrue(evidence["changed"])
        self.assertEqual(evidence["rejected"], [{"tag": "v2.15.0", "reason": "bad archive"}])
        self.assertEqual(update.call_args.args[3]["tag"], "v2.14.0")
        api.compare.assert_not_called()

    def test_main_updates_require_strict_commit_ancestry(self) -> None:
        release = {"tag_name": "v2.15.0", "draft": False, "published_at": "now"}
        api = mock.Mock()
        api.releases.return_value = [release]
        api.compare.return_value = "diverged"
        current = {
            "tag": "v2.14.0",
            "commit": "a" * 40,
            "url": "old",
            "sha256": "b" * 64,
        }
        candidate = {
            "tag": "v2.15.0",
            "version": [2, 15, 0],
            "commit": "c" * 40,
            "url": "new",
            "sha256": "d" * 64,
        }
        with (
            mock.patch.object(UPDATER, "load_lock", return_value=({}, current)),
            mock.patch.object(
                UPDATER, "current_classic_version", return_value=(5, 22, 0)
            ),
            mock.patch.object(
                UPDATER,
                "verify_current_coordinate",
                return_value=((2, 14, 0), False),
            ),
            mock.patch.object(UPDATER, "verify_candidate", return_value=candidate),
            mock.patch.object(UPDATER, "update_lock") as update,
        ):
            evidence = UPDATER.execute(ROOT, apply=False, api=api)
        self.assertFalse(evidence["changed"])
        self.assertIn("strict descendant", evidence["rejected"][0]["reason"])
        update.assert_not_called()

    def test_duplicate_published_tags_fail_as_ambiguous(self) -> None:
        release = {"tag_name": "v1.8.6", "draft": False, "published_at": "now"}
        api = mock.Mock()
        api.releases.return_value = [release, dict(release)]
        with mock.patch.object(UPDATER, "load_lock", return_value=({}, {})):
            with self.assertRaisesRegex(UPDATER.UpdateError, "ambiguous"):
                UPDATER.execute(ROOT, apply=False, api=api)

    def test_pull_request_body_and_outputs_are_bounded_evidence(self) -> None:
        evidence = {
            "changed": True,
            "old": {"tag": "v1.2.0", "commit": "a" * 40, "url": "old", "sha256": "b" * 64},
            "new": {"tag": "v1.8.6", "commit": "c" * 40, "url": "new", "sha256": "d" * 64},
        }
        with tempfile.TemporaryDirectory() as directory:
            body = Path(directory) / "body.md"
            output = Path(directory) / "output"
            UPDATER.write_pr_body(evidence, body)
            UPDATER.write_github_output(evidence, output)
            text = body.read_text()
            self.assertIn("| `tag` | `v1.2.0` | `v1.8.6` |", text)
            self.assertIn("never approved or merged", text)
            self.assertEqual(
                output.read_text().splitlines(),
                ["changed=true", "tag=v1.8.6", f"commit={'c' * 40}", f"sha256={'d' * 64}"],
            )


if __name__ == "__main__":
    unittest.main()
