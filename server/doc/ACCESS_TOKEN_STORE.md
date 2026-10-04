# Access token store

The server owns one `access-tokens.snapshot` in its configured private access
state directory. The directory must exist, belong to the service effective UID,
and have mode 0700. The snapshot is a regular single-link service-owned 0600 file.
Every path component is opened without following symlinks. Ancestors belong to
root or the service UID and have no group/other write permission; a root-owned
sticky public ancestor such as `/tmp` is supported for isolated fixtures. Relative
state paths are anchored at the current working directory without resolving
symlinks. Embedded `.` and `..` components are rejected.

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

The root-managed account allowlist is independent of inherited OP permissions.
Its path has root-owned, non-writable ancestry. The file is regular, single-link,
root-owned and either 0600 when read by root or 0440/0640 with the service effective
group. No group-write/other access is allowed. It contains at most 256 distinct
canonical lowercase ASCII account names, one per LF-terminated line, at most
16 KiB. Missing/invalid files deny. Renaming an account requires root to update
this file. The adapters must authenticate the local peer or canonical account
before calling management operations; token possession is never authorization.

## Focused fixtures

`server/src/tests/access_tokens/store_fixture.c` is an isolated C17 fixture that
includes the store implementation to inject ENOSPC, file fsync, directory fsync,
and rename failures. Compile with the server include directory and an include
root containing `toolkit/access_code.h`, linking the matching `access_code.c`,
OpenSSL crypto and pthread. Run the resulting executable normally and with
`--capacity` for the 1024-token outage/revoke/reopen test. Address/undefined
sanitizers are supported. These fixture builds are not integrated application or
live-game acceptance.

Remaining integration boundaries: remote ownership-collision regeneration needs
a callback result distinguishing reserve collision from an ambiguous activation
failure; current failure retains pending state and never releases a code. Native
Windows private state/IPC has no supported implementation in this POSIX module;
parent platform selection must explicitly reject protected operation, while
preserving the existing open-server build. Root-owned allowlist positive fixtures
require a root-capable isolated test environment. Full server/client builds,
actual account/player behavior, and adapter deadline/dispatch proofs belong to
the coordinated integration acceptance.
