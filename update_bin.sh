#!/bin/bash
#
# Sync only runtime binaries/libs/configs to cluster hosts inferred from project/config/cluster.ini
#
# Defaults:
# - parallel fanout: PARALLEL=32
# - no compression (better for 10GbE LAN)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CONFIG="${1:-$SCRIPT_DIR/project/config/cluster.ini}"

SOURCE_DIR="$SCRIPT_DIR"
REMOTE_DIR="$SCRIPT_DIR"

PARALLEL="${PARALLEL:-32}"

if [ ! -f "$CONFIG" ]; then
  echo "Config not found: $CONFIG"
  echo "Usage: $0 [cluster.ini]"
  exit 1
fi

get_ini() {
  local section="$1" key="$2"
  awk -F'=' -v SECTION="$section" -v KEY="$key" '
    function trim(s) { gsub(/^[ \t]+|[ \t]+$/, "", s); return s }
    /^[ \t]*#/ { next }
    /^[ \t]*\[/ { line = trim($0); f = (line == "[" SECTION "]"); next }
    f && NF >= 2 {
      k = trim($1)
      if (k == KEY) {
        v = $0
        sub(/^[^=]*=/, "", v)
        v = trim(v)
        print v
        exit
      }
    }
  ' "$CONFIG"
}

CLUSTER_NUM="$(get_ini cluster cluster_num)"
DN_PER="$(get_ini cluster datanode_per_cluster)"
FIRST_IP="$(get_ini cluster first_proxy_ip)"
FIRST_PORT="$(get_ini cluster first_proxy_port)"
DN_PORT_START="$(get_ini cluster datanode_port_start)"
COORD_IP="$(get_ini cluster coordinator_ip)"
SSH_USER="$(get_ini ssh user)"
IP_MODE="$(get_ini cluster ip_mode)"
[ -n "$IP_MODE" ] || IP_MODE="distributed"

[ -n "${CLUSTER_NUM:-}" ] && [ -n "${DN_PER:-}" ] && [ -n "${FIRST_IP:-}" ] && [ -n "${FIRST_PORT:-}" ] || {
  echo "Missing required [cluster] keys in $CONFIG"
  exit 1
}
[ -n "${SSH_USER:-}" ] || {
  echo "Missing required [ssh] user in $CONFIG"
  exit 1
}

if [ "$FIRST_IP" = "127.0.0.1" ]; then
  echo "first_proxy_ip=127.0.0.1 (localhost mode) -> update_bin.sh is unnecessary."
  exit 0
fi

PREFIX="${FIRST_IP%.*}."
FIRST_OCTET="${FIRST_IP##*.}"

# Infer endpoints exactly like generate_xml_from_ini.py / run_all_remote.sh
PROXY_IPS=()
DN_IPS=()
ip_idx=0
for ((c=0; c<CLUSTER_NUM; c++)); do
  if [ "$IP_MODE" = "port_simulated" ]; then
    proxy_ip="${PREFIX}$((FIRST_OCTET + c))"
  else
    proxy_ip="${PREFIX}$((FIRST_OCTET + ip_idx))"
    ip_idx=$((ip_idx + 1))
  fi
  PROXY_IPS+=("$proxy_ip")
  for ((d=0; d<DN_PER; d++)); do
    if [ "$IP_MODE" = "port_simulated" ]; then
      dn_ip="$proxy_ip"
    else
      dn_ip="${PREFIX}$((FIRST_OCTET + ip_idx))"
      ip_idx=$((ip_idx + 1))
    fi
    DN_IPS+=("$dn_ip")
  done
done

uniq_lines() { awk '!seen[$0]++'; }

PROXY_HOSTS="$(printf "%s\n" "${PROXY_IPS[@]}" | uniq_lines)"
DN_HOSTS="$(printf "%s\n" "${DN_IPS[@]}" | uniq_lines)"
COORD_HOSTS="$(printf "%s\n" "$COORD_IP" | uniq_lines)"

echo "Coordinator: $COORD_IP"
echo "Proxy hosts: $(echo "$PROXY_HOSTS" | wc -l)"
echo "Datanode hosts: $(echo "$DN_HOSTS" | wc -l)"

TMPDIR="${TMPDIR:-/tmp}"
FILELIST_PROXY="$TMPDIR/unilrc-files-proxy.txt"
FILELIST_DN="$TMPDIR/unilrc-files-datanode.txt"
FILELIST_COORD="$TMPDIR/unilrc-files-coordinator.txt"

# Build file lists relative to SOURCE_DIR. rsync will create directories as needed.
# Add more runtime files here if necessary.
cat >"$FILELIST_PROXY" <<'EOF'
project/cmake/build/run_proxy
project/config/cluster.ini
project/config/clusterInformation.xml
project/config/parameterConfiguration.xml
EOF

cat >"$FILELIST_DN" <<'EOF'
project/cmake/build/run_datanode
project/config/cluster.ini
project/config/clusterInformation.xml
project/config/parameterConfiguration.xml
EOF

cat >"$FILELIST_COORD" <<'EOF'
project/cmake/build/run_coordinator
project/config/cluster.ini
project/config/clusterInformation.xml
project/config/parameterConfiguration.xml
EOF

sync_group() {
  local role="$1" filelist="$2"
  shift 2
  local hosts=("$@")

  if [ "${#hosts[@]}" -eq 0 ]; then
    echo "Skip $role: no hosts"
    return 0
  fi

  echo "Sync $role -> ${#hosts[@]} hosts (parallel=$PARALLEL)"

  printf "%s\n" "${hosts[@]}" | grep -vE '^[[:space:]]*$' | \
    xargs -r -n 1 -P "$PARALLEL" -I {} bash -lc '
      set -e
      ip="$1"
      echo "[$2] $ip"
      sudo rsync -a \
        --files-from="'"$filelist"'" \
        --relative \
        --prune-empty-dirs \
        -e "ssh -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=/root/.ssh/known_hosts" \
        "'"$SOURCE_DIR/"'" "'"$SSH_USER"'@$ip:'"$REMOTE_DIR/"'"
    ' _ {} "$role"
}

mapfile -t PROXY_HOST_ARR < <(printf "%s\n" "$PROXY_HOSTS" | grep -vE '^[[:space:]]*$' || true)
mapfile -t DN_HOST_ARR < <(printf "%s\n" "$DN_HOSTS" | grep -vE '^[[:space:]]*$' || true)
mapfile -t COORD_HOST_ARR < <(printf "%s\n" "$COORD_HOSTS" | grep -vE '^[[:space:]]*$' || true)

sync_group "proxy" "$FILELIST_PROXY" "${PROXY_HOST_ARR[@]}"
sync_group "datanode" "$FILELIST_DN" "${DN_HOST_ARR[@]}"
sync_group "coordinator" "$FILELIST_COORD" "${COORD_HOST_ARR[@]}"

echo "All done."

