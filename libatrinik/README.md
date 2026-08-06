# libatrinik

`libatrinik` is the reusable C17 networking and utility library shared by the
Atrinik classic client, server, tests, and tooling. Its public CMake target is
`Atrinik::Core`; the installed static library is named `libatrinik`.

The library directly depends on the separately released `atrinik/protocol`
package. By default CMake downloads the exact release archive and verifies the
SHA-256 recorded in `dependencies.lock.json`. For local protocol development,
override that source explicitly:

```sh
cmake -S . -B build \
  -DATRINIK_PROTOCOL_SOURCE_DIR=/path/to/protocol
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build --prefix /path/to/prefix
```

Set `LIBATRINIK_USE_INSTALLED_PROTOCOL=ON` to use an installed
`AtrinikProtocol` CMake package instead of the locked source release.

The first standalone release preserves the inherited GPL-2.0-or-later license.
Extraction into an independent repository does not relicense the code.
