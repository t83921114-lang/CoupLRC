#!/usr/bin/env python3
"""
根据 project/config/cluster.ini + all_ips 生成/更新集群配置。

all_ips 模式（ip_mode=all_ips）：
  1. 读取 all_ips（顺序任意）
  2. 排序后：最小->client，次小->coordinator，其余按 cluster 分组 proxy/datanode
  3. 写入 clusterInformation.xml、parameterConfiguration.xml、cluster.ini
  4. 写入仓库根目录 hosts、proxy_hosts
  5. 同步 main_client.cpp 中的 client_ip 硬编码
"""
import configparser
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

from ip_layout import compute_layout, load_ini, repo_root_from_ini, write_lines

CONFIG_DIR = Path(__file__).resolve().parent
INI_PATH = CONFIG_DIR / "cluster.ini"
CLUSTER_XML_PATH = CONFIG_DIR / "clusterInformation.xml"
PARAM_XML_PATH = CONFIG_DIR / "parameterConfiguration.xml"
MAIN_CLIENT_CPP = CONFIG_DIR.parent / "run_cpp" / "main_client.cpp"


def write_cluster_information_xml(clusters: list[dict]) -> None:
    root = ET.Element("clusters")
    root.text = "\n\t"
    for i, cl in enumerate(clusters):
        cluster_el = ET.SubElement(root, "cluster", id=str(i), proxy=cl["proxy"])
        cluster_el.text = "\n\t\t"
        datanodes_el = ET.SubElement(cluster_el, "datanodes")
        datanodes_el.text = "\n\t\t\t"
        for j, uri in enumerate(cl["datanodes"]):
            dn = ET.SubElement(datanodes_el, "datanode", uri=uri)
            dn.tail = "\n\t\t\t" if j < len(cl["datanodes"]) - 1 else "\n\t\t"
        datanodes_el.tail = "\n\t" if i < len(clusters) - 1 else "\n"
        cluster_el.tail = "\n\t" if i < len(clusters) - 1 else "\n"
    ET.ElementTree(root).write(
        CLUSTER_XML_PATH,
        encoding="utf-8",
        xml_declaration=True,
        default_namespace="",
        method="xml",
    )
    print(f"Written {CLUSTER_XML_PATH}")


def update_parameter_configuration_xml(
    cluster_num: int, datanode_per_cluster: int, coordinator_ip: str
) -> None:
    if not PARAM_XML_PATH.exists():
        print(f"Warning: {PARAM_XML_PATH} not found, skip update", file=sys.stderr)
        return
    text = PARAM_XML_PATH.read_text(encoding="utf-8")
    text = re.sub(
        r"(<ClusterNum>)[^<]+(</ClusterNum>)",
        rf"\g<1>{cluster_num}\g<2>",
        text,
        count=1,
    )
    text = re.sub(
        r"(<DatanodeNumPerCluster>)[^<]+(</DatanodeNumPerCluster>)",
        rf"\g<1>{datanode_per_cluster}\g<2>",
        text,
        count=1,
    )
    text = re.sub(
        r"(<CoordinatorIP>)[^<]+(</CoordinatorIP>)",
        rf"\g<1>{coordinator_ip}\g<2>",
        text,
        count=1,
    )
    PARAM_XML_PATH.write_text(text, encoding="utf-8")
    print(
        f"Updated {PARAM_XML_PATH} (ClusterNum, DatanodeNumPerCluster, CoordinatorIP)"
    )


def update_cluster_ini(client_ip: str, coordinator_ip: str, first_proxy_ip: str) -> None:
    text = INI_PATH.read_text(encoding="utf-8")
    text = re.sub(
        r"(^client_ip\s*=\s*).*$",
        rf"\g<1>{client_ip}",
        text,
        count=1,
        flags=re.MULTILINE,
    )
    text = re.sub(
        r"(^coordinator_ip\s*=\s*).*$",
        rf"\g<1>{coordinator_ip}",
        text,
        count=1,
        flags=re.MULTILINE,
    )
    text = re.sub(
        r"(^first_proxy_ip\s*=\s*).*$",
        rf"\g<1>{first_proxy_ip}",
        text,
        count=1,
        flags=re.MULTILINE,
    )
    INI_PATH.write_text(text, encoding="utf-8")
    print(f"Updated {INI_PATH} (client_ip, coordinator_ip, first_proxy_ip)")


def update_main_client_cpp(client_ip: str) -> None:
    if not MAIN_CLIENT_CPP.exists():
        print(f"Warning: {MAIN_CLIENT_CPP} not found, skip update", file=sys.stderr)
        return
    text = MAIN_CLIENT_CPP.read_text(encoding="utf-8")
    new_text, n = re.subn(
        r'(std::string client_ip = ")[^"]+(";)',
        rf"\g<1>{client_ip}\g<2>",
        text,
        count=1,
    )
    if n != 1:
        print(f"Warning: failed to update client_ip in {MAIN_CLIENT_CPP}", file=sys.stderr)
        return
    MAIN_CLIENT_CPP.write_text(new_text, encoding="utf-8")
    print(f"Updated {MAIN_CLIENT_CPP} (client_ip={client_ip})")


def main() -> None:
    cfg = load_ini(INI_PATH)
    layout = compute_layout(cfg, INI_PATH)
    repo_root = repo_root_from_ini(INI_PATH)

    cluster_num = int(cfg["cluster"]["cluster_num"])
    dn_per = int(cfg["cluster"]["datanode_per_cluster"])

    write_cluster_information_xml(layout.clusters)
    update_parameter_configuration_xml(cluster_num, dn_per, layout.coordinator_ip)
    update_cluster_ini(layout.client_ip, layout.coordinator_ip, layout.first_proxy_ip)

    write_lines(
        repo_root / "hosts",
        layout.all_hosts,
        header=(
            f"{len(layout.all_hosts)} nodes: client, coordinator, proxy, datanode "
            "(auto-generated from all_ips)"
        ),
    )
    write_lines(
        repo_root / "proxy_hosts",
        layout.proxy_hosts,
        header=f"{len(layout.proxy_hosts)} proxy nodes (auto-generated from all_ips)",
    )
    print(f"Written {repo_root / 'hosts'}")
    print(f"Written {repo_root / 'proxy_hosts'}")

    update_main_client_cpp(layout.client_ip)

    print("\nRole assignment (sorted):")
    print(f"  client       -> {layout.client_ip}")
    print(f"  coordinator  -> {layout.coordinator_ip}")
    print(f"  clusters     -> {cluster_num} x (1 proxy + {dn_per} datanode)")


if __name__ == "__main__":
    main()
