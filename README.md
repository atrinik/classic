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
its `1.x` branch; replacement development selects `main`. Sound and resources
also remain external so both implementations can consume compatible releases.

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

One successful `Classic validation` result on `main` drives the complete
repository through semantic-release. The first unified version is `v6.0.0`;
later tags are unprefixed semantic versions derived from the squash-merge
title. Source archives, the protocol wheel, portable Windows packages, SPDX,
checksums, attestations, and `ghcr.io/atrinik/classic-server` are built from the
same commit. See [docs/RELEASING.md](docs/RELEASING.md) for the artifact,
rehearsal, recovery, and rollback contracts.
