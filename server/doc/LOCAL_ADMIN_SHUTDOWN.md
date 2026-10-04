# Local administrative shutdown

Linux operators may explicitly enable `--admin_shutdown_socket=/absolute/path/server.sock`.
The Docker entrypoint forwards `ATRINIK_ADMIN_SHUTDOWN_SOCKET` when set. The default
is disabled. A local `server/server-custom.cfg` overlay can set
`admin_shutdown_socket = /absolute/path/server.sock`; this operator-owned file is
ignored by Git and must stay out of pull requests. Unit/plugin tests, scenario provisioning, content benchmarks, celestial
inventory, and world-maker modes never open the endpoint. Access administration also has a separately permission-gated in-game interface;
the local socket never accepts a claimed operator account identity.

Provision the immediate directory as mode 0700 owned by the server UID (the image
uses 10001), and mount that directory into the container at the configured path.
Every ancestor must be owned by root or the server UID and must not be group- or
world-writable. Symlinks, relative paths, and existing endpoint paths fail startup.
The socket is mode 0600. The server never removes an unknown stale socket; inspect
its ownership and process identity before operator-managed recovery.

Only a Linux `SO_PEERCRED` UID 0 peer may send requests. No network listener, shell,
configuration editing, arbitrary command execution, or player impersonation is
provided. An updater must verify the socket's kernel peer UID and PID against the
specific running server/container identity before sending any request. A container
runtime init wrapper changes the main container PID; use the actual server PID or
an entrypoint that directly execs the server. Dedicated server UID and a private
root-controlled mount location are part of that identity boundary.

Requests are at most 1024 bytes. Send one complete line, then half-close the write
side. The game thread performs bounded nonblocking accepts and reads, with one
active connection and a two-second incomplete-request timeout. Exact messages:

```
ATRINIK-ADMIN/1 CAPABILITIES
ATRINIK-ADMIN/1 SHUTDOWN 0123456789abcdef0123456789abcdef 60 Maintenance; reconnect soon.
```

Capabilities respond with `ATRINIK-ADMIN/1 CAPABILITIES shutdown-v1 durable-result-v1`.
A shutdown request uses a fresh cryptographically random 32-character lowercase
hex request ID, canonical decimal seconds from 30 through 600, and 1 through 192
bytes of valid UTF-8 reason text without controls or line separators. The response
`ATRINIK-ADMIN/1 SCHEDULED ID` means that the countdown and initial player warnings
were scheduled; it does not assert successful saving. Matching retries are
idempotent and never extend the countdown. A conflicting pending request or an
already-used ID is rejected. Errors use `ATRINIK-ADMIN/1 ERROR CODE`.

The countdown reuses the game timer and broadcasts the normal initial warning,
reason, minute reminders and five-second warning. A manual `/shutdown stop`, a new
manual countdown, or an unrelated early shutdown cancels the update request.
There is no remote cancellation operation.

After shutdown persistence, the server exclusively publishes
`SOCKET.ID.result` in the private directory with mode 0600. It fsyncs the complete
file, atomically links it into its final name without overwriting an existing file,
and fsyncs the directory. Content is one line:

```
ATRINIK-ADMIN/1 RESULT ID saved
```

Other terminal states are `failed` and `cancelled`. `saved` requires timer expiry
and successful checked account/player/map/world-clock/map-log/journal shutdown.
Failure to save or publish completion causes a nonzero server exit. The updater
must require **both** the matching, owner/mode-verified `saved` receipt and clean
exit of the exact process/container it authenticated. A scheduled acknowledgement,
a stopped process, or exit zero alone is insufficient. Missing receipts or failed
checks must stop the update; sending SIGTERM to bypass the countdown is unsupported.
Receipts are intentionally retained for the root operator/updater to inspect and
remove after completing its transaction; server startup never overwrites them.

Validation uses the server's `server.admin_shutdown` Check suite plus the standalone
`src/tests/admin_shutdown_socket.py LIBRARY PRIVATE_DIRECTORY` fixture in pinned
Linux build workers. Build `src/server/admin_shutdown.c` as a shared library with
`-Isrc/include -std=c17 -Wall -Wextra -Werror -shared -fPIC`. Run the fixture once as
the normal build UID for peer rejection and once as root inside an isolated worker
with a private root-owned tmpfs directory for request and receipt behavior. These
fixtures use no production state. Runtime acceptance additionally requires a
synthetic connected player observing warnings and checked shutdown in an isolated
wrapper topology.

## Access-token administration

When the access worker is configured, the capability set additionally contains
`access-tokens-v1`. Clients parse capabilities as bounded unordered tokens. The
root peer, socket ownership, trusted ancestry, and server UID/PID checks above
also apply to access requests. Windows has no local issuance fallback.

Send `ATRINIK-ADMIN/1 ACCESS <compact JSON>\n`, then write-half-close. The complete
request including framing is at most 1024 bytes. One strict UTF-8 object contains
`schema:"atrinik-access-admin-v1"`, `operation`, and a fresh 32-character lowercase
hex `requestId`. Duplicate, unknown, cross-operation keys and trailing commands
are rejected. The operations accept these additional fields:

