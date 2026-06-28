#!/bin/bash
# 在 leader client（client_hosts 第一个）上执行：
# 1. 本地写 1 条 stripe
# 2. N 轮：所有 client 并行读同一条 stripe，等全部完成后再下一轮
# 3. 汇总：按并行墙钟时间统计（与 main_client 原 normal read 字段对齐）
#
# 用法:
#   ./run_multi_client_read.sh [cluster.ini] [stripe_id] [rounds]

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CONFIG="${1:-$SCRIPT_DIR/project/config/cluster.ini}"
STRIPE_ID="${2:-0}"
ROUNDS="${3:-5}"

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
[ -n "$CLIENT_PORT_BASE" ] || CLIENT_PORT_BASE=44444

if [ "$IP_MODE" = "all_ips" ]; then
  mapfile -t CLIENT_IPS < <(python3 "$IP_LAYOUT" --ini "$CONFIG" --format client-ips)
else
  CLIENT_IPS=("$(get_ini cluster client_ip)")
fi

CLIENT_NUM="${#CLIENT_IPS[@]}"
[ "$CLIENT_NUM" -ge 1 ] || { echo "No client IPs configured"; exit 1; }

LEADER_IP="${CLIENT_IPS[0]}"
LEADER_PORT=$CLIENT_PORT_BASE
LOG_DIR=$(mktemp -d)
trap 'rm -rf "$LOG_DIR"' EXIT

echo "=== Multi-client normal read (leader: $LEADER_IP) ==="
echo "clients=$CLIENT_NUM stripe_id=$STRIPE_ID rounds=$ROUNDS port_base=$CLIENT_PORT_BASE"

run_on_client() {
  local cip="$1" cport="$2" extra_args="$3"
  local cmd="cd $SCRIPT_DIR && $BUILD --client-ip $cip --client-port $cport $extra_args"
  if [ "$cip" = "$LEADER_IP" ]; then
    bash -c "$cmd"
  else
    ssh -n -o ConnectTimeout=10 "${SSH_USER}@${cip}" "$cmd"
  fi
}

print_total_summary() {
  python3 - "$LOG_DIR" "$ROUNDS" "$CLIENT_NUM" "$STRIPE_ID" <<'PY'
import re, sys

log_dir, rounds, client_num = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])

def parse_client_log(path):
    try:
        text = open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return None, None
    m_t = re.search(r"get time:\s*([0-9.eE+-]+)", text)
    m_s = re.search(r"Speed:\s*([0-9.eE+-]+)\s*MB/s", text)
    t = float(m_t.group(1)) if m_t else None
    s = float(m_s.group(1)) if m_s else None
    return t, s

round_walls = []
round_agg_speeds = []
stripe_mbs = []
per_client_times = []

for r in range(1, rounds + 1):
    wall_path = f"{log_dir}/round{r}_wall.txt"
    try:
        wall = float(open(wall_path, encoding="utf-8").read().strip())
    except (OSError, ValueError):
        print(f"Warning: missing wall time for round {r}", file=sys.stderr)
        continue

    rt, rs = [], []
    for i in range(client_num):
        t, s = parse_client_log(f"{log_dir}/round{r}_client{i}.log")
        if t is not None:
            rt.append(t)
            per_client_times.append(t)
        if s is not None:
            rs.append(s)
        if t is not None and s is not None and t > 0:
            stripe_mbs.append(s * t)

    if not rt:
        print(f"Warning: round {r} has no successful client reads", file=sys.stderr)
        continue

    round_walls.append(wall)
    # 并行一轮：N 个 client 各读 1 条 stripe，集群交付量 = N * stripe_mb，耗时 = 墙钟 wall
    stripe_mb = stripe_mbs[-1] if stripe_mbs else (sum(rs) / len(rs) * sum(rt) / len(rt) if rs and rt else 0.0)
    agg_speed = client_num * stripe_mb / wall if wall > 0 else 0.0
    round_agg_speeds.append(agg_speed)

    print(f"  Round {r}: wall={wall:.6f}s  "
          f"(client get time min={min(rt):.6f}s max={max(rt):.6f}s)  "
          f"aggregate Speed={agg_speed:.0f} MB/s")

