# Dependency ownership

| Provider | Consumers | Source of truth |
| --- | --- | --- |
| `protocol/` | `libatrinik/`, `client/`, `server/` | schema, generators, and sibling source override |
| `libatrinik/` | `client/`, `server/` | sibling source override |
| sound | `client/` | external release pinned by the client lock |
| `content@main` Classic target | `server/`, editor/runtime tooling | external immutable release artifact |
| resources | `server/` and runtime collection | external release pinned by the server lock |
| Gridarta | `editor/` | explicit external checkout supplied to `build.sh` |
| organization supply inventory | all modules | `atrinik/atrinik` supply-chain inventory |

The monorepo does not duplicate dependency versions at its root. Module lock
files continue to prove standalone release fallback inputs. Integrated builds
must point protocol and libatrinik consumers at the sibling source trees so one
pull request validates the actual coordinated change.
