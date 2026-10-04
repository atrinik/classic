from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import sys
import unittest
from unittest.mock import call, patch


RELEASE_TOOLS = Path(__file__).resolve().parents[1] / "release"
sys.path.insert(0, str(RELEASE_TOOLS))
SPEC = importlib.util.spec_from_file_location(
    "development_server", RELEASE_TOOLS / "development_server.py"
)
assert SPEC is not None and SPEC.loader is not None
development_server = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(development_server)


REVISION = "a" * 40
PREVIOUS = "b" * 40
DIGEST = "sha256:" + "c" * 64
PREVIOUS_DIGEST = "sha256:" + "d" * 64


def run(**updates: object) -> dict[str, object]:
    value: dict[str, object] = {
        "id": 10,
        "head_sha": REVISION,
        "head_branch": "main",
        "event": "push",
        "path": ".github/workflows/check.yml",
        "repository": {"full_name": development_server.REPOSITORY},
        "head_repository": {"full_name": development_server.REPOSITORY},
        "check_suite_id": 20,
        "status": "completed",
        "conclusion": "success",
    }
    value.update(updates)
    return value


def aggregate(**updates: object) -> dict[str, object]:
    value: dict[str, object] = {
        "name": "Classic validation",
        "app": {"id": 15368},
        "head_sha": REVISION,
        "check_suite": {"id": 20},
        "status": "completed",
        "conclusion": "success",
    }
    value.update(updates)
    return value


