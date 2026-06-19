#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck disable=SC1091
source "$SCRIPT_DIR/limit_common.sh"

run_limit_all_remote 2 "$@"
