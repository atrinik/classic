"""Execute Package Release tag-binding run blocks with inert commands.

This proves the shell decisions in the checked-in steps, not GitHub's ref,
OIDC, or attestation services.
"""
from __future__ import annotations

from pathlib import Path
import re
import subprocess
import tempfile
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[2]
TAG = "v5.120.0"
TAG_COMMIT = "a" * 40
BRANCH_HEAD = "b" * 40
IMAGE_DIGEST = "sha256:" + "c" * 64
GUARD = "Require an immutable tag or validated release-branch definition"
METADATA = "Validate tag, release, checks, and ancestry"
RETAINED = "Validate the retained complete candidate"
PROVENANCE = "Require tag-bound provenance for every published byte"


def run_blocks() -> dict[str, str]:
    """Read the literal Bash run block of each named step without YAML."""
    workflow = (ROOT / ".github/workflows/package-release.yml").read_text(encoding="utf-8")
    result = {}
    for block in re.split(r"(?m)(?=^      - )", workflow)[1:]:
        name = re.search(r"(?m)^(?:      - |        )name: (.+)$", block)
        script = re.search(r"(?m)^        run: \|\n((?:          .*\n?|\n)+)", block)
        if name is not None and script is not None:
            result[name[1]] = textwrap.dedent(script[1])
    missing = {GUARD, METADATA, RETAINED, PROVENANCE} - set(result)
    if missing:
        raise AssertionError(f"package workflow no longer contains: {sorted(missing)}")
    return result


class Fixture:
    def __init__(self, directory: Path, gh_fail: str = "") -> None:
        self.directory = directory
        self.log = directory / "commands"
        self.log.touch()
        bin_directory = directory / "bin"
        bin_directory.mkdir()
        # PATH holds only these inert mocks: no real Git, GitHub API, Python or sleep.
        commands = {
            "git": f"""case "$*" in
  "rev-parse HEAD"|"rev-parse refs/remotes/origin/main") echo {BRANCH_HEAD} ;;
  "rev-parse refs/tags/{TAG}^{{commit}}") echo {TAG_COMMIT} ;;
  "merge-base --is-ancestor "*) exit 0 ;;
  *) exit 90 ;;
esac""",
            "gh": f"""case "$*" in
  "api -H Accept: application/vnd.github+json repos/atrinik/classic/commits/{BRANCH_HEAD}/check-runs?check_name=Classic%20validation&per_page=100 "*) echo 1 ;;
  "api repos/atrinik/classic/actions/runs/456 --jq "*) printf '%s\\n' "$MOCK_RUN_FIELDS" ;;
  "api repos/atrinik/classic/actions/runs/456/jobs?"*|"api repos/atrinik/classic/actions/runs/456/artifacts?"*) echo 1 ;;
  "attestation verify "*)
    [[ -n "$MOCK_GH_FAIL" && "$3" == "$MOCK_GH_FAIL" ]] && exit 1
    exit 0 ;;
  *) exit 91 ;;
esac""",
            "python3": '[[ "$1" == tools/release/validate_release.py ]] || exit 92',
            "sleep": "exit 0",
        }
        for name, body in commands.items():
            executable = bin_directory / name
            executable.write_text(
                "#!/bin/bash\n"
                f"printf '%s\\t' '{name}' \"$@\" >> \"$MOCK_LOG\"\n"
                "printf '\\n' >> \"$MOCK_LOG\"\n" + body + "\n",
                encoding="utf-8",
            )
            executable.chmod(0o700)
        self.environment = {
            "PATH": str(bin_directory),
            "MOCK_LOG": str(self.log),
            "MOCK_GH_FAIL": gh_fail,
            "MOCK_RUN_FIELDS": "",
            "GITHUB_OUTPUT": str(directory / "github-output"),
            "GH_TOKEN": "inert",
            "RELEASE_REPOSITORY": "atrinik/classic",
            "RELEASE_SOURCE_BRANCH": "main",
            "RELEASE_TAG": TAG,
        }

    def run(self, script: str, **environment: str) -> subprocess.CompletedProcess[str]:
        path = self.directory / "step.sh"
        path.write_text(script, encoding="utf-8")
        # GitHub's default `bash -e {0}` shell for run steps.
        return subprocess.run(
            ["/bin/bash", "--noprofile", "--norc", "-e", str(path)],
            cwd=self.directory,
            env=self.environment | environment,
            capture_output=True, text=True, timeout=20,
        )

    def commands(self) -> list[list[str]]:
        return [line.rstrip("\t").split("\t") for line in self.log.read_text().splitlines()]


def run_fields(commit: str, ref: str) -> str:
    return "\t".join((
        "Package Release", ".github/workflows/package-release.yml", "workflow_dispatch",
        "atrinik/classic", "failure", commit, ref,
    ))


class PackageReleaseTagBindingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.steps = run_blocks()

    def run_step(self, name: str, gh_fail: str = "", **environment: str):
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Fixture(Path(temporary), gh_fail)
            result = fixture.run(self.steps[name], **environment)
            return result, fixture.commands()

    def test_candidate_builds_run_only_from_the_release_tag(self) -> None:
        tag_ref = {"RELEASE_REF_TYPE": "tag", "GITHUB_REF": f"refs/tags/{TAG}",
                   "RELEASE_WORKFLOW_SHA": TAG_COMMIT}
        branch_ref = {"RELEASE_REF_TYPE": "branch", "GITHUB_REF": "refs/heads/main",
                      "RELEASE_WORKFLOW_SHA": BRANCH_HEAD}
        result, _ = self.run_step(GUARD, RELEASE_CANDIDATE_RUN_ID="", **tag_ref)
        self.assertEqual(result.returncode, 0, result.stderr)

        result, commands = self.run_step(GUARD, RELEASE_CANDIDATE_RUN_ID="", **branch_ref)
        self.assertEqual(result.returncode, 1)
        self.assertIn(f"candidate builds must run from refs/tags/{TAG}", result.stderr)
        self.assertEqual(commands, [])

        result, _ = self.run_step(GUARD, RELEASE_CANDIDATE_RUN_ID="456", **branch_ref)
        self.assertEqual(result.returncode, 0, result.stderr)

        for ref_type in ("tag", "branch"):
            with self.subTest(ref_type=ref_type):
                result, commands = self.run_step(METADATA, GITHUB_REF_TYPE=ref_type)
                self.assertEqual(result.returncode, 0 if ref_type == "tag" else 1)
                validators = [c for c in commands if c[0] == "python3"]
                self.assertEqual(len(validators), 1 if ref_type == "tag" else 0)
                for validator in validators:
                    self.assertNotIn("--recovery-branch", validator)

    def test_retained_candidate_must_come_from_the_tag_ref_run(self) -> None:
        branch_ref = {"GITHUB_REF_TYPE": "branch", "GITHUB_REF": "refs/heads/main",
                      "CANDIDATE_RUN_ID": "456"}
        result, commands = self.run_step(
            RETAINED, MOCK_RUN_FIELDS=run_fields(TAG_COMMIT, TAG), **branch_ref
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(sum(1 for c in commands if c[0] == "python3"), 1)
        for commit, ref in ((BRANCH_HEAD, "main"), (BRANCH_HEAD, TAG),
                            (TAG_COMMIT, "main"), (TAG_COMMIT, "")):
            with self.subTest(commit=commit[:1], ref=ref):
                result, commands = self.run_step(
                    RETAINED, MOCK_RUN_FIELDS=run_fields(commit, ref), **branch_ref
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(any(c[0] == "python3" for c in commands))

    def test_publication_requires_tag_bound_provenance_for_every_subject(self) -> None:
        environment = {"RELEASE_COMMIT": TAG_COMMIT, "IMAGE_DIGEST": IMAGE_DIGEST}
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Fixture(Path(temporary))
            release = fixture.directory / "build/release"
            release.mkdir(parents=True)
            for name in ("SHA256SUMS", "atrinik-classic-5.120.0.tar.gz"):
                (release / name).write_text(name)
            result = fixture.run(self.steps[PROVENANCE], **environment)
            self.assertEqual(result.returncode, 0, result.stderr)
            verified = [c for c in fixture.commands() if c[:3] == ["gh", "attestation", "verify"]]
            self.assertEqual(
                [c[3] for c in verified],
                ["build/release/SHA256SUMS", "build/release/atrinik-classic-5.120.0.tar.gz",
                 f"oci://ghcr.io/atrinik/classic-server@{IMAGE_DIGEST}"],
            )
            for call in verified:
                self.assertIn("--deny-self-hosted-runners", call)
                flags = [value for value in call[4:] if value != "--deny-self-hosted-runners"]
                policy = dict(zip(flags[::2], flags[1::2]))
                self.assertEqual(policy, {
                    "--repo": "atrinik/classic",
                    "--signer-workflow": "atrinik/classic/.github/workflows/package-release.yml",
                    "--source-digest": TAG_COMMIT,
                    "--source-ref": f"refs/tags/{TAG}",
                })

            fixture.log.write_text("")
            fixture.environment["MOCK_GH_FAIL"] = "build/release/atrinik-classic-5.120.0.tar.gz"
            result = fixture.run(self.steps[PROVENANCE], **environment)
            self.assertEqual(result.returncode, 1)
            self.assertIn("no tag-bound provenance for build/release/atrinik-classic-5.120.0.tar.gz",
                          result.stderr)
            subjects = [c[3] for c in fixture.commands() if c[:3] == ["gh", "attestation", "verify"]]
            self.assertEqual(subjects.count("build/release/atrinik-classic-5.120.0.tar.gz"), 3)
            self.assertNotIn(f"oci://ghcr.io/atrinik/classic-server@{IMAGE_DIGEST}", subjects)

        for name, value in (("RELEASE_COMMIT", "main"), ("IMAGE_DIGEST", ""), ("RELEASE_TAG", "main")):
            with self.subTest(invalid=name):
                result, commands = self.run_step(PROVENANCE, **(environment | {name: value}))
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(any(c[0] == "gh" for c in commands))

    def test_publication_rejects_a_commit_other_than_the_tag(self) -> None:
        result, commands = self.run_step(
            PROVENANCE, RELEASE_COMMIT=BRANCH_HEAD, IMAGE_DIGEST=IMAGE_DIGEST
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(any(c[0] == "gh" for c in commands))


if __name__ == "__main__":
    unittest.main()
