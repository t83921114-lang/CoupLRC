#!/bin/bash
# Shared helpers for rack-aware bandwidth limiting (tc + IFB).

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
    local candidate=""

    if [ -n "$hint_ip" ]; then
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

    candidate=$(ip -o -4 addr show scope global 2>/dev/null | awk '$4 ~ /^10\.10\.1\./ { print $2; exit }')
    if [ -n "$candidate" ] && ip link show "$candidate" 2>/dev/null | grep -q 'state UP'; then
        echo "$candidate"
        return 0
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

get_my_cluster_ip() {
    ip -o -4 addr show scope global 2>/dev/null | awk '{ print $4 }' | cut -d/ -f1 | awk '/^10\.10\.1\.[0-9]+$/ { print; exit }'
}

ip_last_octet() {
    echo "$1" | awk -F. '{ print $4 }'
}

make_cluster_ip() {
    echo "10.10.1.$1"
}

classify_node() {
    local my_ip="$1"
    local config_file="$2"
    local cluster_num first_proxy_ip dn_per
    local first_octet c proxy_oct proxy_ip d dn_oct dn_ip

    cluster_num=$(get_ini cluster cluster_num "$config_file")
    first_proxy_ip=$(get_ini cluster first_proxy_ip "$config_file")
    dn_per=$(get_ini cluster datanode_per_cluster "$config_file")

    if [ -z "$cluster_num" ] || [ -z "$first_proxy_ip" ] || [ -z "$dn_per" ]; then
        echo "Failed to read cluster config: $config_file" >&2
        return 1
    fi

    first_octet=$(ip_last_octet "$first_proxy_ip")
    NODE_ROLE=""
    INTRA_IPS=()
    INTER_IPS=()

    for ((c = 0; c < cluster_num; c++)); do
        proxy_oct=$((first_octet + 4 * c))
        proxy_ip=$(make_cluster_ip "$proxy_oct")

        if [ "$my_ip" = "$proxy_ip" ]; then
            NODE_ROLE=proxy
            for ((d = 1; d <= dn_per; d++)); do
                INTRA_IPS+=("$(make_cluster_ip $((proxy_oct + d)))")
            done
            break
        fi

        for ((d = 1; d <= dn_per; d++)); do
            dn_oct=$((proxy_oct + d))
            dn_ip=$(make_cluster_ip "$dn_oct")
            if [ "$my_ip" = "$dn_ip" ]; then
                NODE_ROLE=datanode
                INTRA_IPS=("$proxy_ip")
                break 2
            fi
        done
    done

    if [ "$NODE_ROLE" = proxy ]; then
        for ((c = 0; c < cluster_num; c++)); do
            proxy_oct=$((first_octet + 4 * c))
            proxy_ip=$(make_cluster_ip "$proxy_oct")
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
    # 0 = datanode 不限速，proxy 仅机架间限速
    # 1 = datanode + proxy 机架内也限速 10Gb
    LIMIT_DATANODE=0
    local arg
    for arg in "$@"; do
        case "$arg" in
            intra|1|yes|on|--intra|--limit-datanode)
                LIMIT_DATANODE=1
                ;;
        esac
    done
}

limit_mode_label() {
    if [ "${LIMIT_DATANODE:-0}" -eq 1 ]; then
        echo "intra-rack=10Gb"
    else
        echo "datanode=unlimited"
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
    my_ip=$(get_my_cluster_ip)
    if [ -z "$my_ip" ]; then
        echo "skip | no cluster IP"
        return 0
    fi

    if ! classify_node "$my_ip" "$config_file"; then
        echo "skip | not proxy/datanode"
        return 0
    fi

    iface=$(detect_iface "$coordinator_ip") || {
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
    max_rate="10gbit"
    inter_label=$(kbps_to_rate_label "$inter_kbps")
    intra_label=$(kbps_to_rate_label "$intra_kbps")

    clear_bandwidth_limits "$iface" 1

    if [ "$NODE_ROLE" = proxy ] && [ "$limit_datanode" -eq 0 ]; then
        # Default mode: HTB egress-only inter-rack shaping on proxy (no IFB).
        apply_proxy_inter_egress_limits "$iface" "$inter_rate" "$max_rate" "${INTER_IPS[@]}" || {
            echo "fail | proxy | $iface | tc setup failed" >&2
            return 1
        }
        echo "ok | proxy | $iface | inter=${inter_label}(${#INTER_IPS[@]}) intra=unlimited"
        return 0
    fi

    ensure_ifb

    # Full mode (intra): egress + IFB ingress shaping.
    tc qdisc add dev "$iface" root handle 1: htb default 30
    tc class add dev "$iface" parent 1: classid 1:1 htb rate "$max_rate" ceil "$max_rate"
    tc class add dev "$iface" parent 1:1 classid 1:10 htb rate "$inter_rate" ceil "$inter_rate" prio 1
    if [ "$limit_datanode" -eq 1 ]; then
        tc class add dev "$iface" parent 1:1 classid 1:20 htb rate "$intra_rate" ceil "$intra_rate" prio 2
    fi
    tc class add dev "$iface" parent 1:1 classid 1:30 htb rate "$max_rate" ceil "$max_rate" prio 3

    # Ingress shaping via IFB redirect.
    tc qdisc add dev "$IFB_DEV" root handle 2: htb default 30
    tc class add dev "$IFB_DEV" parent 2: classid 2:1 htb rate "$max_rate" ceil "$max_rate"
    tc class add dev "$IFB_DEV" parent 2:1 classid 2:10 htb rate "$inter_rate" ceil "$inter_rate" prio 1
    if [ "$limit_datanode" -eq 1 ]; then
        tc class add dev "$IFB_DEV" parent 2:1 classid 2:20 htb rate "$intra_rate" ceil "$intra_rate" prio 2
    fi
    tc class add dev "$IFB_DEV" parent 2:1 classid 2:30 htb rate "$max_rate" ceil "$max_rate" prio 3

    tc qdisc add dev "$iface" handle ffff: ingress
    tc filter add dev "$iface" parent ffff: protocol all u32 match u32 0 0 \
        action mirred egress redirect dev "$IFB_DEV"

    if [ "$NODE_ROLE" = proxy ] && [ "${#INTER_IPS[@]}" -gt 0 ]; then
        add_ip_filters "$iface" "1:0" dst 1:10 1 "${INTER_IPS[@]}"
        add_ip_filters "$IFB_DEV" "2:0" src 2:10 1 "${INTER_IPS[@]}"
    fi

    if [ "$limit_datanode" -eq 1 ] && [ "${#INTRA_IPS[@]}" -gt 0 ]; then
        add_ip_filters "$iface" "1:0" dst 1:20 2 "${INTRA_IPS[@]}"
        add_ip_filters "$IFB_DEV" "2:0" src 2:20 2 "${INTRA_IPS[@]}"
    fi

    if [ "$NODE_ROLE" = proxy ]; then
        if [ "$limit_datanode" -eq 1 ]; then
            echo "ok | proxy | $iface | inter=${inter_label}(${#INTER_IPS[@]}) intra=${intra_label}(${#INTRA_IPS[@]})"
        else
            echo "ok | proxy | $iface | inter=${inter_label}(${#INTER_IPS[@]}) intra=unlimited"
        fi
    else
        echo "ok | datanode | $iface | intra=${intra_label} -> ${INTRA_IPS[*]}"
    fi
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
    if [ "$LIMIT_DATANODE" -eq 1 ]; then
        extra="intra"
        mode_label="intra-rack=10Gb"
    else
        mode_label="datanode=unlimited"
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
