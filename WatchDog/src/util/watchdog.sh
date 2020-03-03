#!/bin/bash

set -e

usage() {
  echo "usage:"
  echo "$0: <ALIVE_FILE> <SLEEPTIME_SECONDS> <STOP_COMMAND>..."
}

if [ "$1" = --help ] ; then
  usage
  exit
fi

if [ $# -ne 3 ] ; then
  usage >&2
  exit 1
fi

ALIVE_FILE="$1"
SLEEPTIME="$2"
STOP_COMMAND=("$@")

# wait for file to appear
sleep "$SLEEPTIME"
if ! [ -r "$ALIVE_FILE" ] ; then
  echo >&2 "$0: Watchdog file "$ALIVE_FILE" has not been appeared in $((2 * $SLEEPTIME))s. Requesting termination."
  "${STOP_COMMAND[@]}"
  exit 2
fi

LAST_TIMESTAMP=$(stat -c %s "$ALIVE_FILE")
while sleep "$SLEEPTIME" ; do
  TIMESTAMP=$(stat -c %s "$ALIVE_FILE")
  if [ $TIMESTAMP -eq $LAST_TIMESTAMP ] ; then
    echo >&2 "$0: Detected stalled simulation at $(date), watchdog file "$ALIVE_FILE" has not been updated for more than $((2 * $SLEEPTIME))s. Requesting termination."
    "${STOP_COMMAND[@]}"
    exit 2
  fi
  LAST_TIMESTAMP=$TIMESTAMP

  # this loop will be terminated by the queue system
done
