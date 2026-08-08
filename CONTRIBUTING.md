# Contributing to Atrinik Classic

Use one branch and pull request for a coordinated change. Read the root and
nearest component `AGENTS.md` files first; they define ownership, invariants,
and exact validation requirements.

## Workflow

1. Initialize classic through the `atrinik/atrinik` wrapper.
2. Create one full `classic` worktree and a profile based on `classic`.
3. Make the smallest coherent change across the owning subtrees.
4. Add focused tests and run every affected dependency and consumer.
5. Run `python3 tools/verify_import_history.py` and `git diff --check`.
6. Use a Conventional Commit title such as `fix(server): ...` or
   `feat(protocol)!: ...`.

Do not vendor content, sound, resources, generated dependency trees, or copies
of sibling source. Do not edit imported history maps or archive refs. Security
reports follow [SECURITY.md](SECURITY.md).

The repository will use a unified classic release line. Until the root release
pipeline is enabled, changes merged to `main` are unreleased; nested component
release workflows are historical and do not run.
