=================================================
=                Atrinik Server                 =
=================================================

[![Coverage](https://codecov.io/gh/atrinik/classic/graph/badge.svg?branch=main&flag=server)](https://codecov.io/gh/atrinik/classic)

 Website: https://www.atrinik.org/

 Dedicated server for the Atrinik game. Protocol declarations and shared C
 facilities come from checksum-pinned source releases. Game content and
 runtime resources come from separately versioned, checksum-pinned archives;
 no Git submodules are required.

=================================================
= 1. Compiling the Atrinik server               =
=================================================

 See INSTALL for complete dependency instructions. From this repository root:
  $ python3 tools/dependencies.py sync
  $ cmake --preset linux-debug
  $ cmake --build --preset linux-debug
  $ ctest --preset linux-debug

 The executable and plugins are written directly to build/linux-debug/. Each C
 suite is a separately addressable CTest test. List or select suites with:
  $ ctest --test-dir build/linux-debug -N
  $ ctest --test-dir build/linux-debug --output-on-failure \
      -R '^server-unit-toolkit.packet$'

 To collect line, function, and branch coverage from the native tests:
  $ cmake --preset linux-coverage
  $ cmake --build --preset linux-coverage
  $ ctest --preset linux-coverage
  $ gcovr --root . --filter 'src/' --exclude 'src/tests/' --print-summary

 The server's simulation, monotonic, UTC wall, round-generation, and authored
 world-time domains are documented in doc/SERVER_CLOCKS.md. New deadlines must
 use the typed server clock API described there.

 The exact source dependencies are recorded in cmake/dependencies.lock.json.
 The first configure downloads their release archives and verifies their
 SHA-256 digests. CMake's FETCHCONTENT_SOURCE_DIR_ATRINIK_PROTOCOL and
 FETCHCONTENT_SOURCE_DIR_LIBATRINIK overrides support coordinated local work.

 Releases are produced from every squash merge to main. semantic-release
 parses the Conventional Commits pull-request title. Classic stays on the
 5.x.x line: a breaking marker, BREAKING CHANGE, or feat produces a minor
 release, and every other conventional type produces at least a patch release.
 Each unified release contains an atrinik-classic-server-VERSION source
 archive, a portable atrinik-classic-server-VERSION-windows-x86_64 ZIP,
 SHA-256 checksums, and a versioned `ghcr.io/atrinik/classic-server` image. The
 newest release also updates the `latest` image alias.
 The scoped source archive embeds the matching protocol and libatrinik source
 under dependencies/ and selects it automatically outside the monorepo.

=================================================
= 2. Running the server                         =
=================================================

 Synchronize the pinned runtime archives, build, and prepare managed links:
  $ python3 tools/dependencies.py sync
  $ cmake --preset linux-debug
  $ cmake --build --preset linux-debug
  $ ./tools/prepare-runtime.sh build/linux-debug
  $ ./server.sh

 The preparation script initializes data/ without replacing existing mutable
 state and creates links to the built server, plugins, content, and resources.
 It refuses to overwrite unmanaged lib or maps paths. Run it again after
 updating a dependency lock or changing build directories.

 For a quiet, local-only run that exercises QUIC without public discovery:
  $ ./server.sh --port_mapping=off --stun_server=off

 The startup log prints the QUIC certificate SHA-256 fingerprint. The matching
 pinned-client procedure is documented in the client repository.

 The launcher passes extra options straight to the binary:
  $ ./server.sh --version

 SIGINT and SIGTERM request the same orderly save and cleanup path as the
 console `shutdown` command. Process supervisors should allow that cleanup to
 finish before using a forced termination.

 It is recommended to generate region maps before starting the server. The
 repository helper performs the build, dependency synchronization, runtime
 preparation, and generation sequence:
  $ ./generate-region-maps.sh

 The generator does not open game or HTTP listening sockets. Generated PNG and
 definition files are written to assets/client-maps/.

 The region maps are not mandatory to play the game, however, some features may
 not work (specifically, the client will only be able to work with dynamic area
 maps and not the full region maps).

 By default, QUIC clients download the manifest, required game data, resources,
 and region maps on separate typed streams of the established game connection.
 Up to three downloads make concurrent progress, while the long-lived game
 stream is serviced first on every network pass. The optional operator-managed
 HTTP origin/CDN path is described in section 2.2. At startup the server reads
 each immutable allowed asset once, computes its SHA-256 digest, and keeps both
 bytes and digest in memory. Region maps are cached per server; later QUIC
 requests send the cached size and digest so an unchanged map is confirmed
 without retransmitting its contents. Restart after changing collected assets
 so the server publishes a new immutable snapshot.

=================================================
= 2.1. Configuring the server                   =
=================================================

 The server has a number of configuration options, optimized for local server
 play. However, if you feel like tweaking them, you will need to change the
 configuration file server.cfg, or create a new file called server-custom.cfg
 and put your custom settings there (this is the preferred way, especially when
 working from a Git repository).

 You can examine the server.cfg file to learn about most of the default settings
 and what they do. Additionally, you can use the -h or --help option when
 executing the server to learn about all the possible options (there is a 1:1
 mapping between the configuration files and arguments passed on the command
 line, so you can use the same options either way; eg, "port = 900" in a
 configuration file and ./atrinik-server --port_quic=900 are the same thing).

 See section 3. for information about running a public server and the necessary
 configuration.

=================================================
= 2.2. External HTTP origin/CDN                 =
=================================================

 HTTP asset delivery through an operator-managed origin is optional and
 disabled by default:
  http_url = off

 With http_url set to off, QUIC clients retrieve assets directly from the game
 server. This is the recommended configuration for community and friend
 servers because it needs no additional public port or hostname.

 A large dedicated server can explicitly deploy a static HTTPS origin or CDN
 and then configure its public base URL:
  http_url = https://cdn.example.com/atrinik

 When http_url is configured, clients first obtain each asset's immutable size
 and SHA-256 digest over the authenticated QUIC connection, then accept the HTTP
 body only when both match. A failed or mismatched request falls back to
 in-band QUIC delivery. Non-loopback origins must use HTTPS.

 The external service must map the following URLs to their directory
 counterparts:
  - https://cdn.example.com/atrinik/          -> assets/
  - https://cdn.example.com/atrinik/resources -> resources/

 Publish the exact `assets/` and release-matched `resources/` snapshot before
 starting the game server with `http_url`. Update the origin atomically and
 invalidate stale CDN entries before restarting after an asset change;
 otherwise clients will reject stale bodies and transfer them again over QUIC.
 Use a public base URL without embedded credentials, query parameters, or a
 fragment because the URL is advertised to every connecting client.

 Atrinik generates or stages the files and advertises `http_url`; it never
 starts or supervises an HTTP listener. Serve only these roots as read-only
 static content, disable uploads and directory listings, and never expose the
 rest of `data/`, which contains mutable and private server state. The operator
 owns deployment, TLS, access controls, cache policy, monitoring, and
 availability. Non-loopback origins must use HTTPS; plain HTTP remains accepted
 only for loopback development origins.

 `assetspath` selects this transport-neutral staging root. The removed
 `httppath` option is rejected with an instruction to migrate; there is no
 mixed-name compatibility mode. `http_url` retains its name because it only
 describes the optional external HTTP(S) origin, not the default QUIC delivery.

=================================================
= 2.3. Running with Docker Compose               =
=================================================

 Release images are published to `ghcr.io/atrinik/classic-server`. Pin a
 version for a persistent deployment, then start it from the repository root:
  $ cp server-custom.cfg.example server-custom.cfg
  $ mkdir -p server-data
  $ ATRINIK_SERVER_IMAGE=ghcr.io/atrinik/classic-server:5.6.0 \
      LOCAL_UID=$(id -u) LOCAL_GID=$(id -g) \
      docker compose -f compose.server.yaml up --no-build -d

 The `latest` tag follows the newest release, but a versioned tag avoids an
 unexpected server upgrade. To build the current source locally instead:
  $ cp server-custom.cfg.example server-custom.cfg
  $ mkdir -p server-data
  $ LOCAL_UID=$(id -u) LOCAL_GID=$(id -g) \
      docker compose -f compose.server.yaml up --build -d

 On a native Linux Docker Engine, the Compose service uses host networking so
 candidate gathering and the PCP/NAT-PMP/UPnP libraries can see the physical
 host's interfaces and default gateway. Docker Desktop still runs Linux
 containers inside its VM; host mode there can expose the VM gateway instead
 of the physical router, preventing automatic router discovery. There are no
 Docker `ports` translations: direct gameplay and in-band asset delivery bind
 host UDP port 1730. The container does not provide an HTTP listener; an origin
 configured through `ATRINIK_HTTP_URL` is a separate operator-managed
 deployment. Player data, the persistent QUIC identity, and other mutable state
 are kept directly in the host's server-data/ folder. Region maps are generated
 while building the image and refreshed into that folder when the container
 starts. On Linux, LOCAL_UID and LOCAL_GID keep those files owned by the user
 running Docker.

 To follow logs or stop the service:
  $ docker compose -f compose.server.yaml logs -f
  $ docker compose -f compose.server.yaml down

 The server-data/ folder is preserved by 'down' and can be backed up with
 ordinary filesystem tools. The container health check verifies both that the
 server process exists and that its game-loop heartbeat is no more than 60
 seconds old; a wedged process therefore becomes unhealthy even if PID 1 still
 exists.

 For a private server, mount a Docker secret or another mode-0600 file and set
 `ATRINIK_JOIN_PASSWORD_FILE` to its container path. The entrypoint prefers that
 file over `ATRINIK_JOIN_PASSWORD`, avoiding disclosure through the container
 environment. Native deployments can use `--join_password_file=PATH`.

 For a public direct server, set ATRINIK_SERVER_PUBLIC=true and optionally
 ATRINIK_JOIN_PASSWORD. No hostname or HTTP URL is required. Large dedicated
 servers can deploy an HTTPS origin/CDN separately and set ATRINIK_HTTP_URL to
 its base URL; clients will prefer it and fall back to QUIC if it is
 unavailable:
  $ ATRINIK_SERVER_PUBLIC=true \
      ATRINIK_HTTP_URL=https://cdn.example.com/atrinik \
      docker compose -f compose.server.yaml up -d

 Host networking on a native Linux Docker Engine is required for automatic
 router discovery in this Compose deployment. A discovered gateway in a Docker
 Desktop private subnet (for example, `192.168.65.0/24`) is the VM gateway, not
 evidence that PCP can reach the physical router. If another service already
 owns one of the Atrinik ports, change the corresponding server configuration
 before starting the container. Set ATRINIK_PORT_MAPPING=off only when the host
 has a direct global address or the route is managed outside Atrinik.


Account password storage and upgrades
-------------------------------------

New and changed account passwords are stored as versioned Argon2id records.
The current record explicitly selects Argon2 version 19, 64 MiB of memory,
three iterations, and one lane. The server refuses records outside its bounded
8-256 MiB memory, 1-10 iteration, and 1-4 lane verification limits before any
password work is performed. Authentication work also uses a server-wide token
bucket with an eight-operation burst and one new token every two seconds; a
connection is disconnected after three failed account passwords.

Existing crypt(3) and 4,096-iteration PBKDF2/SHA-256 records follow an explicit
rehash-on-success policy. They remain unchanged on disk after failed logins and
are replaced atomically with an Argon2id record only after the old password is
successfully verified. Atrinik has no offline account migration step, and
building or testing does not rewrite source-tree account data.

Before first starting this version against mutable data, stop the server and
back up the complete data/accounts directory. Keep that backup until all active
accounts have logged in successfully. Rollback to an older server requires
restoring the backup because older versions cannot read Argon2id records; never
mix account files from before and after the upgrade. Account files are written
through a mode-0600 temporary file, flushed to stable storage, and atomically
replaced. SHA-1-named certificate-chain cache entries are intentionally ignored
after this upgrade and recreated under SHA-256 names; they are cache data and
must not be migrated.

Offline local scenario provisioning
-----------------------------------

The server can provision one account and first-login character into an
isolated, stopped data directory for local manual testing. The Atrinik
workspace wrapper owns the supported workflow; use its `scenario` commands
instead of invoking this mode directly or constructing account/player files.

`--provision_scenario` requires `--provision_account`,
`--provision_character`, `--provision_archetype`, and
`--provision_password_file`. The password file must be a regular,
non-symlink file owned by the current user with mode 0600. It contains one
password with either no line terminator or one LF/CRLF terminator.
The mode initializes game data but starts no listeners, plugins, metaserver,
or console, creates the account through the normal Argon2id persistence code,
reserves an empty player file through exclusive creation, and exits. The first
client login then follows normal character initialization, including the
configured starting map, skills, and items. Existing account or character
files are never replaced, and a partial provisioning attempt is rolled back.

Offline authored-content benchmarks
-----------------------------------

`--content_benchmark` measures the existing text-loader path without opening
listeners or initializing asset serving, plugins, metaserver registration, or
the console. Supply one to sixteen unique logical map IDs as a comma-separated
argument. The mode reports tab-separated, machine-readable records prefixed by
`ATRINIK_CONTENT_BENCHMARK` and exits after measuring initialization, peak
startup RSS where the operating system supports it, forced original map loads,
warm in-memory lookups, swaps, and temporary-map reloads. Nine samples per map
are collected by default; `--content_benchmark_iterations=1..100` overrides
that count.

Use the workspace wrapper with a dedicated profile and isolated state so the
benchmark has exact component inputs and cannot mix its temporary map files
with a running world. For example:

```
./atrinik run server --profile syntax-decision --state syntax-benchmark -- \
  --content_benchmark=/maps/small,/maps/medium,/maps/large \
  --content_benchmark_iterations=9
```

The map IDs above are placeholders; use the representative IDs recorded by the
content syntax evaluation. Original-load samples use `MAP_FLUSH` deliberately
to isolate authored map parsing and instantiation from unique-item overlays,
and the harness deletes each reloaded instance after timing so the next sample
cannot take the server's early warm-map return.

Authoritative gameplay metrics
------------------------------

Lifetime character and account gameplay metrics are private server state.
Character metrics are stored beside `player.dat` in a versioned, atomically
written mode-0600 `metrics.dat`; account metrics are part of the atomic account
file. Back up and restore the complete `data/players` and `data/accounts`
trees, never just one file from a character directory. The operator-only
`/metrics <player> [character|account|all] [category]` command can inspect an
online character and its authenticated owning account. See `doc/METRICS.md`
for registry, persistence, event, privacy, and analytics semantics.

=================================================
= 2.5. Direct QUIC hosting (no port forwarding) =
=================================================

 Current clients and servers use UDP/QUIC for direct play. The metaserver is
 only a public directory and signaling rendezvous: it exchanges short-lived
 UDP candidates and never carries game packets.

 Put the following in server-custom.cfg for a public friend server:
  [meta]
  server_public = true
  server_name = Your Server Name
  server_desc = Private server for friends
  port_quic = 1730
  stun_server = off
  port_mapping = auto
  join_password = choose-a-long-password

 'server_public' is opt-in. If false, the server is not returned by the public
 directory. The join password is verified by the game server over encrypted
 QUIC; the metaserver receives only the boolean fact that a password is
 required. Failed attempts are compared in constant time and limited per peer
 to five per minute, with a 256-per-minute server-wide ceiling. Use a long
 randomly generated password and prefer
 `join_password_file`/`--join_password_file` over command-line or environment
 values. The file must be a regular, non-symlink file containing exactly one
 nonempty line shorter than 1024 bytes. Mode 0600 is recommended; group/other
 permissions produce a startup warning.

 A listed password-protected server also requires a high-entropy rendezvous
 invite before the metaserver reveals any transient QUIC candidate. On first
 protected start, the server creates `data/rendezvous-invite` as an exclusive
 owner-only regular file (mode 0600 on POSIX, protected owner DACL on Windows)
 containing one capability bound to this server's QUIC identity and valid for
 at most seven days. Share the file itself with invited players through a
 protected channel; never paste its contents into server configuration, a
 command line, chat, or logs. The path alone can be changed with
 `rendezvous_invite_file` or `--rendezvous_invite_file=PATH`.

 The server reuses a valid capability across restarts. To revoke or rotate it,
 stop the server, delete that file, and restart; a replacement is generated.
 An expired, permissively readable, malformed, replaced, or wrong-server file
 fails closed instead of being silently overwritten. The separate human join
 password is still authenticated by the game server only after certificate-
 pinned QUIC has connected.

 Addressless listings expose no direct endpoint, so their only connection path
 is the invite-authorized rendezvous exchange. The legacy numeric `server_host`
 setting is no longer published: a future dedicated DNS-hostname setting and
 matching metaserver contract will provide the explicit direct fallback
 without persisting a raw IP address.

 The server discovers same-LAN and direct global IPv6 addresses, tries PCP
 with automatic NAT-PMP negotiation and UPnP IGD to create and renew a UDP
 router mapping, and optionally uses configured STUN for a server-reflexive
 fallback. These automatically discovered endpoints are transient rendezvous
 candidates only; they are never written to the public directory. Rendezvous
 opens a bounded bilateral punch window before QUIC checks begin. Both sides
 send ten paced probes over approximately one second. The QUIC listener
 recognizes an incoming probe before OpenSSL and
 replies one-for-one so the client can learn a peer-reflexive server endpoint.
 The client races candidate handshakes concurrently, preferring LAN and global
 IPv6 routes by default. Its STUN-created IPv4 socket is retained for the
 highest-priority NAT-derived candidate. A private intermediate router mapping
 remains a candidate but is not published as the public directory endpoint;
 ULA IPv6 addresses are classified as LAN.
 Only a completed, certificate-pinned QUIC handshake confirms that any
 candidate is usable.

Some carrier-grade/symmetric NATs and UDP-blocking networks still cannot
establish a direct path when the router offers no mapping protocol and there
is no global IPv6 address. There is intentionally no TURN/game relay fallback,
so those networks must use IPv6, a VPN/overlay, or a separately managed direct
route. Failure is reported explicitly as “No confirmed direct route”.

The client log reports candidate kinds, timing, and the selected route without
recording transient addresses. Operators
with the `stats` or `/stats` command permission also see each player's reported
mode and connection ID in `/who`, formatted as `(route: QUIC/mapped; connection:
...)`. This is client-reported diagnostic metadata, not an authorization
 signal.

 STUN is disabled by default so neither client nor server contacts an implicit
 third party. Operators who want server-reflexive discovery can set
 `stun_server` to a provider they trust; automatic router mapping and direct
 LAN/IPv6/directory candidates do not require it.

 Direct servers are identified by their QUIC certificate fingerprint rather
 than a DNS hostname. The server generates data/quic-identity.pem on first
 start and reuses it thereafter. Keep the file
 private and include it in server-data backups; replacing it makes the
 metaserver and clients see a different server.

 Asset requests use the same punched QUIC connection as gameplay by default,
 but use independent QUIC streams so loss or ordering on a bulk transfer
 cannot hold later gameplay bytes behind an asset body.
 Setting http_url advertises a separately managed origin/CDN and makes it the
 preferred asset source after its body is pinned to metadata from the
 authenticated game connection, with automatic QUIC fallback. Region-map
 responses are verified by SHA-256 and stored in a cache scoped by stable
 server identity; matching cached files receive a compact not-modified
 response. The metaserver never carries assets or live game traffic.

 The startup asset snapshot accepts files up to 128 MiB and at most 1 GiB in
 aggregate. Each authenticated connection is limited to 256 asset requests and
 8 MiB of asset payload per second. The latter is a token-bucket pacer: normal
 exhaustion pauses bulk writes instead of disconnecting the client. Each
 connection has at most three active asset streams, serviced round-robin in
 16 KiB quanta after gameplay. The game FIFO retains its 4 MiB/4096-packet hard
 ceiling and never contains asset bodies. Administrators can inspect separate
 game queue, asset throughput/active-stream/rejection, QUIC, mapping,
 connection, and game-loop percentile metrics with `/stats network`.

=================================================
= 3. Running a public server                    =
=================================================

 !!!
 !!! WARNING:
 !!! READ ALL OF THIS CAREFULLY. FAILURE TO DO SO
 !!! MAY LOCK YOU OUT OF METASERVER ACCESS.
 !!!

 There are several security considerations for running your own server. First
 off, note that with default configuration, any player character is by default
 in development mode. You will want to disable this with the following
 configuration:
  default_permission_groups = None

 Second, by default, the server listens on both IPv4 and IPv6 loopback
 interfaces (if present). It is generally recommended to disable the network
 stacks you don't need, or if you want to support both IPv4/IPv6 and you have
 a system with dual-stack network support, you may enable the configuration for
 that instead. Some examples:
  - IPv4-only:
     network_stack = ipv4=127.0.0.1
  - IPv6-only:
     network_stack = ipv6=::1
  - Dual-stack:
     network_stack = dual

 Third, keep data/quic-identity.pem private and back it up. QUIC encrypts game
 traffic and the client pins this identity's certificate fingerprint.

 In order to advertise the server to the public, you will need to configure
 metaserver options. This enables bounded publication of your server details
 to the Atrinik metaserver. See section
 3.1. for details about the sort of information this exposes. The necessary
 options are:
  server_public = true
  server_name = Your Server Name
  server_desc = Description about your server.

 QUIC does not require server_host or an HTTP URL. `server_host` is a retained
 legacy numeric-IP option and is no longer published; leave it unset for an
 addressless listing. Automatic mapping, STUN, LAN, and global IPv6 discovery
 remain transient rendezvous candidates.

 Direct mode automatically tries a router mapping and does not normally require
 a manual port-forwarding rule. The local firewall must still allow the server
 to send and receive UDP on port_quic, and the router must support PCP,
 NAT-PMP, or UPnP unless direct IPv6 is available. See section 3.2. for details.

 After you have all of the above set up, the next time you launch the server,
 it should connect to the metaserver and update it with your server
 information, allowing players with a compatible client to connect.

 Every publish is one HTTPS POST to `publish.meta.atrinik.org`, signed by the
 persistent P-256 private key in `data/quic-identity.pem`. There is no
 separate metaserver registration key or OTP/challenge request. Keep the QUIC
 identity private and include it with server-state backups; replacing it
 intentionally creates a different server identity.

 The two dynamic services are configured independently:

     metaserver_publish_origin = https://publish.meta.atrinik.org
     metaserver_rendezvous_origin = https://rendezvous.meta.atrinik.org/v1/classic

 The publisher value is a canonical HTTP(S) origin without a path. The signed
 profile path and certificate-derived identity are appended exactly once, and
 the resulting authority is covered by the signature. The rendezvous value is
 a canonical HTTP(S) origin plus an intentional deployment prefix; the shared
 library appends /servers/SERVER-ID?role=server and converts only HTTPS to WSS
 (or HTTP to WS for an explicit local deployment). Userinfo, query, fragment,
 percent encoding, traversal, ambiguous paths, invalid schemes, and output
 overflow are rejected before a background request starts. Publish and
 WebSocket requests do not follow redirects, and the bearer token remains only
 in the WebSocket Authorization header.

 Publication is event-driven. The server publishes once at startup, coalesces
 visible player-count changes for ten seconds, and otherwise sends only a
 jittered liveness heartbeat. `metaserver_heartbeat` configures the heartbeat
 base in seconds; the default 9000 seconds is jittered to 8100-9900 seconds,
 safely below the directory's four-hour expiry. Separate publisher and
 rendezvous two-token buckets, each refilling once every 1920 seconds, cap one
 continuously running process at 47 attempts per route in any 24-hour interval
 while still allowing startup and one prompt coalesced change. Transient
 failures use exponential backoff with jitter up to one hour,
 and `Retry-After` can extend that delay. Permanent publisher or rendezvous
 rejections stop retrying until public state changes, a later successful
 publish supplies a replacement token, or the operator restarts the server.
 A crashed listing disappears no later than four hours after its last accepted
 publication.

 The local buckets are defense in depth: the metaserver remains authoritative
 across process restarts and duplicate processes. Listing visibility, name,
 description, and join-password mode are startup settings; changing them
 requires a server restart, whose startup publication sends the new state.
 Private servers publish one removal/tombstone at startup and retry it until
 accepted, then send no heartbeats or player-count changes.

 The server reserves a fresh unsigned 64-bit publish sequence before every
 network attempt. Its crash-safe high-water mark is kept in two owner-only
 `data/metaserver-publish-sequence-<server-id>.*` files. Back up the pair with
 the matching QUIC identity. Gaps are expected after cancellation, timeout,
 or crash and are never reused. Restoring an older backup may produce one
 authenticated replay response; the server advances the same protected state
 from its non-secret `minimumNextSequence` value and uses a fresh nonce and
 higher sequence on the next scheduled attempt. Never edit, merge, or copy
 sequence files between identities. Exhausting the full 64-bit range requires
 rotating the QUIC identity, which selects a separate sequence pair.

=================================================
= 3.1. Metaserver-exposed information           =
=================================================

 Apart from the obvious identifying information exposed by any IP connection
 (such as the server IP address), the following information is exposed to the
 metaserver when public state changes and at the bounded liveness heartbeat, if
 reporting to the metaserver has been enabled (as per section 3.):
  - Configured server name and description
  - QUIC certificate fingerprint and server identity; no raw address
  - Server version
  - Atrinik HTTP client version and platform (Windows/Linux/other)
  - The public QUIC certificate and a one-request HTTP message signature; the
    private key is never uploaded
  - A fresh random nonce and monotonic publish sequence used only for replay
    defense
  - Number of players online
  - Whether a join password is required (never the password itself)

 The above information is sent over HTTPS to
 `https://publish.meta.atrinik.org/v1/classic/servers/<server-id>/publish`,
 again only if enabled as per section 3. System Web PKI validation authenticates
 the metaserver endpoint; the signed request independently authenticates this
 game server. Signed publishes never follow redirects because the exact
 authority and path are covered by the signature. Signatures, nonces,
 sequences, private material, and returned rendezvous tokens are not logged.

 Deployment order is forward-only: provision and canary the static directory,
 signed publisher, and rendezvous hosts first; release the coordinated
 libatrinik/server/client binaries next; then retire the classic compatibility
 aliases after the observation window. Only after classic traffic is absent may
 meta.atrinik.org become the replacement-stack static directory. There is no
 runtime fallback to the CGI pseudo-base, OTP/update paths, or /v2 routes in
 these binaries.

=================================================
= 3.2. Ports used by Atrinik                    =
=================================================

 The following port is used by the Atrinik game server and must be reachable for
 other players to connect successfully. It can be changed in the
 configuration.

  - 1730/UDP: Direct QUIC gameplay and asset delivery. PCP/NAT-PMP/UPnP mapping,
              IPv6, and rendezvous normally avoid a manual forwarding rule.

 An external HTTP origin/CDN has its own independently managed listener and
 network policy; it is not a port exposed by the Atrinik game server.

=================================================
= 4.1. Licensing (Atrinik server)               =
=================================================

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

 The author can be reached at admin@atrinik.org
 
=================================================
= 4.2. Licensing (uthash)                       =
=================================================

 Copyright (c) 2005-2011, Troy D. Hanson    http://uthash.sourceforge.net
 All rights reserved.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:

     * Redistributions of source code must retain the above copyright
       notice, this list of conditions and the following disclaimer.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
 OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
