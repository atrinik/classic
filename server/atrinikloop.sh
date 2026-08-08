#!/bin/sh
#
# This is the Atrinik loop and log script

set -eu

CMDNAME="./atrinik-server"
maxrestart=500
logdir="data/log"

logcount=0
date=$(/bin/date +%y%m%d_%H-%M-%S_%Z)

if [ -d "$logdir" ]; then
	tar -czf "logs_$date.tar.gz" "$logdir"
fi

rm -rf -- "$logdir"
mkdir -p "$logdir"

while [ "$logcount" -ne "$maxrestart" ]; do
	echo "Starting Atrinik $(date) for the $logcount time..." >"$logdir/$logcount" 2>&1
	$CMDNAME -d >>"$logdir/$logcount" 2>&1

	if [ -f core ]; then
		mv core core.$logcount
		/bin/gzip core.$logcount
	fi

	logcount=$((logcount + 1))
	sleep 5
done
