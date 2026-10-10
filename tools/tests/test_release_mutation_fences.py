"""Execute release mutation scripts with inert commands; no GitHub concurrency proof."""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import re
import subprocess
import tempfile
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[2]
LATCH = "ATRINIK_RELEASE_SUPERSEDED"
REVISION = "b" * 40
CASES = (
    ("Delete the policy-listed empty failed draft", "delete-empty-draft", "python3"),
    ("Resume the incomplete package release", "resume", "gh"),
    ("Resume the retained complete candidate", "resume-retained-candidate", "gh"),
    ("Analyze commits, tag, and publish release notes", "none", "npx"),
)


@dataclass(frozen=True)
class Step:
    condition: str
    script: str


def mutation_steps() -> dict[str, Step]:
    """Read the actual named YAML step blocks without a YAML dependency."""
    workflow = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
    result = {}
    for block in re.split(r"(?m)(?=^      - )", workflow)[1:]:
        name = re.search(r"(?m)^      - name: (.+)$", block)
        if name is None or name[1] not in {case[0] for case in CASES}:
            continue
        condition = re.search(r"(?m)^        if: (.+)$", block)
        script = re.search(r"(?m)^        run: \|\n((?:          .*\n?)+)", block)
        if condition is None or script is None:
            raise AssertionError(f"missing condition or literal Bash run block: {name[1]}")
        expression = condition[1]
        if expression == ">-":
            folded_match = re.match(r"\n((?:          [^\n]*\n?)+)", block[condition.end():])
            if folded_match is None:
                raise AssertionError("missing folded condition")
            folded = folded_match[1]
            expression = " ".join(line.strip() for line in folded.splitlines())
        result[name[1]] = Step(expression, textwrap.dedent(script[1]))
    if set(result) != {case[0] for case in CASES}:
        raise AssertionError("release workflow no longer contains the four mutation steps")
    return result


def eligible(expression: str, action: str, latch: str = "") -> bool:
    """Evaluate only comparisons, &&, || and grouping in these step conditions.

    Unknown syntax/contexts fail instead of silently approximating Actions.
    This is not a general GitHub Actions interpreter.
    """
    tokens = re.findall(r"[\w.-]+|'[^']*'|==|!=|&&|\|\||[()]", expression)
    if "".join(tokens) != re.sub(r"\s+", "", expression):
        raise AssertionError(f"unsupported condition syntax: {expression}")
    values = {"steps.pending-release.outputs.action": action, f"env.{LATCH}": latch}
    position = 0

    def take() -> str:
        nonlocal position
        if position == len(tokens):
            raise AssertionError("incomplete condition")
        value = tokens[position]
        position += 1
        return value

    def atom() -> bool:
        if position < len(tokens) and tokens[position] == "(":
            take()
            result = disjunction()
            if take() != ")":
                raise AssertionError("unclosed condition group")
            return result
        key, operator, literal = take(), take(), take()
        if key not in values or operator not in ("==", "!=") or not literal.startswith("'"):
            raise AssertionError(f"unsupported comparison: {key} {operator} {literal}")
        same = values[key] == literal[1:-1]
        return same if operator == "==" else not same

    def conjunction() -> bool:
        result = atom()
        while position < len(tokens) and tokens[position] == "&&":
            take()
            right = atom()
            result = result and right
        return result

    def disjunction() -> bool:
        result = conjunction()
        while position < len(tokens) and tokens[position] == "||":
            take()
            right = conjunction()
            result = result or right
        return result

    result = disjunction()
    if position != len(tokens):
        raise AssertionError("unconsumed condition tokens")
    return result


class BashFixture:
    def __init__(self, directory: Path):
        self.directory = directory
        self.bin = directory / "bin"
        self.bin.mkdir()
        self.log = directory / "commands"
        self.log.touch()
        self.github_env = directory / "github-env"
        self.github_env.touch()
        self.environment = {
            "PATH": str(self.bin),
            "MOCK_LOG": str(self.log),
            "MOCK_REVISION": REVISION,
            "GITHUB_ENV": str(self.github_env),
            "GITHUB_OUTPUT": str(directory / "github-output"),
            "ATRINIK_RELEASE_BRANCH": "main",
            "RELEASE_REPOSITORY": "atrinik/classic",
            "RELEASE_TAG": "v5.80.0",
            "RELEASE_ID": "123",
            "CANDIDATE_RUN_ID": "456",
        }
        # PATH contains only these inert mocks: no real git, API, node or Python.
        commands = {
            "git": '[[ "$*" == "rev-parse HEAD" ]] || exit 91\nprintf "%s\\n" "$MOCK_REVISION"',
            "python3": """case "$1" in
  tools/release/select_release_batch.py)
    [[ " $* " == *" --require-current "* ]] || exit 92
    exit "$MOCK_CHECK_STATUS" ;;
  tools/release/resolve_pending_release.py)
    [[ " $* " == *" --delete-policy-listed-empty-draft "* ]] || exit 93 ;;
  *) exit 94 ;;
esac""",
            "gh": '[[ "$1 $2 $3" == "workflow run package-release.yml" ]] || exit 95',
            "npx": '[[ " $* " == *" node tools/release/run_semantic_release.mjs "* ]] || exit 96',
        }
        for name, body in commands.items():
            executable = self.bin / name
            executable.write_text(
                "#!/bin/bash\n"
                f"printf '%s\\t' '{name}' \"$@\" >> \"$MOCK_LOG\"\n"
                "printf '\\n' >> \"$MOCK_LOG\"\n" + body + "\n",
                encoding="utf-8",
            )
            executable.chmod(0o700)

    def run(self, step: Step, check_status: int = 0) -> subprocess.CompletedProcess[str]:
        script = self.directory / "step.sh"
        script.write_text(step.script, encoding="utf-8")
        return subprocess.run(
            ["/bin/bash", "--noprofile", "--norc", "-e", "-o", "pipefail", str(script)],
            cwd=self.directory,
            env=self.environment | {"MOCK_CHECK_STATUS": str(check_status)},
            capture_output=True, text=True, timeout=10,
        )

    def commands(self) -> list[list[str]]:
        return [line.rstrip("\t").split("\t") for line in self.log.read_text().splitlines()]

    def mutations(self) -> list[list[str]]:
        return [command for command in self.commands()
                if command[0] in ("gh", "npx") or command[1:2] == ["tools/release/resolve_pending_release.py"]]

    def import_github_env(self) -> None:
        for line in self.github_env.read_text().splitlines():
            key, value = line.split("=", 1)
            if key != LATCH:
                raise AssertionError(f"unexpected fixture environment mutation: {key}")
            self.environment[key] = value


class ReleaseMutationFenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.steps = mutation_steps()

    def test_current_checked_head_runs_each_expected_mutation(self) -> None:
        for name, action, command in CASES:
            with self.subTest(step=name), tempfile.TemporaryDirectory() as temporary:
                fixture = BashFixture(Path(temporary))
                step = self.steps[name]
                self.assertTrue(eligible(step.condition, action))
                result = fixture.run(step)
                self.assertEqual(result.returncode, 0, result.stderr)
                mutations = fixture.mutations()
                self.assertEqual(len(mutations), 1)
                self.assertEqual(mutations[0][0], command)
                checks = [call for call in fixture.commands() if call[1:2] == ["tools/release/select_release_batch.py"]]
                self.assertEqual(len(checks), 1)
                self.assertIn("--recheck", checks[0])
                self.assertIn("--revision", checks[0])
                self.assertIn(REVISION, checks[0])
                self.assertLess(fixture.commands().index(checks[0]), fixture.commands().index(mutations[0]))
                if command == "python3":
                    self.assertIn("--expected-release-id", mutations[0])
                    self.assertIn("123", mutations[0])
                    self.assertIn("--expected-tag", mutations[0])
                    self.assertIn("v5.80.0", mutations[0])
                elif command == "gh":
                    self.assertIn("tag=v5.80.0", mutations[0])
                    field = "candidate_run_id=456" if action == "resume-retained-candidate" else "source_branch=main"
                    self.assertIn(field, mutations[0])
                self.assertEqual(fixture.github_env.read_text(), "")

    def test_superseded_status_three_skips_all_mutations_and_latches(self) -> None:
        for name, _, _ in CASES:
            with self.subTest(step=name), tempfile.TemporaryDirectory() as temporary:
                fixture = BashFixture(Path(temporary))
                result = fixture.run(self.steps[name], check_status=3)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(fixture.mutations(), [])
                self.assertEqual(fixture.github_env.read_text(), f"{LATCH}=true\n")

    def test_api_and_other_non_supersession_errors_fail_without_mutating(self) -> None:
        for name, _, _ in CASES:
            for status in (1, 2, 42):
                with self.subTest(step=name, status=status), tempfile.TemporaryDirectory() as temporary:
                    fixture = BashFixture(Path(temporary))
                    result = fixture.run(self.steps[name], check_status=status)
                    self.assertEqual(result.returncode, status, result.stderr)
                    self.assertEqual(fixture.mutations(), [])
                    self.assertEqual(fixture.github_env.read_text(), "")

    def test_latch_blocks_both_step_condition_and_direct_bash_execution(self) -> None:
        for name, action, _ in CASES:
            with self.subTest(step=name), tempfile.TemporaryDirectory() as temporary:
                fixture = BashFixture(Path(temporary))
                fixture.environment[LATCH] = "true"
                step = self.steps[name]
                self.assertFalse(eligible(step.condition, action, "true"))
                result = fixture.run(step, check_status=0)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(fixture.commands(), [])

    def test_skipped_delete_cannot_revive_semantic_release_after_validation_recovers(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture = BashFixture(Path(temporary))
            delete = self.steps[CASES[0][0]]
            semantic = self.steps[CASES[3][0]]
            self.assertTrue(eligible(delete.condition, "delete-empty-draft"))
            result = fixture.run(delete, check_status=3)
            self.assertEqual(result.returncode, 0, result.stderr)
            fixture.import_github_env()  # Model only the runner's env-file propagation.
            self.assertEqual(fixture.environment.get(LATCH), "true")
            before = fixture.commands()
            self.assertFalse(eligible(semantic.condition, "delete-empty-draft", fixture.environment[LATCH]))
            # Exercise the shell defense too, with a now-successful mocked checker.
            result = fixture.run(semantic, check_status=0)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(fixture.commands(), before)
            self.assertEqual(fixture.mutations(), [])


if __name__ == "__main__":
    unittest.main()
