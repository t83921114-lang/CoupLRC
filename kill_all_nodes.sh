#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
HOSTS_FILE="hosts"

USER="root"

REMOTE_COMMAND="cd $SCRIPT_DIR && sh kill_all.sh"

PARALLEL=5

echo "Running command on all nodes..."
sudo pdsh -R ssh -w ^$HOSTS_FILE -l $USER -f $PARALLEL "$REMOTE_COMMAND"

if [ $? -eq 0 ]; then
	echo "Command executed successfully on all nodes."
else
	echo "Failed to execute command on some nodes."
fi

cd "$SCRIPT_DIR"
sh kill_all.sh