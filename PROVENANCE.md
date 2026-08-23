# History and provenance

Atrinik Classic was assembled on 2026-08-08 from the five public source
repositories below. Each history was assembled with `git-filter-repo` 2.47.0
using `--to-subdirectory-filter`, then merged without squashing. Dates,
messages, parent topology, and file contents are preserved in the imported
histories.

| Source repository | Source main tip | Prefix | Rewritten tip | Integration commit |
| --- | --- | --- | --- | --- |
| `atrinik/legacy-client` | `a8d0ab66cafac894f6f9375eddb60fd02ed4c778` | `client/` | `f7a31731c374565650c90f333833af4b8148876c` | `093df61f9348aab482c0400c72c422dd48fe7627` |
| `atrinik/legacy-server` | `b3c32b13a220fb9cc330133183de5b1818b29d41` | `server/` | `b35c0bd5f81d1fe4f3b8ba9890b9a8f9bcf6b213` | `1a4dd183c1b889c6fd69276ac6ab15becdefe13e` |
| `atrinik/legacy-editor` | `ac547a5ba8259bbde649caabdabe51c2861352f4` | `editor/` | `5f6eabf3bb9122a19850d5b8ee9f930134a6d4a1` | `14f20bc6d4539954582864af7c6d37a498a1cf3e` |
| `atrinik/legacy-libatrinik` | `a5e161f5dae6896ae0ee218b807f547ffdeb49c8` | `libatrinik/` | `0b89e9015c36d9f30ae5eb8c0b85f7bada43bac8` | `6116b66bb8e1f91385b31e16256ab36b4fb41fcc` |
| `atrinik/legacy-protocol` | `eebc3921f364108a0ddd6d4beb9e1f86a274c862` | `protocol/` | `89579d3b6ccea3a0978990cfe9189ce49282e22d` | `a8c65e6eef38425e00cfdb8a97f8475a642a6086` |

The exact machine-readable manifest, commit maps, tool digest, source trees,
preserved `AGENTS.md` blobs, and retired branch targets are under
`docs/history/`. Rewritten component histories are ancestors of `main`; the
archived source repositories remain authoritative for the unchanged original
commit graphs. After import and local-workspace migration verification, classic
retired the complete `history/*` branch namespace as well as every
component-prefixed and tag-namespace archival ref. It rebuilt one unprefixed
release sequence from `v5.0.19`; `docs/history/release-tags.json` records its
exact targets. Only the explicitly retained releases and tags remain
live.

Atrinik-authored source throughout the classic monorepo, including the protocol
module, is distributed under GPL-2.0-or-later through the root `LICENSE.md`.
Required third-party asset and code notices remain authoritative. That source
license is not a blanket outbound reuse ban: exact, independently separable
material may be inspected as source reference, copied, migrated or ported,
translated or adapted, or relicensed in an MIT destination only after the
[canonical provenance audit](https://github.com/atrinik/atrinik/blob/main/docs/PROVENANCE.md)
proves each selected contribution is the applicable named grantor's original
work. Each contribution must be solely authored by that grantor and fall within
the row's temporal scope. Distinct contributions may cite different rows only
when each independently satisfies one row. Rows cannot be combined to cover
jointly authored contributions, generated output, or inseparable mixed work.
Later material needs contemporaneous compatible permission. The grants
authorize only proven destination use. They neither change the
GPL-2.0-or-later terms or notices of the source distributed here nor by
themselves approve a GPL dependency, linked or combined binary, bundle, or
surrounding material.

The standalone protocol's MIT terms were introduced by Zoey Rose in original
commit `7e10ecaa279489fcdb843ecbe020fc9befeba4fd` (rewritten as
`9145787d2c453415441238d80cad5966fe017ee6`). Its imported path history contains
only Zoey Rose's original contributions plus automated dependency updates. On
2026-08-07, Zoey Rose explicitly authorized distributing her original classic
protocol work under GPL-2.0-or-later and removing the subtree MIT license.
