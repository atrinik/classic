from __future__ import annotations

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


class WorkflowContractTests(unittest.TestCase):
    def text(self, name: str) -> str:
        return (ROOT / ".github" / "workflows" / name).read_text(encoding="utf-8")

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
        workflow = self.text("check.yml")
        server = workflow[
            workflow.index("  server:\n    name: Server validation") : workflow.index(
                "  client:\n    name: Client validation"
            )
        ]
        coverage = server.index("cmake --preset linux-coverage")
        release = server.index("cmake --preset linux-release")
        sanitizers = server.index("cmake --preset linux-sanitizers")
        self.assertLess(coverage, release)
        self.assertLess(release, sanitizers)
        self.assertIn("cmake --build --preset linux-release --parallel", server)
        self.assertIn("ctest --preset linux-release", server)

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
        self.assertEqual(candidate.count("fetch-depth: 0"), 2)
        self.assertEqual(metadata_job.count("fetch-depth: 0"), 2)
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
        self.assertEqual(build.count("--network none"), 2)
        self.assertIn("persist-credentials: false", build)
        self.assertIn("Stage pinned Windows server dependency", build)
        self.assertIn(
            "65ab99547ecc8277434527607d24f8a1b02a2344ed4cea475bed751606e60202",
            build,
        )
        self.assertIn("--env CCACHE_DIR=/tmp/atrinik-libatrinik-ccache", build)
        self.assertIn("-S server", build)
        self.assertIn("-B server/build/windows-pr-server", build)
        self.assertIn("-DENABLE_PYTHON_PLUGIN=OFF", build)
        self.assertIn("--target atrinik-server", build)
        self.assertIn("--network none", server_build)
        self.assertIn("--env GH_TOKEN=", server_build)
        self.assertIn("--env GITHUB_TOKEN=", server_build)
        self.assertIn("-DPATCH_EXECUTABLE=", server_build)
        self.assertIn(
            "-DSOURCE_DIR=/workspace/build/libpcpnatpmp-source", server_build
        )
        self.assertIn(
            "-DPATCH_FILE=/workspace/server/cmake/patches/libpcpnatpmp-mingw.patch",
            server_build,
        )
        self.assertIn(
            "-P /workspace/server/cmake/apply_patch_idempotent.cmake",
            server_build,
        )
        self.assertIn(
            "-DFETCHCONTENT_SOURCE_DIR_LIBPCPNATPMP=/workspace/build/libpcpnatpmp-source",
            server_build,
        )
        self.assertLess(
            server_build.index("-P /workspace/server/cmake/apply_patch_idempotent.cmake"),
            server_build.index("x86_64-w64-mingw32.shared-cmake"),
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
        self.assertIn("working-directory: client", build)
        self.assertIn("python3 tools/dependencies.py sync", build)
        self.assertIn("bash tools/build-windows-package.sh", build)
        self.assertIn("ATRINIK_DISCORD_APPLICATION_ID_FILE", build)
        self.assertIn("/data/discord-application-id", build)
        self.assertIn("not in archive.read(executable)", build)
        self.assertIn(
            "--target libatrinik-path libatrinik-rendezvous "
            "libatrinik-metaserver-publisher \\",
            build,
        )
        self.assertIn("libatrinik-metaserver-url libatrinik-stun \\", build)
        self.assertIn(
            "libatrinik/build/windows-tests/libatrinik-stun.exe", build
        )
        self.assertIn("client-rich-presence-tests.exe", build)
        self.assertIn("python3 tools/ci/stage_windows_runtime.py", build)
        self.assertIn("actions/upload-artifact@043fb46d1a93c77aae656e7c1c64a875d1fc6a0a", build)
        self.assertIn("Build portable Windows server package", build)
        self.assertIn("bash tools/build-windows-package.sh build/windows-pr-package", build)
        self.assertIn("smoke_windows_server_package.ps1", build)
        self.assertIn("server/build/windows-pr-package/*.zip", build)

        self.assertIn("runs-on: windows-2025", run)
        self.assertIn("actions/download-artifact@3e5f45b2cfb9172054b4087a40e8e0b5a5461e7c", run)
        self.assertIn('"libatrinik-path.exe"', run)
        self.assertIn("New-Item -ItemType Junction", run)
        self.assertIn('"libatrinik-path.exe") $junction', run)
        self.assertIn('"libatrinik-rendezvous.exe"', run)
        self.assertIn('"libatrinik-metaserver-publisher.exe"', run)
        self.assertIn('"libatrinik-metaserver-url.exe"', run)
        self.assertIn('"libatrinik-stun.exe"', run)
        self.assertIn('"client-rich-presence-tests.exe"', run)
        self.assertIn('"atrinik-classic-server-*-windows-x86_64.zip"', run)
        self.assertIn('"smoke_windows_server_package.ps1"', run)
        smoke = (ROOT / "tools" / "ci" / "smoke_windows_server_package.ps1").read_text(
            encoding="utf-8"
        )
        self.assertIn('"maps/regions.reg"', smoke)
        self.assertIn('"--port_mapping=off"', smoke)
        self.assertIn('"--stun_server=off"', smoke)
        self.assertIn('[System.Net.Sockets.UdpClient]::new(0)', smoke)
        self.assertIn('"--port_quic=$serverPort"', smoke)
        self.assertIn(
            '"--metaserver_publish_origin=http://127.0.0.1:9"', smoke
        )
        self.assertIn(
            '"--metaserver_rendezvous_origin=http://127.0.0.1:9/v1/classic"',
            smoke,
        )
        self.assertIn('$process.StandardInput.WriteLine("shutdown")', smoke)
        self.assertLess(
            smoke.index('"Server ready\\. Waiting for connections"'),
            smoke.index('$process.StandardInput.WriteLine("shutdown")'),
        )
        self.assertIn("AddSeconds(60)", smoke)
        self.assertIn("WaitForExit(30000)", smoke)
        self.assertIn("$remainderTask.Wait(10000)", smoke)
        self.assertIn("WaitForExit(10000)", smoke)
        self.assertIn("$process.Kill($true)", smoke)
        self.assertIn("$process.Dispose()", smoke)
        self.assertIn('"Server ready\\. Waiting for connections"', smoke)
        self.assertIn('"fixtures/metaserver-publisher-v1.json"', run)

        self.assertIn("- windows-test", aggregate)
        self.assertIn("--windows-required", aggregate)
        self.assertIn("--windows-result", aggregate)


if __name__ == "__main__":
    unittest.main()
