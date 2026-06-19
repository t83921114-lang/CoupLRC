#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck disable=SC1091
source "$SCRIPT_DIR/limit_common.sh"

INPUT_GB="${1:-}"
if [ -z "$INPUT_GB" ]; then
    echo "Usage: $0 <inter_rack_gb> [intra]" >&2
    echo "  allowed inter-rack: 0.5, 1, 2, 5, 10" >&2
    echo "  default: datanode unlimited, proxy inter-rack only" >&2
    echo "  add 'intra' to also limit proxy<->datanode at 10Gb/s" >&2
    exit 1
fi

shift
run_limit_all_remote "$INPUT_GB" "$@"
