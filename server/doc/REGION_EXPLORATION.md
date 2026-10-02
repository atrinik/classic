# Account region-map exploration

The Classic server owns region-map discoveries for an authenticated account.
Characters on that account share the union, including simultaneous sessions.
A fresh client receives the same discoveries. This is separate from MAP2's
remembered geometry, current lighting and inventory region-map reveal items.

## Discovery and ownership

`draw_client_map2()` records the resolved map identity and local tile coordinates
only after its content and visibility checks accept a public, non-unique map
cell at depth zero with FOW false and a region map in its region ancestry.
Blocked LOS, retained roof boundaries, other depths, absent content, private
maps and maps without a region map never grant persistence. Discovery cannot
load another map, change gameplay LOS, or reveal actors or lights.

`exploration.c` owns the account cache on the single simulation thread. Sessions
share a store by authenticated account name; they never upload discoveries.
The last session normally saves and releases the store. Socket cleanup also
releases exceptional sessions. Failed writes retain dirty state for retry on
reconnect, and final server cleanup retries all remaining stores.

Each map is keyed by canonical logical path, with row-major local coordinates.
Paths start with `/`, contain at most 255 bytes, and contain no empty, `.` or
`..` segment, backslash, colon or control character. Width and height are each
1–256; a store contains at most 4096 maps. Coordinates outside the map and
entries above the limit are rejected. A changed map size resets only that map's
bitmap because old coordinates no longer identify the same geometry. Changes
to region image layout do not invalidate logical map-local coordinates.

## Durable storage

The sidecar is `<account_make_path(name)>.exploration`, next to the existing
account file. Credential and character-roster rewrites cannot erase it. Missing
sidecars mean empty exploration. This is initialized runtime state: include it
in account backups and the complete mutable-datapath activation archive.

The binary format is:

- Eight ASCII magic/version bytes `AEXP0001`.
- Zero or more records until EOF: big-endian unsigned 16-bit path byte length,
  exactly that many path bytes without a terminator, big-endian unsigned 16-bit
  width and height, then exactly `ceil(width * height / 8)` bitmap bytes.
- Bits are row-major, least-significant-bit first within each byte. Unused bits
  in the last byte must be zero. Duplicate paths are invalid.

Readers bound the entire file to 36 MiB and the record count to 4096. Validation
is transactional: malformed/truncated/oversized data discards the staged store,
logs a diagnostic, and disables writes for that account session. The original
file is preserved for operator recovery rather than replaced with an empty one.

Writes use the existing mode-0600 atomic file replacement API, including its
flush/fsync/rename behavior. Discovery writes are batched at most once per five
seconds; logout, socket teardown and orderly server shutdown force a save.
There is no write when nothing changed. A crash may lose the most recent
unsaved batch; failed saves keep dirty data in memory and log an error.

## Protocol 1081

`REGION_EXPLORATION` is server-to-client command 29. The ordinary packet envelope
provides framing; its payload begins with an unsigned 8-bit operation:

| Operation | Remaining payload |
| --- | --- |
| `0` RESET | Empty; clears the previous account's session cache and rendered FOW. |
| `1` MAP | NUL-terminated logical path (1–255 bytes), big-endian u16 width, big-endian u16 height, exactly `ceil(width * height / 8)` bitmap bytes. |

The bounds and bit order match persistence. Unknown operations, truncated or
extra bytes, invalid dimensions/path/padding, and a 4097th distinct map are
rejected before mutating the client cache. MAP replaces the cached record for
its path. RESET starts every successful character session before snapshot
records, even when the account has no discoveries. Disconnect clears client
session state. Exact-version negotiation rejects incompatible older peers;
there is no client-upload command or compatibility persistence branch.

Initial snapshots and later map revisions stream at most 32 records per map
draw, stopping when the socket queue exceeds 1 MiB or 1024 queued fragments.
Per-session revision tracking retries remaining records on later draws and
keeps simultaneous characters synchronized without exceeding the transport's
4 MiB/4096-fragment queue limits. Each map record is independently complete;
there is no unbounded staging transaction or implicit snapshot-complete marker.

The client caches records independently of asynchronous region PNG/definition
loading, applies matching logical maps once assets are ready, and refreshes
open map/minimap clones. Normal MAP2 updates can still reveal currently visible
cells immediately. Server snapshots restore history after reconnect and region
transitions; client disks are not authoritative.

## Migration and validation

Existing per-character `client-maps/*.tiles` files remain untouched but are no
longer loaded or written. Previously local exploration is not imported: it has
no server-verifiable account ownership or visibility provenance. Existing
accounts begin recording newly observed cells after the upgrade. No account,
character, content artifact or initialized runtime file requires manual edits.

Server account tests cover shared sessions, another account, reload into fresh
session state, dimensions, corruption preservation, failed-save retry and
paced replay. The request suite drives normal `draw_client_map2()` through
visible, concealed, upper-level, private and non-region cases. Client parser and
region-map integration tests cover bounded transactional decoding and delayed
assets/reconnect rendering. Protocol generation tests bind both consumers to
version 1081 and command 29. Use the workspace Classic profile for integrated
server/client tests and the supervised isolated lifecycle for runtime checks.
