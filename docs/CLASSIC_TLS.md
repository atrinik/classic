# Classic Linux TLS cohort

Native Linux Classic builds select `/opt/atrinik/tls` with
`ATRINIK_CLASSIC_TLS_PREFIX`. The server needs OpenSSL's `SSL_get_peer_addr` API;
system OpenSSL 3 cannot supply it. Windows MXE uses its own target dependencies.

`tools/ci/classic_tls.lock.json` pins the immutable `atrinik/devcontainer` producer
commit and SHA-256 of the installer, manifest and shared archive helper.
`tools/ci/bootstrap_classic_tls.py` verifies all three before executing the recipe.
The producer pins OpenSSL 4.0.3 and curl 8.22.0 archive digests, runs upstream QUIC
API tests and installs shared libraries, default/legacy providers, configuration,
headers and license notices under the private prefix. A missing producer commit,
checksum failure, unsuccessful recipe, or incomplete installation fails the job.
A populated prefix is never adopted or overwritten.

Linux Check, opt-in PR benchmarks and daily performance builds bootstrap under
the existing digest-pinned Classic image, then mount the prefix read-only into
the unprivileged offline build container. The separate GPU runtime container also
receives the prefix at the same path. The release candidate's extracted library
check bootstraps with its Ubuntu runner's compiler. No job sets global
`LD_LIBRARY_PATH`, `OPENSSL_MODULES`, `OPENSSL_CONF` or changes `PATH` to select TLS.
Python and other system programs retain their ordinary OpenSSL 3 dependencies.
The prefix must be built and qualified on each consumer image/Ubuntu environment;
evidence from a different Linux image does not establish compatibility.

The server Dockerfile builds TLS in a separate stage using the same pinned Ubuntu
base as its build/runtime stages. It copies the entire prefix into both stages.
Runtime ownership changes apply only to `server` and `maps`; TLS remains root-owned.
Application and plugin RUNPATHs choose the private libraries through the native
CMake contract without changing system library search policy.

Focused source validation:

```sh
python3 -m unittest tools.tests.test_classic_tls
bash -n tools/ci/prepare_classic_tls.sh tools/ci/run_linux_check.sh tools/ci/run_gpu_coverage.sh
python3 tools/verify_import_history.py
git diff --check
```

Those checks do not build OpenSSL, an application, or a Docker image. Required
acceptance also needs the producer commit to be publicly available, actual Linux
CI/extracted-consumer builds and server image qualification. Record their exact
revisions and environments in the delivery evidence before reporting success.
