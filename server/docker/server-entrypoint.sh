#!/bin/sh
set -eu

# Legacy admission settings require explicit offline migration. Never forward
# a removed secret through argv, and reject before touching persistent state.
if [ -n "${ATRINIK_JOIN_PASSWORD:-}" ] ||
   [ -n "${ATRINIK_JOIN_PASSWORD_FILE:-}" ] ||
   [ -e /run/secrets/atrinik_join_password ]; then
    echo "Legacy admission configuration requires offline migration to access tokens." >&2
    exit 1
fi

# Initialize a new host data folder from the image defaults.
if [ ! -e data/.atrinik-initialized ]; then
    cp -R install_data/. data/
    touch data/.atrinik-initialized
fi

# Generated data assets are disposable and separate from persistent player
# state. Classic validates and creates assets/data; packaged region maps already
# live below assets/client-maps.
mkdir -p data/tmp


if [ -n "${ATRINIK_SERVER_HOST:-}" ]; then
    set -- --server_host="${ATRINIK_SERVER_HOST}" "$@"
fi

if [ -n "${ATRINIK_ADMIN_SHUTDOWN_SOCKET:-}" ]; then
    set -- --admin_shutdown_socket="${ATRINIK_ADMIN_SHUTDOWN_SOCKET}" "$@"
fi

exec ./atrinik-server \
    --network_stack="${ATRINIK_NETWORK_STACK:-dual}" \
    --no_console \
    --http_url="${ATRINIK_HTTP_URL:-off}" \
    --mapspath="${ATRINIK_MAPS_PATH:-/opt/atrinik/maps}" \
    --server_public="${ATRINIK_SERVER_PUBLIC:-false}" \
    --port_quic="${ATRINIK_QUIC_PORT:-1730}" \
    --port_mapping="${ATRINIK_PORT_MAPPING:-auto}" \
    "$@"
