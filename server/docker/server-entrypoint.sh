#!/bin/sh
set -eu

# Initialize a new host data folder from the image defaults.
if [ ! -e data/.atrinik-initialized ]; then
    cp -R install_data/. data/
    touch data/.atrinik-initialized
fi

# Generated data assets are disposable and separate from persistent player
# state. Classic validates and creates assets/data; packaged region maps already
# live below assets/client-maps.
mkdir -p data/tmp

if [ -r "${ATRINIK_JOIN_PASSWORD_FILE:-/run/secrets/atrinik_join_password}" ]; then
    set -- --join_password_file="${ATRINIK_JOIN_PASSWORD_FILE:-/run/secrets/atrinik_join_password}" "$@"
elif [ -n "${ATRINIK_JOIN_PASSWORD:-}" ]; then
    set -- --join_password="${ATRINIK_JOIN_PASSWORD}" "$@"
fi

if [ -n "${ATRINIK_SERVER_HOST:-}" ]; then
    set -- --server_host="${ATRINIK_SERVER_HOST}" "$@"
fi

exec ./atrinik-server \
    --network_stack="${ATRINIK_NETWORK_STACK:-dual}" \
    --no_console \
    --http_url="${ATRINIK_HTTP_URL:-off}" \
    --server_public="${ATRINIK_SERVER_PUBLIC:-false}" \
    --port_quic="${ATRINIK_QUIC_PORT:-1730}" \
    --port_mapping="${ATRINIK_PORT_MAPPING:-auto}" \
    "$@"
