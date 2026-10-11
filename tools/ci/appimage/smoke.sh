#!/usr/bin/env bash
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
if [[ $# != 2 || ! $2 =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "usage: $0 PACKAGE.AppImage VERSION" >&2
  exit 2
fi
[[ ${ATRINIK_APPIMAGE_SMOKE_CONTAINER:-} == 1 ]]
package=$(realpath -e "$1")
version=$2
# A fresh runtime must not accidentally satisfy missing application libraries.
if ldconfig -p | grep -E 'libSDL3|libcares|libcurl|libssl.so.3'; then
  echo 'clean runtime unexpectedly contains application libraries' >&2
  exit 1
fi
work=$(mktemp -d)
trap 'status=$?; if (( status != 0 )); then find "$work" -name "*.log" -type f -exec cat {} +; fi; chmod -R u+w "$work"; rm -rf -- "$work"' EXIT
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
# Preserve the default .atrinik/<major>.x semantics through the actual client.
env -u ATRINIK_CONFIG_DIR HOME="$work/home" \
  timeout 30 "$appdir/AppRun" --help > "$work/default-home.log" 2>&1
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
[[ -s "$appdir/usr/share/games/atrinik/client.cfg" ]]
[[ -s "$appdir/usr/share/doc/atrinik/LICENSE.md" ]]
[[ -d "$appdir/usr/share/games/atrinik/sound" ]]
# No host SDL install or FUSE was needed for relocation and startup.
printf 'AppImage %s clean-runtime extraction, relocation and OpenSSL smoke passed\n' "$version"
