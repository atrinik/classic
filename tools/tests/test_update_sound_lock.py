from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "update_sound_lock", ROOT / "tools" / "release" / "update_sound_lock.py"
)
assert SPEC is not None and SPEC.loader is not None
UPDATER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(UPDATER)


class FakeAPI(UPDATER.GitHubAPI):
    def __init__(self, releases: list[dict[str, object]]) -> None:
        self.release_pages = [releases]
        self.commits: dict[str, str] = {}
        self.comparisons: dict[tuple[str, str], str] = {}

    def get(self, endpoint: str) -> object:
        if "/releases?" in endpoint:
            page = int(endpoint.rsplit("=", 1)[1])
            return self.release_pages[page - 1] if page <= len(self.release_pages) else []
        if "/git/ref/tags/" in endpoint:
            tag = endpoint.rsplit("/", 1)[1]
            return {"object": {"type": "commit", "sha": self.commits[tag]}}
        if "/compare/" in endpoint:
            pair = endpoint.rsplit("/", 1)[1].split("...", 1)
            return {"status": self.comparisons[tuple(pair)]}
        raise AssertionError(endpoint)


class UpdateSoundLockTests(unittest.TestCase):
    def coordinate(self, tag: str, commit: str, digest: str) -> dict[str, object]:
        return {
            "name": "sound",
            "repository": "atrinik/sound",
            "tag": tag,
            "commit": commit,
            "url": (
                f"https://github.com/atrinik/sound/releases/download/{tag}/"
                f"atrinik-sound-{tag[1:]}.tar.gz"
            ),
            "sha256": digest,
            "destination": "sound",
            "strip_components": 1,
        }

    def release(
        self, tag: str, archive: bytes, *, draft: bool = False,
        published_at: str | None = "now",
    ) -> tuple[dict[str, object], dict[str, bytes]]:
        version = tag[1:]
        archive_name = f"atrinik-sound-{version}.tar.gz"
        base = f"https://github.com/atrinik/sound/releases/download/{tag}"
        archive_url = f"{base}/{archive_name}"
        checksum = f"{hashlib.sha256(archive).hexdigest()}  {archive_name}\n".encode()
        checksum_url = f"{base}/SHA256SUMS"
        return (
            {
                "tag_name": tag,
                "draft": draft,
                "published_at": published_at,
                "assets": [
                    {
                        "name": archive_name,
                        "browser_download_url": archive_url,
                        "size": len(archive),
                    },
                    {
                        "name": "SHA256SUMS",
                        "browser_download_url": checksum_url,
                        "size": len(checksum),
                    },
                ],
            },
            {archive_url: archive, checksum_url: checksum},
        )

    def downloader(self, downloads: dict[str, bytes]):
        def download(url: str, output, maximum: int) -> tuple[str, int]:
            data = downloads[url]
            self.assertLessEqual(len(data), maximum)
            output.write(data)
            return hashlib.sha256(data).hexdigest(), len(data)

        return download

    def test_pagination_is_bounded(self) -> None:
        first = [{"tag_name": f"v1.0.{index}"} for index in range(100)]
        api = FakeAPI(first)
        api.release_pages = [first, [{"tag_name": "v1.1.0"}]]
        self.assertEqual(len(api.releases("atrinik/sound")), 101)
        api.release_pages = [first, first]
        with mock.patch.object(UPDATER, "MAX_RELEASE_PAGES", 2):
            with self.assertRaisesRegex(UPDATER.UpdateError, "pagination"):
                api.releases("atrinik/sound")

    def test_current_coordinate_requires_published_release(self) -> None:
        digest = "a" * 64
        current = self.coordinate("v1.5.0", "b" * 40, digest)
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(
                UPDATER.UpdateError,
                r"sound dependency: tag=v1\.5\.0; .*expected_sha256=" + digest,
            ):
                UPDATER.verify_current_coordinate(
                    current, [], mock.Mock(), Path(directory), self.downloader({})
                )

    def test_current_coordinate_checks_tag_checksum_and_archive(self) -> None:
        tag = "v1.5.0"
        commit = "a" * 40
        archive = b"verified sound source archive"
        release, downloads = self.release(tag, archive)
        digest = hashlib.sha256(archive).hexdigest()
        current = self.coordinate(tag, commit, digest)
        api = FakeAPI([release])
        api.commits[tag] = commit
        with tempfile.TemporaryDirectory() as directory:
            version = UPDATER.verify_current_coordinate(
                current, [release], api, Path(directory), self.downloader(downloads)
            )
        self.assertEqual(version, (1, 5, 0))

    def test_current_coordinate_rejects_archive_digest_drift(self) -> None:
        tag = "v1.5.0"
        commit = "a" * 40
        archive = b"verified sound source archive"
        release, downloads = self.release(tag, archive)
        current = self.coordinate(tag, commit, "b" * 64)
        api = FakeAPI([release])
        api.commits[tag] = commit
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(UPDATER.UpdateError, "SHA256SUMS digest differs"):
                UPDATER.verify_current_coordinate(
                    current, [release], api, Path(directory), self.downloader(downloads)
                )

    def test_execute_selects_highest_verified_descendant(self) -> None:
        current_tag = "v1.5.0"
        current_commit = "a" * 40
        current_archive = b"current"
        current_release, current_downloads = self.release(current_tag, current_archive)
        candidate_tag = "v1.6.0"
        candidate_commit = "b" * 40
        candidate_archive = b"candidate"
        candidate_release, candidate_downloads = self.release(candidate_tag, candidate_archive)
        api = FakeAPI([current_release, candidate_release])
        api.commits.update({current_tag: current_commit, candidate_tag: candidate_commit})
        api.comparisons[(current_commit, candidate_commit)] = "ahead"
        downloads = {**current_downloads, **candidate_downloads}
        current = self.coordinate(
            current_tag, current_commit, hashlib.sha256(current_archive).hexdigest()
        )
        root = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__("shutil").rmtree(root, ignore_errors=True))
        (root / "client").mkdir()
        value = {"schema_version": 1, "dependencies": [current]}
        (root / "client/dependencies.lock.json").write_text(
            json.dumps(value, indent=2) + "\n", encoding="utf-8"
        )
        with mock.patch.object(UPDATER, "validate_lock_with_loader"):
            evidence = UPDATER.execute(
                root, apply=False, api=api, downloader=self.downloader(downloads)
            )
        self.assertTrue(evidence["changed"])
        self.assertEqual(evidence["new"]["tag"], candidate_tag)
        self.assertEqual(evidence["new"]["sha256"], hashlib.sha256(candidate_archive).hexdigest())

    def test_pr_body_and_outputs_are_bounded_evidence(self) -> None:
        evidence = {
            "changed": True,
            "old": {"tag": "v1.5.0", "commit": "a" * 40, "url": "old", "sha256": "b" * 64},
            "new": {"tag": "v1.6.0", "commit": "c" * 40, "url": "new", "sha256": "d" * 64},
        }
        with tempfile.TemporaryDirectory() as directory:
            body = Path(directory) / "body.md"
            output = Path(directory) / "output"
            UPDATER.write_pr_body(evidence, body)
            UPDATER.write_github_output(evidence, output)
            self.assertIn("| `tag` | `v1.5.0` | `v1.6.0` |", body.read_text())
            self.assertIn("SHA256SUMS", body.read_text())
            self.assertEqual(output.read_text().splitlines(), [
                "changed=true", "tag=v1.6.0", "commit=" + "c" * 40,
                "sha256=" + "d" * 64,
            ])


if __name__ == "__main__":
    unittest.main()
