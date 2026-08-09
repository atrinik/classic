---
name: classic-protocol-change
description: Change classic IDs, packets, generated bindings, producers/consumers, fixtures, and compatibility across the monorepo.
---

# Classic protocol change

Read root and protocol guides plus every affected client/server/library guide.
Trace `protocol/schema/game-commands.json` through generation, dispatch,
encoding/decoding, mutation, fixtures, and dependency metadata. The schema owns
IDs; producers/consumers may own payload layout.

Specify framing, order, widths, signedness, byte order, lengths/bounds, valid
states, compatibility, and malformed-input outcomes before editing. Change
source definitions, regenerate, update every consumer, and prefer one coherent
transition over indefinite dual paths. Keep parsers bounded/transactional and
test truncation, oversize, unknown IDs, ordering, boundaries, round trips,
reconnects, and versions as relevant.

Run direct protocol commands from the classic worktree root:

```sh
python3 protocol/tools/generate.py --check
python3 -m unittest discover -s protocol/tests -p 'test_*.py'
cmake -S protocol -B protocol/build -DCMAKE_BUILD_TYPE=Release
cmake --build protocol/build --parallel
ctest --test-dir protocol/build --output-on-failure
git diff --check
```

Then return to the wrapper root and validate the selected full classic
worktree/profile:

```sh
./atrinik build libatrinik --profile PROFILE --test
./atrinik build server --profile PROFILE --test
./atrinik build client --profile PROFILE --test
```

Use `classic-runtime` for sequencing/reconnect proof. Report every consumer and
any explicitly issue-owned compatibility-removal follow-up.
