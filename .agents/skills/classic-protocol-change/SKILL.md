---
name: classic-protocol-change
description: Trace, redesign, implement, and validate Atrinik classic wire contracts across protocol schemas, generated bindings, client producers or consumers, server producers or consumers, fixtures, and dependency metadata. Use when command IDs, packet framing, payloads, state transitions, QUIC or classic transport behavior, or malformed-input handling changes.
---

# Classic protocol change

## Establish the contract

1. Read the root guide and `protocol/AGENTS.md`, plus the client, server, and
   library guides for every affected consumer.
2. Trace the existing command from `protocol/schema/game-commands.json` through
   generators, dispatch, encoding, decoding, state mutation, and tests. The
   schema owns identifiers; packet layouts may still live in producers and
   consumers.
3. Specify framing, field order, widths, signedness, byte order, lengths, limits,
   valid states, compatibility behavior, and malformed-input outcomes before
   editing.
4. Decide the transition boundary. Prefer one coordinated monorepo change over
   indefinite dual paths. Never renumber or reuse a durable ID accidentally.

## Implement and test

- Edit schema or generator inputs first, then regenerate committed bindings.
- Keep parsers bounded and transactional: malformed or incomplete input must not
  leave partially mutated state.
- Update all producers, consumers, fixtures, fuzz or boundary tests, dependency
  metadata, and documentation in the same PR.
- Cover truncation, oversized lengths, unknown IDs, invalid ordering, boundary
  values, round trips, reconnects, and version transitions where relevant.

## Validate the complete closure

Run protocol checks in `classic/protocol/`, then integrated wrapper builds from
the `atrinik/atrinik` workspace root:

```sh
python3 protocol/tools/generate.py --check
python3 -m unittest discover -s protocol/tests -p 'test_*.py'
cmake -S protocol -B protocol/build -DCMAKE_BUILD_TYPE=Release
cmake --build protocol/build --parallel
ctest --test-dir protocol/build --output-on-failure
./atrinik build libatrinik --profile PROFILE --test
./atrinik build server --profile PROFILE --test
./atrinik build client --profile PROFILE --test
git diff --check
```

Use one full classic worktree for coherent changes. If runtime sequencing or
reconnect behavior is involved, also follow `classic-runtime`. Report every
consumer checked and any explicitly deferred compatibility removal.
