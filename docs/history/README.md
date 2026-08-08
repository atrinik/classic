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
guides, tag counts, and digests. Verify the committed evidence with:

```sh
python3 tools/verify_import_history.py
git fsck --full --strict
```

Component release tags are rewritten as `client/v5.3.1`,
`server/v5.5.1`, and equivalent collision-free names. Archival refs preserve
original objects and original annotated tags. The former repositories remain
the authoritative home of their 70 historical GitHub release records and
assets.

Do not regenerate a commit map in place. A later import must add a new,
reviewable manifest entry and map while retaining this evidence.
