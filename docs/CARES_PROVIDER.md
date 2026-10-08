# Qualified c-ares providers

The standalone server compiler stage uses the published
`ghcr.io/atrinik/classic-build:1.16.0` index
`sha256:e1c366dbf83ef987765ff913bbb193868314a139ae1e00cc134b22a3159464f2`.
Its qualified Linux amd64 platform is
`sha256:66a637c76f07d32ff933886a96bc5bd720fc695cf20a4acb9c1710983fdb7478`,
from producer source `4be36a1f1eebb667aad77c227f7855a4a656d150`.
This supplies curl 8.18.0, c-ares 1.34.6 and OpenSSL 3.5.5. The resolver contract
requires c-ares and disables curl's threaded resolver.

[The runtime provider lock](../server/docker/runtime-provider.lock.json) records
exact inspected shared-object hashes, relative SONAME symlinks, full notices,
resolver configuration/evidence, distro package versions and the producer receipt
hashes. [The staging/verification helper](../server/docker/runtime-provider.py)
checks those inputs before copying only the bounded runtime payload. It preserves
symlinks and verifies the staged result. Headers, pkg-config metadata, compiler
binaries and the rest of the build prefix are excluded. Both full curl and c-ares
licenses remain in `/usr/local/share/licenses/`.

The destination uses the producer's immutable Ubuntu 26.04 base and signed
`20260810T000000Z` snapshot. A public distro CA bundle is copied from the compiler
stage before HTTPS package acquisition, then the locked `ca-certificates` package
owns runtime system trust. HTTPS peer verification and APT Release signature and
package hash verification remain enabled. Historical Release expiry is disabled
for this fixed snapshot only. The runtime installs exact versions from the lock,
including the complete selected `Depends`/`Pre-Depends` graph for curl and the
Python/server/plugin runtime roots. The 67-package closure comes from the actual
published-provider source-9136 server/plugin/Python inventory, receipt hash
`b70a977a3ffaf4c2d3d3fefd9a6e4a1cff4ee63692024a68418c5d6c22dfcd3b`.
The lock retains each dependency expression and its installed, version-checked
alternative or virtual provider selection; graph reachability must match the exact
locked package set. This includes indirect GD image/X11 libraries, Python FFI and
compression libraries, and `openssl-provider-legacy` because the actual
`libssl3t64` package declares that dependency. This is package dependency closure,
not a Classic call to load an optional OpenSSL provider. It never installs distro
`libcurl4t64` over the qualified provider.

Image assembly runs `ldconfig`, verifies installed package versions, and checks the
actual OpenSSL version loaded by Python. It then inspects loader resolution for
the real server, access-status helper, arena/Python plugins, curl/c-ares,
libpython and Python extension modules. Unresolved dependencies, unexpected curl
or SSL loader paths, and missing indirect curl evidence fail assembly. Executable
and configuration ancestry remains root-managed; UID 10001 retains the established
maps, data and generated asset ownership. Entry point, admin and health interfaces
are unchanged.

Run bounded source checks from the Classic root:

```sh
python3 -m unittest tools.tests.test_server_runtime_provider -v
python3 tools/verify_import_history.py
git diff --check
```

The fixture tests exercise exact staging, internal symlink closure, missing or
changed libraries/notices, resolver-policy rejection, package drift and loader
failure handling. Synthetic loader outputs test the verifier's decisions; they do
not establish destination ABI compatibility. A full Docker build must execute the
real assembly verifier. Normal server/plugin/Python behavior, DNS and HTTP
cancellation, public TLS and QUIC qualification remain required on that built
image before runtime acceptance. A published compiler receipt or source test pass
does not prove these destination gates. This source change does not authorize a
stable release or live server cutover.
