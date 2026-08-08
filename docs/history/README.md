# Imported history

Every source commit was rewritten below its destination prefix without pruning.
Use ordinary Git history commands:

```sh
git log --follow -- client/src/client/main.c
git log --follow -- server/src/server/main.c
```

To translate an original commit ID, search the appropriate map:

```sh
rg '^ORIGINAL_SHA ' docs/history/client-commit-map.txt
```

`imports.json` records source and rewritten tips, import commits, trees, agent
guides, original import tag counts, retired branch targets, and digests. The
rewritten graphs are integrated into `main`. The archived source repositories
named in the manifest remain authoritative for the original graphs; classic
does not retain a parallel `history/*` branch namespace. Verify the committed
evidence with:

```sh
python3 tools/verify_import_history.py
git fsck --full --strict
```

The 70 prefixed component tags and 217 temporary tag-namespace archival refs
used to verify the import were retired when classic adopted one release line.
Historical tags are the exact unprefixed set recorded in `release-tags.json`,
beginning with `v5.0.19` at
`f2cdf68710d157d4fae44a0582972129e6c4db9e`. New automatic tags begin at
`v6.0.0` and must follow that file's post-consolidation ancestry and semantic
ordering rules. The complete
`history/*` branch namespace was retired after import and workspace-migration
verification; `imports.json` preserves the final ref targets as evidence. The
former repositories remain the authoritative home of original branches and
commits, historical component tag objects, GitHub release records, notes, pull
refs, and assets.

Two retired refs were not component `main` tips. The imported client PR tip
`history/client/pr-48` is recorded here and was carried into classic PR #14;
the source server branch corresponding to
`history/server/feat/stable-content-identities-port` remains in the archived
server repository. Their final rewritten targets are recorded alongside the
ten component-history refs in `imports.json`.

Do not regenerate a commit map in place. A later import must add a new,
reviewable manifest entry and map while retaining this evidence.

`component-release-map.json` records the last release in each former component
repository and its corresponding rewritten commit. It is the compatibility
bridge from independent version trains to the unified release line.
