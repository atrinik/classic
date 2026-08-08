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

The squash-merge title is the unified release input: breaking changes bump the
major version, `feat` bumps the minor version, and every other accepted
Conventional Commit type bumps the patch version.

Do not vendor content, sound, resources, generated dependency trees, or copies
of sibling source. Do not edit imported history maps or archive refs. Security
reports follow [SECURITY.md](SECURITY.md).

The repository will use a unified classic release line. Until the root release
pipeline is enabled, changes merged to `main` are unreleased. Nested component
workflows and semantic-release files are inert migration inputs, not active
configuration. Do not enable, update, or remove them independently; they are
retired only when root automation preserves their build, packaging, tagging,
publication, and dependency-update capabilities.

## Module requirements

### Client

Follow [client/INSTALL](client/INSTALL) for the dependency validation, CMake,
and CTest workflow. Preserve license and attribution files when changing
bundled graphics or fonts. Update source and sound locks only to immutable
published releases after independently verifying their checksums.

### Server

Follow [server/INSTALL](server/INSTALL), keep compiler warnings enabled, and
preserve the repository's GPL-2.0-or-later terms and bundled third-party
notices. Update dependency locks only to immutable published releases after
independently verifying the tag, commit, release-asset URL, and SHA-256 digest.
Do not add Git submodules or references to confidential or unreleased work.
Use precise component names rather than vague age-based labels.

### Editor

Run the packaging validation in [editor/README.md](editor/README.md). Do not
add an upstream Gridarta checkout, generated JAR, or editor build output to the
repository.

### Library

Run the library validation in
[libatrinik/README.md](libatrinik/README.md). Public API changes need tests and
an explicit account of ownership, error, lifetime, thread-safety, and
compatibility consequences. Keep `libatrinik/pathfinding/` buildable without
the protocol package or networking toolkit dependencies; result storage stays
context-owned and adapters retain ownership of state and policy. Update the
protocol fallback lock only to an immutable published release whose checksum
has been independently verified.

### Protocol

Follow [protocol/README.md](protocol/README.md). Change the canonical schema and
regenerate every checked-in C and Python binding together; never hand-edit
generated output or copy command identifiers into consumers.
