#!/bin/bash
# B 方案：在 client 端执行，通过 SSH 在各 proxy/datanode 节点上启动进程
# 配置文件默认：项目根目录下 project/config/cluster.ini

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CONFIG="${1:-$SCRIPT_DIR/project/config/cluster.ini}"

if [ ! -f "$CONFIG" ]; then
  echo "Config not found: $CONFIG"
  echo "Usage: $0 [cluster.ini]"
  exit 1
fi

# 读 INI：取 [section] 下 key 的值（去掉首尾空格）
# 使用字面匹配 [section]，避免在 awk 正则中 [cluster] 被当作字符类
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
        sub(/^[^=]*=/, "", v)   # keep value even if it contains '='
        v = trim(v)
        print v
        exit
      }
    }
  ' "$CONFIG"
}

CLUSTER_NUM=$(get_ini cluster cluster_num)
DN_PER=$(get_ini cluster datanode_per_cluster)
FIRST_IP=$(get_ini cluster first_proxy_ip)
FIRST_PORT=$(get_ini cluster first_proxy_port)
DN_PORT_START=$(get_ini cluster datanode_port_start)
COORD_IP=$(get_ini cluster coordinator_ip)
SSH_USER=$(get_ini ssh user)
IP_MODE=$(get_ini cluster ip_mode)
REMOTE_REPO="$SCRIPT_DIR"
[ -n "$IP_MODE" ] || IP_MODE="distributed"

# 校验
[ -n "$CLUSTER_NUM" ] && [ -n "$DN_PER" ] && [ -n "$FIRST_IP" ] && [ -n "$FIRST_PORT" ] || {
  echo "Missing required [cluster] keys in $CONFIG"
  exit 1
}
[ -n "$SSH_USER" ] || {
  echo "Missing required [ssh] user in $CONFIG"
  exit 1
}

# 127.0.0.1 则所有节点 IP 均为 127.0.0.1
if [ "$FIRST_IP" = "127.0.0.1" ]; then
  USE_LOCALHOST=1
  IP_MODE="colocated"
else
  USE_LOCALHOST=0
  PREFIX="${FIRST_IP%.*}."
  FIRST_OCTET="${FIRST_IP##*.}"
fi

BUILD="$REMOTE_REPO/project/cmake/build"

# 本机运行时只在最开始 kill 一次，避免每次循环都杀掉已启动的进程
if [ "$USE_LOCALHOST" = 1 ]; then
  pkill -9 run_datanode 2>/dev/null || true
  pkill -9 run_proxy 2>/dev/null || true
  sleep 0.002
fi

ip_idx=0
c=0
while [ "$c" -lt "$CLUSTER_NUM" ]; do
  PROXY_PORT=$((FIRST_PORT + c))

  # Same logic as generate_xml_from_ini.py
  if [ "$USE_LOCALHOST" = 1 ]; then
    PROXY_IP="127.0.0.1"
  elif [ "$IP_MODE" = "port_simulated" ]; then
    PROXY_IP="${PREFIX}$((FIRST_OCTET + c))"
  else
    PROXY_IP="${PREFIX}$((FIRST_OCTET + ip_idx))"
    ip_idx=$((ip_idx + 1))
  fi

  echo "Cluster $c: proxy ${PROXY_IP}:${PROXY_PORT}"

  # Start datanodes (port_simulated / localhost: same host as proxy, different ports)
  d=0
  while [ "$d" -lt "$DN_PER" ]; do
    DP=$((DN_PORT_START + c * DN_PER + d))
    if [ "$USE_LOCALHOST" = 1 ] || [ "$IP_MODE" = "port_simulated" ]; then
      DN_IP="$PROXY_IP"
    else
      DN_IP="${PREFIX}$((FIRST_OCTET + ip_idx))"
      ip_idx=$((ip_idx + 1))
    fi
    echo "  datanode $d: ${DN_IP}:${DP}"
    if [ "$DN_IP" = "127.0.0.1" ]; then
      # Detach output to avoid blocking this script
      bash -c "cd $REMOTE_REPO && nohup $BUILD/run_datanode ${DN_IP}:${DP} </dev/null >\"/tmp/unilrc-datanode-${DN_IP//./_}-${DP}.log\" 2>&1 &" || {
        echo "Failed: datanode $d of cluster $c at $DN_IP"
      }
    else
      # Use -n and nohup + redirection so ssh returns immediately (no stdout/stderr attached)
      ssh -n -o ConnectTimeout=5 "${SSH_USER}@${DN_IP}" "cd $REMOTE_REPO && pkill -9 run_datanode 2>/dev/null || true; sleep 0.002; nohup $BUILD/run_datanode ${DN_IP}:${DP} </dev/null >\"/tmp/unilrc-datanode-${DN_IP//./_}-${DP}.log\" 2>&1 &" || {
        echo "Failed: datanode $d of cluster $c at $DN_IP"
      }
    fi
    d=$((d + 1))
  done

  # Start proxy (on PROXY_IP host)
  if [ "$PROXY_IP" = "127.0.0.1" ]; then
    bash -c "cd $REMOTE_REPO && sleep 0.002 && nohup $BUILD/run_proxy ${PROXY_IP}:${PROXY_PORT} ${COORD_IP} </dev/null >\"/tmp/unilrc-proxy-${PROXY_IP//./_}-${PROXY_PORT}.log\" 2>&1 &" || {
      echo "Failed: proxy of cluster $c at $PROXY_IP"
    }
  else
    ssh -n -o ConnectTimeout=5 "${SSH_USER}@${PROXY_IP}" "cd $REMOTE_REPO && pkill -9 run_proxy 2>/dev/null || true; sleep 0.002; nohup $BUILD/run_proxy ${PROXY_IP}:${PROXY_PORT} ${COORD_IP} </dev/null >\"/tmp/unilrc-proxy-${PROXY_IP//./_}-${PROXY_PORT}.log\" 2>&1 &" || {
      echo "Failed: proxy of cluster $c at $PROXY_IP"
    }
  fi

  c=$((c + 1))
done
echo "Done."
