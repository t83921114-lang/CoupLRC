#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CONFIG="$SCRIPT_DIR/project/config/cluster.ini"
USER="root"

get_ini() {
  local section="$1" key="$2"
  awk -F'=' -v SECTION="$section" -v KEY="$key" '
    function trim(s) { gsub(/^[ \t]+|[ \t]+$/, "", s); return s }
    /^[ \t]*#/ { next }
    /^[ \t]*\[/ { line = trim($0); f = (line == "[" SECTION "]"); next }
    f && $1 ~ "^[ \t]*" KEY "[ \t]*$" { print trim($2); exit }
  ' "$CONFIG"
}

COORD_IP="$(get_ini cluster coordinator_ip)"
[ -n "$COORD_IP" ] || { echo "coordinator_ip not set in $CONFIG"; exit 1; }

REMOTE_COMMAND="cd $SCRIPT_DIR && sh run_coordinator.sh"

PARALLEL=5

echo "Running coordinator on $COORD_IP ..."
pdsh -R ssh -w "$COORD_IP" -l $USER -f $PARALLEL "$REMOTE_COMMAND"

if [ $? -eq 0 ]; then
	echo "Command executed successfully on all nodes."
else
	echo "Failed to execute command on some nodes."
fi
