#!/bin/bash
# Shared helpers for rack-aware bandwidth limiting (tc + IFB).
# Egress (root htb on the physical iface) shapes outbound traffic by dst IP.
# Ingress (ifb0 via mirred redirect) shapes inbound traffic by src IP.
# Result: both directions are capped -- proxy<->proxy at inter-rack rate,
# proxy<->datanode at intra-rack rate.

INTRA_RACK_GB=10
IFB_DEV=ifb0

gb_to_kbps() {
    case "$1" in
        0.5) echo 512000 ;;
        1)   echo 1048576 ;;
        2)   echo 2097152 ;;
        5)   echo 5242880 ;;
        10)  echo 10485760 ;;
        *)
            echo "Unsupported bandwidth: ${1} Gb (allowed: 0.5, 1, 2, 5, 10)" >&2
            return 1
            ;;
    esac
}

kbps_to_tc_rate() {
    echo "${1}kbit"
}

kbps_to_rate_label() {
    case "$1" in
        512000)   echo "0.5Gb/s" ;;
        1048576)  echo "1Gb/s" ;;
        2097152)  echo "2Gb/s" ;;
        5242880)  echo "5Gb/s" ;;
        10485760) echo "10Gb/s" ;;
        *)        echo "${1}kbit/s" ;;
    esac
}

