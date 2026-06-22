#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck disable=SC1091
source "$SCRIPT_DIR/limit_common.sh"

INPUT_GB="${1:-}"
if [ -z "$INPUT_GB" ]; then
    echo "Usage: $0 <inter_rack_gb> [no-intra]" >&2
    echo "  allowed inter-rack: 0.5, 1, 2, 5, 10" >&2
    echo "  default: intra-rack proxy<->datanode fixed at ${INTRA_RACK_GB}Gb/s (HTB egress)" >&2
    echo "  add 'no-intra' to disable intra-rack limits (datanode unlimited)" >&2
    exit 1
fi

shift
run_limit_all_remote "$INPUT_GB" "$@"
