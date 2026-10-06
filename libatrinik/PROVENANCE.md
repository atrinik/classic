# Provenance and ownership

This repository was extracted from `common/toolkit/` in
[`atrinik/atrinik`](https://github.com/atrinik/atrinik) at source commit
`e572cb02e`. The filtered Git history preserves the commits that affected the
library. The library files carry the monorepo's GPL-2.0-or-later terms recorded
in the root `LICENSE.md`.

The extraction does not relicense any code. Source-file copyright and license
notices remain authoritative. New contributions must be compatible with the
repository's GPL-2.0-or-later license unless a separately reviewed licensing
change establishes otherwise.

The Atrinik maintainers own API review, dependency updates, security fixes,
and releases. Releases use immutable semantic-version tags and include a
SHA-256 manifest. Classic game command identities and generated C/Python
bindings are owned by the sibling `protocol/` module in this monorepo.
Integrated builds select that source, and scoped library releases embed the
matching unified-version protocol tree under `dependencies/`.

`dependencies.lock.json` retains the separate historical v1.0.0 protocol
artifact fallback. The shared access-authority specification now published by
`atrinik/protocol` has a different source layout and does not supply the Classic
CMake command-binding package. Its v2.9.0 provenance is recorded in
[`protocol/README.md`](../protocol/README.md); it does not replace that archival
lock or the sibling Classic bindings.
