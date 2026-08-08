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
guides, original import tag counts, and digests. The original commit graphs
remain reachable through `history/original/<component>/main`; rewritten graphs
remain under `history/<component>/main`. Verify the committed evidence with:

```sh
python3 tools/verify_import_history.py
git fsck --full --strict
```

The 70 prefixed component tags and 217 temporary tag-namespace archival refs
used to verify the import were retired when classic adopted one release line.
Active tags are the exact unprefixed set in `release-tags.json`, beginning with
`v5.0.19` at `f2cdf68710d157d4fae44a0582972129e6c4db9e`. The five non-tag original-history
branches preserve source commit reachability. The former repositories remain
the authoritative home of historical component tag objects, GitHub release
records, notes, pull refs, and assets.

Do not regenerate a commit map in place. A later import must add a new,
reviewable manifest entry and map while retaining this evidence.
