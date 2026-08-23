# Classic build acceleration

The client and server enable target-scoped precompiled headers by default. The
client applies `global.h` only to the `atrinik` executable, and the server
applies it only to the `atrinik-server-core` object library. Standalone tests,
plugins, libatrinik, protocol consumers, and downstream projects do not inherit
the precompiled header. Set `-DENABLE_PRECOMPILED_HEADERS=OFF` to retain an
ordinary compilation path. The integrated root graph exposes independent
`ATRINIK_CLIENT_ENABLE_PRECOMPILED_HEADERS` and
`ATRINIK_SERVER_ENABLE_PRECOMPILED_HEADERS` options, and its coverage preset
disables both for the same conventional coverage path as the standalone
presets.

Unity builds remain unsupported. CMake's explicit `-DCMAKE_UNITY_BUILD=ON`
experiment exposed existing translation-unit isolation requirements: repeated
client widget helpers and enumerators, plus libatrinik's per-file
`TOOLKIT_API` state, collide when sources are combined. The experiment was
stopped at compilation, no timing claim is made for it, and no production
option enables unity builds.

## Reproduce the benchmark

Configure one build tree per compiler and mode from the Classic repository
root. These client commands show the GCC Debug pair; replace `client` with
`server`, and the target/source paths as shown below, for the server pair.

```sh
cmake -S client -B client/build/benchmark-gcc-debug-conventional -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
  -DENABLE_PRECOMPILED_HEADERS=OFF \
  -DFETCHCONTENT_SOURCE_DIR_ATRINIK_PROTOCOL="$PWD/protocol" \
  -DFETCHCONTENT_SOURCE_DIR_LIBATRINIK="$PWD/libatrinik"
cmake -S client -B client/build/benchmark-gcc-debug-pch -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
  -DENABLE_PRECOMPILED_HEADERS=ON \
  -DFETCHCONTENT_SOURCE_DIR_ATRINIK_PROTOCOL="$PWD/protocol" \
  -DFETCHCONTENT_SOURCE_DIR_LIBATRINIK="$PWD/libatrinik"
python3 tools/benchmark_build_acceleration.py \
  --build-dir client/build/benchmark-gcc-debug-conventional \
  --target atrinik --source client/src/client/main.c \
  --header client/src/include/global.h --jobs 16 --runs 3 \
  --output client/build/benchmark-gcc-debug-conventional.json
python3 tools/benchmark_build_acceleration.py \
  --build-dir client/build/benchmark-gcc-debug-pch \
  --target atrinik --source client/src/client/main.c \
  --header client/src/include/global.h --jobs 16 --runs 3 \
  --output client/build/benchmark-gcc-debug-pch.json
```

For the server, use target `atrinik-server`, representative source
`server/src/server/main.c`, header `server/src/include/global.h`, and build
directories below `server/build/`. Set `CC=clang CXX=clang++` while configuring
the Clang pair. The script cleans the configured tree before each run, measures
an immediate warm build, advances and restores the representative source mtime,
then advances and restores the common-header mtime. It records elapsed seconds,
aggregate process-group peak RSS on Linux, rebuilt object/PCH counts, compiler,
CMake version, logical CPU count, job count, configuration flags, and the tail
of each build log in stable JSON.

## 2026-08-10 measurements

The benchmark used commit `51fda5d9a73b168711cde67b0db56c7c6d2bf0d3`, CMake
4.2.3, Ninja 1.13.2, an AMD Ryzen 9 5950X (16 cores/32 threads), 16 parallel
jobs, GCC 15.2.0, and Clang 21.1.8. Values are medians of three runs. Peak RSS
is the aggregate Linux process-group sample, not per-compiler RSS.

| Target | Compiler | PCH | Clean (s) | Peak RSS (MiB) | Warm (s) | One source (s/objects) | `global.h` (s/objects) |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |
| Client | GCC | off | 2.998 | 31.9 | 0.047 | 0.683 / 1 | 2.493 / 107 |
| Client | GCC | on | 2.207 | 32.4 | 0.047 | 0.510 / 1 | 1.739 / 118 |
| Client | Clang | off | 3.247 | 42.0 | 0.047 | 0.669 / 1 | 2.656 / 107 |
| Client | Clang | on | 1.499 | 33.3 | 0.047 | 0.415 / 1 | 1.313 / 118 |
| Server | GCC | off | 4.916 | 32.9 | 0.053 | 0.651 / 1 | 5.392 / 252 |
| Server | GCC | on | 3.352 | 39.4 | 0.052 | 0.490 / 1 | 2.601 / 255 |
| Server | Clang | off | 4.618 | 32.8 | 0.052 | 0.502 / 1 | 4.167 / 252 |
| Server | Clang | on | 3.064 | 35.3 | 0.065 | 0.453 / 1 | 2.354 / 255 |

PCH reduced median clean time by 26%–54% and representative incremental time
by 10%–38%. GCC server peak RSS increased by 6.5 MiB in this sample; the other
three pairs changed by less or decreased. A common-header edit still invalidates
the core, and PCH deliberately broadens that invalidation to every source in
the scoped target (11 additional client artifacts and three additional server
artifacts here). The clean-build improvement is material enough to justify the
default, while the opt-out and conventional coverage jobs preserve diagnostic
and compatibility comparison paths.

The local sandbox could not download the server's pinned libpcpnatpmp archive,
so the measurements reused an existing CMake download cache only after its
SHA-256 matched the lock value
`65ab99547ecc8277434527607d24f8a1b02a2344ed4cea475bed751606e60202`.
Ordinary connected configurations fetch and verify the same locked archive.
