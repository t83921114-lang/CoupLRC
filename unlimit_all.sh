#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
HOSTS_FILE="${SCRIPT_DIR}/hosts"

USER="root"

REMOTE_COMMAND="cd \"$SCRIPT_DIR\" && bash unlimit.sh"

PARALLEL=5

echo ">> unlimit ..."
PDSH_OUTPUT=$(sudo pdsh -R ssh -w ^$HOSTS_FILE -l $USER -f $PARALLEL "$REMOTE_COMMAND" 2>&1)
echo "$PDSH_OUTPUT"

if echo "$PDSH_OUTPUT" | grep -q 'ssh exited with exit code'; then
	echo ">> failed on some nodes" >&2
	exit 1
fi

echo ">> done"
