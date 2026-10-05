# Local administrative shutdown

Linux operators may explicitly enable `--admin_shutdown_socket=/absolute/path/server.sock`.
The Docker entrypoint forwards `ATRINIK_ADMIN_SHUTDOWN_SOCKET` when set. The default
is disabled. A local `server/server-custom.cfg` overlay can set
`admin_shutdown_socket = /absolute/path/server.sock`; this operator-owned file is
ignored by Git and must stay out of pull requests. Unit/plugin tests, scenario provisioning, content benchmarks, celestial
inventory, and world-maker modes never open the endpoint. Existing in-game commands
and game protocol IDs are unchanged.

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
Account exploration sidecars, including dirty accounts retained after socket
teardown, participate in the checked persistence result.
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
