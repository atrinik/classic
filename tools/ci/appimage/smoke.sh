#!/usr/bin/env bash
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
if [[ $# != 2 || ! $2 =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "usage: $0 PACKAGE.AppImage VERSION" >&2
  exit 2
fi
[[ ${ATRINIK_APPIMAGE_SMOKE_CONTAINER:-} == 1 && $EUID != 0 ]]
probe=${ATRINIK_APPIMAGE_RUNTIME_PROBE:?trusted runtime probe is required}
[[ -f ${probe} && ! -L ${probe} && -x ${probe} ]]
package=$(realpath -e "$1")
version=$2
# A fresh runtime must not accidentally satisfy missing application libraries.
# Base apt can require OpenSSL; the probe verifies our exact bundled version.
if ldconfig -p | grep -E 'libSDL3|libcares|libcurl'; then
  echo 'clean runtime unexpectedly contains application libraries' >&2
  exit 1
fi
work=$(mktemp -d)
tls_pid=
trap 'status=$?; if [[ -n ${tls_pid} ]]; then kill "$tls_pid" 2>/dev/null || true; wait "$tls_pid" 2>/dev/null || true; fi; if (( status != 0 )); then find "$work" -name "*.log" -type f -exec cat {} +; fi; chmod -R u+w "$work"; rm -rf -- "$work"' EXIT
mkdir -p "$work/extract" "$work/unrelated directory" "$work/home" "$work/host libraries"
cd "$work/extract"
# This is the separately qualified runtime smoke. Release inspection never runs
# a candidate payload. Extraction requires no FUSE mount or privileged operation.
"$package" --appimage-extract > "$work/extraction.log" 2>&1
mv squashfs-root "$work/relocated Atrinik.AppDir"
appdir="$work/relocated Atrinik.AppDir"
# Make every installed byte read-only while still permitting an unprivileged run.
chmod -R a+rX,a-w "$appdir"
chmod 755 "$work"
# A colliding host SDL filename is deliberately invalid: bundle precedence wins.
printf 'invalid host SDL library\n' > "$work/host libraries/libSDL3.so.0"
cd "$work/unrelated directory"
env HOME="$work/home" ATRINIK_CONFIG_DIR='relative configuration' \
  LD_LIBRARY_PATH="$work/host libraries" \
  timeout 30 "$appdir/AppRun" --help > "$work/help.log" 2>&1
grep -F 'List of available options' "$work/help.log"
grep -F 'Loading configuration from ./client.cfg' "$work/help.log"
major=${version%%.*}
custom_state="$work/unrelated directory/relative configuration/.atrinik/${major}.x"
[[ -d "$custom_state" ]]
printf '# package persistence sentinel\n' > "$custom_state/client-custom.cfg"
env HOME="$work/home" ATRINIK_CONFIG_DIR='relative configuration' \
  timeout 30 "$appdir/AppRun" --help > "$work/persistent.log" 2>&1
grep -F "Loading configuration from $custom_state/client-custom.cfg" "$work/persistent.log"
grep -Fx '# package persistence sentinel' "$custom_state/client-custom.cfg"
# Preserve the default .atrinik/<major>.x semantics through the actual client.
env -u ATRINIK_CONFIG_DIR HOME="$work/home" \
  timeout 30 "$appdir/AppRun" --help > "$work/default-home.log" 2>&1
[[ -d "$work/home/.atrinik/${major}.x" ]]
# The trusted helper loads the bundled multimedia/network libraries; no helper
# or test-only entrypoint is distributed inside the production client.
bundle_env=(env LD_LIBRARY_PATH="$appdir/usr/lib"
  OPENSSL_MODULES="$appdir/usr/lib/ossl-modules"
  OPENSSL_CONF="$appdir/usr/share/atrinik/openssl.cnf")
"${bundle_env[@]}" "$probe" --assets "$appdir" > "$work/assets.log" 2>&1
cat "$work/assets.log"
# Provider activation and CA parsing exercise the exact bundled OpenSSL binary.
env LD_LIBRARY_PATH="$appdir/usr/lib" OPENSSL_MODULES="$appdir/usr/lib/ossl-modules" \
  OPENSSL_CONF="$appdir/usr/share/atrinik/openssl.cnf" \
  "$appdir/usr/bin/openssl" list -providers > "$work/openssl.log"
grep -F 'OpenSSL Default Provider' "$work/openssl.log"
grep -F 'OpenSSL Legacy Provider' "$work/openssl.log"
env LD_LIBRARY_PATH="$appdir/usr/lib" OPENSSL_MODULES="$appdir/usr/lib/ossl-modules" \
  OPENSSL_CONF="$appdir/usr/share/atrinik/openssl.cnf" \
  "$appdir/usr/bin/openssl" crl2pkcs7 -nocrl \
  -certfile "$appdir/usr/share/atrinik/ca-bundle.crt" -out /dev/null
# Local, private HTTPS proves cURL actually accepts configured OpenSSL trust and
# rejects an untrusted certificate. Network-none containers retain loopback only.
"${bundle_env[@]}" "$appdir/usr/bin/openssl" req -x509 -newkey rsa:2048 -nodes -days 1 \
  -subj /CN=localhost -addext subjectAltName=DNS:localhost \
  -keyout "$work/server.key" -out "$work/server.crt" > "$work/certificate.log" 2>&1
"${bundle_env[@]}" "$appdir/usr/bin/openssl" s_server -accept 127.0.0.1:44330 -www -quiet \
  -cert "$work/server.crt" -key "$work/server.key" > "$work/tls-server.log" 2>&1 &
tls_pid=$!
tls_ready=0
for _ in {1..20}; do
  if SSL_CERT_FILE="$work/server.crt" "${bundle_env[@]}" "$probe" --curl https://localhost:44330/ > "$work/tls-trusted.log" 2>&1; then
    tls_ready=1
    break
  fi
  sleep 0.2
done
[[ $tls_ready == 1 ]]
if SSL_CERT_FILE="$appdir/usr/share/atrinik/ca-bundle.crt" \
    "${bundle_env[@]}" "$probe" --curl https://localhost:44330/ > "$work/tls-untrusted.log" 2>&1; then
  echo 'cURL accepted a certificate absent from the trust bundle' >&2
  exit 1
fi
grep -F 'SSL peer certificate' "$work/tls-untrusted.log"
[[ -s "$appdir/usr/share/games/atrinik/client.cfg" ]]
[[ -s "$appdir/usr/share/doc/atrinik/LICENSE.md" ]]
[[ -d "$appdir/usr/share/games/atrinik/sound" ]]
# No host SDL install or FUSE was needed for relocation and startup.
printf 'AppImage %s clean-runtime relocation, persistent configuration, media decoders, c-ares and TLS smoke passed\n' "$version"
