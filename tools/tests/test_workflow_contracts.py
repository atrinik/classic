from __future__ import annotations

from pathlib import Path
import json
import os
import subprocess
import tempfile
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[2]


class WorkflowContractTests(unittest.TestCase):
    def text(self, name: str) -> str:
        return (ROOT / ".github" / "workflows" / name).read_text(encoding="utf-8")

    def test_content_updater_has_a_narrow_human_reviewed_mutation_boundary(self) -> None:
        workflow = self.text("update-content.yml")
        self.assertIn("  schedule:\n", workflow)
        self.assertIn("  workflow_dispatch:\n", workflow)
        self.assertIn("group: classic-content-lock-update", workflow)
        self.assertIn("cancel-in-progress: false", workflow)
        self.assertIn(
            "github.event_name == 'workflow_dispatch' || vars.DEPENDENCY_UPDATE_SCHEDULE_ENABLED == 'true'",
            workflow,
        )
        self.assertEqual(workflow[: workflow.index("jobs:")].count("contents: read"), 1)
        self.assertNotIn("\n  contents: write", workflow)
        self.assertNotIn("\n  pull-requests: write", workflow)
        self.assertIn("persist-credentials: false", workflow)
        self.assertIn("test \"${GITHUB_REF}\" = refs/heads/main", workflow)
        self.assertIn("tools/release/update_content_lock.py", workflow)
        token = workflow.index("name: Mint the installation token")
        verification = workflow.index("name: Verify all content releases")
        ownership = workflow.index("name: Fail closed on unexpected automation")
        mutation = workflow.index("name: Create or update the single App-owned")
        self.assertLess(verification, ownership)
        self.assertLess(ownership, token)
        self.assertLess(token, mutation)
        self.assertIn(
            "actions/create-github-app-token@bcd2ba49218906704ab6c1aa796996da409d3eb1 # v3.2.0",
            workflow,
        )
        self.assertIn("permission-contents: write", workflow)
        self.assertIn("permission-pull-requests: write", workflow)
        self.assertIn("--force-with-lease=", workflow)
        self.assertIn("server/dependencies.lock.json", workflow)
        for forbidden in (
            "gh pr review", "gh pr merge", "gh release", "git tag",
            "workflow dispatch", "repos/${GITHUB_REPOSITORY}/branches/main",
        ):
            self.assertNotIn(forbidden, workflow)

    def test_nested_component_automation_remains_retired(self) -> None:
        for module in ("client", "editor", "libatrinik", "protocol", "server"):
            with self.subTest(module=module):
                github = ROOT / module / ".github"
                if github.exists():
                    self.assertFalse(
                        any(
                            path.is_file() or path.is_symlink()
                            for path in github.rglob("*")
                        )
                    )
                self.assertFalse((ROOT / module / ".releaserc.json").exists())

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
        candidate = self.text("build-release-candidate.yml")
        self.assertIn('test "${DISPATCH_REF}" = refs/heads/main', workflow)
        self.assertIn("refs/remotes/origin/main", workflow)
        self.assertIn("needs: preflight", workflow)
        self.assertNotIn("contents: write", workflow)
        self.assertNotIn("contents: write", candidate)
        self.assertIn("source_epoch: ${{ needs.preflight.outputs.source_epoch }}", workflow)

    def test_release_consumers_use_the_digest_pinned_bundle_offline(self) -> None:
        candidate = self.text("build-release-candidate.yml")
        package = self.text("package-release.yml")
        descriptor = json.loads((ROOT / "dependencies.bundle.json").read_text())
        self.assertEqual(descriptor["image"], "ghcr.io/atrinik/classic-dependencies")
        self.assertRegex(descriptor["digest"], r"^sha256:[0-9a-f]{64}$")
        self.assertRegex(descriptor["material_digest"], r"^sha256:[0-9a-f]{64}$")
        self.assertIn("Verify durable dependency bundle", candidate)
        self.assertEqual(candidate.count("candidate-dependencies-${{"), 4)
        self.assertNotIn("name: release-dependencies-", candidate)
        self.assertEqual(candidate.count("--network none"), 2)
        self.assertEqual(candidate.count("ATRINIK_DEPENDENCY_DOWNLOADS="), 2)
        self.assertEqual(candidate.count("ATRINIK_DEPENDENCY_CACHE_DIR="), 1)
        self.assertIn("tools/release/install_dependency_bundle.sh", candidate)
        self.assertIn("tools/release/install_dependency_bundle.sh", package)
        self.assertNotIn("dependencies.py sync", candidate)
        client_script = (ROOT / "client/tools/build-windows-package.sh").read_text()
        self.assertIn('dependency_sync_arguments+=(--cache "${dependency_downloads}" --refresh --offline)', client_script)
        installer = (ROOT / "tools/release/install_dependency_bundle.sh").read_text()
        self.assertIn('gh attestation verify "oci://${reference}"', installer)
        self.assertGreaterEqual(candidate.count("GH_TOKEN: ${{ github.token }}"), 1)

    def test_dependency_bundle_publication_is_trusted_and_digest_preserving(self) -> None:
        workflow = self.text("publish-dependency-bundle.yml")
        self.assertIn("branches: [main]", workflow)
        self.assertNotIn("paths:", workflow)
        self.assertIn("if: github.ref == 'refs/heads/main'", workflow)
        self.assertIn('test "${GITHUB_REF}" = refs/heads/main', workflow)
        self.assertIn("refs/remotes/origin/main", workflow)
        self.assertIn("packages: write", workflow)
        self.assertIn("oras cp --from-oci-layout", workflow)
        self.assertIn("9ce999f8d2de03fc03968b29d743077a58783e545e5eaa53917ca177352d0e59", workflow)
        self.assertIn("dependency_bundle.py build", workflow)
        self.assertIn("immutable dependency material tag exists", workflow)
        self.assertIn("tools/release/check_registry_version.py", workflow)
        self.assertIn("steps.material-tag.outputs.package_exists", workflow)
        self.assertIn("steps.material-tag.outputs.exists", workflow)
        self.assertIn("steps.material-tag.outputs.digest", workflow)
        self.assertIn('case "${PACKAGE_EXISTS}:${TAG_EXISTS}" in', workflow)
        self.assertIn("bootstrap_missing_package:", workflow)
        self.assertIn("github.event_name == 'workflow_dispatch'", workflow)
        self.assertIn("materials-428265fcc11e9e3f7fc534659b55008cd26e9da6c74da054074279b7bc4af2e9", workflow)
        self.assertIn("sha256:ffe1fa8d28a323d502d01400e2260b7b5eec37842e762c439b88bd9ee823923e", workflow)
        self.assertIn("use the documented recovery path", workflow)
        self.assertNotIn("oras repo tags", workflow)
        self.assertNotIn("pull_request:", workflow)
        self.assertNotIn("contents: write", workflow)
        self.assertIn("attestations: read", self.text("release.yml"))
        self.assertIn("attestations: read", self.text("release-rehearsal.yml"))
        package = self.text("package-release.yml")
        candidate_job = package.split("\n  candidate:\n", 1)[1].split(
            "\n  publish:\n", 1
        )[0]
        self.assertIn("attestations: read", candidate_job)

    def test_dependency_bundle_publication_bootstrap_is_explicit_and_fail_closed(self) -> None:
        workflow = self.text("publish-dependency-bundle.yml")
        step = workflow.index("        name: Publish or verify the immutable material tag")
        start = workflow.index("        run: |\n", step) + len("        run: |\n")
        lines = []
        for line in workflow[start:].splitlines(keepends=True):
            if line.strip() and not line.startswith("          "):
                break
            lines.append(line)
        script = textwrap.dedent("".join(lines))
        expected = json.loads(
            (ROOT / "dependencies.bundle.json").read_text(encoding="utf-8")
        )["digest"]

        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            fake_oras = directory / "oras"
            fake_oras.write_text(
                "#!/bin/sh\n"
                "printf '%s\\n' \"$*\" >>\"${ORAS_LOG}\"\n"
                "case \"$1\" in\n"
                "  cp) exit 0 ;;\n"
                "  resolve) printf '%s\\n' \"${EXPECTED_RESOLVE}\" ;;\n"
                "  *) exit 2 ;;\n"
                "esac\n",
                encoding="utf-8",
            )
            fake_oras.chmod(0o755)

            def run_case(
                *, package_exists: str, tag_exists: str,
                allow_missing: str, existing_digest: str = "",
            ) -> tuple[subprocess.CompletedProcess[str], str]:
                log = directory / "oras.log"
                log.unlink(missing_ok=True)
                output = directory / "github-output"
                output.unlink(missing_ok=True)
                environment = os.environ.copy()
                environment.update(
                    {
                        "ALLOW_MISSING_PACKAGE": allow_missing,
                        "EXISTING_DIGEST": existing_digest,
                        "EXPECTED_RESOLVE": expected,
                        "GITHUB_OUTPUT": str(output),
                        "ORAS_LOG": str(log),
                        "PACKAGE_EXISTS": package_exists,
                        "TAG_EXISTS": tag_exists,
                    }
                )
                result = subprocess.run(
                    [
                        "bash", "-euo", "pipefail", "-c",
                        script.replace("build/oras/oras", str(fake_oras)),
                    ],
                    cwd=ROOT,
                    env=environment,
                    check=False,
                    capture_output=True,
                    text=True,
                )
                return result, log.read_text() if log.exists() else ""

            result, log = run_case(
                package_exists="false", tag_exists="false", allow_missing="false"
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("use the documented recovery path", result.stderr)
            self.assertEqual(log, "")

            result, log = run_case(
                package_exists="false", tag_exists="false", allow_missing="true"
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("package is missing after bootstrap", result.stderr)
            self.assertEqual(log, "")

            result, log = run_case(
                package_exists="true", tag_exists="false", allow_missing="false"
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(log.count("cp --from-oci-layout"), 1)
            self.assertEqual(
                log.count("resolve ghcr.io/atrinik/classic-dependencies:"), 1
            )

            result, log = run_case(
                package_exists="true",
                tag_exists="true",
                allow_missing="false",
                existing_digest=expected,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotIn("cp --from-oci-layout", log)
            self.assertEqual(
                log.count("resolve ghcr.io/atrinik/classic-dependencies:"), 1
            )

            result, log = run_case(
                package_exists="true",
                tag_exists="true",
                allow_missing="false",
                existing_digest="sha256:" + "0" * 64,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("exists at sha256:", result.stderr)
            self.assertEqual(log, "")

            result, log = run_case(
                package_exists="false", tag_exists="true", allow_missing="true"
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("invalid material-tag audit result", result.stderr)
            self.assertEqual(log, "")

    def test_release_rebinds_current_main_after_waiting_for_bundle(self) -> None:
        workflow = self.text("release.yml")
        bundle = workflow.index("Require the exact durable dependency bundle")
        rebind = workflow.index("Rebind release mutation to current main")
        pending = workflow.index("Detect an incomplete semantic release")
        self.assertLess(bundle, rebind)
        self.assertLess(rebind, pending)
        rebind_step = workflow[rebind:pending]
        self.assertIn("git fetch --no-tags origin main", rebind_step)
        self.assertIn('refs/remotes/origin/main', rebind_step)
        self.assertIn('test "${commit}" = "${RELEASE_TRIGGER_SHA}"', rebind_step)

    def test_dependency_bundle_install_fails_before_pull_without_attestation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            binaries = Path(temporary)
            (binaries / "gh").write_text(
                "#!/usr/bin/env sh\nexit 42\n", encoding="utf-8"
            )
            docker_marker = binaries / "docker-called"
            (binaries / "docker").write_text(
                f"#!/usr/bin/env sh\ntouch '{docker_marker}'\nexit 0\n",
                encoding="utf-8",
            )
            (binaries / "gh").chmod(0o755)
            (binaries / "docker").chmod(0o755)
            environment = os.environ.copy()
            environment.update(
                {
                    "GITHUB_REPOSITORY": "atrinik/classic",
                    "PATH": f"{binaries}:{environment['PATH']}",
                }
            )
            result = subprocess.run(
                ["tools/release/install_dependency_bundle.sh"],
                cwd=ROOT,
                env=environment,
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 42)
            self.assertFalse(docker_marker.exists())

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
        preflight_job = package[
            package.index("  preflight:") : package.index("  discord-config:")
        ]
        candidate_job = package[
            package.index("  candidate:") : package.index("  publish:")
        ]
        metadata_job = candidate[
            candidate.index("  metadata:") : candidate.index("  sources:")
        ]
        self.assertIn("contents: write", preflight_job)
        self.assertIn("Validate tag, release, checks, and ancestry", preflight_job)
        self.assertNotIn("gh release edit", preflight_job)
        for job in (candidate_job, metadata_job):
            self.assertIn("contents: read", job)
            self.assertNotIn("contents: write", job)
        self.assertEqual(package[: package.index("jobs:")].count("contents: read"), 1)

    def test_current_main_preflight_uses_the_trusted_recovery_verifier(self) -> None:
        workflow = self.text("package-release.yml")
        preflight_job = workflow[
            workflow.index("  preflight:") : workflow.index("  discord-config:")
        ]
        self.assertIn("Check out the current main recovery definition", preflight_job)
        self.assertIn("fetch-depth: 0", preflight_job)
        self.assertIn("ref: main", preflight_job)
        self.assertIn('test "${GITHUB_REF}" = refs/heads/main', preflight_job)
        self.assertIn("recovery_arguments+=(--recovery-main)", preflight_job)
        self.assertIn("python3 tools/release/validate_release.py", preflight_job)

    def test_semantic_release_skips_cross_repository_issue_comments(self) -> None:
        config = (ROOT / ".releaserc.cjs").read_text(encoding="utf-8")
        workflow = self.text("release.yml")
        self.assertIn("failCommentCondition: false", config)
        self.assertIn("successCommentCondition: false", config)
        self.assertNotIn("issues: write", workflow)
        self.assertNotIn("pull-requests: write", workflow)

    def test_semantic_release_resolves_one_pending_draft_before_analysis(self) -> None:
        workflow = self.text("release.yml")
        detection = workflow.index("id: pending-release")
        recovery = workflow.index("Resume the incomplete package release")
        deletion = workflow.index("Delete the policy-listed empty failed draft")
        analysis = workflow.index("Analyze commits, tag, and publish release notes")
        self.assertLess(detection, recovery)
        self.assertLess(detection, deletion)
        self.assertLess(recovery, analysis)
        self.assertLess(deletion, analysis)
        self.assertIn("tools/release/resolve_pending_release.py", workflow)
        self.assertIn("steps.pending-release.outputs.action == 'resume'", workflow)
        self.assertIn(
            "steps.pending-release.outputs.action == 'delete-empty-draft'", workflow
        )
        self.assertEqual(workflow.count("tools/release/resolve_pending_release.py"), 2)
        delete_step = workflow[deletion:recovery]
        self.assertIn('RELEASE_TAG: ${{ steps.pending-release.outputs.tag }}', delete_step)
        self.assertIn("--delete-policy-listed-empty-draft", delete_step)
        self.assertIn('--expected-tag "${RELEASE_TAG}"', delete_step)
        self.assertIn('--expected-release-id "${RELEASE_ID}"', delete_step)
        self.assertNotIn("gh api --method DELETE", delete_step)
        self.assertIn("--ref main", workflow)
        self.assertIn("if: steps.pending-release.outputs.action != 'resume'", workflow)

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

    def test_release_mutations_share_one_concurrency_namespace(self) -> None:
        release = self.text("release.yml")
        package = self.text("package-release.yml")
        recovery = self.text("recover-release.yml")
        candidate = self.text("build-release-candidate.yml")
        self.assertIn("group: classic-release-publication", release)
        self.assertIn("group: classic-release-publication", package)
        self.assertIn("group: classic-release-publication", recovery)
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
        self.assertIn("contents: write", promoter)
        self.assertIn("Check out the trusted promotion verifier", promoter)
        self.assertIn("path: build/release-automation", promoter)
        self.assertIn("ref: ${{ github.sha }}", promoter)
        self.assertIn(
            "build/release-automation/tools/release/locked_inputs.py", promoter
        )
        self.assertIn('--root "${GITHUB_WORKSPACE}"', promoter)
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
        self.assertIn("Reconcile GitHub's latest release designation", promoter)
        self.assertIn("-f make_latest=true", promoter)
        self.assertIn("--tag latest", promoter)
        resolver = (ROOT / "tools" / "release" / "check_latest_release.py").read_text(
            encoding="utf-8"
        )
        self.assertIn("--paginate", resolver)
        self.assertIn('"--jq"', resolver)
        self.assertIn('".[]"', resolver)
        self.assertNotIn("--slurp", resolver)
        self.assertIn("/releases?per_page=100", resolver)

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

    def test_server_validation_exercises_release_ndebug(self) -> None:
        runner = (ROOT / "tools" / "ci" / "run_linux_check.sh").read_text(
            encoding="utf-8"
        )
        server = runner[runner.index("  server)") : runner.index("  client)")]
        coverage = server.index("cmake --preset linux-coverage")
        release = server.index("cmake --preset linux-release")
        sanitizers = server.index("cmake --preset linux-sanitizers")
        self.assertLess(coverage, release)
        self.assertLess(release, sanitizers)
        self.assertIn("cmake --build --preset linux-release --parallel", server)
        for preset in ("linux-coverage", "linux-release", "linux-sanitizers"):
            self.assertIn(f"ctest --preset {preset} --parallel 4", server)
            self.assertIn(f"ctest --preset {preset} --parallel 4 -LE performance", server)

        presets = json.loads((ROOT / "server/CMakePresets.json").read_text())
        self.assertEqual(
            {
                preset["name"]: preset["execution"]["jobs"]
                for preset in presets["testPresets"]
            },
            {
                "linux-debug": 4,
                "linux-sanitizers": 4,
                "linux-coverage": 4,
                "linux-release": 4,
            },
        )

        cmake = (ROOT / "server/CMakeLists.txt").read_text()
        self.assertNotIn("RESOURCE_LOCK server-test-runtime", cmake)
        self.assertIn("tools/run_isolated_test.py", cmake)
        self.assertIn("-fprofile-update=atomic", cmake)

        migration = (
            ROOT / "server/src/tests/assetspath_migration.py"
        ).read_text()
        self.assertNotIn("timeout=15", migration)
        self.assertEqual(
            migration.count("timeout=SERVER_TIMEOUT_SECONDS"), 3
        )

    def test_core_validation_runs_libatrinik_sanitizers(self) -> None:
        runner = (ROOT / "tools" / "ci" / "run_linux_check.sh").read_text(
            encoding="utf-8"
        )
        core = runner[runner.index("  core)") : runner.index("  server)")]
        self.assertIn("cmake --preset linux-sanitizers", core)
        self.assertIn("cmake --build --preset linux-sanitizers --parallel", core)
        self.assertIn("ctest --preset linux-sanitizers", core)
        self.assertIn("ASAN_OPTIONS=detect_leaks=0:halt_on_error=1", core)
        self.assertIn("UBSAN_OPTIONS=halt_on_error=1", core)

        presets = json.loads((ROOT / "libatrinik/CMakePresets.json").read_text())
        for section in ("configurePresets", "buildPresets", "testPresets"):
            self.assertIn(
                "linux-sanitizers",
                {preset["name"] for preset in presets[section]},
            )

    def test_component_validation_compares_conventional_and_pch_builds(self) -> None:
        runner = (ROOT / "tools" / "ci" / "run_linux_check.sh").read_text(
            encoding="utf-8"
        )
        server = runner[runner.index("  server)") : runner.index("  client)")]
        client_start = runner.index("  client)")
        client = runner[client_start : runner.index("  client-benchmark)", client_start)]
        for name, component in (("server", server), ("client", client)):
            with self.subTest(component=name):
                coverage = component.index("cmake --preset linux-coverage")
                release = component.index("cmake --preset linux-release")
                sanitizers = component.index("cmake --preset linux-sanitizers")
                self.assertLess(coverage, release)
                self.assertLess(release, sanitizers)
                self.assertIn("-DENABLE_PRECOMPILED_HEADERS=OFF", component)
                self.assertEqual(
                    component.count("-DENABLE_PRECOMPILED_HEADERS=OFF"), 1
                )
                self.assertIn("ctest --preset linux-coverage", component)
                self.assertIn("ctest --preset linux-release", component)
                self.assertIn("ctest --preset linux-sanitizers", component)

    def test_movement_regression_compares_immutable_baselines_across_contracts(self) -> None:
        runner = (ROOT / "tools" / "ci" / "run_linux_check.sh").read_text(
            encoding="utf-8"
        )
        client_start = runner.index("  client-benchmark)")
        client = runner[client_start : runner.index("esac", client_start)]

        self.assertIn("movement_contract_paths=(", client)
        self.assertIn("movement_rendering_paths=(", client)
        for path in (
            "client/src/client/animations.c",
            "client/src/client/image.c",
            "client/src/client/lighting.c",
            "client/src/client/sprite.c",
            "client/src/client/texture.c",
            "client/src/client/video.c",
            "client/src/gui/misc/effects.c",
            "client/src/gui/toolkit/surface_primitives.c",
            "client/src/gui/widgets/map.c",
            "client/src/gui/widgets/minimap.c",
            "client/src/gui/widgets/render_profiler.c",
        ):
            self.assertIn(path, client)
        self.assertIn('"${movement_rendering_paths[@]}"', client)
        self.assertIn(
            '"${benchmark_base_sha}:client/tools/movement_benchmark_schema.py"',
            client,
        )
        self.assertIn(
            "comparison_note=bootstrap-base-missing-movement-instrumentation",
            client,
        )
        self.assertIn(
            "comparison_note=baseline-movement-schema-mismatch",
            client,
        )
        mismatch = client.index("comparison_note=baseline-movement-schema-mismatch")
        normal = client.index(
            "comparison_note=performance-calibration-pending-sibling-integration",
            mismatch,
        )
        self.assertIn("movement_action=compare", client[mismatch:normal])
        self.assertIn("baseline_needed=true", client[mismatch:normal])
        self.assertIn('--comparison-note "${comparison_note}"', client)
        self.assertIn("--informational-performance", client)
        self.assertIn('--baseline-schema "${movement_baseline_schema}"', client)
        self.assertIn('cp -- "${baseline_root}/client/tools/movement_benchmark_schema.py"', client)
        self.assertIn("movement_event_name} == merge_group", client)
        self.assertIn("the full movement matrix is candidate-only", client)
        self.assertEqual(client.count("--discrete-manifest"), 2)
        self.assertIn("movement_matrix_arguments+=(--full-matrix)", client)
        self.assertIn("lighting-regression-generation-failed", client)
        self.assertIn(
            "lighting-base-missing-benchmark-instrumentation",
            client,
        )
        self.assertNotIn("lighting_bootstrap", client)
        self.assertNotIn("instrumentation_patch", client)
        self.assertNotIn('git -C "${baseline_root}" apply', client)
        self.assertIn(
            'git -C "${baseline_root}" diff --quiet HEAD --',
            client,
        )

    def test_timed_pr_benchmarks_are_explicit_isolated_and_fork_safe(self) -> None:
        check = self.text("check.yml")
        client = check[
            check.index("  client:\n    name: Client validation") : check.index(
                "  integrated:"
            )
        ]
        self.assertNotIn("ATRINIK_BENCHMARK", client)
        self.assertNotIn("movement-comment.md", check)
        self.assertNotIn("movement-regression-comment", check)
        self.assertIn("linux-client-ccache-${{ github.run_attempt }}", client)
        self.assertIn("path: build/ci-evidence/ccache-client.tsv", client)
        for runner_identity in ("CI", "ImageOS", "ImageVersion", "RUNNER_ARCH", "RUNNER_OS"):
            self.assertIn(f"--env {runner_identity} \\", client)
        self.assertNotIn("schedule", check[: check.index("jobs:")])
        self.assertIn("  workflow_dispatch:\n", check[: check.index("jobs:")])
        self.assertIn(
            "github.event_name == 'workflow_dispatch' && github.run_id || github.ref",
            check[: check.index("jobs:")],
        )

        workflow = self.text("pr-benchmarks.yml")
        triggers = workflow[: workflow.index("jobs:")]
        for action in ("opened", "reopened", "synchronize", "labeled", "unlabeled"):
            self.assertIn(action, triggers)
        self.assertNotIn("pull_request_target", workflow)
        self.assertIn("contains(github.event.pull_request.labels.*.name, 'ci: benchmark')", workflow)
        self.assertIn("benchmark_client=false", workflow)
        self.assertIn("benchmark_server=false", workflow)
        self.assertIn("event_relevant=false", workflow)
        self.assertIn("unrelated label transition was ignored", workflow)
        self.assertIn("github.run_id || github.event.pull_request.number", workflow)
        self.assertIn("no benchmark-sensitive path changed", workflow)
        self.assertIn("tools/ci/run_linux_check.sh client-benchmark", workflow)
        self.assertIn("tools/ci/run_linux_check.sh server-benchmark", workflow)
        self.assertIn("github.event.pull_request.base.sha", workflow)
        self.assertIn("github.event.pull_request.head.sha", workflow)
        self.assertIn("--network none", workflow)
        self.assertIn("persist-credentials: false", workflow)
        self.assertIn("if: always()", workflow)
        aggregate = check[check.index("  classic-validation:") :]
        self.assertNotIn("benchmark", aggregate.lower())
        self.assertIn("name: Classic validation", aggregate)
        self.assertIn("CHANGES_RESULT: ${{ needs.changes.result }}", workflow)
        self.assertIn("classification finished with", workflow)
        comment = workflow[workflow.index("  movement-comment:") :]
        self.assertIn(
            "github.event.pull_request.head.repo.full_name == github.repository",
            comment,
        )
        self.assertIn("      actions: read\n      pull-requests: write\n", comment)
        self.assertNotIn("contents: write", comment)
        self.assertNotIn("id-token: write", comment)
        self.assertIn("continue-on-error: true", comment)
        self.assertIn("Publish one bounded pre-rendered summary comment", comment)
        self.assertIn("evidence/movement-comment.md", comment)
        self.assertIn("test \"$(wc -c <evidence/movement-comment.md)\" -le 65536", comment)
        self.assertIn("--paginate", comment)
        self.assertNotIn("--slurp", comment)
        self.assertIn("sed -n '1p'", comment)

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
        digest = "d1f082eb28891600a9cf018a1d4310b9f3e1f985f82139fa48fbd4ac77b623bb"
        image = f"ghcr.io/atrinik/windows-build:1.2.1@sha256:{digest}"
        self.assertEqual(
            candidate.count(f"WINDOWS_BUILD_CACHE_EPOCH: 1.2.1-{digest}"), 1
        )
        self.assertEqual(candidate.count(image), 4)
        self.assertNotIn("ghcr.io/atrinik/windows-build:1.0.5", candidate)
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

    def test_discord_application_id_is_release_only_package_data(self) -> None:
        package = self.text("package-release.yml")
        candidate = self.text("build-release-candidate.yml")
        config = package[
            package.index("  discord-config:") : package.index("  candidate:")
        ]
        candidate_job = package[
            package.index("  candidate:") : package.index("  publish:")
        ]
        client = candidate[
            candidate.index("  client-windows:") : candidate.index("  server-windows:")
        ]
        cmake = (ROOT / "client" / "CMakeLists.txt").read_text(encoding="utf-8")
        package_script = (ROOT / "client" / "tools" / "build-windows-package.sh").read_text(
            encoding="utf-8"
        )
        self.assertIn("if: inputs.candidate_run_id == ''", config)
        self.assertIn("needs: preflight", config)
        self.assertIn("environment: discord-release", config)
        self.assertIn("secrets.DISCORD_APPLICATION_ID", config)
        self.assertIn("umask 077", config)
        self.assertNotIn("echo", config)
        self.assertIn("retention-days: 1", config)
        artifact_name = "name: discord-application-id-${{ inputs.tag }}"
        self.assertEqual(config.count(artifact_name), 1)
        self.assertEqual(client.count(artifact_name), 1)
        self.assertIn("needs: [preflight, discord-config]", candidate_job)
        self.assertIn("needs.discord-config.result == 'success'", candidate_job)
        self.assertNotIn("environment: discord-release", candidate)
        self.assertNotIn("secrets.DISCORD_APPLICATION_ID", candidate)
        self.assertIn("ATRINIK_DISCORD_APPLICATION_ID_FILE", client)
        self.assertIn("/workspace/build/discord-config/discord-application-id", client)
        self.assertIn('PATTERN "discord-application-id" EXCLUDE', cmake)
        self.assertIn("18446744073709551615", cmake)
        self.assertIn("if: inputs.rehearsal != true", client)
        self.assertIn(
            'discord_config_file=${ATRINIK_DISCORD_APPLICATION_ID_FILE:-}',
            package_script,
        )
        self.assertIn(
            '"-DATRINIK_DISCORD_APPLICATION_ID_FILE=${discord_config_file}"',
            package_script,
        )

    def test_only_release_metadata_checkouts_require_full_history(self) -> None:
        candidate = self.text("build-release-candidate.yml")
        metadata_job = candidate[
            candidate.index("  metadata:") : candidate.index("  sources:")
        ]
        remaining_jobs = candidate[candidate.index("  sources:") :]
        self.assertEqual(candidate.count("fetch-depth: 0"), 1)
        self.assertEqual(metadata_job.count("fetch-depth: 0"), 1)
        self.assertNotIn("fetch-depth: 0", remaining_jobs)

    def test_core_uploads_exercised_python_release_tool_coverage(self) -> None:
        workflow = self.text("check.yml")
        core = workflow[workflow.index("  core:") : workflow.index("  windows-test-build:")]
        self.assertIn("python3 -m pip install coverage==7.15.3", core)
        self.assertIn("python3 -m coverage run --branch --source=tools", core)
        self.assertIn("python3 -m coverage run --append --branch --source=tools", core)
        self.assertIn("python3 -m coverage xml --omit='tools/tests/*'", core)
        tools_upload = core[
            core.index("- name: Upload release-tool coverage") : core.index(
                "- name: Upload libatrinik coverage"
            )
        ]
        libatrinik_upload = core[core.index("- name: Upload libatrinik coverage") :]
        self.assertIn("files: tools/coverage.xml", tools_upload)
        self.assertIn("flags: release-tools", tools_upload)
        self.assertNotIn("libatrinik/coverage.xml", tools_upload)
        self.assertIn("files: libatrinik/coverage.xml", libatrinik_upload)
        self.assertIn("flags: libatrinik", libatrinik_upload)
        self.assertNotIn("tools/coverage.xml", libatrinik_upload)

    def test_native_worldmaker_build_uses_the_server_compiler_cache(self) -> None:
        script = (ROOT / "server" / "tools" / "build-windows-package.sh").read_text(
            encoding="utf-8"
        )
        self.assertIn("command -v ccache", script)
        self.assertIn("-DCMAKE_C_COMPILER_LAUNCHER=", script)

    def test_native_windows_security_tests_are_cross_built_and_run_on_windows(self) -> None:
        workflow = self.text("check.yml")
        build = workflow[
            workflow.index("  windows-test-build:") : workflow.index("  windows-test:")
        ]
        server_build = build[build.index("      - name: Cross-build Windows server") :]
        run_start = workflow.index("  windows-test:")
        run = workflow[run_start : workflow.index("  server:", run_start)]
        aggregate = workflow[workflow.index("  classic-validation:") :]
        digest = "d1f082eb28891600a9cf018a1d4310b9f3e1f985f82139fa48fbd4ac77b623bb"
        image = f"ghcr.io/atrinik/windows-build:1.2.1@sha256:{digest}"

        self.assertIn("if: needs.changes.outputs.windows == 'true'", build)
        self.assertEqual(build.count(image), 4)
        self.assertEqual(build.count(digest), 4)
        self.assertNotIn("ghcr.io/atrinik/windows-build:1.0.5", build)
        self.assertEqual(build.count("--network none"), 3)
        self.assertIn("persist-credentials: false", build)
        self.assertIn("Verify and stage deterministic package inputs offline", build)
        self.assertIn("bundle-verify", build)
        self.assertIn("server/tools/dependencies.py source", build)
        self.assertIn("--source-lock server/cmake/immutable_sources.lock.json", build)
        self.assertGreaterEqual(build.count("--offline"), 3)
        self.assertNotIn("curl --fail --location --proto '=https'", build)
        self.assertIn("--env CCACHE_DIR=/tmp/atrinik-libatrinik-ccache", build)
        self.assertIn("-S server", build)
        self.assertIn("-B server/build/windows-pr-server", build)
        self.assertIn("-DENABLE_PYTHON_PLUGIN=OFF", build)
        self.assertIn("--target atrinik-server", build)
        self.assertIn("--network none", server_build)
        self.assertIn("--env GH_TOKEN=", server_build)
        self.assertIn("--env GITHUB_TOKEN=", server_build)
        self.assertNotIn("-DPATCH_EXECUTABLE=", server_build)
        self.assertIn(
            "-DFETCHCONTENT_SOURCE_DIR_LIBPCPNATPMP=/workspace/build/libpcpnatpmp-mingw-source",
            server_build,
        )
        self.assertLess(
            server_build.index("x86_64-w64-mingw32.shared-cmake"),
            server_build.index("cmake --build server/build/windows-pr-server"),
        )
        self.assertIn(
            "--env CCACHE_TEMPDIR=/tmp/atrinik-libatrinik-ccache-tmp", build
        )
        self.assertIn("--env CCACHE_MAXSIZE=250M", build)
        self.assertNotIn("--env CCACHE_DIR=/opt/mxe", build)
        self.assertIn("-DBUILD_TESTING=ON", build)
        self.assertIn("python3 client/tools/dependencies.py sync --root client", build)
        self.assertIn("python3 server/tools/dependencies.py sync --root server", build)
        self.assertIn("bash tools/build-windows-package.sh", build)
        self.assertIn("ATRINIK_DISCORD_APPLICATION_ID_FILE", build)
        self.assertIn("/data/discord-application-id", build)
        self.assertIn("not in archive.read(executable)", build)
        self.assertIn(
            "--target libatrinik-path libatrinik-rendezvous "
            "libatrinik-signals \\",
            build,
        )
        self.assertIn("libatrinik-metaserver-publisher \\", build)
        self.assertIn(
            "libatrinik-metaserver-url libatrinik-socket-address "
            "libatrinik-socket-quic \\",
            build,
        )
        self.assertIn("libatrinik-stun \\", build)
        self.assertIn(
            "libatrinik/build/windows-tests/libatrinik-socket-address.exe", build
        )
        self.assertIn(
            "libatrinik/build/windows-tests/libatrinik-socket-quic.exe", build
        )
        self.assertIn(
            "libatrinik/build/windows-tests/libatrinik-stun.exe", build
        )
        self.assertIn(
            "libatrinik/build/windows-tests/libatrinik-signals.exe", build
        )
        self.assertIn("client-rich-presence-tests.exe", build)
        self.assertIn("python3 tools/ci/stage_windows_runtime.py", build)
        self.assertIn("actions/upload-artifact@043fb46d1a93c77aae656e7c1c64a875d1fc6a0a", build)
        self.assertIn("Build portable Windows server package", build)
        self.assertIn("bash tools/build-windows-package.sh build/windows-pr-package", build)
        self.assertIn(
            "--env ATRINIK_DEPENDENCY_DOWNLOADS=/workspace/build/dependency-inputs/downloads",
            build,
        )
        self.assertIn(
            "--env ATRINIK_DEPENDENCY_CACHE_DIR=/workspace/build/dependency-source-cache",
            build,
        )
        self.assertIn("smoke_windows_server_package.ps1", build)
        self.assertIn("server/build/windows-pr-package/*.zip", build)

        self.assertIn("runs-on: windows-2025", run)
        self.assertIn("actions/download-artifact@3e5f45b2cfb9172054b4087a40e8e0b5a5461e7c", run)
        self.assertIn('"libatrinik-path.exe"', run)
        self.assertIn("New-Item -ItemType Junction", run)
        self.assertIn('"libatrinik-path.exe") $junction', run)
        self.assertIn('"libatrinik-rendezvous.exe"', run)
        self.assertIn('"libatrinik-signals.exe"', run)
        self.assertIn('"libatrinik-signals-test-traceback-*.txt"', run)
        self.assertIn('"Expected exactly one traceback block"', run)
        self.assertIn("Start-Process", run)
        self.assertIn('"C0000005"', run)
        self.assertIn("--handled-exception", run)
        self.assertIn("--exception-guard", run)
        self.assertIn("$originalAppData", run)
        self.assertIn("Remove-Item Env:APPDATA -ErrorAction SilentlyContinue", run)
        self.assertIn("Remove-Item Env:APPDATA", run)
        self.assertIn('"Exception code: 0xc0000005"', run)
        self.assertIn('"Access type: write (1)"', run)
        self.assertIn('"Exception module base:"', run)
        self.assertIn('"Exception module name:"', run)
        self.assertIn("libatrinik-signals\\.exe", run)
        self.assertIn("$orderedFields", run)
        self.assertIn('"libatrinik-metaserver-publisher.exe"', run)
        self.assertIn('"libatrinik-metaserver-url.exe"', run)
        self.assertIn('"libatrinik-socket-address.exe"', run)
        self.assertIn('"libatrinik-socket-quic.exe"', run)
        self.assertIn('"libatrinik-stun.exe"', run)
        self.assertIn('"client-rich-presence-tests.exe"', run)
        self.assertIn('"atrinik-classic-server-*-windows-x86_64.zip"', run)
        self.assertIn('"smoke_windows_server_package.ps1"', run)
        smoke = (ROOT / "tools" / "ci" / "smoke_windows_server_package.ps1").read_text(
            encoding="utf-8"
        )
        self.assertIn('"maps/regions.reg"', smoke)
        shutdown_loop = smoke[
            smoke.index("$shutdownDeadline =") : smoke.index(
                'if ($process.ExitCode -ne 0)', smoke.index("$shutdownDeadline =")
            )
        ]
        self.assertIn("AddSeconds(30)", shutdown_loop)
        self.assertIn("while (-not $process.HasExited", shutdown_loop)
        self.assertIn('$process.StandardInput.WriteLine("shutdown")', shutdown_loop)
        self.assertIn("$process.StandardInput.Flush()", shutdown_loop)
        self.assertIn("$process.WaitForExit(1000)", shutdown_loop)
        self.assertIn("$shutdownAttempts", shutdown_loop)
        self.assertIn("$shutdownTimedOut", shutdown_loop)
        self.assertIn("$process.Kill($true)", shutdown_loop)
        self.assertLess(
            smoke.index("$process.StandardOutput.ReadToEndAsync()"),
            smoke.index("$shutdownDeadline ="),
        )
        self.assertLess(
            smoke.index("$remainderTask.Result"),
            smoke.index('"Packaged server did not shut down after $shutdownAttempts "'),
        )
        self.assertIn('"--port_mapping=off"', smoke)
        self.assertIn('"--stun_server=off"', smoke)
        self.assertIn('[System.Net.Sockets.UdpClient]::new(0)', smoke)
        self.assertIn('"--port_quic=$serverPort"', smoke)
        self.assertIn('"--network_stack=ipv4=127.0.0.1"', smoke)
        self.assertIn('[void]$startInfo.Environment.Remove($name)', smoke)
        self.assertIn('$startInfo.Environment["NO_PROXY"]', smoke)
        self.assertIn(
            '"--metaserver_publish_origin=http://127.0.0.1:9"', smoke
        )
        self.assertIn(
            '"--metaserver_rendezvous_origin=http://127.0.0.1:9/v1/classic"',
            smoke,
        )
        self.assertIn("Get-NetUDPEndpoint -LocalPort $serverPort", smoke)
        self.assertIn('$listenerEndpoints[0].LocalAddress -ne "127.0.0.1"', smoke)
        self.assertIn('if ($output -match "Discovered a direct")', smoke)
        self.assertIn("$bodySucceeded -and $cleanupFailures.Count -ne 0", smoke)
        self.assertIn('$process.StandardInput.WriteLine("shutdown")', smoke)
        self.assertLess(
            smoke.index('"Server ready\\. Waiting for connections"'),
            smoke.index('$process.StandardInput.WriteLine("shutdown")'),
        )
        self.assertIn("AddSeconds(60)", smoke)
        self.assertNotIn("WaitForExit(30000)", smoke)
        self.assertIn("$remainderTask.Wait(10000)", smoke)
        self.assertIn("WaitForExit(10000)", smoke)
        self.assertIn("$process.Kill($true)", smoke)
        self.assertIn("$process.Dispose()", smoke)
        self.assertIn('"Server ready\\. Waiting for connections"', smoke)
        self.assertIn('"fixtures/metaserver-publisher-v1.json"', run)

        self.assertIn("- windows-test", aggregate)
        self.assertIn("--windows-required", aggregate)
        self.assertIn("--windows-result", aggregate)

    def test_check_stages_one_verified_bundle_for_offline_consumers(self) -> None:
        workflow = self.text("check.yml")
        staging = workflow[
            workflow.index("  dependency-inputs:") : workflow.index("  core:")
        ]
        self.assertIn("name: Verified dependency inputs", staging)
        self.assertIn("bundle-key", staging)
        self.assertIn("bundle-stage", staging)
        for material in (
            "client/dependencies.lock.json",
            "server/dependencies.lock.json",
            "server/cmake/immutable_sources.lock.json",
        ):
            self.assertEqual(staging.count(material), 2)
        self.assertIn("actions/cache/restore@", staging)
        self.assertIn("actions/cache/save@", staging)
        self.assertIn("classic-dependency-archives-v1-", staging)
        self.assertIn("steps.dependency-key.outputs.digest", staging)
        self.assertIn("github.run_id", staging)
        self.assertIn("github.run_attempt", staging)
        self.assertIn("steps.dependency-stage.outputs.cache_changed == 'true'", staging)
        self.assertIn("restore-keys:", staging)
        self.assertIn("name: classic-dependency-inputs", staging)
        self.assertIn("retention-days: 1", staging)
        self.assertNotIn("contents: write", staging)

        core = workflow[
            workflow.index("  core:") : workflow.index("  windows-test-build:")
        ]
        self.assertIn("needs: dependency-inputs", core)
        self.assertIn("--network none", core)

        consumers = {
            "windows": workflow[
                workflow.index("  windows-test-build:") : workflow.index(
                    "  windows-test:"
                )
            ],
            "server": workflow[
                workflow.index("  server:\n    name: Server validation") : workflow.index(
                    "  client:\n    name: Client validation"
                )
            ],
            "client": workflow[
                workflow.index("  client:\n    name: Client validation") : workflow.index(
                    "  integrated:\n    name: Integrated client/server graph"
                )
            ],
            "integrated": workflow[
                workflow.index("  integrated:\n    name: Integrated client/server graph") : workflow.index(
                    "  classic-validation:"
                )
            ],
        }
        for name, job in consumers.items():
            with self.subTest(name=name):
                self.assertIn("dependency-inputs", job)
                self.assertIn("name: classic-dependency-inputs", job)
                self.assertIn("path: build/dependency-inputs", job)
                self.assertIn("--network none", job)
                self.assertNotRegex(job, r"\b(curl|wget)\b")

        runner = (ROOT / "tools" / "ci" / "run_linux_check.sh").read_text(
            encoding="utf-8"
        )
        self.assertIn("bundle-verify", runner)
        self.assertEqual(runner.count("--offline"), 8)
        self.assertIn('build/dependency-source-cache', runner)
        self.assertIn('--downloads "${dependency_downloads}"', runner)
        self.assertIn('"${baseline_root}/client/dependencies.lock.json"', runner)
        self.assertIn("FETCHCONTENT_SOURCE_DIR_LIBPCPNATPMP", runner)
        aggregate = workflow[workflow.index("  classic-validation:") :]
        self.assertIn("- dependency-inputs", aggregate)
        self.assertIn("--dependency-inputs-result", aggregate)

    def test_daily_report_stages_verified_inputs_and_keeps_timing_informational(
        self,
    ) -> None:
        workflow = self.text("daily-client-performance.yml")
        benchmark = workflow[
            workflow.index("  benchmark:") : workflow.index("  publish:")
        ]
        self.assertIn("ref: ${{ github.sha }}", benchmark)
        self.assertIn(
            'test "$(git rev-parse HEAD)" = "${GITHUB_SHA}"', benchmark
        )
        self.assertIn("bundle-key", benchmark)
        self.assertIn("bundle-stage", benchmark)
        self.assertIn("--output build/dependency-inputs", benchmark)
        self.assertIn("actions/cache/restore@", benchmark)
        self.assertIn("actions/cache/save@", benchmark)
        self.assertIn("steps.dependency-stage.outputs.cache_changed == 'true'", benchmark)
        self.assertIn("--network none", benchmark)
        self.assertIn("ATRINIK_MOVEMENT_EVENT_NAME: schedule", benchmark)
        self.assertIn("ATRINIK_MOVEMENT_MATRIX: full", benchmark)
        self.assertNotIn("continue-on-error: true", benchmark)
        self.assertIn("id: benchmark", benchmark)
        self.assertIn("CLIENT_RESULT: ${{ steps.benchmark.outcome }}", benchmark)
        self.assertIn('--client-result "${CLIENT_RESULT}"', benchmark)
        self.assertIn("path: build/ci-evidence", benchmark)
        self.assertIn(
            "daily-client-performance-${{ github.run_id }}-${{ github.run_attempt }}",
            benchmark,
        )

        runner = (ROOT / "tools" / "ci" / "run_linux_check.sh").read_text(
            encoding="utf-8"
        )
        start = runner.index("elif [[ ${movement_action} == candidate-only ]]")
        candidate_only = runner[start : runner.index("else\n      python3", start)]
        self.assertIn('--comparison-note "${comparison_note}"', candidate_only)
        self.assertIn("movement_matrix_arguments+=(--full-matrix)", candidate_only)

        publish = workflow[workflow.index("  publish:") :]
        self.assertIn("if: always() && github.ref == 'refs/heads/main'", publish)
        self.assertIn("contents: write", publish)
        self.assertIn("ref: ${{ github.sha }}", publish)
        self.assertIn(
            'test "$(git rev-parse HEAD)" = "${GITHUB_SHA}"', publish
        )
        self.assertIn(
            "daily-client-performance-${{ github.run_id }}-${{ github.run_attempt }}",
            publish,
        )
        self.assertNotIn("pull_request:", workflow[: workflow.index("jobs:")])
        self.assertIn(
            "The current compatible trend no longer meets the sustained regression condition.",
            publish,
        )

    def test_linux_checks_pin_image_and_isolate_compiler_caches(self) -> None:
        workflow = self.text("check.yml")
        digest = "d0ec0a31f97fa1d699f62b81bbe697d95b335f44f1c99fde8704dfc528e2102f"
        image = f"ghcr.io/atrinik/classic-build:1.2.3@sha256:{digest}"
        cache_action = "actions/cache@55cc8345863c7cc4c66a329aec7e433d2d1c52a9"

        self.assertEqual(workflow.count(image), 1)
        self.assertEqual(workflow.count(f"sha256:{digest}"), 2)
        self.assertEqual(workflow.count(cache_action), 4)
        self.assertEqual(workflow.count(f"actions/cache/restore@{cache_action.split('@')[1]}"), 1)
        self.assertEqual(workflow.count(f"actions/cache/save@{cache_action.split('@')[1]}"), 1)
        self.assertEqual(workflow.count("tools/ci/linux_cache_key.py"), 4)
        self.assertEqual(workflow.count("tools/ci/run_linux_check.sh"), 8)
        self.assertEqual(workflow.count("--env CCACHE_DIR=/cache/ccache"), 4)
        self.assertEqual(workflow.count("chmod 1777"), 4)
        self.assertIn("tools/ci/measure_linux_image.sh", workflow)
        self.assertNotIn("classic-client-sdl-mixer-ubuntu", workflow)
        self.assertNotIn("packages: read", workflow)
        self.assertNotIn("docker/login-action", workflow)
        self.assertNotIn("packages: read", self.text("codeql.yml"))

        core = workflow[
            workflow.index("  core:") : workflow.index("  windows-test-build:")
        ]
        server = workflow[
            workflow.index("  server:\n    name: Server validation") : workflow.index(
                "  client:\n    name: Client validation"
            )
        ]
        client = workflow[
            workflow.index("  client:\n    name: Client validation") : workflow.index(
                "  integrated:\n    name: Integrated client/server graph"
            )
        ]
        integrated = workflow[
            workflow.index("  integrated:\n    name: Integrated client/server graph") : workflow.index(
                "  classic-validation:\n    name: Classic validation"
            )
        ]
        expected_materials = {
            "core": {
                ".github/workflows/check.yml",
                "tools/ci/run_linux_check.sh",
                "protocol/CMakeLists.txt",
                "libatrinik/CMakeLists.txt",
                "libatrinik/CMakePresets.json",
            },
            "server": {
                ".github/workflows/check.yml",
                "tools/ci/run_linux_check.sh",
                "server/CMakeLists.txt",
                "server/CMakePresets.json",
            },
            "client": {
                ".github/workflows/check.yml",
                "tools/ci/run_linux_check.sh",
                "client/CMakeLists.txt",
                "client/CMakePresets.json",
            },
            "integrated": {
                ".github/workflows/check.yml",
                "tools/ci/run_linux_check.sh",
                "CMakeLists.txt",
                "CMakePresets.json",
                "protocol/CMakeLists.txt",
                "libatrinik/CMakeLists.txt",
                "client/CMakeLists.txt",
                "server/CMakeLists.txt",
            },
        }
        jobs = (
            (core, "core", True),
            (server, "server", True),
            (client, "client", True),
            (integrated, "integrated", False),
        )
        for job, component, uploads_coverage in jobs:
            with self.subTest(component=component):
                if uploads_coverage:
                    self.assertIn("id-token: write", job)
                self.assertIn("persist-credentials: false", job)
                self.assertIn(f"--component {component}", job)
                self.assertIn(f"classic-ccache/{component}", job)
                self.assertEqual(
                    job.count("--material tools/ci/run_linux_check.sh"), 1
                )
                for material in expected_materials[component]:
                    self.assertEqual(job.count(f"--material {material}"), 1)
                self.assertIn("restore-keys:", job)
                self.assertNotIn("apt-get", job)
                self.assertNotIn("ubuntu@sha256:", job)

        runner = (ROOT / "tools" / "ci" / "run_linux_check.sh").read_text(
            encoding="utf-8"
        )
        self.assertIn("-DCMAKE_C_COMPILER_LAUNCHER=ccache", runner)
        self.assertIn("-DCMAKE_CXX_COMPILER_LAUNCHER=ccache", runner)
        self.assertIn("CCACHE_BASEDIR", runner)
        self.assertIn("CCACHE_NOHASHDIR=true", runner)
        self.assertIn("ccache --zero-stats", runner)
        self.assertIn("ccache --print-stats", runner)
        self.assertIn("ccache --show-config", runner)
        self.assertIn("ccache --show-stats", runner)

        measurement = (ROOT / "tools" / "ci" / "measure_linux_image.sh").read_text(
            encoding="utf-8"
        )
        for field in (
            "runner_image_os",
            "runner_image_version",
            "runner_arch",
            "kernel",
            "cpu_count",
            "docker_client_version",
            "docker_server_version",
            "cold_pull_ms",
            "warm_pull_ms",
            "cold_start_ms",
            "warm_start_ms",
        ):
            with self.subTest(measurement_field=field):
                self.assertIn(f"printf '{field}\\t", measurement)


if __name__ == "__main__":
    unittest.main()
