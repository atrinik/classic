# Access token store

Admission and token history never collect network addresses or address-derived identifiers. The game process allows one authentication attempt per connection and admits canonical codes to a fixed 32-job worker queue; these bounds require no player-IP table. Malformed codes consume no worker jobs, and completed or cancelled jobs release their slots. A full queue can temporarily reject legitimate attempts during overload, but failed attempts do not consume a process-wide minute quota. Retained admission history contains timestamps, not source addresses. Transport libraries may handle addresses transiently to route live traffic, but accepted application sockets do not retain them.

The server owns one `access-tokens.snapshot` in its configured private access
state directory. The directory must exist, belong to the service effective UID,
and have mode 0700. The snapshot is a regular single-link service-owned 0600 file.
Every path component is opened without following symlinks. Ancestors belong to
root or the service UID and have no group/other write permission; a root-owned
sticky public ancestor such as `/tmp` is supported for isolated fixtures. Relative
state paths are anchored at the current working directory without resolving
symlinks. Embedded `.` and `..` components are rejected.

The supervised runtime may explicitly borrow its launcher's already locked data
file descriptor. `access_state_lock_fd` validates an effective-UID-owned 0700
directory and retains the same open-file description with `F_DUPFD_CLOEXEC`,
checking fresh device/inode, type and permissions across duplication. The native
caller validates the command-line descriptor/path binding before invoking this
API. It keeps the original descriptor open for ordinary game persistence paths
and marks it close-on-exec; the supervisor remains responsible for preventing
multiple intentional borrowers. Closing the native reference never explicitly
unlocks the supervisor's lease. An independently opened descriptor still conflicts
with the exclusive lock.

`access_store_open_at` and `access_store_absent_at` accept a single child name
beneath that locked private data capability. They reject slash, dot and dotdot
names, do not follow child symlinks, and preserve the same store validation and
transaction implementation as ordinary paths. Opening a child compares fresh
before/opened/after directory identities; subsequent operations use its pinned
descriptor. Renaming the data pathname cannot redirect these operations. These
APIs do not create a `/proc/self/fd` exception in ordinary path traversal,
external store selection or offline helpers.

A directory `flock` provides exclusive ownership. The enclosing game state lock
must also be held by the runtime and offline inspector so an absent token store
cannot bypass the stopped-state requirement. `access_store_absent` accepts only
an absent final component beneath a verified existing parent, never a missing
ancestor or dangling symlink. Bootstrap is explicit; ordinary open never creates
an empty replacement for missing or malformed state. The inspector acquires the
same exclusive directory lock, validates the same bytes and permissions, and
never creates, reconciles, expires or rewrites anything. It supports read-only
mounts. Open mode without a store is a distinct caller-owned status variant.

The version-1 binary snapshot uses explicit big-endian 64-bit integers,
length-prefixed UTF-8 strings, exact identity binding, and a trailing SHA-256
integrity checksum. This checksum detects corruption, not a malicious writer who
already owns the state directory. The format stores V and I, never C or R. Optional
expiry has an explicit presence field; zero is not an encoded no-expiry value.
Expiry is at most 253402300799. Management revision is distinct from the durable
commit sequence: recording admission never invalidates management CAS or changes
the token's authorization revision.

All serialized snapshots are limited to 8 MiB. The store retains at most 1024
nonremoved tokens, 16 successful admission timestamps per token, 4096 management
receipts for at least seven days, and 4096 minimal removal tombstones for at least
90 days. Receipt exhaustion rejects a new mutation before effects. Tokens are
never evicted to make room. A token's pending remote synchronization flag is
bounded by the 1024 retained tokens; at most 32 operations are exposed in one
outbox dispatch page. Issuance stops with 32 pending routes. Revocation remains
local-first with a full remote backlog, so all 1024 tokens can be durably denied
during an outage. A full tombstone store leaves a removed token revoked and
retains its pending receipt; retry the identical remove request after capacity
becomes available. It never erases required evidence.

Commit writes a uniquely created 0600 temporary file, checks file fsync/close,
checks the destination, atomically replaces it, fsyncs the directory, and validates
a complete readback before exposing success. Failure after replacement poisons
the handle and denies admission. A failed observed-expiry commit also poisons the
handle even if replacement did not occur. Operator recovery must resolve storage
and clock validity before reopening; reopening is not a claim that failed writes
occurred. Unexpected process death may leave protected temporary snapshots; the
runtime does not automatically adopt them or delete uncertain files.

Issuance durably stages a pending token and receipt, invokes a bounded authenticated
remote reserve/activate callback without holding the store mutex, then rechecks
state/revision before locally activating. Concurrent revoke wins. Only the initial
committed response contains the code. Retries/result queries return the receipt
without the code, including after expiry. Interrupted pending issues are durably
revoked on normal startup; inspection never performs that reconciliation. The
remote adapter must derive separate request identities for reserve/activate/revoke
and must never claim commitment without an authenticated durable acknowledgement.

