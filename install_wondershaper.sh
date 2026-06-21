#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
HOSTS_FILE="${SCRIPT_DIR}/hosts"

USER="root"

# 在每个节点安装 wondershaper (克隆到 /tmp，不污染项目目录)
REMOTE_COMMAND='
if command -v wondershaper >/dev/null 2>&1; then
    echo "wondershaper already installed"; exit 0;
fi
TMP_DIR=$(mktemp -d)
git clone --depth 1 https://github.com/magnific0/wondershaper.git "$TMP_DIR/wondershaper" && \
cd "$TMP_DIR/wondershaper" && \
sudo make install
RET=$?
rm -rf "$TMP_DIR"
exit $RET
'
PARALLEL=5

echo "Running command on all nodes..."
pdsh -R ssh -w ^$HOSTS_FILE -l $USER -f $PARALLEL "$REMOTE_COMMAND"

if [ $? -eq 0 ]; then
	echo "Command executed successfully on all nodes."
else
	echo "Failed to execute command on some nodes."
fi
