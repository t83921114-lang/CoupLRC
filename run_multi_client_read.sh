#!/bin/bash
# 在 leader client（client_hosts 第一个）上执行：
# 1. 本地写 1 条 stripe
# 2. 重复 N 轮：所有 client 并行各读 1 次；每轮各 client 先在 coordinator 的屏障(barrier)处对齐，
#    等齐所有 client 后一次性放行，从而消除 ssh/启动先后带来的读取错开。
#    并行时间 = max(各 client 的 get time)，与 main_client 内 get 计时一致。
#
# 用法:
#   ./run_multi_client_read.sh [cluster.ini] [stripe_id] [rounds]

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CONFIG="${1:-$SCRIPT_DIR/project/config/cluster.ini}"
STRIPE_ID="${2:-0}"
ROUNDS="${3:-5}"
PARAM_XML="$SCRIPT_DIR/project/config/parameterConfiguration.xml"

get_ini() {
  local section="$1" key="$2"
  awk -F'=' -v SECTION="$section" -v KEY="$key" '
    function trim(s) { gsub(/^[ \t]+|[ \t]+$/, "", s); return s }
    /^[ \t]*#/ { next }
    /^[ \t]*\[/ {
      line = trim($0)
      f = (line == "[" SECTION "]")
      next
    }
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

IP_LAYOUT="$SCRIPT_DIR/project/config/ip_layout.py"
BUILD="$SCRIPT_DIR/project/cmake/build/main_client"
SSH_USER=$(get_ini ssh user)
CLIENT_PORT_BASE=$(get_ini cluster client_port_base)
IP_MODE=$(get_ini cluster ip_mode)

[ -n "$SSH_USER" ] || { echo "Missing [ssh] user in $CONFIG"; exit 1; }
[ -f "$IP_LAYOUT" ] || { echo "Missing $IP_LAYOUT"; exit 1; }
[ -f "$BUILD" ] || { echo "Missing $BUILD (run compile first)"; exit 1; }
[ -f "$PARAM_XML" ] || { echo "Missing $PARAM_XML"; exit 1; }
[ -n "$CLIENT_PORT_BASE" ] || CLIENT_PORT_BASE=44444

if [ "$IP_MODE" = "all_ips" ]; then
  mapfile -t CLIENT_IPS < <(python3 "$IP_LAYOUT" --ini "$CONFIG" --format client-ips)
else
  CLIENT_IPS=("$(get_ini cluster client_ip)")
fi

CLIENT_NUM="${#CLIENT_IPS[@]}"
[ "$CLIENT_NUM" -ge 1 ] || { echo "No client IPs configured"; exit 1; }

STRIPE_MB=$(python3 - "$PARAM_XML" <<'PY'
import sys
import xml.etree.ElementTree as ET

root = ET.parse(sys.argv[1]).getroot()
k = int(root.find("k").text)
block_size = int(root.find("BlockSize").text)
print(k * block_size / 1024 / 1024)
PY
)

LEADER_IP="${CLIENT_IPS[0]}"
LEADER_PORT=$CLIENT_PORT_BASE
# Unique barrier session for this run; each round uses (SESSION, round) as the barrier key so
# all clients rendezvous at the coordinator and start their get() together (removes start skew).
SESSION="mcread_$$_$(date +%s)"
LOG_DIR=$(mktemp -d)
SUMMARY_FILE="$LOG_DIR/aggregate_summary.tsv"
trap 'rm -rf "$LOG_DIR"' EXIT

echo "=== Multi-client normal read (parallel aggregate) ==="
echo "leader=$LEADER_IP clients=$CLIENT_NUM stripe_id=$STRIPE_ID stripe_mb=$STRIPE_MB rounds=$ROUNDS"

run_on_client() {
  local cip="$1" cport="$2" extra_args="$3"
  local cmd="cd $SCRIPT_DIR && $BUILD --client-ip $cip --client-port $cport $extra_args"
  if [ "$cip" = "$LEADER_IP" ]; then
    bash -c "$cmd"
  else
    ssh -n -o ConnectTimeout=10 "${SSH_USER}@${cip}" "$cmd"
  fi
}

parse_client_log() {
  python3 - "$1" <<'PY'
import re, sys

text = open(sys.argv[1], encoding="utf-8", errors="replace").read()
m_t = re.search(r"get time:\s*([0-9.eE+-]+)", text)
if not m_t:
    m_t = re.search(r"Average time:\s*([0-9.eE+-]+)", text)
m_s = re.search(r"Speed:\s*([0-9.eE+-]+)\s*MB/s", text)
if m_t and m_s:
    print(f"{m_t.group(1)}\t{m_s.group(1)}")
PY
}

echo ""
echo "[1/2] Populate stripe $STRIPE_ID on leader $LEADER_IP:$LEADER_PORT ..."
run_on_client "$LEADER_IP" "$LEADER_PORT" "--populate 1"

echo "Waiting 5s ..."
sleep 5

echo ""
echo "[2/2] $ROUNDS rounds of parallel read (stripe $STRIPE_ID, $CLIENT_NUM clients) ..."
: >"$SUMMARY_FILE"

for ((r = 1; r <= ROUNDS; r++)); do
  echo "--- Round $r/$ROUNDS ---"
  PIDS=()
  T0=$(date +%s.%N)
  for ((i = 0; i < CLIENT_NUM; i++)); do
    CIP="${CLIENT_IPS[$i]}"
    CPORT=$((CLIENT_PORT_BASE + i))
    LOG_FILE="$LOG_DIR/round${r}_client${i}.log"
    (
      run_on_client "$CIP" "$CPORT" "--read-stripe $STRIPE_ID --read-rounds 1 --barrier-session $SESSION --barrier-round $r --client-index $i --client-num $CLIENT_NUM"
    ) >"$LOG_FILE" 2>&1 &
    PIDS+=($!)
  done

  FAIL=0
  for pid in "${PIDS[@]}"; do
    wait "$pid" || FAIL=1
  done
  T1=$(date +%s.%N)

  if [ "$FAIL" -ne 0 ]; then
    echo "Round $r failed. Logs: $LOG_DIR/round${r}_client*.log"
    for ((i = 0; i < CLIENT_NUM; i++)); do
      echo "=== client$i (${CLIENT_IPS[$i]}:$((CLIENT_PORT_BASE + i))) ==="
      cat "$LOG_DIR/round${r}_client${i}.log"
    done
    exit 1
  fi

  LAUNCH_WALL=$(python3 - "$T0" "$T1" <<'PY'
import sys
print(float(sys.argv[2]) - float(sys.argv[1]))
PY
)

  GET_TIMES=()
  GET_SPEEDS=()
  for ((i = 0; i < CLIENT_NUM; i++)); do
    parsed=$(parse_client_log "$LOG_DIR/round${r}_client${i}.log" || true)
    if [ -z "$parsed" ]; then
      echo "Round $r: missing get time in client$i log"
      cat "$LOG_DIR/round${r}_client${i}.log"
      exit 1
    fi
    get_t=${parsed%%$'\t'*}
    speed=${parsed#*$'\t'}
    GET_TIMES+=("$get_t")
    GET_SPEEDS+=("$speed")
  done

  WALL=$(python3 - "${GET_TIMES[@]}" <<'PY'
import sys
print(max(float(x) for x in sys.argv[1:]))
PY
)
  AGG=$(python3 - "$STRIPE_MB" "${GET_TIMES[@]}" <<'PY'
import sys
stripe_mb = float(sys.argv[1])
times = [float(x) for x in sys.argv[2:]]
print(sum(stripe_mb / t for t in times if t > 0))
PY
)

  echo "$r	$WALL	$AGG" >>"$SUMMARY_FILE"
  echo "Parallel get time (max over clients): ${WALL}s"
  echo "Total throughput (sum of per-client throughput): ${AGG} MB/s  (sum of ${STRIPE_MB} MB / get_time over ${CLIENT_NUM} clients)"
  echo "Launch wall time (incl. ssh/startup, reference only): ${LAUNCH_WALL}s"

  for ((i = 0; i < CLIENT_NUM; i++)); do
    echo "  client$i (${CLIENT_IPS[$i]}): get time=${GET_TIMES[$i]}s  speed=${GET_SPEEDS[$i]} MB/s"
  done
done

echo ""
python3 - "$SUMMARY_FILE" "$ROUNDS" "$CLIENT_NUM" "$STRIPE_ID" "$STRIPE_MB" <<'PY'
import sys

path, rounds, client_num, stripe_id, stripe_mb = sys.argv[1:]
rounds = int(rounds)
client_num = int(client_num)
stripe_mb = float(stripe_mb)

walls, aggs = [], []
with open(path, encoding="utf-8") as f:
    for line in f:
        _, wall, agg = line.rstrip("\n").split("\t")
        walls.append(float(wall))
        aggs.append(float(agg))

if not aggs:
    print("No successful rounds")
    sys.exit(0)

print("=== Multi-client parallel read summary ===")
print(f"clients={client_num}  rounds={len(aggs)}  stripe_id={stripe_id}  stripe_mb={stripe_mb}")
print(f"Average parallel get time (max over clients per round): {sum(walls) / len(walls):.6f}s")
print(f"Total throughput = sum of per-client throughput (per round):")
print(f"  Average total throughput: {sum(aggs) / len(aggs):.0f} MB/s")
print(f"  Max total throughput: {max(aggs):.0f} MB/s")
print(f"  Min total throughput: {min(aggs):.0f} MB/s")
print("Multi-client normal read test end")
PY

echo ""
echo "Logs: $LOG_DIR"
