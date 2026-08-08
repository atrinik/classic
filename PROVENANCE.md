# History and provenance

Atrinik Classic was assembled on 2026-08-08 from the five public source
repositories below. Each history was rewritten with `git-filter-repo` 2.47.0
using `--to-subdirectory-filter`, then merged without squashing. Commit authors,
dates, messages, parent topology, and file contents were retained; commit IDs
necessarily changed because every path gained a prefix.

| Source repository | Source main tip | Prefix | Rewritten tip | Integration commit |
| --- | --- | --- | --- | --- |
| `atrinik/legacy-client` | `a8d0ab66cafac894f6f9375eddb60fd02ed4c778` | `client/` | `8004d4420445ad717a364827944d8790e0a34062` | `6dc9f136336773fc4e5081e78323b0c489ca6446` |
| `atrinik/legacy-server` | `b3c32b13a220fb9cc330133183de5b1818b29d41` | `server/` | `7cae183c396cd0855d4969092389c968c0d20f30` | `083c3fd64996d929c0d324e050449551b1922a7d` |
| `atrinik/legacy-editor` | `ac547a5ba8259bbde649caabdabe51c2861352f4` | `editor/` | `26e9139ca3da0a3cea5903e8cd42e114ad9029a3` | `ae1a14dff80c8f4f06d8f046b630798e35ebe2e8` |
| `atrinik/legacy-libatrinik` | `a5e161f5dae6896ae0ee218b807f547ffdeb49c8` | `libatrinik/` | `c23cf3a5777e15483ff7a5305f37dd9de8939b28` | `e143e832477b2bcd5e18dcd038b2ce0bb70b5d16` |
| `atrinik/legacy-protocol` | `eebc3921f364108a0ddd6d4beb9e1f86a274c862` | `protocol/` | `89579d3b6ccea3a0978990cfe9189ce49282e22d` | `148486ab814839dec0ce3069b8205b660215bc29` |

The exact machine-readable manifest, commit maps, tool digest, source trees,
and preserved `AGENTS.md` blobs are under `docs/history/`. Rewritten release
tags use `<component>/vX.Y.Z`; archival refs retain original objects under the
`archive/original/` namespace. Historical releases and their assets remain in
the former repositories.

Atrinik-authored source throughout the classic monorepo, including the protocol
module, is distributed under GPL-2.0-or-later through the root `LICENSE.md`.
Required third-party asset and code notices remain authoritative. This does not
rely on, expand, or alter any provenance grant for the separate MIT replacement
implementation.

The standalone protocol's MIT terms were introduced by Zoey Rose in original
commit `7e10ecaa279489fcdb843ecbe020fc9befeba4fd` (rewritten as
`9145787d2c453415441238d80cad5966fe017ee6`). Its imported path history contains
only Zoey Rose's original contributions plus automated dependency updates. On
2026-08-07, Zoey Rose explicitly authorized distributing her original classic
protocol work under GPL-2.0-or-later and removing the subtree MIT license.
