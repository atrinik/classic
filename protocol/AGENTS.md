# Atrinik classic protocol module guide

- The `protocol/` module is the source of truth for shared game-command identities and
  their generated C and Python bindings. It does not own all packet payload
  layouts; trace those through their client/server producers and consumers.
- Edit `schema/game-commands.json`, then run `python3 tools/generate.py`.
  Never hand-edit committed generated files or duplicate identifiers elsewhere.
- Authored payload contracts shared by endpoints live under `include/`; the
  book-editor contract is `include/atrinik/protocol/book_edit.h`. Keep these
  installed headers and their endpoint validation synchronized.
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
- Commits and pull-request titles use Conventional Commits. Classic uses one
  repository-wide release line; update sibling consumers in the same pull
  request when generated bindings change.
- Installed protocol packages use same-minor compatibility because a classic
  breaking change advances the minor version while major stays 5. Embedded
  scoped dependencies must carry the same generated `VERSION` as their parent
  release archive.
- Keep build/package output under `build/`, preserve unrelated work, and finish
  with `git diff --check`.
- Update this `AGENTS.md` in the same change when major rework alters ownership,
  schemas, generation, compatibility policy, consumers, or validation.

- Revision 1081 reserves retired SETUP subtype 3 and adds ACCESS_AUTH/ADMIN,
  RESULT/ADMIN_RESULT/POLICY. Policy follows authenticated VERSION before
  setup/resources/accounts on every connection; update all native and Python
  consumers together. Raw access payloads require sensitive packet handling.

- Revision 1082 appends BOOK_EDIT at C2S 25 and S2C 32 while preserving
  published v1081 access IDs. The version handshake rejects earlier peers.
