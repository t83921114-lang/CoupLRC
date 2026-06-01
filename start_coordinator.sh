#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
USER="root"

REMOTE_COMMAND="cd $SCRIPT_DIR && sh run_coordinator.sh"

PARALLEL=5

echo "Running command on all nodes..."
pdsh -R ssh -w 10.10.1.2 -l $USER -f $PARALLEL "$REMOTE_COMMAND"

if [ $? -eq 0 ]; then
	echo "Command executed successfully on all nodes."
else
	echo "Failed to execute command on some nodes."
fi
