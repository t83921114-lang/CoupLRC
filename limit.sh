#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck disable=SC1091
source "$SCRIPT_DIR/limit_common.sh"

INTER_GB="${1:-}"
if [ -z "$INTER_GB" ]; then
    echo "Usage: $0 <inter_rack_gb> [intra]" >&2
    echo "  inter-rack bandwidth: 0.5 | 1 | 2 | 5 | 10 (Gb/s)" >&2
    echo "  default: datanode unlimited, proxy inter-rack only" >&2
    echo "  add 'intra' to also limit proxy<->datanode at ${INTRA_RACK_GB} Gb/s" >&2
    exit 1
fi

shift
parse_limit_datanode_mode "$@"

INTER_KBPS=$(gb_to_kbps "$INTER_GB")
INTRA_KBPS=$(gb_to_kbps "$INTRA_RACK_GB")

apply_bandwidth_limits "$INTER_KBPS" "$INTRA_KBPS" "$SCRIPT_DIR" "$LIMIT_DATANODE"
