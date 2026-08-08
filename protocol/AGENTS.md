# Atrinik protocol repository guide

- This repository is the source of truth for shared game-command identities and
  their generated C and Python bindings. It does not own all packet payload
  layouts; trace those through their client/server producers and consumers.
- Edit `schema/game-commands.json`, then run `python3 tools/generate.py`.
  Never hand-edit committed generated files or duplicate identifiers elsewhere.
- Preserve stable names and IDs unless a coordinated breaking transition is
  explicit. Define framing, field order, widths, signedness, byte order,
  lengths, limits, state transitions, and malformed-input behavior before
  changing a wire contract.
- Update every producer, consumer, fixture, and lock file together through a
  workspace profile. Remove a superseded command only after all selected
  consumers have moved; avoid indefinite parallel compatibility paths.
- Keep parsers transactional and bounded. Cover truncated, oversized,
  out-of-order, unknown, and boundary-value input as well as round trips.
- Validate with `python3 tools/generate.py --check`,
  `python3 -m unittest discover -s tests -p 'test_*.py'`, the repository CMake
  and CTest suites, and wrapper builds for every affected consumer.
- Commits and pull-request titles use Conventional Commits. Every squash merge
  is released by semantic-release; coordinate a release-consuming update when
  generated bindings change.
- Keep build/package output under `build/`, preserve unrelated work, and finish
  with `git diff --check`.
- Update this `AGENTS.md` in the same change when major rework alters ownership,
  schemas, generation, compatibility policy, consumers, or validation.
