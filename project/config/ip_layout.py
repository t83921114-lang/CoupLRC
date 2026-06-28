#!/usr/bin/env python3
"""
从 all_ips 读取全部 IP，排序后分配角色：
  最小的 client_num 个 IP -> client（多机 client 测试）
  下一个 IP -> coordinator
  剩余按 cluster.ini 分组：每组 1 proxy + datanode_per_cluster datanode

期望 IP 数 = client_num + 1 + cluster_num * (1 + datanode_per_cluster)
"""
from __future__ import annotations

import argparse
import configparser
import ipaddress
import sys
from dataclasses import dataclass
from pathlib import Path

CONFIG_DIR = Path(__file__).resolve().parent
DEFAULT_INI = CONFIG_DIR / "cluster.ini"


@dataclass
class ClusterLayout:
    client_ip: str
    client_ips: list[str]
    coordinator_ip: str
    first_proxy_ip: str
    clusters: list[dict]
    all_hosts: list[str]
    proxy_hosts: list[str]
    node_ips: list[str]


def load_ini(ini_path: Path) -> configparser.ConfigParser:
    cfg = configparser.ConfigParser()
    if not ini_path.exists():
        print(f"Error: {ini_path} not found", file=sys.stderr)
        sys.exit(1)
    cfg.read(ini_path, encoding="utf-8")
    return cfg


def repo_root_from_ini(ini_path: Path) -> Path:
    return ini_path.resolve().parent.parent.parent


def resolve_all_ips_path(cfg: configparser.ConfigParser, repo_root: Path) -> Path:
    rel = cfg["cluster"].get("all_ips_file", "all_ips").strip()
    path = Path(rel) if Path(rel).is_absolute() else repo_root / rel
    if not path.exists():
        print(f"Error: all_ips not found: {path}", file=sys.stderr)
        sys.exit(1)
    return path


def load_all_ips(path: Path) -> list[str]:
    ips: list[str] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        try:
            ipaddress.ip_address(line)
        except ValueError:
            print(f"Error: invalid IP in {path}: {line}", file=sys.stderr)
            sys.exit(1)
        ips.append(line)
    return ips


def sort_ips(ips: list[str]) -> list[str]:
    return sorted(ips, key=lambda x: ipaddress.ip_address(x))


def allocate_all_ips(
    raw_ips: list[str],
    cluster_num: int,
    dn_per: int,
    client_num: int,
    first_port: int,
    dn_start: int,
) -> ClusterLayout:
    group_size = 1 + dn_per
    expected = client_num + 1 + cluster_num * group_size
    if len(raw_ips) != expected:
        print(
            f"Error: all_ips count {len(raw_ips)} != expected {expected} "
            f"({client_num} client + 1 coordinator + {cluster_num} * (1 + {dn_per}))",
            file=sys.stderr,
        )
        sys.exit(1)

    sorted_ips = sort_ips(raw_ips)
    client_ips = sorted_ips[:client_num]
    client_ip = client_ips[0]
    coordinator_ip = sorted_ips[client_num]
    node_ips = sorted_ips[client_num + 1 :]

    clusters: list[dict] = []
    proxy_hosts: list[str] = []
    idx = 0
    for c in range(cluster_num):
        proxy_ip = node_ips[idx]
        idx += 1
        proxy_hosts.append(proxy_ip)
        proxy_port = first_port + c
        datanodes: list[str] = []
        for d in range(dn_per):
            datanodes.append(f"{node_ips[idx]}:{dn_start + c * dn_per + d}")
            idx += 1
        clusters.append({"proxy": f"{proxy_ip}:{proxy_port}", "datanodes": datanodes})

    return ClusterLayout(
        client_ip=client_ip,
        client_ips=client_ips,
        coordinator_ip=coordinator_ip,
        first_proxy_ip=proxy_hosts[0],
        clusters=clusters,
        all_hosts=sorted_ips,
        proxy_hosts=proxy_hosts,
        node_ips=node_ips,
    )


