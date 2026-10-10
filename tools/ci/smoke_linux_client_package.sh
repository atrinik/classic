#!/usr/bin/env bash
set -euo pipefail

if [[ $# != 2 || ! $2 =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "usage: $0 PACKAGE.deb VERSION" >&2
  exit 2
fi
package=$(realpath -e "$1")
version=$2
[[ $(dpkg-deb -f "$package" Package) == atrinik ]]
[[ $(dpkg-deb -f "$package" Version) == "$version" ]]
[[ $(dpkg-deb -f "$package" Architecture) == amd64 ]]
# This script runs only in an owned, clean runtime container, never on a host.
[[ ${ATRINIK_DEB_SMOKE_CONTAINER:-} == 1 && $EUID == 0 ]]
apt-get -o Acquire::Retries=3 -o APT::Update::Error-Mode=any update
apt-get install -y --no-install-recommends "$package"
[[ $(dpkg-query -W -f='${Version}' atrinik) == "$version" ]]
test -x /usr/games/atrinik
test -s /usr/share/applications/atrinik.desktop
test -s /usr/share/pixmaps/atrinik.png
test -d /usr/share/games/atrinik/sound
test -s /usr/share/doc/atrinik/LICENSE.md
if ldd /usr/games/atrinik | grep -F 'not found'; then
  echo "installed client has an unresolved runtime library" >&2
  exit 1
fi
if command -v cc || command -v cmake || command -v python3; then
  echo "runtime smoke unexpectedly contains build tools" >&2
  exit 1
fi
smoke_home=$(mktemp -d)
trap 'rm -rf -- "$smoke_home"' EXIT
chown 65534:65534 "$smoke_home"
cd "$smoke_home"
setpriv --reuid 65534 --regid 65534 --clear-groups \
  env HOME="$smoke_home" ATRINIK_CONFIG_DIR="$smoke_home/config" \
  timeout 30 /usr/games/atrinik --help > "$smoke_home/help.log" 2>&1
grep -F 'List of available options' "$smoke_home/help.log"
# Repeat installation, then remove: package management must not erase user data.
apt-get install -y --no-install-recommends "$package"
install -d "$smoke_home/config/.atrinik"
printf '%s\n' preserved > "$smoke_home/config/.atrinik/package-smoke-marker"
apt-get remove -y atrinik
grep -Fx preserved "$smoke_home/config/.atrinik/package-smoke-marker"
test ! -e /usr/games/atrinik
echo "Debian client installation/removal smoke passed"