| Operation | Fields |
| --- | --- |
| `issue` | `expectedRevision`, `label`, optional `expiresAt` |
| `list` | optional `revision`, `cursor`, `limit` |
| `history` | `tokenId`, optional `revision` |
| `revoke`, `remove` | `expectedRevision`, `tokenId` |
| `status` | none |
| `result` | `targetRequestId` |

Revisions are canonical unsigned-64 decimal **strings**. Token/request identities
are 32 lowercase hex characters. Labels are 1–128 UTF-8 bytes without controls.
Omitted expiry means never; explicit expiry is a positive UTC-seconds decimal
string, at most `253402300799`. List limits are JSON integers 1–64. A returned
cursor is opaque to callers and must be supplied with its page revision; stale
revisions conflict rather than mixing pages. History describes real admissions,
not administrative reads.

The worker performs durable operations outside the simulation thread. Request
reads retain their two-second deadline; execution is bounded to 30 seconds and
response delivery to 35 seconds from the completed request. Responses support
partial nonblocking writes:

```
ATRINIK-ADMIN/1 ACCESS N
```

Exactly `N` JSON bytes follow, then EOF. `N` is canonical decimal 1–32768. The
response envelope contains exactly `schema`, `operation`, `requestId`, `outcome`,
`revision`, and `result`. Initialized revisions are decimal strings. Absent open
status uses JSON `null` for the envelope revision. Closed outcomes contain no raw
filesystem diagnostics. Only the initial committed `issue` result can contain
`code`; recovery and retries never return it. A disconnected client does not
cancel or roll back a committed mutation. Recover by original request ID using
`result`, then explicitly revoke/remove the inaccessible token if needed.

`tools/access_admin.py` verifies no-symlink socket ancestry, endpoint owner/mode,
and actual kernel server UID/PID before transmitting. Those identity arguments
must identify the actual server process, including any container PID mapping.
For example, read status with:

```
python3 tools/access_admin.py --socket /private/admin/server.sock \
  --server-uid 10001 --server-pid SERVER_PID status
```

All seven operations are exposed as subcommands; use `--help` for their flags.
The CLI reports the generated request ID to stderr before sending. Issuance
requires interactive terminal input and writes the code directly to the
controlling terminal, or accepts an explicitly preopened `--code-fd` greater than
2 referring to a linked, writable, owner-owned mode-0600 regular file. It never
accepts a code in argv/environment, and ordinary stdout contains only metadata.
Validate the code destination before issuance; failure or a lost response must
use receipt recovery rather than a fresh issuance ID. Python cannot guarantee
that every temporary immutable string is erased from its process memory.

Status results are exact tagged unions. Initialized stores contain `state`
(`initialized`), `schemaVersion` (1), certificate `serverIdentity` (64 lowercase
hex), `policy` (`open` or `protected`), `integrity` (`ok` or `failed`), `durability`
(`ok` or `indeterminate`), `revision`, and integer `pendingRouteSync` (0–32).
Absent open stores contain only `state:"absent_open"`, `schemaVersion`,
`serverIdentity`, and `policy:"open"`. Missing protected state fails. Empty or
fully expired initialized protected stores remain protected and valid for update
health; status never renews or evaluates individual token expiry.

The native `atrinik-access-status` executable emits the identical bare status
union followed by LF for stopped-server inspection:

```
atrinik-access-status --data-dir /data --store-dir /data/access-tokens \
  --certificate /data/quic-identity.pem --policy protected
```

Run as the service UID against the complete state. It acquires the same exclusive
data-directory lock held by a running server before reading the store under its
exclusive lock. It opens no listeners, never initializes/reconciles state, and
works with a read-only state mount. It verifies trusted no-symlink ancestry and
an owner-owned mode-0600 bounded certificate file, hashes the DER leaf certificate,
and fails for malformed state, ownership failures, or an active server. Only a
truly missing final store directory under trusted ancestry qualifies as absent
open state. Parent path failures and dangling links never qualify.

Access persistence participates in checked shutdown before a successful durable
shutdown receipt; the updater must drain administrative writes under its operation
fence and back up the store, receipts, audit/outbox, identity, configuration, and
account/player/world state together. Existing protected state is never silently
recreated after loss. Bootstrap is an explicit separate server operation.

Focused validation adds `src/tests/access_admin_cli_test.py` (isolated Python
framing/output fixtures), `src/tests/access_admin_native.py LIBRARY PRIVATE_DIR`
(real native store/adapter lifecycle), and root-only
`src/tests/access_admin_socket.py LIBRARY PRIVATE_DIR` (standalone endpoint).
`src/tests/access_status_test.py BINARY STORE_LIBRARY PRIVATE_DIR` checks the
offline tagged union, exclusive locks, unchanged snapshots, and certificate
protections. Each native fixture uses a separate empty mode-0700 directory in the pinned Linux
worker; the original shutdown fixtures still run with root and non-root peers.

For cross-consumer status acceptance, the existing offline fixture optionally
accepts `--socket-library LIBRARY --evidence-dir NEW_DIRECTORY --source-sha SHA`.
Run this mode in the isolated root fixture worker. It records exact inspector
stdout and actual root-socket frames for initialized protected, initialized open,
and absent open state, plus bounded provenance and hashes. The fixture creates no
tokens; it verifies the exact status field sets before writing evidence. Export
only the evidence directory, never its neighboring test certificate, key, or
store directories. Record every native provider revision separately when a
fixture binary combines unintegrated source branches.