get_ini() {
    local section="$1" key="$2" file="$3"
    awk -F'=' -v SECTION="$section" -v KEY="$key" '
        function trim(s) { gsub(/^[ \t]+|[ \t]+$/, "", s); return s }
        /^[ \t]*#/ { next }
        /^[ \t]*\[/ {
            line = trim($0)
            in_section = (line == "[" SECTION "]")
            next
        }
        in_section && NF >= 2 {
            k = trim($1)
            if (k == KEY) {
                v = $0
                sub(/^[^=]*=/, "", v)
                print trim(v)
                exit
            }
        }
    ' "$file"
}

detect_iface() {
    local hint_ip="$1"
    local ip_prefix="${2:-}"
    local candidate=""

    if [ -n "$hint_ip" ] && [ "$hint_ip" != "127.0.0.1" ] && [ "$hint_ip" != "0.0.0.0" ]; then
        candidate=$(ip route get "$hint_ip" 2>/dev/null | awk '
            / dev / {
                for (i = 1; i <= NF; i++) {
                    if ($i == "dev") { print $(i + 1); exit }
                }
            }')
        if [ -n "$candidate" ] && ip link show "$candidate" 2>/dev/null | grep -q 'state UP'; then
            echo "$candidate"
            return 0
        fi
    fi

    if [ -n "$ip_prefix" ]; then
        candidate=$(ip -o -4 addr show scope global 2>/dev/null | awk -v p="$ip_prefix" '$4 ~ "^" p { print $2; exit }')
        if [ -n "$candidate" ] && ip link show "$candidate" 2>/dev/null | grep -q 'state UP'; then
            echo "$candidate"
            return 0
        fi
    fi

    candidate=$(ip route show default 2>/dev/null | awk '/default/ { print $5; exit }')
    if [ -n "$candidate" ] && ip link show "$candidate" 2>/dev/null | grep -q 'state UP'; then
        echo "$candidate"
        return 0
    fi

    candidate=$(ip -o link show 2>/dev/null | awk -F': ' '
        $2 !~ /^(lo|ifb)/ && $0 ~ /UP/ { print $2; exit }')
    if [ -n "$candidate" ]; then
        echo "$candidate"
        return 0
    fi

    return 1
}

parse_cluster_ip_base() {
    local first_ip="$1"

    if [ "$first_ip" = "127.0.0.1" ]; then
        CLUSTER_USE_LOCALHOST=1
        CLUSTER_IP_PREFIX=""
        CLUSTER_IP_BASE=0
        return 0
    fi

    CLUSTER_USE_LOCALHOST=0
    CLUSTER_IP_PREFIX="${first_ip%.*}."
    CLUSTER_IP_BASE="${first_ip##*.}"
    return 0
}

cluster_ip_at() {
    echo "${CLUSTER_IP_PREFIX}$((CLUSTER_IP_BASE + $1))"
}

resolve_node_hosts_file() {
    local config_file="$1"
    local rel repo_root

    rel=$(get_ini cluster node_hosts_file "$config_file")
    [ -n "$rel" ] || rel="node_hosts"
    repo_root="$(cd "$(dirname "$config_file")/../.." && pwd)"
    if [[ "$rel" = /* ]]; then
        echo "$rel"
    else
        echo "$repo_root/$rel"
    fi
}

load_node_hosts() {
    local config_file="$1"
    local hosts_path ip expected

    NODE_HOSTS=()
    hosts_path=$(resolve_node_hosts_file "$config_file")
    if [ ! -f "$hosts_path" ]; then
        echo "node_hosts not found: $hosts_path" >&2
        return 1
    fi

    while IFS= read -r ip || [ -n "$ip" ]; do
        ip="${ip%%#*}"
        ip="${ip#"${ip%%[![:space:]]*}"}"
        ip="${ip%"${ip##*[![:space:]]}"}"
        [ -n "$ip" ] || continue
        NODE_HOSTS+=("$ip")
    done < "$hosts_path"

    expected=$((CLUSTER_NUM * (1 + DN_PER)))
    if [ "${#NODE_HOSTS[@]}" -ne "$expected" ]; then
        echo "node_hosts count ${#NODE_HOSTS[@]} != expected $expected" >&2
        return 1
    fi

    export NODE_HOSTS
    return 0
}

node_host_at() {
    echo "${NODE_HOSTS[$1]}"
}

read_cluster_layout() {
    local config_file="$1"
    local cluster_num first_proxy_ip dn_per ip_mode

    cluster_num=$(get_ini cluster cluster_num "$config_file")
    first_proxy_ip=$(get_ini cluster first_proxy_ip "$config_file")
    dn_per=$(get_ini cluster datanode_per_cluster "$config_file")
    ip_mode=$(get_ini cluster ip_mode "$config_file")
    [ -n "$ip_mode" ] || ip_mode="distributed"

    if [ -z "$cluster_num" ] || [ -z "$first_proxy_ip" ] || [ -z "$dn_per" ]; then
        echo "Failed to read cluster config: $config_file" >&2
        return 1
    fi

    parse_cluster_ip_base "$first_proxy_ip" || return 1
    export CLUSTER_NUM="$cluster_num"
    export DN_PER="$dn_per"
    export IP_MODE="$ip_mode"
    return 0
}

enumerate_cluster_ips() {
    local config_file="$1"
    local c d ip_idx=0 proxy_ip dn_ip

    read_cluster_layout "$config_file" || return 1

    if [ "$IP_MODE" = "hosts_list" ]; then
        load_node_hosts "$config_file" || return 1
        for ((c = 0; c < CLUSTER_NUM; c++)); do
            echo "$(node_host_at "$ip_idx")"
            ip_idx=$((ip_idx + 1))
            for ((d = 0; d < DN_PER; d++)); do
                echo "$(node_host_at "$ip_idx")"
                ip_idx=$((ip_idx + 1))
            done
        done
        return 0
    fi

    for ((c = 0; c < CLUSTER_NUM; c++)); do
        if [ "$CLUSTER_USE_LOCALHOST" = 1 ]; then
            proxy_ip="127.0.0.1"
        elif [ "$IP_MODE" = "port_simulated" ]; then
            proxy_ip=$(cluster_ip_at "$c")
        else
            proxy_ip=$(cluster_ip_at "$ip_idx")
            ip_idx=$((ip_idx + 1))
        fi
        echo "$proxy_ip"

        for ((d = 0; d < DN_PER; d++)); do
            if [ "$CLUSTER_USE_LOCALHOST" = 1 ] || [ "$IP_MODE" = "port_simulated" ]; then
                dn_ip="$proxy_ip"
            else
                dn_ip=$(cluster_ip_at "$ip_idx")
                ip_idx=$((ip_idx + 1))
            fi
            if [ "$CLUSTER_USE_LOCALHOST" = 1 ] || [ "$IP_MODE" = "port_simulated" ]; then
                continue
            fi
            echo "$dn_ip"
        done
    done
}

get_my_cluster_ip() {
    local config_file="$1"
    local cluster_ip local_ip

    if ! read_cluster_layout "$config_file"; then
        return 1
    fi

    if [ "$CLUSTER_USE_LOCALHOST" = 1 ]; then
        echo "127.0.0.1"
        return 0
    fi

    while IFS= read -r cluster_ip; do
        [ -n "$cluster_ip" ] || continue
        while IFS= read -r local_ip; do
            if [ "$cluster_ip" = "$local_ip" ]; then
                echo "$cluster_ip"
                return 0
            fi
        done < <(ip -o -4 addr show scope global 2>/dev/null | awk '{ print $4 }' | cut -d/ -f1)
    done < <(enumerate_cluster_ips "$config_file")

    return 1
}

classify_node() {
    local my_ip="$1"
    local config_file="$2"
    local c d ip_idx=0 proxy_ip dn_ip

    if ! read_cluster_layout "$config_file"; then
        return 1
    fi

    if [ "$IP_MODE" = "hosts_list" ]; then
        load_node_hosts "$config_file" || return 1
    fi

    NODE_ROLE=""
    INTRA_IPS=()
    INTER_IPS=()

    for ((c = 0; c < CLUSTER_NUM; c++)); do
        if [ "$CLUSTER_USE_LOCALHOST" = 1 ]; then
            proxy_ip="127.0.0.1"
        elif [ "$IP_MODE" = "port_simulated" ]; then
            proxy_ip=$(cluster_ip_at "$c")
        elif [ "$IP_MODE" = "hosts_list" ]; then
            proxy_ip=$(node_host_at "$ip_idx")
            ip_idx=$((ip_idx + 1))
        else
            proxy_ip=$(cluster_ip_at "$ip_idx")
            ip_idx=$((ip_idx + 1))
        fi

        if [ "$my_ip" = "$proxy_ip" ]; then
            NODE_ROLE=proxy
            for ((d = 0; d < DN_PER; d++)); do
                if [ "$CLUSTER_USE_LOCALHOST" = 1 ] || [ "$IP_MODE" = "port_simulated" ]; then
                    INTRA_IPS+=("$proxy_ip")
                elif [ "$IP_MODE" = "hosts_list" ]; then
                    INTRA_IPS+=("$(node_host_at "$ip_idx")")
                    ip_idx=$((ip_idx + 1))
                else
                    INTRA_IPS+=("$(cluster_ip_at "$ip_idx")")
                    ip_idx=$((ip_idx + 1))
                fi
            done
            break
        fi

        for ((d = 0; d < DN_PER; d++)); do
            if [ "$CLUSTER_USE_LOCALHOST" = 1 ] || [ "$IP_MODE" = "port_simulated" ]; then
                dn_ip="$proxy_ip"
            elif [ "$IP_MODE" = "hosts_list" ]; then
                dn_ip=$(node_host_at "$ip_idx")
                ip_idx=$((ip_idx + 1))
            else
                dn_ip=$(cluster_ip_at "$ip_idx")
                ip_idx=$((ip_idx + 1))
            fi
            if [ "$my_ip" = "$dn_ip" ]; then
                NODE_ROLE=datanode
                INTRA_IPS=("$proxy_ip")
                break 2
            fi
        done
    done

    if [ "$NODE_ROLE" = proxy ]; then
        ip_idx=0
        for ((c = 0; c < CLUSTER_NUM; c++)); do
            if [ "$CLUSTER_USE_LOCALHOST" = 1 ]; then
                proxy_ip="127.0.0.1"
            elif [ "$IP_MODE" = "port_simulated" ]; then
                proxy_ip=$(cluster_ip_at "$c")
            elif [ "$IP_MODE" = "hosts_list" ]; then
                proxy_ip=$(node_host_at "$ip_idx")
                ip_idx=$((ip_idx + 1 + DN_PER))
            else
                proxy_ip=$(cluster_ip_at "$ip_idx")
                ip_idx=$((ip_idx + 1 + DN_PER))
            fi
            if [ "$proxy_ip" != "$my_ip" ]; then
                INTER_IPS+=("$proxy_ip")
            fi
        done
    fi

    if [ -z "$NODE_ROLE" ]; then
        return 1
    fi

    export NODE_ROLE INTRA_IPS INTER_IPS
    return 0
}

ensure_ifb() {
    modprobe ifb numifbs=1 2>/dev/null || true
    if ! ip link show "$IFB_DEV" >/dev/null 2>&1; then
        ip link add "$IFB_DEV" type ifb
    fi
    ip link set dev "$IFB_DEV" up
}

clear_bandwidth_limits() {
    local iface="$1"
    local quiet="${2:-0}"

    if [ -z "$iface" ]; then
        iface=$(detect_iface "$(get_ini cluster coordinator_ip "${SCRIPT_DIR}/project/config/cluster.ini")") || return 1
    fi

    if command -v wondershaper >/dev/null 2>&1; then
        wondershaper -c -a "$iface" 2>/dev/null || true
    fi

    tc qdisc del dev "$iface" root 2>/dev/null || true
    tc qdisc del dev "$iface" ingress 2>/dev/null || true
    tc qdisc del dev "$IFB_DEV" root 2>/dev/null || true

    if [ "$quiet" -eq 0 ]; then
        echo "ok | cleared | $iface"
    fi
}

parse_limit_datanode_mode() {
    # 1 (default) = proxy<->datanode 机架内固定 10Gb（HTB egress+ingress）
    # 0 = 关闭机架内限速（datanode 不限速，proxy 仅机架间限速）
    LIMIT_DATANODE=1
    local arg
    for arg in "$@"; do
        case "$arg" in
            no-intra|no_intra|--no-intra)
                LIMIT_DATANODE=0
                ;;
            intra|1|yes|on|--intra|--limit-datanode)
                LIMIT_DATANODE=1
                ;;
        esac
    done
}

limit_mode_label() {
    if [ "${LIMIT_DATANODE:-1}" -eq 1 ]; then
        echo "intra-rack=${INTRA_RACK_GB}Gb"
    else
        echo "intra=unlimited"
    fi
}

add_ip_filters() {
    local dev="$1"
    local parent="$2"
    local field="$3"
    local classid="$4"
    local prio="$5"
    shift 5
    local ip

    for ip in "$@"; do
        [ -n "$ip" ] || continue
        if [ "$field" = "dst" ]; then
            tc filter add dev "$dev" protocol ip parent "$parent" prio "$prio" \
                u32 match ip dst "$ip/32" flowid "$classid" || return 1
        else
            tc filter add dev "$dev" protocol ip parent "$parent" prio "$prio" \
                u32 match ip src "$ip/32" flowid "$classid" || return 1
        fi
    done
}

# ---------------------------------------------------------------------------
# Egress shaping (outbound, on the physical iface, classified by dst IP).
# ---------------------------------------------------------------------------

apply_proxy_inter_egress_limits() {
    local iface="$1"
    local inter_rate="$2"
    local max_rate="$3"
    shift 3

    tc qdisc add dev "$iface" root handle 1: htb default 30
    tc class add dev "$iface" parent 1: classid 1:1 htb rate "$max_rate" ceil "$max_rate"
    tc class add dev "$iface" parent 1:1 classid 1:10 htb rate "$inter_rate" ceil "$inter_rate" prio 1
    tc class add dev "$iface" parent 1:1 classid 1:30 htb rate "$max_rate" ceil "$max_rate" prio 3

    if [ "$#" -gt 0 ]; then
        add_ip_filters "$iface" "1:0" dst 1:10 1 "$@" || return 1
    fi
}

apply_proxy_egress_limits() {
    local iface="$1"
    local inter_rate="$2"
    local intra_rate="$3"
    local max_rate="$4"
    shift 4
    local -a inter_ips=()
    local -a intra_ips=()
    local section=inter
    local ip

    for ip in "$@"; do
        if [ "$ip" = "--" ]; then
            section=intra
            continue
        fi
        if [ "$section" = inter ]; then
            inter_ips+=("$ip")
        else
            intra_ips+=("$ip")
        fi
    done

    tc qdisc add dev "$iface" root handle 1: htb default 30
    tc class add dev "$iface" parent 1: classid 1:1 htb rate "$max_rate" ceil "$max_rate"
    tc class add dev "$iface" parent 1:1 classid 1:10 htb rate "$inter_rate" ceil "$inter_rate" prio 1
    tc class add dev "$iface" parent 1:1 classid 1:20 htb rate "$intra_rate" ceil "$intra_rate" prio 2
    tc class add dev "$iface" parent 1:1 classid 1:30 htb rate "$max_rate" ceil "$max_rate" prio 3

    if [ "${#inter_ips[@]}" -gt 0 ]; then
        add_ip_filters "$iface" "1:0" dst 1:10 1 "${inter_ips[@]}" || return 1
    fi
    if [ "${#intra_ips[@]}" -gt 0 ]; then
        add_ip_filters "$iface" "1:0" dst 1:20 2 "${intra_ips[@]}" || return 1
    fi
}

apply_datanode_intra_egress_limits() {
    local iface="$1"
    local intra_rate="$2"
    local max_rate="$3"
    shift 3

    tc qdisc add dev "$iface" root handle 1: htb default 30
    tc class add dev "$iface" parent 1: classid 1:1 htb rate "$max_rate" ceil "$max_rate"
    tc class add dev "$iface" parent 1:1 classid 1:20 htb rate "$intra_rate" ceil "$intra_rate" prio 2
    tc class add dev "$iface" parent 1:1 classid 1:30 htb rate "$max_rate" ceil "$max_rate" prio 3

    if [ "$#" -gt 0 ]; then
        add_ip_filters "$iface" "1:0" dst 1:20 2 "$@" || return 1
    fi
}

# ---------------------------------------------------------------------------
# Ingress shaping (inbound). The physical iface ingress is redirected to IFB
# via mirred; HTB on the IFB device classifies by src IP (the remote peer).
# ---------------------------------------------------------------------------

setup_ingress_redirect() {
    local iface="$1"

    ensure_ifb || return 1
    tc qdisc del dev "$IFB_DEV" root 2>/dev/null || true
    tc qdisc add dev "$iface" handle ffff: ingress || return 1
    tc filter add dev "$iface" parent ffff: protocol ip u32 \
        match u32 0 0 action mirred egress redirect dev "$IFB_DEV" || return 1
}

apply_proxy_inter_ingress_limits() {
    local ifb="$1"
    local inter_rate="$2"
    local max_rate="$3"
    shift 3

    tc qdisc add dev "$ifb" root handle 1: htb default 30
    tc class add dev "$ifb" parent 1: classid 1:1 htb rate "$max_rate" ceil "$max_rate"
    tc class add dev "$ifb" parent 1:1 classid 1:10 htb rate "$inter_rate" ceil "$inter_rate" prio 1
    tc class add dev "$ifb" parent 1:1 classid 1:30 htb rate "$max_rate" ceil "$max_rate" prio 3

    if [ "$#" -gt 0 ]; then
        add_ip_filters "$ifb" "1:0" src 1:10 1 "$@" || return 1
    fi
}

apply_proxy_ingress_limits() {
    local ifb="$1"
    local inter_rate="$2"
    local intra_rate="$3"
    local max_rate="$4"
    shift 4
    local -a inter_ips=()
    local -a intra_ips=()
    local section=inter
    local ip

    for ip in "$@"; do
        if [ "$ip" = "--" ]; then
            section=intra
            continue
        fi
        if [ "$section" = inter ]; then
            inter_ips+=("$ip")
        else
            intra_ips+=("$ip")
        fi
    done

    tc qdisc add dev "$ifb" root handle 1: htb default 30
    tc class add dev "$ifb" parent 1: classid 1:1 htb rate "$max_rate" ceil "$max_rate"
    tc class add dev "$ifb" parent 1:1 classid 1:10 htb rate "$inter_rate" ceil "$inter_rate" prio 1
    tc class add dev "$ifb" parent 1:1 classid 1:20 htb rate "$intra_rate" ceil "$intra_rate" prio 2
    tc class add dev "$ifb" parent 1:1 classid 1:30 htb rate "$max_rate" ceil "$max_rate" prio 3

    if [ "${#inter_ips[@]}" -gt 0 ]; then
        add_ip_filters "$ifb" "1:0" src 1:10 1 "${inter_ips[@]}" || return 1
    fi
    if [ "${#intra_ips[@]}" -gt 0 ]; then
        add_ip_filters "$ifb" "1:0" src 1:20 2 "${intra_ips[@]}" || return 1
    fi
}

apply_datanode_ingress_limits() {
    local ifb="$1"
    local intra_rate="$2"
    local max_rate="$3"
    shift 3

    tc qdisc add dev "$ifb" root handle 1: htb default 30
    tc class add dev "$ifb" parent 1: classid 1:1 htb rate "$max_rate" ceil "$max_rate"
    tc class add dev "$ifb" parent 1:1 classid 1:20 htb rate "$intra_rate" ceil "$intra_rate" prio 2
    tc class add dev "$ifb" parent 1:1 classid 1:30 htb rate "$max_rate" ceil "$max_rate" prio 3

    if [ "$#" -gt 0 ]; then
        add_ip_filters "$ifb" "1:0" src 1:20 2 "$@" || return 1
    fi
}

apply_bandwidth_limits() {
    local inter_kbps="$1"
    local intra_kbps="$2"
    local script_dir="$3"
    local limit_datanode="${4:-0}"
    local config_file="${script_dir}/project/config/cluster.ini"
    local coordinator_ip my_ip iface inter_rate intra_rate max_rate
    local inter_label intra_label

    if [ ! -f "$config_file" ]; then
        echo "Config not found: $config_file" >&2
        return 1
    fi

    coordinator_ip=$(get_ini cluster coordinator_ip "$config_file")
    first_proxy_ip=$(get_ini cluster first_proxy_ip "$config_file")
    parse_cluster_ip_base "$first_proxy_ip"

    my_ip=$(get_my_cluster_ip "$config_file") || true
    if [ -z "$my_ip" ]; then
        echo "skip | no cluster IP"
        return 0
    fi

    if ! classify_node "$my_ip" "$config_file"; then
        echo "skip | not proxy/datanode"
        return 0
    fi

    iface=$(detect_iface "$coordinator_ip" "$CLUSTER_IP_PREFIX") || {
        echo "fail | no active interface" >&2
        return 1
    }

    if [ "$NODE_ROLE" = datanode ] && [ "$limit_datanode" -eq 0 ]; then
        clear_bandwidth_limits "$iface" 1
        echo "ok | datanode | $iface | unlimited"
        return 0
    fi

    inter_rate=$(kbps_to_tc_rate "$inter_kbps")
    intra_rate=$(kbps_to_tc_rate "$intra_kbps")
    max_rate="$intra_rate"
    inter_label=$(kbps_to_rate_label "$inter_kbps")
    intra_label=$(kbps_to_rate_label "$intra_kbps")

    clear_bandwidth_limits "$iface" 1

    if [ "$NODE_ROLE" = proxy ] && [ "$limit_datanode" -eq 0 ]; then
        # Legacy mode: proxy 仅机架间限速（出+入），机架内不限速。
        apply_proxy_inter_egress_limits "$iface" "$inter_rate" "$max_rate" "${INTER_IPS[@]}" || {
            echo "fail | proxy | $iface | tc egress setup failed" >&2
            return 1
        }
        setup_ingress_redirect "$iface" || {
            echo "fail | proxy | $iface | tc ingress setup failed" >&2
            return 1
        }
        apply_proxy_inter_ingress_limits "$IFB_DEV" "$inter_rate" "$max_rate" "${INTER_IPS[@]}" || {
            echo "fail | proxy | $iface | tc ingress setup failed" >&2
            return 1
        }
        echo "ok | proxy | $iface | inter=${inter_label}(${#INTER_IPS[@]}) intra=unlimited | egress+ingress"
        return 0
    fi

    if [ "$NODE_ROLE" = proxy ]; then
        # Default: inter-rack -> other proxies, intra-rack -> local datanodes; both directions.
        apply_proxy_egress_limits "$iface" "$inter_rate" "$intra_rate" "$max_rate" \
            "${INTER_IPS[@]}" -- "${INTRA_IPS[@]}" || {
            echo "fail | proxy | $iface | tc egress setup failed" >&2
            return 1
        }
        setup_ingress_redirect "$iface" || {
            echo "fail | proxy | $iface | tc ingress setup failed" >&2
            return 1
        }
        apply_proxy_ingress_limits "$IFB_DEV" "$inter_rate" "$intra_rate" "$max_rate" \
            "${INTER_IPS[@]}" -- "${INTRA_IPS[@]}" || {
            echo "fail | proxy | $iface | tc ingress setup failed" >&2
            return 1
        }
        echo "ok | proxy | $iface | inter=${inter_label}(${#INTER_IPS[@]}) intra=${intra_label}(${#INTRA_IPS[@]}) | egress+ingress"
        return 0
    fi

    # Datanode: shape traffic to/from the local proxy (egress + ingress).
    apply_datanode_intra_egress_limits "$iface" "$intra_rate" "$max_rate" "${INTRA_IPS[@]}" || {
        echo "fail | datanode | $iface | tc egress setup failed" >&2
        return 1
    }
    setup_ingress_redirect "$iface" || {
        echo "fail | datanode | $iface | tc ingress setup failed" >&2
        return 1
    }
    apply_datanode_ingress_limits "$IFB_DEV" "$intra_rate" "$max_rate" "${INTRA_IPS[@]}" || {
        echo "fail | datanode | $iface | tc ingress setup failed" >&2
        return 1
    }
    echo "ok | datanode | $iface | intra=${intra_label} <-> ${INTRA_IPS[*]} | egress+ingress"
}

run_limit_all_remote() {
    local inter_gb="$1"
    shift
    local script_dir hosts_file user parallel remote_cmd mode_label extra=""
    local pdsh_output

    script_dir="$(cd "$(dirname "$0")" && pwd)"
    hosts_file="${script_dir}/hosts"
    user="root"
    parallel=5

    parse_limit_datanode_mode "$@"
    mode_label="$(limit_mode_label)"
    if [ "$LIMIT_DATANODE" -eq 0 ]; then
        extra="no-intra"
    fi

    remote_cmd="cd \"$script_dir\" && bash limit_${inter_gb}Gb.sh ${extra}"

    echo ">> limit inter-rack=${inter_gb}Gb, ${mode_label} ..."
    pdsh_output=$(sudo pdsh -R ssh -w ^"$hosts_file" -l "$user" -f "$parallel" "$remote_cmd" 2>&1)
    echo "$pdsh_output"

    if echo "$pdsh_output" | grep -qE 'ssh exited with exit code|fail \|'; then
        echo ">> failed on some nodes" >&2
        return 1
    fi

    echo ">> done"
}