def compute_distributed_layout(cfg: configparser.ConfigParser) -> ClusterLayout:
    sect = cfg["cluster"]
    cluster_num = int(sect["cluster_num"])
    dn_per = int(sect["datanode_per_cluster"])
    first_ip = sect["first_proxy_ip"].strip()
    first_port = int(sect["first_proxy_port"])
    dn_start = int(sect["datanode_port_start"])
    coordinator_ip = sect.get("coordinator_ip", "0.0.0.0").strip()
    client_ip = sect.get("client_ip", "127.0.0.1").strip()

    if first_ip == "127.0.0.1":
        prefix, first_octet = "", 0
        use_localhost = True
    else:
        parts = first_ip.rsplit(".", 1)
        prefix = parts[0] + "."
        first_octet = int(parts[1])
        use_localhost = False

    clusters: list[dict] = []
    proxy_hosts: list[str] = []
    node_ips: list[str] = []
    ip_idx = 0
    for c in range(cluster_num):
        if use_localhost:
            proxy_ip = "127.0.0.1"
        else:
            proxy_ip = f"{prefix}{first_octet + ip_idx}"
            ip_idx += 1
        proxy_hosts.append(proxy_ip)
        node_ips.append(proxy_ip)
        datanodes: list[str] = []
        for d in range(dn_per):
            if use_localhost:
                dn_ip = "127.0.0.1"
            else:
                dn_ip = f"{prefix}{first_octet + ip_idx}"
                ip_idx += 1
                node_ips.append(dn_ip)
            datanodes.append(f"{dn_ip}:{dn_start + c * dn_per + d}")
        clusters.append({"proxy": f"{proxy_ip}:{first_port + c}", "datanodes": datanodes})

    all_hosts = sort_ips([client_ip, coordinator_ip] + list(dict.fromkeys(node_ips)))
    return ClusterLayout(
        client_ip=client_ip,
        client_ips=[client_ip],
        coordinator_ip=coordinator_ip,
        first_proxy_ip=proxy_hosts[0],
        clusters=clusters,
        all_hosts=all_hosts,
        proxy_hosts=proxy_hosts,
        node_ips=node_ips,
    )


def compute_layout(cfg: configparser.ConfigParser, ini_path: Path) -> ClusterLayout:
    ip_mode = cfg["cluster"].get("ip_mode", "distributed").strip()
    if ip_mode == "all_ips":
        repo_root = repo_root_from_ini(ini_path)
        all_ips_path = resolve_all_ips_path(cfg, repo_root)
        raw_ips = load_all_ips(all_ips_path)
        return allocate_all_ips(
            raw_ips,
            int(cfg["cluster"]["cluster_num"]),
            int(cfg["cluster"]["datanode_per_cluster"]),
            int(cfg["cluster"].get("client_num", "1")),
            int(cfg["cluster"]["first_proxy_port"]),
            int(cfg["cluster"]["datanode_port_start"]),
        )
    if ip_mode == "hosts_list":
        print(
            "Error: ip_mode=hosts_list is removed; use all_ips and project/config/all_ips",
            file=sys.stderr,
        )
        sys.exit(1)
    return compute_distributed_layout(cfg)


def write_lines(path: Path, lines: list[str], header: str | None = None) -> None:
    body = []
    if header:
        body.append(f"# {header}")
    body.extend(lines)
    path.write_text("\n".join(body) + "\n", encoding="utf-8")


def main_cli() -> None:
    parser = argparse.ArgumentParser(description="Resolve cluster IP layout from all_ips")
    parser.add_argument("--ini", type=Path, default=DEFAULT_INI, help="cluster.ini path")
    parser.add_argument(
        "--format",
        choices=[
            "node-ips",
            "proxy-ips",
            "client-ip",
            "client-ips",
            "coordinator-ip",
            "all-hosts",
        ],
        required=True,
    )
    args = parser.parse_args()
    cfg = load_ini(args.ini)
    layout = compute_layout(cfg, args.ini)

    if args.format == "node-ips":
        for ip in layout.node_ips:
            print(ip)
    elif args.format == "proxy-ips":
        for ip in layout.proxy_hosts:
            print(ip)
    elif args.format == "client-ip":
        print(layout.client_ip)
    elif args.format == "client-ips":
        for ip in layout.client_ips:
            print(ip)
    elif args.format == "coordinator-ip":
        print(layout.coordinator_ip)
    elif args.format == "all-hosts":
        for ip in layout.all_hosts:
            print(ip)


if __name__ == "__main__":
    main_cli()
