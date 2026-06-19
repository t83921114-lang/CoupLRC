#!/bin/bash
exec bash "$(cd "$(dirname "$0")" && pwd)/limit.sh" 10 "$@"
