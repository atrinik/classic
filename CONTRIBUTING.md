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

The squash-merge title is the unified release input. Classic stays on the
`5.x.x` line: a breaking marker or `feat` advances the minor version, and every
other accepted Conventional Commit type advances the patch version.

Do not vendor content, sound, resources, generated dependency trees, or copies
of sibling source. Do not edit imported history maps or archive refs. Security
reports follow [SECURITY.md](SECURITY.md). All Atrinik-authored classic source
remains GPL-2.0-or-later under [LICENSE.md](LICENSE.md); preserve compatible
third-party licenses, notices, and attributions.

That distribution license is not a blanket outbound reuse ban. Exact,
independently separable material may be inspected as source reference, copied,
migrated or ported, translated or adapted, or relicensed in an MIT destination
only after the
[canonical provenance audit](https://github.com/atrinik/atrinik/blob/main/docs/PROVENANCE.md)
proves each selected contribution is the applicable named grantor's original
work. Each contribution must be solely authored by that grantor and fall within
the row's temporal scope. Distinct contributions may cite different rows only
when each independently satisfies one row. Rows cannot be combined to cover
jointly authored contributions, generated output, or inseparable mixed work.
Later material needs contemporaneous compatible permission. The grants
authorize only proven destination use. They neither change this repository's
source license or notices nor by themselves approve a GPL dependency, linked
or combined binary, bundle, or surrounding material.

## Copyright headers

Use `The Atrinik Project` as the exact collective holder for every new or
updated blanket Atrinik copyright header. Existing blanket forms such as
`Atrinik contributors`, `Atrinik Development Team`, or bare `Atrinik` migrate
prospectively: do not churn untouched files, but normalize the holder when an
otherwise edited Atrinik-authored file already carries that blanket header.
`The Atrinik Project` is canonical because it already predominates in modern
MIT source headers; the other forms remain historical inventory, not templates
for new blanket attribution.

The exact blanket format is `Copyright START[-END] The Atrinik Project`: use a
single year when `START` is the current year and an ASCII-hyphenated range
otherwise. Only the surrounding comment delimiters vary by file format. When
normalizing an existing blanket form, omit `(C)`, `(c)`, `©`, commas, and
trailing punctuation so the notice matches this form exactly.

Whenever an Atrinik-authored file is edited, update each existing Atrinik-owned
copyright notice in the same change: retain its original start year and set its
terminal year to the current calendar year. For example, in 2026:

- `Copyright 2021-2024 The Atrinik Project` becomes
  `Copyright 2021-2026 The Atrinik Project`;
- `Copyright 2026 The Atrinik Project` remains a single-year notice;
- `Copyright 2024 The Atrinik Project` becomes
  `Copyright 2024-2026 The Atrinik Project`; and
- `Copyright (C) 2009-2014 Zoey Rose and Atrinik Development Team` becomes
  `Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team`.

The named-holder example deliberately retains its more precise mixed
attribution instead of replacing it with the canonical blanket holder. Always
preserve named holders, original start years, Crossfire, Daimonin and other
upstream notices, third-party attribution, SPDX identifiers, license terms,
and provenance text. Leave upstream and third-party notice years unchanged. Do
not add a header to a file that lacks one, and do not rewrite vendored,
imported, preserved-history, or third-party files under this rule. Update
generated headers through their authoritative generator or template rather
than editing generated output.

Repository `LICENSE` notice lines are a separate legal and attribution surface.
Do not normalize them as source headers; change one only through deliberate
repository-owned legal review.

The root workflows own the unified classic release line. A successful aggregate
check for the current `main` commit triggers semantic-release and the guarded
package workflow; pull-request, merge-group, failed, stale, and non-main checks
cannot publish. The retired nested component workflow and semantic-release
files remain available in Git history; never restore them as independent
release trains.

Linux Check compiles protocol/libatrinik, server, and client inside the exact
digest-pinned `ghcr.io/atrinik/classic-build` image declared in
`.github/workflows/check.yml`. Each job restores only its own ccache directory,
uses explicit C and C++ CMake launchers, and normalizes workspace paths through
`CCACHE_BASEDIR`. Cache keys include the runner platform, trust scope, GCC and
image identities, relevant configuration files, and a manual epoch. Pull
request and merge-group entries never share a write key with `main`; only the
ccache directory is persisted, never a CMake build tree.

To update the image, first publish and validate an immutable devcontainer
candidate. Use that candidate digest only for pre-merge consumer evidence. Once
the devcontainer change is released, verify the release image's embedded
inventory and source coordinates, then replace both image and digest constants
with its versioned digest before making the Classic pull request ready. Update
the GCC identity if needed, and increment `CLASSIC_LINUX_CCACHE_EPOCH` for any
cache contract change not represented by the hashed configuration inputs. Run
the cache-key unit tests, then use cold and repeated Check runs to inspect the
raw ccache artifacts and image timing summary before accepting the pin.
Reusable `classic-build` and `windows-build` images are public: pull them
anonymously in pull-request validation and do not grant `packages: read` or log
in to GHCR merely to consume either image.

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