class DevelopmentServerTests(unittest.TestCase):
    def canonical_environment(self) -> dict[str, str]:
        return {
            "GITHUB_REPOSITORY": development_server.REPOSITORY,
            "GITHUB_REF": development_server.SOURCE_REF,
            "GITHUB_SHA": REVISION,
            "GITHUB_WORKFLOW_REF": (
                f"{development_server.REPOSITORY}/"
                f"{development_server.WORKFLOW}@{development_server.SOURCE_REF}"
            ),
            "GITHUB_EVENT_NAME": "push",
        }

    def test_context_rejects_each_noncanonical_identity_before_publication(self) -> None:
        cases = (
            {"GITHUB_REPOSITORY": "fork/classic"},
            {"GITHUB_REF": "refs/pull/7/merge"},
            {"GITHUB_WORKFLOW_REF": "atrinik/classic/.github/workflows/other.yml@refs/heads/main"},
            {"GITHUB_EVENT_NAME": "pull_request"},
        )
        for update in cases:
            with self.subTest(update=update):
                environment = self.canonical_environment()
                environment.update(update)
                with patch.dict(os.environ, environment, clear=True), patch.object(
                    development_server, "command", return_value=REVISION
                ):
                    with self.assertRaisesRegex(RuntimeError, "canonical main workflow|unsupported"):
                        development_server.context(REVISION)

    def test_context_rejects_checkout_head_drift_and_tracked_changes(self) -> None:
        with patch.dict(os.environ, self.canonical_environment(), clear=True):
            with patch.object(development_server, "command", return_value=PREVIOUS):
                with self.assertRaisesRegex(RuntimeError, "checkout revision mismatch"):
                    development_server.context(REVISION)
            with patch.object(
                development_server, "command", side_effect=[REVISION, " M tools/release/development_server.py"]
            ):
                with self.assertRaisesRegex(RuntimeError, "source is dirty"):
                    development_server.context(REVISION)

    def test_current_main_rejects_ref_or_commit_drift(self) -> None:
        for value in (
            {"ref": "refs/heads/other", "object": {"type": "commit", "sha": REVISION}},
            {"ref": development_server.SOURCE_REF, "object": {"type": "commit", "sha": PREVIOUS}},
            {"ref": development_server.SOURCE_REF, "object": {"type": "tag", "sha": REVISION}},
        ):
            with self.subTest(value=value), patch.object(development_server, "api", return_value=value):
                with self.assertRaisesRegex(RuntimeError, "no longer current main"):
                    development_server.current_main(REVISION)

    def test_checked_rejects_untrusted_or_nonfinal_check_runs(self) -> None:
        rejected = (
            run(event="pull_request"),
            run(head_repository={"full_name": "fork/classic"}),
            run(path=".github/workflows/renamed-check.yml"),
            run(status="in_progress", conclusion=None),
            run(conclusion="failure"),
        )
        for candidate in rejected:
            with self.subTest(candidate=candidate), patch.object(
                development_server, "api", return_value={"workflow_runs": [candidate]}
            ):
                if candidate["status"] == "completed" and candidate["conclusion"] == "failure":
                    with self.assertRaisesRegex(RuntimeError, "main Check workflow failed"):
                        development_server.checked(REVISION)
                else:
                    self.assertFalse(development_server.checked(REVISION))

    def test_checked_requires_one_trusted_matching_aggregate(self) -> None:
        failures = (
            aggregate(name="Classic validation (copy)"),
            aggregate(app={"id": 1}),
            aggregate(check_suite={"id": 99}),
            aggregate(status="queued", conclusion=None),
            aggregate(conclusion="failure"),
            [aggregate(), aggregate()],
        )
        for checks in failures:
            entries = checks if isinstance(checks, list) else [checks]
            with self.subTest(checks=entries), patch.object(
                development_server, "api", side_effect=[{"workflow_runs": [run()]}, {"check_runs": entries}]
            ):
                with self.assertRaisesRegex(RuntimeError, "trusted aggregate"):
                    development_server.checked(REVISION)

    def test_find_tag_accepts_only_exact_first_page_package_404(self) -> None:
        absent = (1, {"status": "404", "message": "Package not found."}, "not found")
        with patch.object(development_server.registry, "request", return_value=absent):
            self.assertEqual(development_server.find_tag("development"), (False, None))
        for response in (
            (1, {"status": "404", "message": "Not Found"}, "not found"),
            (1, {"status": "401", "message": "Package not found."}, "unauthorized"),
            (1, [], "transport failure"),
            (0, {"malformed": True}, ""),
        ):
            with self.subTest(response=response), patch.object(
                development_server.registry, "request", return_value=response
            ):
                with self.assertRaisesRegex(RuntimeError, "cannot audit|non-list"):
                    development_server.find_tag("development")

    def test_find_tag_rejects_pagination_exhaustion_and_later_page_errors(self) -> None:
        full = [{"metadata": {"container": {"tags": []}}, "name": DIGEST}] * 100
        with patch.object(development_server.registry, "request", return_value=(0, full, "")):
            with self.assertRaisesRegex(RuntimeError, "bounded window"):
                development_server.find_tag("development")
        with patch.object(
            development_server.registry, "request", side_effect=[(0, full, ""), (1, {"status": "404", "message": "Package not found."}, "")]
        ):
            with self.assertRaisesRegex(RuntimeError, "cannot audit"):
                development_server.find_tag("development")

    def test_verify_rejects_source_tag_digest_and_revision_label_mismatch(self) -> None:
        with patch.object(development_server, "find_tag", return_value=(True, PREVIOUS_DIGEST)):
            with self.assertRaisesRegex(RuntimeError, "source tag does not match"):
                development_server.verify(REVISION, DIGEST, False)
        labels = {"org.opencontainers.image.revision": PREVIOUS}
        with patch.object(development_server, "find_tag", return_value=(True, DIGEST)), patch.object(
            development_server, "labels", return_value=labels
        ):
            with self.assertRaisesRegex(RuntimeError, "source revision mismatch"):
                development_server.verify(REVISION, DIGEST, False)

    def test_labels_rejects_bad_source_version_channel_and_source_label(self) -> None:
        base = {
            "org.opencontainers.image.source": "https://github.com/" + development_server.REPOSITORY,
            "org.opencontainers.image.version": "0.0.0",
            development_server.CHANNEL_LABEL: "development",
            "org.opencontainers.image.revision": REVISION,
        }
        for key, value in (
            ("org.opencontainers.image.source", "https://github.com/fork/classic"),
            ("org.opencontainers.image.version", "5.0.0"),
            (development_server.CHANNEL_LABEL, "stable"),
        ):
            object_value = {"Os": "linux", "Architecture": "amd64", "RepoDigests": [development_server.IMAGE + "@" + DIGEST], "Config": {"Labels": {**base, key: value}}}
            with self.subTest(key=key), patch.object(development_server, "command", side_effect=["", __import__("json").dumps([object_value])]):
                with self.assertRaisesRegex(RuntimeError, "source/channel/version"):
                    development_server.labels(DIGEST)

    def test_attestation_uses_exact_signer_source_ref_and_digest_arguments(self) -> None:
        with patch.object(development_server, "command", return_value="") as command:
            development_server.attest(DIGEST, REVISION)
        command.assert_called_once_with(
            "gh", "attestation", "verify", "oci://" + development_server.IMAGE + "@" + DIGEST,
            "--repo", development_server.REPOSITORY, "--signer-workflow",
            development_server.REPOSITORY + "/" + development_server.WORKFLOW,
            "--source-ref", development_server.SOURCE_REF, "--source-digest", REVISION,
        )

    def test_promotion_ancestry_rejects_divergent_or_older_sources(self) -> None:
        for comparison in (
            {"status": "behind", "merge_base_commit": {"sha": PREVIOUS}},
            {"status": "ahead", "merge_base_commit": {"sha": "e" * 40}},
        ):
            with self.subTest(comparison=comparison), patch.object(development_server, "api", return_value=comparison):
                with self.assertRaisesRegex(RuntimeError, "cannot move backwards or to a divergent"):
                    development_server.promotion_allowed(PREVIOUS, REVISION)

    def test_promote_rejects_same_source_with_a_changed_digest_before_alias_mutation(self) -> None:
        with patch.object(development_server, "preflight"), patch.object(development_server, "verify"), patch.object(
            development_server, "find_tag", side_effect=[(True, PREVIOUS_DIGEST), (True, PREVIOUS_DIGEST)]
        ), patch.object(development_server, "labels", return_value={"org.opencontainers.image.revision": REVISION}), patch.object(
            development_server, "attest"
        ), patch.object(development_server, "command") as command:
            with self.assertRaisesRegex(RuntimeError, "same source was rebuilt"):
                development_server.promote(REVISION, DIGEST)
        command.assert_not_called()

    def test_promote_never_mutates_before_all_attestations_and_current_main(self) -> None:
        with patch.object(development_server, "preflight"), patch.object(development_server, "verify"), patch.object(
            development_server, "find_tag", side_effect=[(True, PREVIOUS_DIGEST), (True, PREVIOUS_DIGEST)]
        ), patch.object(development_server, "labels", return_value={"org.opencontainers.image.revision": PREVIOUS}), patch.object(
            development_server, "attest", side_effect=RuntimeError("bad attestation")
        ), patch.object(development_server, "command") as command:
            with self.assertRaisesRegex(RuntimeError, "bad attestation"):
                development_server.promote(REVISION, DIGEST)
        command.assert_not_called()

    def test_promote_rechecks_current_main_before_alias_mutation(self) -> None:
        with patch.object(development_server, "preflight"), patch.object(
            development_server, "verify"
        ), patch.object(
            development_server, "find_tag", return_value=(True, None)
        ), patch.object(
            development_server, "current_main", side_effect=RuntimeError("main advanced")
        ), patch.object(development_server, "command") as command:
            with self.assertRaisesRegex(RuntimeError, "main advanced"):
                development_server.promote(REVISION, DIGEST)
        command.assert_not_called()

    def test_audit_rejects_missing_shared_package(self) -> None:
        with patch.object(development_server, "preflight"), patch.object(
            development_server, "find_tag", return_value=(False, None)
        ):
            with self.assertRaisesRegex(RuntimeError, "server package missing"):
                development_server.audit(REVISION)


if __name__ == "__main__":
    unittest.main()
