# Account region-map exploration

The Classic server owns a map-path-to-bitfield store for each authenticated
account. Characters share discoveries, including simultaneous sessions. Fresh
clients request the same history. Client caches and requests never grant server
exploration. This is separate from MAP2 remembered geometry, current lighting
and inventory region-map reveal items.

## Discovery and execution cost

`draw_client_map2()` records the resolved map identity and local tile coordinates
only after its content and visibility checks accept a public, non-unique map
cell at depth zero with FOW false and a region map in its region ancestry.
Blocked LOS, retained roof boundaries, other depths, absent content, private
maps and maps without a region map never grant persistence. Discovery cannot
load another map, change gameplay LOS, or reveal actors or lights.

`exploration.c` owns this state on the single simulation thread. Each socket
holds its own session pointer and that session points directly to its account.
Accounts and map paths use the existing `uthash` implementation. Repeated cells
on the same map use the session's last-map cache. Unchanged discoveries perform
no account/session scan, pending-queue work or write. A newly set bit is queued
only for sessions sharing that account.

Each session has a hashed, coalescing pending-map FIFO. Flushing one socket
visits only its own pending records, never every account, session or stored map.
At most 32 records are sent per map draw, stopping when the socket queue exceeds
1 MiB or 1024 queued fragments. Backpressure retains pending bits. Pending maps
are bounded at 10,000 per session; exhausting that bound through requests is a
protocol limit error, and an exhausted session is disconnected rather than
silently losing live discoveries. Reconnect reconciliation recovers history.

Map paths start with `/`, contain at most 255 bytes, and contain no empty, `.`
or `..` segment, backslash, colon or ASCII control character. Width and height
are each 1–256; an account contains at most 10,000 maps. Initial dimensions
establish the row-major bitfield layout. There is no map revision or geometry
consistency check, reset, migration or comparison. Later coordinates outside
that initial layout are ignored; client rendering clips to its region image.

## Durable storage

The sidecar is `<account_make_path(name)>.exploration`, next to the account
file. Credential and roster rewrites cannot erase it. Missing sidecars mean an
empty store. Include sidecars in account backups and complete mutable-datapath
activation archives.

The existing binary format remains:

- Eight ASCII magic/version bytes `AEXP0001`.
- Records until EOF: big-endian unsigned 16-bit path byte length, that many path
  bytes without a terminator, big-endian unsigned 16-bit width and height, then
  exactly `ceil(width * height / 8)` bitmap bytes.
- Bits are row-major and least-significant-bit first in each byte. Unused tail
  bits must be zero. Duplicate paths are invalid.

Readers bound the file to 96 MiB and 10,000 records. Hash lookups detect duplicate
paths while loading. Parsing is transactional: malformed/truncated/oversized
files discard staged state, log a diagnostic and disable writes for that
account session. The original file remains available for operator recovery.

Dirty accounts use the existing mode-0600 atomic replacement API, including
flush/fsync/rename, at most once per five seconds. Logout, socket teardown and
orderly shutdown force a save. Clean accounts do not write. Failed saves retain
dirty state for retry across reconnect and log an error. Checked logout reports
sidecar save failures alongside character and account failures. Before publishing
a shutdown receipt, the server also checks detached dirty exploration accounts;
a failed sidecar save prevents a successful receipt and clean exit. A crash may lose the
most recent unsaved batch. This intentionally remains one account snapshot,
not a per-map file tree or journal: a save is linear in that account's data,
while discovery, idle flush and region synchronization do not scan it.

The scale test reports actual bytes and mark/save/load microseconds for 10,000
24×24 maps and 10,000 256×256 maps. The latter has 78.125 MiB of bitmaps plus
record overhead; it is a deliberate persistence-cost bound, not a typical
region. Save/load timings are diagnostics, not hardware-dependent assertions.

## Protocol 1082

Server-to-client `REGION_EXPLORATION` uses command 32. Its payload starts
with one unsigned 8-bit operation:

