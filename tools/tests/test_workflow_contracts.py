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

    def test_package_dispatch_is_bound_to_a_tag_or_current_main_recovery(self) -> None:
        workflow = self.text("package-release.yml")
        self.assertIn('test "${RELEASE_REF_TYPE}" = tag', workflow)
        self.assertIn('test "${GITHUB_REF}" = "refs/tags/${RELEASE_TAG}"', workflow)
        self.assertIn('test "${RELEASE_REF_TYPE}" = branch', workflow)
        self.assertIn('test "${GITHUB_REF}" = refs/heads/main', workflow)
        self.assertIn('refs/remotes/origin/main', workflow)
        self.assertIn('test "$(git rev-parse HEAD)"', workflow)
        self.assertIn('refs/tags/${RELEASE_TAG}^{commit}', workflow)
        self.assertIn('select(.name == "Classic validation"', workflow)
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

    def test_current_main_candidate_uses_the_trusted_recovery_verifier(self) -> None:
        workflow = self.text("build-release-candidate.yml")
        metadata_job = workflow[
            workflow.index("  metadata:") : workflow.index("  sources:")
        ]
        self.assertIn("Check out the trusted recovery verifier", metadata_job)
        self.assertIn("fetch-depth: 0", metadata_job)
        self.assertIn("path: build/release-automation", metadata_job)
        self.assertIn("ref: ${{ github.sha }}", metadata_job)
        self.assertIn("if test \"${GITHUB_REF}\" = refs/heads/main", metadata_job)
        self.assertIn(
            "build/release-automation/tools/release/validate_release.py",
            metadata_job,
        )
        self.assertIn("recovery_arguments+=(--recovery-main)", metadata_job)
        self.assertIn("verifier=tools/release/validate_release.py", metadata_job)

    def test_semantic_release_skips_cross_repository_issue_comments(self) -> None:
        config = (ROOT / ".releaserc.cjs").read_text(encoding="utf-8")
        workflow = self.text("release.yml")
        self.assertIn("failCommentCondition: false", config)
        self.assertIn("successCommentCondition: false", config)
        self.assertNotIn("issues: write", workflow)
        self.assertNotIn("pull-requests: write", workflow)

    def test_semantic_release_resumes_one_pending_draft_before_analysis(self) -> None:
        workflow = self.text("release.yml")
        detection = workflow.index("id: pending-release")
        recovery = workflow.index("Resume the incomplete package release")
        analysis = workflow.index("Analyze commits, tag, and publish release notes")
        self.assertLess(detection, recovery)
        self.assertLess(recovery, analysis)
        self.assertIn("if ((${#drafts[@]} > 1))", workflow)
        self.assertIn("git merge-base --is-ancestor", workflow)
        self.assertIn("--ref main", workflow)
        self.assertIn("if: steps.pending-release.outputs.tag == ''", workflow)
        self.assertIn("gh api --paginate \\\n", workflow)
        self.assertIn(".[] | select(.draft == true)", workflow)
        self.assertIn("(.assets | length)", workflow)
        self.assertIn("retained-candidate recovery is required", workflow)
        self.assertNotIn("gh api --paginate --slurp", workflow)

    def test_retained_candidate_recovery_is_bound_and_does_not_rebuild(self) -> None:
        workflow = self.text("package-release.yml")
        self.assertIn("candidate_run_id:", workflow)
        self.assertIn("if: inputs.candidate_run_id == ''", workflow)
        self.assertIn("if: inputs.candidate_run_id != ''", workflow)
        self.assertIn("complete-release-candidate-'", workflow)
        self.assertIn("Validate complete release candidate", workflow)
        self.assertIn("Publish unified release", workflow)
        self.assertIn(".size_in_bytes > 0", workflow)
        self.assertIn('startswith("sha256:")', workflow)
        self.assertIn(".github/workflows/package-release.yml", workflow)
        self.assertIn(".head_repository.full_name", workflow)
        self.assertIn('"${run_commit}" HEAD', workflow)
        self.assertIn("run-id: ${{ inputs.candidate_run_id || github.run_id }}", workflow)
        self.assertIn("retained candidate has no matching versioned server image", workflow)
        self.assertIn("--recovery-main", workflow)
        self.assertIn(
            "build/release-automation/tools/release/locked_inputs.py", workflow
        )
        self.assertNotIn("--clobber", workflow)

    def test_candidate_and_package_use_distinct_concurrency_namespaces(self) -> None:
        package = self.text("package-release.yml")
        candidate = self.text("build-release-candidate.yml")
        self.assertIn("group: classic-package-release-", package)
        self.assertIn("classic-release-candidate-", candidate)

    def test_latest_alias_has_one_globally_serialized_owner(self) -> None:
        package = self.text("package-release.yml")
        promoter = self.text("promote-latest.yml")
        queue_job = package[package.index("  queue-latest:") :]
        self.assertNotIn("imagetools create", package)
        self.assertIn("push-to-registry: true", package)
        self.assertIn("needs: publish", queue_job)
        self.assertIn("if: always() && needs.publish.result == 'success'", queue_job)
        self.assertIn("group: classic-promote-latest", promoter)
        self.assertIn("Check out the trusted promotion verifier", promoter)
        self.assertIn("path: build/release-automation", promoter)
        self.assertIn("ref: ${{ github.sha }}", promoter)
        self.assertIn(
            "build/release-automation/tools/release/locked_inputs.py", promoter
        )
        self.assertIn(
            "build/release-automation/tools/release/check_registry_version.py",
            promoter,
        )
        self.assertEqual(
            promoter.count(
                "build/release-automation/tools/release/check_latest_release.py"
            ),
            2,
        )
        self.assertEqual(
            promoter.count(
                "build/release-automation/tools/release/check_registry_version.py"
            ),
            2,
        )
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
        self.assertIn("Require the exact current main promotion definition", promoter)
        self.assertIn("PROMOTION_WORKFLOW_SHA: ${{ github.sha }}", promoter)
        self.assertIn('test "$(git rev-parse HEAD)"', promoter)
        self.assertIn("refs/remotes/origin/main", promoter)

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

    def test_only_release_metadata_checkouts_require_full_history(self) -> None:
        candidate = self.text("build-release-candidate.yml")
        metadata_job = candidate[
            candidate.index("  metadata:") : candidate.index("  sources:")
        ]
        remaining_jobs = candidate[candidate.index("  sources:") :]
        self.assertEqual(candidate.count("fetch-depth: 0"), 2)
        self.assertEqual(metadata_job.count("fetch-depth: 0"), 2)
        self.assertNotIn("fetch-depth: 0", remaining_jobs)

    def test_native_worldmaker_build_uses_the_server_compiler_cache(self) -> None:
        script = (ROOT / "server" / "tools" / "build-windows-package.sh").read_text(
            encoding="utf-8"
        )
        self.assertIn("command -v ccache", script)
        self.assertIn("-DCMAKE_C_COMPILER_LAUNCHER=", script)


if __name__ == "__main__":
    unittest.main()
