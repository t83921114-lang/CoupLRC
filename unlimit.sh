#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck disable=SC1091
source "$SCRIPT_DIR/limit_common.sh"

CONFIG_FILE="${SCRIPT_DIR}/project/config/cluster.ini"
COORDINATOR_IP=""
if [ -f "$CONFIG_FILE" ]; then
    COORDINATOR_IP=$(get_ini cluster coordinator_ip "$CONFIG_FILE")
fi

IFACE=$(detect_iface "$COORDINATOR_IP") || {
    echo "fail | no active interface" >&2
    exit 1
}

clear_bandwidth_limits "$IFACE"