| Operation | Remaining payload |
| --- | --- |
| `0` RESET | Authenticated account name, NUL terminated (1–`MAX_BUF-1` bytes). Binds the session to that account's client cache. |
| `1` BITMAP | Path cstring, width u16, height u16, complete bitmap. |
| `2` PATCH | Path cstring, width u16, height u16, count u16, then `count` pairs of byte-index u16 and OR-mask u8. |
| `3` EMPTY | Path cstring; this account has no exploration record for the requested path. |

All multibyte integers are big-endian. Paths, dimensions and bitmap bounds match
storage. PATCH contains 1–8192 entries in strictly increasing index order,
nonzero masks, valid byte indices and no unused tail bits. BITMAP and PATCH
merge into the account cache by OR. The server chooses PATCH only when its
count-and-pair bytes are smaller than a full bitmap; otherwise it sends BITMAP.

Client-to-server `REGION_EXPLORATION` command 25 is playing-only. Its payload is
operation `0`, path cstring, cached-byte-count u16, and that many cache bytes.
Zero count means a cache miss. For a known map a nonzero count must exactly
match its stored bitmap size, with canonical zero padding. For an unknown path,
any bounded cache up to 8192 bytes yields EMPTY, without creating a server map.

The server computes `authoritative_bits & ~cached_bits` and queues only missing
bits. A matching cache sends no BITMAP/PATCH. Client-supplied bits never mutate
server exploration. All requests are fully validated before enqueuing state.
Unknown operations, invalid paths/lengths, trailing/truncated bytes and excess
pending requests fail through the normal bounded packet parser.

RESET is sent for each successful character session. It carries the canonical
authenticated account name so the client can bind a cache under that identity
and the pinned server certificate. No 10,000-map snapshot is sent at login.
Once region definitions are ready, the client requests their map paths using
its cache; live discoveries arrive through the same per-session pending queue.
Different characters on the same account receive shared live bits. Transport
ordering and monotonic OR merges require no revision history, acknowledgments,
subscriptions or reconciliation journal. Exact-version login rejects older
peers.

## Client resource bounds

The optional client cache keeps at most 10,000 records and 1 MiB of bitmap
allocations. This fits 10,000 typical 24×24 maps (720,000 bytes), including
headroom for the small minority of larger maps. An otherwise valid new record
that exceeds either limit is rejected without evicting or modifying existing
records. Oversized optional disk caches are complete cache misses. The server
store's larger persistence limits do not imply equivalent client allocations.

A connection authenticates one account. Repeated active RESETs for that account
leave views and request deduplication unchanged; a different account is rejected
until disconnect. Reconnecting restores hidden cached bits and schedules fresh
requests, including when the account is unchanged.

Replay is deferred to the once-per-frame client service with a shared budget of
65,536 map lookups, bitmap bytes and visited cells. Zero bytes skip cell work;
only set bits reach the view. Each view resumes its cursor, unchanged records
are skipped by revision, and new packets coalesce into a subsequent pass without
restarting the current one. Views take turns when work spans frames. Dense or
large histories therefore appear progressively. Packet handlers and asset/FOW
updates cannot replenish this budget. Changed shared views invalidate each
popup's derived fog zoom, which is rebuilt when rendered.

## Migration and validation

Existing `AEXP0001` server files remain readable. Old per-character client
`client-maps/*.tiles` files remain untouched and are not imported: they have no
server-verifiable ownership or visibility provenance. No account, character,
content artifact or initialized runtime file requires manual edits.

Server tests cover shared accounts, fresh-session reload, bounded parsing,
corruption preservation, failed-save retry, sparse responses, forged caches,
unknown maps, own-session flushing, backpressure and 10,000-map scaling. The
request suite drives normal `draw_client_map2()` through visible, concealed,
upper-level, private and non-region cases. Deterministic counters assert that
idle work and repeated discoveries perform no map/account lookup or full-store
scan. Client tests cover transactional decoding, cache identity, region loading
and incremental rendering. Use the workspace Classic profile for integrated
validation and the supervised isolated lifecycle for runtime checks.
