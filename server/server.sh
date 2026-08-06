#!/bin/sh
#
# Script to run the server.

set -eu

if [ ! -x "./atrinik-server" ] || [ ! -d "lib" ] || [ ! -d "maps" ] || [ ! -d "resources" ]; then
	echo "Runtime is not prepared; run tools/prepare-runtime.sh from the repository root." >&2
	exit 1
fi

if [ ! -d "data" ]; then
	cp -R "install_data" "data"
fi
mkdir -p "data/tmp"

# Start up the server. If running from a terminal, pass options to the
# executable. Otherwise, start up the server with some sane options,
# which includes redirecting the log to a file.
if [ -t 1 ]; then
	./atrinik-server "$@"
else
	./atrinik-server --logfile=logfile.log "$@"
fi
