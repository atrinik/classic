# Atrinik Classic

This is the maintained classic Atrinik game implementation in one monorepo. It
combines the complete, path-preserving histories of the former client, server,
editor, libatrinik, and protocol repositories so coordinated maintenance can
happen in one branch and pull request.

The modern replacement implementation remains a clean-room, MIT-licensed set of
repositories. Consolidating classic Atrinik does not change that direction.

## Layout

| Path | Ownership |
| --- | --- |
| `client/` | SDL3 C17 graphical client |
| `server/` | C17 game server and classic scripting runtime |
| `editor/` | Gridarta packaging utility |
| `libatrinik/` | Shared C17 libraries |
| `protocol/` | Classic command schema and bindings |

Authored maps and gameplay content remain in
[`atrinik/content`](https://github.com/atrinik/content). Classic builds select
the immutable `classic` runtime target derived from `main`; replacement
development consumes the corresponding `main` target. Sound and resources also
remain external so both implementations can consume compatible releases.

## Workspace setup

Use the thin [`atrinik/atrinik`](https://github.com/atrinik/atrinik) wrapper. A
normal initialization clones replacement development only; add classic
explicitly when needed:

```sh
./atrinik init --with classic
./atrinik status
./atrinik topology show classic
```

Create a full monorepo worktree for coordinated work:

```sh
./atrinik worktree create classic my-change --branch feat/my-change
./atrinik profile create my-change --from classic
./atrinik profile set my-change classic --worktree my-change
./atrinik build server --profile my-change --test
./atrinik build client --profile my-change --test
```

`./atrinik build all --profile my-change --test` uses the supported integrated
CMake graph. That graph configures `protocol/` and `libatrinik/` once, then
links the client and server to the same `Atrinik::Protocol` and
`Atrinik::Core` targets. The root presets expose equivalent native builds:

```sh
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug
```

The `linux-release`, `linux-coverage`, and `linux-sanitizers` root presets use
the same graph. Shared warning, coverage, sanitizer, and precompiled-header
options are explicit across both consumers. Use
`ATRINIK_CLIENT_ENABLE_PRECOMPILED_HEADERS` and
`ATRINIK_SERVER_ENABLE_PRECOMPILED_HEADERS` to control the target-scoped PCH
policies independently. Coverage and sanitizers are rejected together because
they would request incompatible instrumentation;
the coverage preset disables precompiled headers to retain the conventional
diagnostic path. Consumer-specific behavior such as `ENABLE_PYTHON_PLUGIN`
remains scoped to its target. The component presets in `client/`, `server/`,
and `libatrinik/` remain the supported standalone, packaging, and
installed-consumer paths.

See [CONTRIBUTING.md](CONTRIBUTING.md), [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md),
and the nearest component `AGENTS.md` before editing.

## History

The import rewrote each source history below its destination path, so ordinary
path history remains useful:

```sh
git log --follow -- client/src/client/main.c
git log --follow -- server/src/server/main.c
```

Original-to-rewritten commit maps and verification instructions are in
[docs/history/README.md](docs/history/README.md). The former repositories remain
available as read-only release and closed-issue archives.

## Releases

After the documented post-merge rehearsal and activation step, one successful
`Classic validation` result on `main` drives the complete repository through
semantic-release. The first unified version is `v5.6.0`; later tags remain on
the unprefixed `v5.x.x` line and are derived from the squash-merge title.
Source archives, the protocol wheel, portable Windows packages, SPDX,
checksums, locked runtime-input evidence, attestations, and
`ghcr.io/atrinik/classic-server` are built from the same commit. See
[docs/RELEASING.md](docs/RELEASING.md) for the artifact,
rehearsal, recovery, and rollback contracts.
