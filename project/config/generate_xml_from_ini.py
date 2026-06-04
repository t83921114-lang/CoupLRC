#!/usr/bin/env python3
"""
根据 project/config/cluster.ini 生成 clusterInformation.xml，并更新 parameterConfiguration.xml
中的 ClusterNum、DatanodeNumPerCluster（及可选 CoordinatorIP）。
与 run_all_remote.sh 共用同一份 INI。
"""
import configparser
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

CONFIG_DIR = Path(__file__).resolve().parent
INI_PATH = CONFIG_DIR / "cluster.ini"
CLUSTER_XML_PATH = CONFIG_DIR / "clusterInformation.xml"
PARAM_XML_PATH = CONFIG_DIR / "parameterConfiguration.xml"


def load_ini():
    cfg = configparser.ConfigParser()
    if not INI_PATH.exists():
        print(f"Error: {INI_PATH} not found", file=sys.stderr)
        sys.exit(1)
    cfg.read(INI_PATH, encoding="utf-8")
    return cfg


def compute_cluster_info(cfg):
    sect = cfg["cluster"]
    n = int(sect["cluster_num"])
    dn_per = int(sect["datanode_per_cluster"])
    first_ip = sect["first_proxy_ip"].strip()
    first_port = int(sect["first_proxy_port"])
    dn_start = int(sect["datanode_port_start"])
    use_localhost = first_ip == "127.0.0.1"
    if use_localhost:
        prefix, first_octet = "", 0
    else:
        parts = first_ip.rsplit(".", 1)
        prefix = parts[0] + "."
        first_octet = int(parts[1])
    clusters = []
    # IP allocation:
    # - If first_proxy_ip is 127.0.0.1, keep all IPs as 127.0.0.1 (option B).
    # - Otherwise, allocate unique IPs globally starting from first_proxy_ip:
    #   proxy + all datanodes across all clusters each get a distinct IP by incrementing the last octet.
    ip_idx = 0
    for c in range(n):
        proxy_ip = "127.0.0.1" if use_localhost else f"{prefix}{first_octet + ip_idx}"
        if not use_localhost:
            ip_idx += 1
        proxy_port = first_port + c
        datanodes = []
        for d in range(dn_per):
            dn_ip = "127.0.0.1" if use_localhost else f"{prefix}{first_octet + ip_idx}"
            if not use_localhost:
                ip_idx += 1
            datanodes.append(f"{dn_ip}:{dn_start + c * dn_per + d}")
        clusters.append({"proxy": f"{proxy_ip}:{proxy_port}", "datanodes": datanodes})
    return clusters, n, dn_per, sect.get("coordinator_ip", "0.0.0.0").strip()


def write_cluster_information_xml(clusters):
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
    tree = ET.ElementTree(root)
    tree.write(
        CLUSTER_XML_PATH,
        encoding="utf-8",
        xml_declaration=True,
        default_namespace="",
        method="xml",
    )
    print(f"Written {CLUSTER_XML_PATH}")


def update_parameter_configuration_xml(cluster_num, datanode_per_cluster, coordinator_ip):
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
    print(f"Updated {PARAM_XML_PATH} (ClusterNum, DatanodeNumPerCluster, CoordinatorIP)")


def main():
    cfg = load_ini()
    clusters, cluster_num, dn_per, coordinator_ip = compute_cluster_info(cfg)
    write_cluster_information_xml(clusters)
    update_parameter_configuration_xml(cluster_num, dn_per, coordinator_ip)


if __name__ == "__main__":
    main()