Revoke commits a local deny before remote synchronization. Remove performs the
same local-first revoke under its original request ID, then waits for confirmed
remote revocation before erasing verifier, label and history and writing a minimal
tombstone. Successful admission commits its real observed timestamp before
returning a session reference. No account identity is fabricated: this component
records access-gate admission only. Account-login auditing is not implemented here.

The store mutex serializes management and admission writes. These APIs perform
filesystem I/O and belong on the bounded worker, not the simulation thread.
`access_store_session_check` uses trylock; BUSY pauses dispatch. An observed expiry
remains BUSY across wall-clock rollback until the worker persists expiry or fences
on failure. Worker `access_store_expire` retains the greatest observed tick. A
shutdown flush fences new writes/admissions before checking durability; the caller
still performs normal checked player/account/world saves and transport draining.
Close requires callers to have drained all outstanding API users/callbacks.

In-game administration uses the current playing character’s existing
`/cmd_permission` grant for `access`; `[OP]` grants it automatically. The main
loop checks permission on each request and again before delivering its result.
Workers receive only a copied permission decision, never player/socket pointers.
The local Unix adapter authenticates root peers for bootstrap. Token possession
alone is never administration authorization. Account registration, passwords and
saved OP permissions retain their ordinary behavior. Access policy/store settings
remain startup-only and cannot be changed through `/config`.

## Focused fixtures

`server/src/tests/access_tokens/store_fixture.c` is an isolated C17 fixture that
includes the store implementation to inject ENOSPC, file fsync, directory fsync,
and rename failures. Compile with the server include directory and an include
root containing `toolkit/access_code.h`, linking the matching `access_code.c`,
OpenSSL crypto and pthread. Run the resulting executable normally and with
`--capacity` for the 1024-token outage/revoke/reopen test. Address/undefined
sanitizers are supported. These fixture builds are not integrated application or
live-game acceptance.

Reserve-only ownership collisions use the private callback outcome
`ACCESS_ROUTE_COLLISION`. At most three total credential generations are attempted;
each remote retry durably stages a fresh token/index and independent remote request
ID while retaining the original management receipt. Activation conflicts remain
ambiguous and never trigger collision retry. Every attempt shares one monotonic
30-second deadline; the authenticated route adapter must honor the supplied
remaining budget. Terminal revocations have priority in the bounded outbox page.
The worker retries one pending revocation per maintenance pass, rotating stable
token IDs across all retained pending revocations. Pending maintenance interleaves
with queued requests; this does not guarantee admission during sustained overload.

Remaining integration boundaries: native
Windows private state/IPC has no supported implementation in this POSIX module;
parent platform selection must explicitly reject protected operation, while
preserving the existing open-server build. Full server/client builds,
actual account/player behavior, and adapter deadline/dispatch proofs belong to
the coordinated integration acceptance.

The store fixture also checks descriptor ownership/mode/type rejection, fresh
identity mismatch, strict path and child-symlink rejection, retained state after
pathname replacement, and shared-lock lifetime across a forked borrower. Wrong
ownership and descriptor-rebinding observations are injected deterministically;
these cases need no privileged filesystem changes.

### Supervised inherited data capability

Linux builds emit `atrinik-server-capabilities.json` beside the linked executable.
Its exact schema is `schema_version: 1`, `datapath_fd: true`, and `server_sha256`
(the lowercase SHA-256 of that executable). A wrapper verifies the artifact and
binary under its immutable-generation lease before selecting this contract.
Windows builds do not advertise it.

The supervisor may pass the literal command-line pair `--datapath_fd=N
--datapath=./data`, where N is a canonical decimal descriptor greater than 2.
Startup pins the generation working directory with a close-on-exec descriptor,
requires root or effective-user ownership and no group/other write permission,
and performs link checks relative to that descriptor. The generation's `./data` must be a symlink with the exact target
`/proc/self/fd/N`; startup checks the link and target identity against the open,
owned, mode-0700 directory. The supervisor must pin the generation, repeat the
identity check immediately before launch, and retain its exclusive shared lock.
Ordinary path opening never accepts this symlink exception. The logical `./data`
spelling preserves existing private-map and savebed identities.

The original descriptor remains open for the entire process and is marked
close-on-exec. The server retains its own close-on-exec duplicate of the same
locked open-file description; cleanup closes its duplicate and never unlocks the
supervisor's lease. Default token-store children are opened relative to this
pinned descriptor with the usual no-follow checks. Explicit store paths retain
their ordinary strict path checks.

Configuration files (including nested includes), value-file expansion, offline
initialization/scenario/test modes, and Windows cannot supply this capability.
