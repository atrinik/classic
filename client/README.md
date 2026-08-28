=================================================
=                Atrinik Client                 =
=================================================

[![Coverage](https://codecov.io/gh/atrinik/classic/graph/badge.svg?branch=main&flag=client)](https://codecov.io/gh/atrinik/classic)

 Website: http://www.atrinik.org/

 Client package for the Atrinik game.

=================================================
= 1. Compiling the Atrinik client               =
=================================================

 See INSTALL for complete dependency and source-artifact instructions.

 From the repository root, build a Linux debug client with:
  $ python3 tools/dependencies.py sync
  $ cmake --preset linux-debug
  $ cmake --build --preset linux-debug
  $ ctest --preset linux-debug

 The executable is written to build/linux-debug/atrinik.

 To collect line, function, and branch coverage from the native tests:
  $ cmake --preset linux-coverage
  $ cmake --build --preset linux-coverage
  $ ctest --preset linux-coverage
  $ gcovr --root . --filter 'src/' --exclude 'src/tests/' --print-summary

 The client requires SDL 3.4 or newer, SDL3_image 3.2 or newer, SDL3_ttf 3.2
 or newer, and SDL3_mixer 3.2.4 or newer. The supported mixer contract contains
 exactly WAV, STBVORBIS, OPUS, VOC, AIFF, AU, DRMP3, SINEWAVE, and RAW; MIDI and
 module decoders are deliberately absent. Sound effects and music are required
 on every supported platform. The production client requires a hardware GPU
 supported by SDL_GPU through Vulkan, Direct3D 12, or Metal, including RGBA8
 and R32_UINT render-target support plus fragment storage buffers. There is no
 software renderer or fallback;
 startup reports the selected backend, device, and driver failure and exits if
 the required GPU contract is unavailable.

 Releases are produced from every squash merge to main. semantic-release
 parses the Conventional Commits pull-request title. Classic stays on the
 5.x.x line: a breaking marker, BREAKING CHANGE, or feat produces a minor
 release, and every other conventional type produces at least a patch release.
 Each unified release contains an atrinik-classic-client-VERSION source
 archive, a portable atrinik-classic-client-VERSION-windows-x86_64 ZIP, and
 SHA-256 checksums. The repository tag is the authoritative build version.
 The scoped source archive embeds the matching protocol and libatrinik source
 under dependencies/ and selects it automatically outside the monorepo.

=================================================
= 2. Running the client                         =
=================================================

 Run the executable with the repository root as the working directory so it can find its
 configuration, graphics, fonts, sounds, and other data files:
  $ build/linux-debug/atrinik

 Set `ATRINIK_CONFIG_DIR` to give a process an isolated configuration base.
 The client creates its normal `.atrinik/<major>.x/` hierarchy below that
 directory. This is useful when running concurrent development clients without
 sharing settings, cached server data, or connection preferences:
  $ ATRINIK_CONFIG_DIR=/absolute/path/to/client-state build/linux-debug/atrinik

 On first use of a `.atrinik/<major>.x/` directory, the client checks for
 legacy same-major directories such as `5.34`, `5.35`, or `5.34~`. It selects
 the highest valid numeric version (including patch components, with
 deterministic suffix handling) and migrates its files without overwriting
 existing user data. The source is retained until the destination is usable;
 interrupted or conflicting migrations leave a recovery marker and resume on
 a later run. Directories for unrelated or older major releases are left
 untouched and are never imported.

 If you used a different BUILD_DIR or CMake preset, adjust the executable path
 accordingly. Extracted portable Windows packages contain atrinik.exe and all
 required runtime assets and DLLs; run atrinik.exe from inside that package.

=================================================
= 2.0. Discord Rich Presence                    =
=================================================

 Discord Rich Presence is optional, Linux- and Windows-only, and defaults to
 `Server, zone and character`. Choose a privacy tier under Client settings:

 - `Off` opens no Discord IPC connection and clears activity published by this
   client run.
 - `Game only` shares only `Playing Atrinik Classic` and elapsed play time.
 - `Server` additionally shares `On <friendly server>`.
 - `Server and zone` additionally shares `Exploring <friendly zone>`.
 - `Server, zone and character` additionally shares `<character name> - Level
   <level>` and combines the friendly server and zone in the other field.

 Activity exists only while a character is in the playing state. It is cleared
 on logout, disconnect, opt-out/privacy reduction, and clean shutdown. Direct,
 manual, and command-line servers are always shown as `Private server`; only a
 public-directory display name may be shared. Zone text uses the friendly map
 display name and then friendly region name. Markup, controls, invalid UTF-8,
 whitespace, and length are normalized before any update. Account names,
 host/IP, port, server ID, certificate fingerprint, internal map path,
 coordinates, connection data, invites, passwords, party data, OAuth material,
 and Discord secrets are never included. The active character name and level
 are shared only by the highest tier while playing; they are normalized and
 bounded like all other presence text. This feature has no join, spectate,
 deep-link, account-linking, or Discord authentication capability.

 Discord must be running locally. If it is absent, starts later, restarts,
 refuses data, or returns malformed data, the game continues normally and the
 bounded nonblocking backend retries and logs each failure-state transition
 once without payload data. Linux socket peers and Windows named-pipe servers
 must belong to the current user. Activity updates, including reconnect
 replays and clears, are deduplicated, coalesced, and kept below Discord's
 rate limit. The elapsed timestamp begins
 once per transition into play and remains stable across map changes.

 Source builds have no Application ID by default and therefore perform no
 Discord IPC. For local testing, set the public, non-secret
 `ATRINIK_DISCORD_APPLICATION_ID` environment variable, create the ignored
 one-line `data/discord-application-id`, or configure an installed build with
 `-DATRINIK_DISCORD_APPLICATION_ID_FILE=/absolute/path/to/file`. The ID is
 validated as a numeric Discord snowflake. It is observable in every configured
 package and the opening IPC handshake and grants no authenticated access.

 Official Windows packages obtain that file only in the `discord-release`
 GitHub Actions environment from `DISCORD_APPLICATION_ID`. The production
 value is staged with restrictive permissions, retained for one day, installed
 as package data, never compiled into the executable, and never printed. Release
 rehearsal and normal CI use no production ID. The Discord application uses the
 project-owned blue-crystal icon and matching nocturnal-crystal cover selected
 for this feature; its Rich Presence large-image asset key is `atrinik` with
 hover text `Atrinik Classic`.

 An opt-in live smoke test requires a developer-owned application with an
 `atrinik` Rich Presence art asset and a running Discord desktop client. Start
 Atrinik with its Application ID, enter a character, confirm the selected tier,
 change zones, select `Off`, re-enable it, restart Discord, and confirm update,
 clear, and reconnect behavior. This smoke is deliberately outside CTest and
 must never use OAuth or any secret; deterministic CTest coverage uses a fake
 partial-I/O peer instead.

 The main server screen has a Route button for choosing a preferred direct
 route per server. The client races all candidates concurrently and selects a
 successful route in the order LAN, global IPv6, peer-reflexive, mapped, STUN,
 then directory. A specific choice is moved to the front of its direct or NAT
 group without disabling fallback routes. Private/ULA IPv6 addresses are
 treated as LAN candidates. The log reports punch send/receive totals for
 diagnosing restrictive NATs. These choices are stored in
 settings/connection-preferences.dat beneath the client's versioned user
 configuration directory.

 Addressless password-protected metaserver entries first require the
 high-entropy invite file generated by that exact server. Paste its single
 capability into the masked, paste-only session prompt, or configure only a
 protected path with `--rendezvous_invite_file=PATH`. A file must be a
 mode-0600 regular,
 non-symlink file. The capability is validated against the selected server,
 used for one rendezvous connection attempt, cleansed immediately afterward,
 and never sent as a candidate or logged. If an operator explicitly publishes
 an Address and Port, the client can instead bypass rendezvous and connect to
 that public endpoint with only the existing post-QUIC in-game join password.
 The capability is never written to client configuration, URLs, logs, or metrics.

 The packaged directory configuration is an explicit trusted pair:

     metaserver = https://classic.meta.atrinik.org/index.xml https://rendezvous.meta.atrinik.org/v1/classic

 The first value is the complete static protocol-4 XML URL. The second is the
 dynamic signaling origin and classic path prefix. Additional metaserver lines
 are tried last-to-first as complete pairs; a directory is never combined with
 another line's rendezvous origin. Userinfo, inherited query/fragment, encoded
 or traversal paths, and unsupported schemes fail closed. The client never
 derives either endpoint from meta.atrinik.org or another pseudo-base.

 `--nometa` disables metaserver access and discards every endpoint loaded so
 far. A later explicit `--metaserver="DIRECTORY RENDEZVOUS"` starts a new list
 and re-enables access. This ordered form lets supervised or canary launches
 select only their explicit pair without retaining the packaged production
 endpoint as a fallback. A later `--nometa` disables the new list again.

 Protocol 4 requires one bounded, fresh, transactionally valid snapshot with at
 most 512 certificate-pinned servers. Address and Port are either both omitted
 for rendezvous-only bootstrap or both present as an explicit canonical DNS
 endpoint. HTTP 200 bodies are committed to the local cache only after complete
 XML, content-type, strong-ETag, freshness, ordering, and generation validation.
 A 304 reuses that exact cached body only before its embedded expiry. A network
 or malformed-response failure may use the same unexpired last-known-good body;
 an expired body remains only a generation high-water mark and is never shown.
 Equal generations must be byte-identical and older generations never replace
 newer ones. The cache contains only the public directory projection: tickets,
 candidates, invite capabilities, authorization transcripts, and join
 passwords remain one-attempt memory and are never cached.

 The same prompt accepts the separate human server join password, which is
 checked only after certificate-pinned QUIC connects. The password is kept
 only in memory for that attempt and is not written to the connection-
 preferences file or another persistent client file. Automated launches
 should provide it through a restricted file with
 `--join_password_file=PATH`; `--join_password=PASSWORD` remains available
 for interactive debugging but exposes the value in the process argument list.
 The password file must be a regular, non-symlink file containing exactly one
 nonempty line shorter than 1024 bytes. Group/other permissions produce a
 warning; mode 0600 is recommended for the join-password file.

 A join password is cleansed after the server accepts or rejects it and on
 every earlier connection failure, cancellation, directory refresh, server
 replacement, or client shutdown. Connection errors distinguish rejected or
 expired invites, rate limits (including a bounded numeric Retry-After), an
 offline server, timeout, and an incompatible protocol without displaying
 tickets, capabilities, endpoints, or passwords.

 Directory entries may intentionally omit a public hostname and port. Those
 servers remain joinable through authenticated rendezvous candidates, so
 launching a private friend server does not require publishing its raw IP.

 Direct rendezvous uses `stun.cloudflare.com:3478` over UDP by default to
 discover the public address of the same socket later used for UDP punching
 and QUIC candidate checks. This sends a DNS query and a STUN request to
 Cloudflare during an Internet connection attempt. Override the endpoint with
 `--stun_server=HOST:PORT` or the equivalent `stun_server = HOST:PORT`
 configuration entry. Use `--stun_server=off` or `stun_server = off` to make
 no STUN DNS or UDP request. STUN only discovers an address; it is not TURN or
 a gameplay relay and cannot guarantee traversal through every NAT, CGNAT, VPN,
 or firewall. LAN, global IPv6, mapped, and directory routes remain available
 when STUN is disabled or discovery fails.

 Region maps are stored beneath a directory scoped by the stable server ID or
 authenticated certificate fingerprint, so a reused hostname and port cannot
 inherit another server's cache. On later visits the client sends the cached
 size and SHA-256 digest; the server returns a compact not-modified response
 when they match. A changed map is downloaded once, digest-verified, and
 atomically replaces the cached copy. HTTP/CDN bodies are accepted only after
 their size and SHA-256 match metadata obtained over authenticated QUIC;
 non-loopback origins must use HTTPS.

 In-band bodies use up to three independent QUIC asset streams. Gameplay keeps
 its own long-lived stream and receives the first service opportunity on every
 transport-thread pass. Each asset response declares one immutable size and
 SHA-256 digest, streams the body without per-chunk request round trips, and is
 rejected on early EOF, surplus bytes, reset, oversize, or digest mismatch.
 Cancelling one request resets only its stream. Required startup files are fed
 into the same bounded scheduler instead of downloading serially.

=================================================
= 2.1. Frozen renderer evidence                 =
=================================================

 The files under `src/tests/fixtures/player_view/` preserve the immutable MAP,
 asset, ordering, settings, and pixel evidence captured before the software
 renderer was removed. Test builds expose `--gpu-player-view MANIFEST`, which
 verifies those inputs and feeds them through the normal MAP/MAP2 decoder,
 `map_draw_map()`, production GPU composition, presentation fence, and explicit
 final-frame readback. The deleted CPU renderer and former `--player-view*`
 software command family are not linked. Movement manifests additionally replay
 the frozen closed MAP2 stream and an A-to-B-to-A map lifecycle; the returned A
 checkpoint must be byte-identical to the initial completed GPU frame.

 GPU correctness and performance evidence must come from fenced production-path
 runs on qualified Vulkan, Direct3D 12, and Metal hardware. Those runs record
 exact revision and dirty identity, backend, driver, device, shader cohort,
 viewport/depth workload, CPU submission, GPU completion, present wait, upload
 and allocation counts, retained bytes, recovery events, and final readback
 checkpoints. A software or CPU-emulated GPU may supplement conformance testing
 but never satisfies release performance qualification.

 Configure a Release test build, then run `--gpu-player-view-benchmark MANIFEST
 WORKLOAD` from the client data directory. Set `ATRINIK_GPU_CONFORMANCE_OUTPUT`
 to the destination JSONL artifact and `ATRINIK_GPU_CONFORMANCE_DRIVER` to the
 backend under test. Controlled runners on the documented reference or
 minimum-supported GPU additionally set
 `ATRINIK_GPU_CONFORMANCE_QUALIFIED_HARDWARE=1` and
 `ATRINIK_GPU_CONFORMANCE_HARDWARE_TIER=reference|minimum`. This entry point
 uses the production widget, popup, tooltip, minimap, map, and presentation
 path; backend-native attestation, not the environment request, establishes
 hardware qualification. It rejects dirty builds and unknown revision
 identity. Run every workload row three times in a fresh process. Each record
 contains raw frame windows and separate stage, cold-upload, steady-state
 churn, retained-resource, and changing animation checkpoints. Upload evidence
 attributes immutable source textures, changed painter instances, compact
 light data, and per-batch stable-slot uniforms independently. Static plateaus
 require the first three classes to remain zero and bound slot uniforms to one
 16 to 1024-byte push per world batch; animation rows additionally permit only
 bounded changed-instance deltas.

 The client deliberately uses a 1:1 logical-to-output-pixel window contract.
 High-density backing stores are not requested: one SDL window coordinate is
 one renderer output pixel, which preserves nearest-neighbor pixel-art sampling
 and makes fixture, screenshot, fullscreen, and benchmark coordinates
 identical. Qualification rejects a workload when the queried logical window
 or renderer output size differs from its declared pixel dimensions.

 Run `--gpu-player-view-lifecycle
 src/tests/fixtures/player_view/brynknot-movement.xml` with
 `ATRINIK_GPU_CONFORMANCE_LIFECYCLE_OUTPUT` set to record production-path cold
 upload, resize/restore, teleport, reconnect state cleanup, foreground resume, fullscreen,
 display migration, swapchain recreation, device loss, and screenshot
 readback. Each action records its own elapsed time, uploads, resource churn,
 and bounded recovery result before the statistics reset for 40 fenced
 complete-client frames. Lifecycle mode uses a visible headful window and real
 SDL window transitions; hidden windows remain confined to fixture and
 benchmark collection. Every event is followed by those 40 frames and
 must return to the applicable steady-state row budget without source, light,
 or instance upload, readback, fallback, or resource churn; slot uniforms stay
 within the same per-world-batch bound. The manual `GPU qualification`
 workflow runs and validates both contracts for Vulkan, Direct3D 12, and Metal
 on reference and minimum hardware tiers. Fullscreen approval is keyed by GPU
 backend, hardware tier, and the recorded output-pixel display mode; changing a
 runner's display mode therefore requires a fresh human-reviewed golden.

 Every production fixture pins the shared widget defaults and saved layout.
 The collection also includes `gpu-ui-closure.xml`; its named state sweep
 covers the intro server browser, the ready account/password login form,
 gameplay widgets and text, overlays,
 popup families (character selection, settings, color picker, connection and
 credential modals, credits, book, painting, and region map), region-map/minimap
 FOW updates, and both player screenshot
 commands. Screenshot copies are enqueued asynchronously and completed by
 later event-loop fence polling; synchronous readback is reserved for the
 qualification checkpoint helper. Qualification writes hash-bound PNG review
 artifacts for the initial/final frames, every UI state, and every lifecycle
 event; the verifier rehashes them and validates their PNG dimensions before
 the workflow uploads them for human approval. `--collect-lifecycle` checks the
 complete structural artifact without consuming the pixel contract;
 `--lifecycle` is the later closure gate that requires human-approved goldens.

 Map records carry stable sparse-cell identities and semantic draw variants.
 Each primary or auxiliary target retains its own command snapshot and instance
 buffer, compares the next painter stream by identity, and uploads only changed
 contiguous instance ranges. Queued commands retain their source asset through
 submission, including when a surface cache entry is invalidated or evicted.

=================================================
= 3.1. Licensing (Atrinik client)               =
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
= 3.2. Licensing (Atrinik client graphics)      =
=================================================
 See the monorepo's root `ATTRIBUTIONS.md`.

=================================================
= 3.3. Licensing (Atrinik client sounds)        =
=================================================
 See the LICENSE file in the 'sound/background' directory for background music,
 or the LICENSE file in the 'sound/effects' directory for sound effects.

=================================================
= 3.4. Licensing (Atrinik client fonts)         =
=================================================
 See the respective font file's copyright field.

=================================================
= 3.5. Licensing (uthash)                       =
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
