# Classic rendezvous admission v1

Classic's sibling protocol module owns the admission identifiers in
`schema/rendezvous-admission.json`, generated C and Python bindings, and the shared
synthetic conformance fixture. The MIT shared specification remains authoritative
for cross-repository transport semantics:
[protocol rendezvous admission v1](https://github.com/atrinik/protocol/blob/bbe1d53abb4a506893af2275f58c67468e454676/spec/rendezvous-admission.md).
Classic library implements the native HTTPS client/parser/solver and Classic
client consumes it. Worker implements challenge, upgrade verification and replay.
Game rendezvous is disabled. This handshake does not allocate a game command ID
or change game revision 1083.

## Exact transport contract

POST `/v1/rendezvous/challenge/classic/{serverId}` over verified HTTPS on the exact
configured rendezvous authority; serverId is 64 lowercase hex. Map only the
configured WSS rendezvous origin. No HTTP downgrade, redirects, query, cookies or
compression. Request `Content-Type` is `application/json`. Successful challenge
response is status200 with `Content-Type: application/json` and
`Cache-Control: no-store`; other statuses are terminal, including bounded429
which may report a validated retry delay but never automatically retries.
Request is canonical compact JSON, at most 256 UTF-8 bytes, exact
key order `schema,ticketHash`; schema is generated `SCHEMA`, and ticketHash is
SHA256 of the 32 decoded ticket bytes, never its hexadecimal text.

Response is canonical compact JSON, at most 512 bytes, exact key order
`schema,challenge,difficulty,expiresAt`, difficulty integer 24. Challenge is
`v1.KEYID.IAT.TICKETHASH.NONCE.MAC`: KEYID `[A-Za-z0-9_-]{1,32}`, IAT canonical
positive decimal UTC seconds, hash/MAC 64 lowercase hex and nonce 32 lowercase
hex encoding 16 random bytes. IAT and expiry fit 253402300799; expiry is exactly
IAT+60. Reject future IAT and reached expiry. Native verifies grammar, ticket hash,
fixed difficulty and matching expiry; only Worker holds signing keys.

HMAC-SHA256 input is ASCII `HMAC_DOMAIN`, NUL, configured authority (host and
explicit port without scheme), NUL, `classic`, NUL, serverId, NUL, token prefix
before MAC. Current/previous source-tag key verification preserves existing
rotation overlap. SHA256 work input is ASCII `WORK_DOMAIN`, NUL, complete token,
NUL, solution (16 lowercase hex characters as ASCII). First three digest bytes
must be zero. Purpose strings come exclusively from generated identifiers.

Every client upgrade sends `HEADER: TOKEN.SOLUTION`, at most 256 bytes. Ordinary
clients additionally send `TICKET_HEADER: ticket64lowerhex`, forbid Authorization
and `CLIENT_NONCE_HEADER`. Protected clients send `Authorization: Bearer
grant64lowerhex` and `CLIENT_NONCE_HEADER: nonce64lowerhex`, use grant as ticket,
and forbid the ordinary ticket header. Secrets never enter URL/subprotocol.
Server-role upgrades remain GET
`wss://configured-authority/v1/classic/servers/{serverId}?role=server`. Protected server
controls request subprotocol `atrinik-access-rendezvous-v1`; ordinary server
controls omit `Sec-WebSocket-Protocol`. They forbid client-only admission/ticket/
client-nonce headers and perform no client work. Missing admission returns fixed428 before room dispatch; invalid proof
fixed403; old Worker challenge404 is terminal with no downgrade fallback.

Protected grants redeem atomically before socket allocation. Room reserves
admission and claims purpose-separated ticket replay before allocation; replay
retains 24h even on later failure, across rotations/hibernation/generations.
Candidates must match the preauthorized ticket. `access_init`/`access_ready`
confirm the already redeemed grant/client nonce within two seconds; no candidate
before confirmation and no alternate redemption path. Internal v4/v5 mismatch
fails closed. At every await recheck expiry/control/generation/liveness.

Native solver uses at most 2^27 trials and an absolute monotonic-clock deadline
`min(original attempt deadline, solver start + 10s)`. Convert the remaining UTC
proof/grant lifetime to a monotonic bound once when work begins; wall-clock
adjustments cannot extend the bound. Check cancellation and
deadline every4096trials or more often. Work is a computational cost gate, not
anonymous fairness. Failure requires a fresh ticket/grant, never extends the
original attempt or protected grant's15s deadline, and never bypasses proof.

## Generation, packaging and provenance

Run `python3 protocol/tools/generate.py` after schema edits; `--check` verifies
both game and admission bindings. C includes
`<atrinik/protocol/rendezvous_admission.h>`; Python imports
`atrinik_protocol.rendezvous_admission`. Sibling, embedded and installed protocol
sources all carry these identifiers. A locked fallback lacking the generated
header must fail configuration clearly rather than resurrect endpoint literals.
Existing CMake install of generated headers and Python package discovery include
the new bindings; protocol source packaging includes schema, docs and fixtures.

`fixtures/rendezvous-admission-v1.json` is copied byte-for-byte from MIT
`atrinik/protocol` PR49 branch `fix/rendezvous-admission-173-20261011`, path
`fixtures/rendezvous-admission-v1.json`, SHA256
`6c98dd74b612c67af7b2692367a7c4cab3c55769859110256b4a67dbb874ca74`.
The immutable digest identifies the frozen reviewed bytes while the companion
source commit is being prepared; no future commit or release pin is asserted.
Its solved proof retains the original vector from revision
`bbe1d53abb4a506893af2275f58c67468e454676`. Its MIT license and copyright are retained in
`fixtures/rendezvous-admission-LICENSE.txt`. This is compatible inbound MIT fixture
reuse into GPL Classic, not outbound Classic relicensing and not reliance on a
historical sole-author grant. It contains only synthetic keys, `.invalid`
authority and repeated-byte identities. No Classic implementation was copied
into the MIT source. The schema, generator extension, local specification and
independent Python checks are new Classic GPL-2.0-or-later work under root license.
The fixture has a solved24bit proof; tests recompute exact HMAC/work inputs and
all positive/negative vectors and shared request/response grammar cases without
brute-force mining.