if not round_walls:
    print("Normal read test summary: no successful rounds")
    sys.exit(0)

total_wall = sum(round_walls)
avg_wall = total_wall / len(round_walls)
total_reads = len(round_walls) * client_num
stripe_mb = sum(stripe_mbs) / len(stripe_mbs) if stripe_mbs else 0.0
total_data_mb = total_reads * stripe_mb

throughput = total_reads / total_wall if total_wall > 0 else 0.0
speed = total_data_mb / total_wall if total_wall > 0 else 0.0
max_speed = max(round_agg_speeds) if round_agg_speeds else 0.0
min_speed = min(round_agg_speeds) if round_agg_speeds else 0.0

print("")
print("=== Normal read test summary (multi-client parallel) ===")
print(f"clients={client_num}  rounds={len(round_walls)}  stripe_id={sys.argv[4]}")
print(f"Total time: {total_wall:.6f}   # 各轮并行墙钟之和（leader 实测）")
print(f"Average time: {avg_wall:.6f}   # 平均每轮并行耗时")
print(f"Throughput (stripes/s): {throughput:.6f}   # {total_reads} 次读 / 总墙钟")
print(f"Speed: {speed:.0f} MB/s   # 集群聚合带宽 = 总读取数据量 / 总墙钟")
print(f"Max speed: {max_speed:.0f} MB/s   # 各轮聚合带宽的最大值")
print(f"Min speed: {min_speed:.0f} MB/s   # 各轮聚合带宽的最小值")
if per_client_times:
    print(f"(per-client get time avg={sum(per_client_times)/len(per_client_times):.6f}s, "
          f"min={min(per_client_times):.6f}s, max={max(per_client_times):.6f}s — 仅供参考)")
print("Normal read test end")
PY
}

echo ""
echo "[1/2] Populate stripe $STRIPE_ID on leader $LEADER_IP:$LEADER_PORT ..."
run_on_client "$LEADER_IP" "$LEADER_PORT" "--populate 1"

echo "Waiting 5s ..."
sleep 5

echo ""
echo "[2/2] $ROUNDS rounds of parallel read (stripe $STRIPE_ID) ..."
for ((r = 1; r <= ROUNDS; r++)); do
  echo "--- Round $r/$ROUNDS ---"
  PIDS=()
  T0=$(date +%s.%N)
  for ((i = 0; i < CLIENT_NUM; i++)); do
    CIP="${CLIENT_IPS[$i]}"
    CPORT=$((CLIENT_PORT_BASE + i))
    LOG_FILE="$LOG_DIR/round${r}_client${i}.log"
    (
      echo "[client$i $CIP:$CPORT]"
      run_on_client "$CIP" "$CPORT" "--read-stripe $STRIPE_ID"
    ) >"$LOG_FILE" 2>&1 &
    PIDS+=($!)
  done
  FAIL=0
  for pid in "${PIDS[@]}"; do
    wait "$pid" || FAIL=1
  done
  T1=$(date +%s.%N)
  python3 - "$T0" "$T1" >"$LOG_DIR/round${r}_wall.txt" <<'PY'
import sys
print(float(sys.argv[2]) - float(sys.argv[1]))
PY
  if [ "$FAIL" -ne 0 ]; then
    echo "Round $r failed. Logs: $LOG_DIR/round${r}_client*.log"
    exit 1
  fi
  for ((i = 0; i < CLIENT_NUM; i++)); do
    echo "--- client$i (${CLIENT_IPS[$i]}) ---"
    cat "$LOG_DIR/round${r}_client${i}.log"
  done
done

echo ""
echo "Per-round parallel summary:"
print_total_summary

echo ""
echo "All $ROUNDS rounds completed. Logs: $LOG_DIR"
