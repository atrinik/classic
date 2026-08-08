from __future__ import annotations

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


class WorkflowContractTests(unittest.TestCase):
    def text(self, name: str) -> str:
        return (ROOT / ".github" / "workflows" / name).read_text(encoding="utf-8")

    def test_semantic_release_follows_successful_current_main_check(self) -> None:
        workflow = self.text("release.yml")
        self.assertIn("  workflow_dispatch:\n", workflow)
        self.assertIn("  workflow_run:\n", workflow)
        self.assertIn("    workflows: [Check]\n", workflow)
        self.assertIn("    types: [completed]\n", workflow)
        self.assertIn("github.event.workflow_run.event == 'push'", workflow)
        self.assertIn("github.event.workflow_run.conclusion == 'success'", workflow)
        self.assertIn("github.event.workflow_run.head_branch == 'main'", workflow)
        self.assertIn("RELEASE_TRIGGER_SHA", workflow)
        self.assertIn('test "${commit}" = "${RELEASE_TRIGGER_SHA}"', workflow)
        self.assertIn("refs/remotes/origin/main", workflow)
        self.assertIn('select(.name == "Classic validation"', workflow)
        self.assertIn('test "${GITHUB_REF}" = refs/heads/main', workflow)

    def test_rehearsal_is_bound_to_current_main(self) -> None:
        workflow = self.text("release-rehearsal.yml")
        self.assertIn('test "${DISPATCH_REF}" = refs/heads/main', workflow)
        self.assertIn("refs/remotes/origin/main", workflow)
        self.assertIn("needs: preflight", workflow)

    def test_package_dispatch_is_bound_to_its_exact_tag_ref(self) -> None:
        workflow = self.text("package-release.yml")
        self.assertIn('test "${RELEASE_REF_TYPE}" = tag', workflow)
        self.assertIn('test "${GITHUB_REF}" = "refs/tags/${RELEASE_TAG}"', workflow)
        self.assertNotIn("immutable-releases", workflow)
        self.assertIn("--verify-only", workflow)
        self.assertLess(
            workflow.index("--verify-only"),
            workflow.index("gh release edit"),
        )

    def test_production_metadata_can_read_drafts_without_broad_write_access(self) -> None:
        package = self.text("package-release.yml")
        candidate = self.text("build-release-candidate.yml")
        candidate_job = package[
            package.index("  candidate:") : package.index("  publish:")
        ]
        metadata_job = candidate[
            candidate.index("  metadata:") : candidate.index("  sources:")
        ]
        for job in (candidate_job, metadata_job):
            self.assertIn("permissions:\n", job)
            self.assertIn("contents: write", job)
        self.assertEqual(package[: package.index("jobs:")].count("contents: read"), 1)
        self.assertNotIn("gh release edit", metadata_job)

    def test_semantic_release_skips_cross_repository_issue_comments(self) -> None:
        config = (ROOT / ".releaserc.cjs").read_text(encoding="utf-8")
        workflow = self.text("release.yml")
        self.assertIn("failCommentCondition: false", config)
        self.assertIn("successCommentCondition: false", config)
        self.assertNotIn("issues: write", workflow)
        self.assertNotIn("pull-requests: write", workflow)

    def test_candidate_and_package_use_distinct_concurrency_namespaces(self) -> None:
        package = self.text("package-release.yml")
        candidate = self.text("build-release-candidate.yml")
        self.assertIn("group: classic-package-release-", package)
        self.assertIn("classic-release-candidate-", candidate)

    def test_latest_alias_has_one_globally_serialized_owner(self) -> None:
        package = self.text("package-release.yml")
        promoter = self.text("promote-latest.yml")
        self.assertNotIn("imagetools create", package)
        self.assertIn("push-to-registry: true", package)
        self.assertIn("group: classic-promote-latest", promoter)
        self.assertIn("gh attestation verify", promoter)
        self.assertIn("--tag latest", promoter)
        resolver = (ROOT / "tools" / "release" / "check_latest_release.py").read_text(
            encoding="utf-8"
        )
        self.assertIn("/releases/latest", resolver)

    def test_recovery_and_manual_promotion_require_main_definitions(self) -> None:
        recovery = self.text("recover-release.yml")
        promoter = self.text("promote-latest.yml")
        self.assertIn('test "${GITHUB_REF}" = refs/heads/main', recovery)
        self.assertIn('test "${GITHUB_REF}" = refs/heads/main', promoter)

    def test_automatic_and_recovery_notes_use_the_same_pinned_toolchain(self) -> None:
        release = self.text("release.yml")
        recovery = self.text("recover-release.yml")
        check = self.text("check.yml")
        packages = (
            "--package=semantic-release@25.0.9",
            "--package=@semantic-release/commit-analyzer@13.0.1",
            "--package=@semantic-release/release-notes-generator@14.1.1",
            "--package=@semantic-release/exec@7.1.0",
            "--package=conventional-changelog-conventionalcommits@9.3.1",
        )
        for workflow in (release, recovery, check):
            for package in packages:
                with self.subTest(package=package):
                    self.assertEqual(workflow.count(package), 1)
        self.assertIn("node tools/release/run_semantic_release.mjs", release)
        self.assertIn("node tools/release/generate_release_notes.mjs", recovery)
        self.assertIn("node --test tools/tests/test_first_parent_release.mjs", check)

    def test_recovery_uses_first_parent_notes_file(self) -> None:
        recovery = self.text("recover-release.yml")
        self.assertIn("--github-output", recovery)
        self.assertIn("steps.recovery.outputs.previous_tag", recovery)
        self.assertIn("--notes-file build/release-notes.md", recovery)
        self.assertNotIn("--generate-notes", recovery)

    def test_rehearsal_image_requests_provenance_and_sbom(self) -> None:
        candidate = self.text("build-release-candidate.yml")
        self.assertGreaterEqual(candidate.count("provenance: mode=max"), 1)
        self.assertGreaterEqual(candidate.count("sbom: true"), 1)

    def test_windows_packages_persist_toolchain_bound_compiler_caches(self) -> None:
        candidate = self.text("build-release-candidate.yml")
        cache_action = (
            "actions/cache@55cc8345863c7cc4c66a329aec7e433d2d1c52a9"
        )
        self.assertEqual(candidate.count(cache_action), 2)
        client_job = candidate[
            candidate.index("  client-windows:") : candidate.index("  server-windows:")
        ]
        server_job = candidate[
            candidate.index("  server-windows:") : candidate.index("  server-image:")
        ]
        digest = "9cc373f620a577328fc0a7a7fa823bddaca6d7dc75ac73bcf21be421c49676f7"
        self.assertEqual(
            candidate.count(f"WINDOWS_BUILD_CACHE_EPOCH: 1.0.5-{digest}"), 1
        )
        for job, component in ((client_job, "client"), (server_job, "server")):
            with self.subTest(component=component):
                self.assertIn(f"path: {component}/build/ccache", job)
                self.assertIn(f"windows-{component}-ccache-v1-", job)
                self.assertEqual(job.count(digest), 2)
                self.assertEqual(job.count("${{ env.WINDOWS_BUILD_CACHE_EPOCH }}"), 2)
                self.assertIn("${{ runner.os }}-${{ runner.arch }}", job)
                self.assertIn("${{ needs.metadata.outputs.version }}", job)
                self.assertIn(f"'{component}/src/**'", job)
                self.assertNotIn(f"'{component}/**'", job)
                self.assertIn("ccache --zero-stats", job)
                self.assertIn("ccache --show-stats", job)
                self.assertIn("--env CCACHE_MAXSIZE=500M", job)
                self.assertIn(
                    f"--env CCACHE_TEMPDIR=/tmp/atrinik-{component}-ccache", job
                )
                self.assertIn(
                    "--env PATH=/opt/mxe/.ccache/bin:/opt/mxe/usr/bin:", job
                )
        self.assertEqual(candidate.count("ccache --zero-stats"), 2)
        self.assertEqual(candidate.count("ccache --show-stats"), 2)
        self.assertEqual(candidate.count("--env CCACHE_MAXSIZE=500M"), 2)
        self.assertEqual(candidate.count("--env PATH=/opt/mxe/.ccache/bin:"), 2)

    def test_only_release_metadata_checkout_requires_full_history(self) -> None:
        candidate = self.text("build-release-candidate.yml")
        self.assertEqual(candidate.count("fetch-depth: 0"), 1)
        metadata_job = candidate[
            candidate.index("  metadata:") : candidate.index("  sources:")
        ]
        self.assertIn("fetch-depth: 0", metadata_job)

    def test_native_worldmaker_build_uses_the_server_compiler_cache(self) -> None:
        script = (ROOT / "server" / "tools" / "build-windows-package.sh").read_text(
            encoding="utf-8"
        )
        self.assertIn("command -v ccache", script)
        self.assertIn("-DCMAKE_C_COMPILER_LAUNCHER=", script)


if __name__ == "__main__":
    unittest.main()
