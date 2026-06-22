#!/bin/bash
# Reproduce CloudLab-style recovery comparison on Aliyun:
#   - sync CodeType config to every node
#   - full cluster restart
#   - mandatory inter-rack bandwidth limit (default 1Gb)
#   - coordinator ready check, then main_client
#
# Usage:
#   ./benchmark_recovery.sh lotus
#   ./benchmark_recovery.sh uniform
#   ./benchmark_recovery.sh compare          # lotus then uniform (two full restarts)
#   ./benchmark_recovery.sh lotus 0.5        # inter-rack 0.5Gb (stronger separation)
#   ./benchmark_recovery.sh lotus 1 no-limit # skip tc (baseline only)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

HOSTS_FILE="${HOSTS_FILE:-hosts}"
SSH_USER="${SSH_USER:-root}"
COORD_IP="$(awk -F'=' '/^coordinator_ip/ { gsub(/[ \t]/,"",$2); print $2 }' project/config/cluster.ini)"
CLUSTER_NUM="$(awk -F'=' '/^cluster_num/ { gsub(/[ \t]/,"",$2); print $2 }' project/config/cluster.ini)"
PARAM_XML="$SCRIPT_DIR/project/config/parameterConfiguration.xml"
COORD_LOG="/tmp/unilrc-coordinator.log"

usage() {
    echo "Usage: $0 <lotus|uniform|compare> [inter_gb] [no-limit]" >&2
    echo "  inter_gb: 0.5 | 1 | 2 | ... (default 1)" >&2
    exit 1
}

[[ $# -ge 1 ]] || usage
MODE="$1"
shift || true

INTER_GB="${1:-1}"
APPLY_LIMIT=1
if [[ "${2:-}" == "no-limit" ]] || [[ "${1:-}" == "no-limit" ]]; then
    APPLY_LIMIT=0
    if [[ "${1:-}" == "no-limit" ]]; then
        INTER_GB=1
    fi
fi

set_code_preset() {
    local preset="$1"
    python3 - "$preset" "$PARAM_XML" <<'PY'
import sys
import xml.etree.ElementTree as ET

preset = sys.argv[1]
path = sys.argv[2]
presets = {
    "lotus":   ("LotusLRC",   "24", "1", "4"),
    "uniform": ("UniformLRC", "24", "2", "2"),
}
if preset not in presets:
    raise SystemExit(f"unknown preset: {preset}")
code, k, r, z = presets[preset]
tree = ET.parse(path)
root = tree.getroot()
for tag, val in (("CodeType", code), ("k", k), ("r", r), ("z", z)):
    el = root.find(tag)
    if el is None:
        el = ET.SubElement(root, tag)
    el.text = val
tree.write(path, encoding="UTF-8", xml_declaration=True)
print(f"parameterConfiguration.xml -> CodeType={code} k={k} r={r} z={z}")
PY
}

sync_config() {
    echo ">> sync project/config to all nodes ..."
    while IFS= read -r ip || [[ -n "$ip" ]]; do
        ip="${ip%%#*}"
        ip="${ip#"${ip%%[![:space:]]*}"}"
        ip="${ip%"${ip##*[![:space:]]}"}"
        [[ -n "$ip" ]] || continue
        rsync -az --delete \
            -e "ssh -o ConnectTimeout=8 -o StrictHostKeyChecking=accept-new" \
            "$SCRIPT_DIR/project/config/" \
            "${SSH_USER}@${ip}:${SCRIPT_DIR}/project/config/" \
            || echo "warn: config sync failed on $ip" >&2
    done < "$HOSTS_FILE"
}

wait_coordinator_ready() {
    local need="${CLUSTER_NUM:-11}"
    local deadline=$((SECONDS + 180))
    echo ">> wait coordinator ($COORD_IP) Proxy Check ok x$need ..."
    while (( SECONDS < deadline )); do
        local ok
        ok="$(ssh -o ConnectTimeout=5 "${SSH_USER}@${COORD_IP}" \
            "grep -c '\\[Proxy Check\\] ok' '$COORD_LOG' 2>/dev/null || echo 0")"
        if [[ "$ok" -ge "$need" ]]; then
            echo ">> coordinator ready ($ok/$need proxies)"
            return 0
        fi
        sleep 2
    done
    echo ">> timeout waiting for coordinator; tail log:" >&2
    ssh "${SSH_USER}@${COORD_IP}" "tail -30 '$COORD_LOG' 2>/dev/null" || true
    return 1
}

start_coordinator() {
    echo ">> start coordinator on $COORD_IP ..."
    ssh -o ConnectTimeout=8 "${SSH_USER}@${COORD_IP}" bash -s <<EOF
set -e
cd "$SCRIPT_DIR"
pkill -9 run_coordinator 2>/dev/null || true
sleep 1
nohup ./project/cmake/build/run_coordinator >"$COORD_LOG" 2>&1 &
EOF
    wait_coordinator_ready
}

run_one_benchmark() {
    local preset="$1"
    echo ""
    echo "======== benchmark: $preset (inter-rack=${INTER_GB}Gb, limit=$APPLY_LIMIT) ========"
    set_code_preset "$preset"
    sync_config

    sh kill_all_nodes.sh
    sleep 5

    bash run_all_remote.sh
    sleep 5

    if [[ "$APPLY_LIMIT" -eq 1 ]]; then
        bash "limit_all_${INTER_GB}Gb.sh"
    else
        echo ">> skip bandwidth limit (baseline; Lotus advantage usually disappears)"
    fi

    start_coordinator
    sh test.sh
}

case "$MODE" in
    lotus|uniform)
        run_one_benchmark "$MODE"
        ;;
    compare)
        run_one_benchmark lotus
        echo ""
        echo "======== compare: second run (uniform) ========"
        run_one_benchmark uniform
        ;;
    *)
        usage
        ;;
esac

echo ">> done"
