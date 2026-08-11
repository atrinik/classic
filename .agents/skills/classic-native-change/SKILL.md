---
name: classic-native-change
description: Implement and validate classic C17/CMake code, tests, headers, and cross-module APIs in one monorepo worktree.
---

# Classic native change

## Orient the change

1. Read the root `AGENTS.md`, then every affected module's nearest
   `AGENTS.md`, the root `CONTRIBUTING.md`, module build files, and tests.
2. Identify module ownership before editing. Shared reusable native code belongs
   in `libatrinik/`; client and server policy stays in its consumer.
3. Trace public API, generated protocol, runtime, and persistence effects across
   sibling modules. Keep coordinated changes in one monorepo branch and PR.
4. Preserve unrelated work and never edit generated outputs when their source
   input is available.
5. Apply the root `CONTRIBUTING.md` copyright-header contract to every edited
   existing Atrinik-authored header; preserve named, mixed, and upstream
   attribution and update generated headers only at their source.

## Implement safely

- Use C17 and the existing allocation, ownership, lifetime, error, logging, and
  formatting conventions.
- Update CMake source lists, installs, exports, dependency overrides, and focused
  tests together.
- Keep `protocol/` and `libatrinik/` sibling-source overrides authoritative for
  integrated validation. Component release locks remain fallback integrity
  inputs; do not copy shared sources or protocol constants.
- Treat warnings, sanitizer findings, ownership ambiguity, and integer or bounds
  errors as defects.
- For cross-module APIs, document allocation, mutability, thread-safety, error,
  and compatibility contracts at the boundary.

## Validate

From the `atrinik/atrinik` workspace root, create or select one full `classic`
worktree and run the smallest affected closure through the wrapper:

```sh
./atrinik topology show PROFILE
./atrinik build libatrinik --profile PROFILE --test
./atrinik build server --profile PROFILE --test
./atrinik build client --profile PROFILE --test
```

Omit unaffected builds only after proving there is no public API, protocol, or
shared-library impact. For substantial changes, run the module coverage preset;
run the server sanitizer preset for server or shared runtime changes. Finish
with module dependency verification, repository-wide formatting checks, and:

```sh
git diff --check
```

For live behavior, also use the `classic-runtime` skill and its complete
topology lifecycle. Report exact commands, results, prerequisites, and cleanup.
