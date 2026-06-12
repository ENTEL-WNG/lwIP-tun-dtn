#!/usr/bin/env python3
"""
gen_node_configs.py - Generate per-node TOML config files from a contact-plan JSON.

Usage:
    python3 generate_network.py [--network DIR]

If no argument is given, defaults to 'contact_plan_throughput'.

Address scheme (IPv6 ULA):
    For an edge between node A and node B (lo = min(A,B), hi = max(A,B)):
        fd00:<lo_hex>:<hi_hex>::<node_id>

    Example  node2 <-> node3:
        node2  ->  fd00:02:03::2
        node3  ->  fd00:02:03::3

MAC address scheme:
    For an edge between node A and node B (lo < hi):
        00:<lo_hex>:00:<hi_hex>:00:<node_id_hex>

    Example  node1 <-> node2:
        node1  ->  00:01:00:02:00:01
        node2  ->  00:01:00:02:00:02

Per-interface reachable addresses (directional BFS excluding the local node):
    dtn_addresses  - all IPv6 addrs of reachable nodes (isDtnNode == true)
                     in the direction of this interface, plus this interface's
                     own local_addr if the local node is a DTN node
    addresses      - same but for isDtnNode == false nodes
"""

import argparse
import sys
import tomllib
from collections import deque
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import networkx as nx

# ---------------------------------------------------------------------------
# Address helpers
# ---------------------------------------------------------------------------

def node_hex(n: int) -> str:
    return f"{n:02x}"


def link_ipv6(a: int, b: int) -> tuple:
    """Return (addr_a, addr_b) /64 IPv6 addresses for the a<->b link.
    Last octet is the node's own ID: fd00:<lo>:<hi>::<node_id>
    """
    lo, hi = min(a, b), max(a, b)
    prefix = f"fd00:{node_hex(lo)}:{node_hex(hi)}"
    return f"{prefix}::{a:x}/64", f"{prefix}::{b:x}/64"


def link_mac(a: int, b: int) -> tuple:
    """Return (mac_a, mac_b) MAC addresses for the a<->b link.
    Last byte is the node's own ID: 00:<lo>:00:<hi>:00:<node_id>
    """
    lo, hi = min(a, b), max(a, b)
    lo_h, hi_h = node_hex(lo), node_hex(hi)
    mac_a = f"00:{lo_h}:00:{hi_h}:00:{a:02x}"
    mac_b = f"00:{lo_h}:00:{hi_h}:00:{b:02x}"
    return mac_a, mac_b


def strip_prefix(addr: str) -> str:
    return addr.split("/")[0]


def iface_name(local: int, remote: int) -> str:
    return f"node{local}-node{remote}"


# ---------------------------------------------------------------------------
# Graph helpers
# ---------------------------------------------------------------------------

def build_adjacency(edges: list) -> dict:
    adj = {}
    for edge in edges:
        a, b = edge["from"], edge["to"]
        adj.setdefault(a, set()).add(b)
        adj.setdefault(b, set()).add(a)
    return adj


def reachable_nodes(start_in_sec: int, adj: dict) -> set:
    """BFS from start_in_sec over the full graph (used for node-level reachability)."""
    visited = {start_in_sec}
    queue = deque([start_in_sec])
    while queue:
        node = queue.popleft()
        for nbr in adj.get(node, []):
            if nbr not in visited:
                visited.add(nbr)
                queue.append(nbr)
    return visited


def reachable_via(first_hop: int, exclude: int, adj: dict) -> set:
    """BFS starting at first_hop, never stepping back through exclude.
    Returns all reachable nodes including first_hop itself."""
    visited = {exclude, first_hop}
    queue = deque([first_hop])
    while queue:
        node = queue.popleft()
        for nbr in adj.get(node, []):
            if nbr not in visited:
                visited.add(nbr)
                queue.append(nbr)
    visited.discard(exclude)
    return visited


# ---------------------------------------------------------------------------
# Core builder
# ---------------------------------------------------------------------------

def build_node_data(data: dict) -> dict:
    defaults = data.get("contact_plan.defaults", {})
    default_rate = defaults.get("rate_in_bits_per_sec", 1)
    default_range = defaults.get("range", 1)

    # Node metadata lookup
    node_meta = {}
    for n in data.get("nodes", []):
        node_meta[n["id"]] = {
            "name":   n.get("name"),
            "is_dtn": n.get("isDtnNode", False),
        }

    edges = data.get("edges", [])
    adj = build_adjacency(edges)

    all_ids = set(node_meta.keys())
    for e in edges:
        all_ids.add(e["from"])
        all_ids.add(e["to"])

    # node_addrs[node_id] -> list of bare IPv6 addresses that belong to this node
    node_addrs: dict[int, list[str]] = {nid: [] for nid in all_ids}
    # Temporary storage for interface dicts before directional BFS is run
    iface_map: dict[int, list] = {nid: [] for nid in all_ids}

    for edge in edges:
        a = edge["from"]
        b = edge["to"]

        addr_a, addr_b = link_ipv6(a, b)
        mac_a,  mac_b  = link_mac(a, b)

        if strip_prefix(addr_a) not in node_addrs[a]:
            node_addrs[a].append(strip_prefix(addr_a))
        if strip_prefix(addr_b) not in node_addrs[b]:
            node_addrs[b].append(strip_prefix(addr_b))

        for local, remote, local_addr, remote_addr, local_mac, remote_mac in [
            (a, b, addr_a, addr_b, mac_a, mac_b),
            (b, a, addr_b, addr_a, mac_b, mac_a),
        ]:
            if any(i["iface"] == iface_name(local, remote) for i in iface_map[local]):
                continue
            iface_map[local].append({
                "iface":          iface_name(local, remote),
                "local_addr":     local_addr,
                "local_mac":      local_mac,
                "remote_addr":    remote_addr,
                "remote_mac":     remote_mac,
                "remote_node_id": remote,
            })

    # Now attach per-interface directional reachable address lists
    for nid in all_ids:
        local_meta = node_meta.get(nid, {"is_dtn": False})
        for iface in iface_map[nid]:
            remote = iface["remote_node_id"]

            # All nodes reachable by going through this interface (excludes local node)
            direction_nodes = reachable_via(remote, exclude=nid, adj=adj)

            dtn_addrs   = []
            plain_addrs = []

            # Own local_addr on this interface (so the node can ping itself)
            own_addr = strip_prefix(iface["local_addr"])
            if local_meta["is_dtn"]:
                dtn_addrs.append(own_addr)
            else:
                plain_addrs.append(own_addr)

            # All addresses of nodes reachable in this direction
            for rid in sorted(direction_nodes):
                meta = node_meta.get(rid, {"is_dtn": False})
                for addr in node_addrs.get(rid, []):
                    if meta["is_dtn"]:
                        dtn_addrs.append(addr)
                    else:
                        plain_addrs.append(addr)

            iface["dtn_addresses"] = dtn_addrs
            iface["addresses"]     = plain_addrs

    # Build node-level reachable address lists (union across all directions + self)
    result = {}
    for nid in sorted(all_ids):
        reachable = reachable_nodes(nid, adj)

        dtn_addrs   = []
        plain_addrs = []
        for rid in sorted(reachable):
            meta = node_meta.get(rid, {"is_dtn": False})
            for addr in node_addrs.get(rid, []):
                if meta["is_dtn"]:
                    dtn_addrs.append(addr)
                else:
                    plain_addrs.append(addr)

        meta = node_meta.get(nid, {"name": None, "is_dtn": False})
        nid_hex = node_hex(nid)
        name = meta["name"] if meta["name"] is not None else f"node{nid}"
        result[nid] = {
            "id":                  nid,
            "name":                name,
            "is_dtn":              meta["is_dtn"],
            "interfaces":          iface_map[nid],
            "dtn_addresses":       dtn_addrs,
            "addresses":           plain_addrs,
            "tun_ipv6_addr":  f"fd00:ffff:{nid_hex}::1/64",
            "lwip_ipv6_addr": f"fd00:ffff:{nid_hex}::{nid + 1:x}/64",
        }

    return result


# ---------------------------------------------------------------------------
# TOML renderer
# ---------------------------------------------------------------------------

def toml_str_list(items: list) -> str:
    quoted = ", ".join(f'"{v}"' for v in items)
    return f"[{quoted}]"


def render_toml(node: dict, contact_plan_text: str) -> str:
    lines = []
    nid = node["id"]
    lines.append(f"# Auto-generated config for node {nid}")
    lines.append("")
    lines.append("[node]")
    lines.append(f"id             = {nid}")
    lines.append(f'name           = "{node["name"]}"')
    lines.append(f"is_dtn         = {str(node['is_dtn']).lower()}")
    lines.append(f"dtn_addresses  = {toml_str_list(node['dtn_addresses'])}")
    lines.append(f"addresses      = {toml_str_list(node['addresses'])}")
    lines.append(f'tun_ipv6_addr  = "{node["tun_ipv6_addr"]}"')
    lines.append(f'lwip_ipv6_addr = "{node["lwip_ipv6_addr"]}"')
    lines.append("")

    for iface in node["interfaces"]:
        lines.append("[[interface]]")
        lines.append(f'name           = "{iface["iface"]}"')
        lines.append(f'local_addr     = "{iface["local_addr"]}"')
        lines.append(f'local_mac      = "{iface["local_mac"]}"')
        lines.append(f'remote_addr    = "{iface["remote_addr"]}"')
        lines.append(f'remote_mac     = "{iface["remote_mac"]}"')
        lines.append(f"remote_node_id = {iface['remote_node_id']}")
        lines.append(f"dtn_addresses  = {toml_str_list(iface['dtn_addresses'])}")
        lines.append(f"addresses      = {toml_str_list(iface['addresses'])}")
        lines.append("")

    # Indenting the original TOML ensures it stays valid under the new key
    for line in contact_plan_text.splitlines():
        lines.append(f"{line}")

    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Docker Compose generator
# ---------------------------------------------------------------------------

def generate_compose(node_data: dict, config_dir: Path, out_path: Path, cpus: str = "1.0", memory: str = "", capture_interval: int = 30) -> None:
    """
    Write a docker-compose.yml that models the contact-plan topology.

    Per-service rules:
      - All nodes use Dockerfile.node; the TOML's is_dtn flag decides
        whether init_node.py launches lwip_tun or sleeps.
      - The **repo root** is bind-mounted read-write at /repo inside every
        container, so any file edit on the host is immediately visible
        without rebuilding or restarting (see "hot-reload" note below).
      - CONFIG_PATH points to the per-node TOML inside /repo.
      - Every pair of nodes that share a link is placed on a dedicated
        Docker network named  net-nodeA-nodeB  (lo < hi).
      - Each service's network entry carries the MAC address derived from
        the contact-plan addressing scheme (local_mac for that interface).

    Hot-reload workflow:
      Edit any file on the host, then re-run setup inside the container:
        docker exec -it nodeN python3 /repo/init_node.py /repo/<network>/nodeN.toml
      For DTN nodes this will re-apply sysctl/ip6tables/routing rules and
      re-exec lwip_tun.  For regular nodes it just re-applies routes.
    """
    import yaml  # requires PyYAML

    # The compose file sits at  networks/<plan>/docker-compose.yml
    # We need to express the repo root as a relative path from that location,
    # which is two levels up: ../../
    repo_root_host = "../.."
    repo_root_container = "/repo"

    # Collect unique link pairs to build top-level network definitions
    edges_by_pair: dict[tuple, bool] = {}
    for nid, node in node_data.items():
        for iface in node["interfaces"]:
            remote = iface["remote_node_id"]
            pair = (min(nid, remote), max(nid, remote))
            edges_by_pair[pair] = True

    networks: dict[str, dict] = {}
    for lo, hi in sorted(edges_by_pair):
        net_name = f"net-node{lo}-node{hi}"
        networks[net_name] = {"driver": "bridge", "enable_ipv6": True}

    # The contact-plan sub-directory name (last component of config_dir)
    plan_subdir = config_dir.name   # e.g. "contact_plan_1"

    # TEST_CASE_NUMBER is baked into every service. Edit docker-compose.yml by
    # hand to change it before `docker compose up`.
    test_case_number = 0

    services: dict[str, dict] = {}
    for nid in sorted(node_data):
        node = node_data[nid]
        svc_name = f"node{nid}"

        # Path to this node's TOML inside the container (via the repo mount)
        container_config = f"{repo_root_container}/networks/{plan_subdir}/{svc_name}.toml"

        # Build per-network entries, attaching the correct MAC address
        svc_networks: dict[str, dict] = {}
        for iface in node["interfaces"]:
            remote   = iface["remote_node_id"]
            net_name = f"net-node{min(nid, remote)}-node{max(nid, remote)}"
            svc_networks[net_name] = {
                "mac_address": iface["local_mac"],
            }

        service: dict = {
            "build": {
                # Context is the repo root so the full source tree is available
                # to the Docker build (Makefile, source files, scripts, etc.)
                "context": repo_root_host,
                "dockerfile": "Dockerfile.node",
            },
            "container_name": svc_name,
            "privileged": True,
            "hostname": node["name"],
            "environment": {
                "DTN_CONFIG_PATH": container_config,
                "TEST_CASE_NUMBER": "${TEST_CASE_NUMBER}",
            },
            # Mount the entire repo into the container so:
            #   1. The compiled lwip_tun binary is always up-to-date (or can be
            #      rebuilt with `docker exec nodeN make -C /repo`).
            #   2. Any config / script change on the host is instantly visible
            #      inside the container without a restart.
            "volumes": [
                f"{repo_root_host}:{repo_root_container}"
            ],
            "working_dir": repo_root_container,
            "cap_add": ["NET_ADMIN"],
            # init_node.py reads CONFIG_PATH and either execs lwip_tun (DTN)
            # or keeps the container alive (regular node).
            "entrypoint": [
                "python3",
                f"{repo_root_container}/init_node.py",
                container_config,
            ],
        }

        if svc_networks:
            service["networks"] = svc_networks

        limits: dict = {"cpus": cpus}
        if memory:
            limits["memory"] = memory
        service["deploy"] = {"resources": {"limits": limits}}

        services[svc_name] = service

    # Capture service — runs in host network namespace (network_mode: host) so
    # tcpdump can capture directly on the Docker bridge interfaces (br-xxxx).
    # Capturing on a bridge interface sees ALL frames including unicast between
    # containers, unlike a container veth where the bridge FDB only forwards
    # frames addressed to that port.
    #
    # The capture logic lives in a generated shell script (capture.sh) so there
    # are no Compose/shell escaping issues.  TEST_CASE_NUMBER is read from the
    # environment variable set by the container.
    #
    # Produces two files per run:
    #   capture.pcap  — binary pcap  (-U: unbuffered writes)
    #   capture.txt   — human-readable (-e: iface+link header, -tttt: full timestamp)
    # Live Wireshark: docker exec capture tcpdump -i any ip6 -nn -U -w - | wireshark -k -i -
    script_container_path = f"{repo_root_container}/networks/capture.sh"

    services["capture"] = {
        "image": "nicolaka/netshoot",
        "container_name": "capture",
        "network_mode": "host",
        "entrypoint": ["sh", script_container_path],
        "privileged": True,
        "environment": {
            "TEST_CASE_NUMBER": "${TEST_CASE_NUMBER}",
            "PLAN_NAME": plan_subdir,
        },
        "volumes": [f"{repo_root_host}:{repo_root_container}"],
    }

    services["metrics"] = {
        "build": {
            "context": repo_root_host,
            "dockerfile": "Dockerfile.metrics",
        },
        "container_name": "metrics",
        "environment": {
            "PLAN_NAME":        plan_subdir,
            "TEST_CASE_NUMBER": "${TEST_CASE_NUMBER}",
            "METRICS_OUT":      f"{repo_root_container}/networks/{plan_subdir}/captures/${{TEST_CASE_NUMBER}}/metrics.jsonl",
            "CAPTURE_INTERVAL": str(capture_interval),
        },
        "volumes": [
            "/var/run/docker.sock:/var/run/docker.sock:ro",
            f"{repo_root_host}:{repo_root_container}",
        ],
        "command": ["python3", f"{repo_root_container}/networks/get_metrics.py"],
    }

    compose = {
        "services": services,
        "networks": networks,
    }

    out_path.write_text(yaml.dump(compose, default_flow_style=False, sort_keys=False, width=float("inf")))
    print(f"  wrote {out_path}")

    env_path = out_path.parent / ".env"
    if not env_path.exists():
        env_path.write_text(f"TEST_CASE_NUMBER={test_case_number}\n")
        print(f"  wrote {env_path}")
    else:
        print(f"  skipped {env_path} (already exists)")


# ---------------------------------------------------------------------------
# Graph visualisation
# ---------------------------------------------------------------------------

def generate_graph(data: dict, node_data: dict, out_path: Path) -> None:
    """Render the contact-plan topology as a PNG with interface labels."""

    node_meta = {n["id"]: n for n in data.get("nodes", [])}
    edges     = data.get("edges", [])

    G = nx.MultiGraph()
    for nid in node_data:
        G.add_node(nid)
    for edge in edges:
        G.add_edge(edge["from"], edge["to"],
                   start_in_sec=edge.get("start_in_sec"), end_in_sec=edge.get("end_in_sec"))

    pos = nx.spring_layout(G, seed=42, k=2.5)

    fig, ax = plt.subplots(figsize=(16, 10))
    ax.set_facecolor("#f8f9fa")
    fig.patch.set_facecolor("#f8f9fa")

    # Nodes
    dtn_nodes   = [n for n in G.nodes if node_meta.get(n, {}).get("isDtnNode", False)]
    plain_nodes = [n for n in G.nodes if n not in dtn_nodes]

    nx.draw_networkx_nodes(G, pos, nodelist=dtn_nodes,   node_color="#4a90d9",
                           node_size=1800, ax=ax)
    nx.draw_networkx_nodes(G, pos, nodelist=plain_nodes, node_color="#e07b39",
                           node_size=1800, ax=ax)

    # Node labels  (id + optional name)
    node_labels = {}
    for nid in G.nodes:
        meta = node_meta.get(nid, {})
        label = f"Node {nid}"
        if meta.get("name"):
            label += f"\n{meta['name']}"
        node_labels[nid] = label
    nx.draw_networkx_labels(G, pos, labels=node_labels,
                            font_size=8, font_color="white", font_weight="bold", ax=ax)

    # Edges — draw each (u,v) pair once; offset parallel edges slightly
    drawn_pairs: dict[tuple, int] = {}
    for u, v, edata in G.edges(data=True):
        key = (min(u, v), max(u, v))
        drawn_pairs[key] = drawn_pairs.get(key, 0) + 1

    pair_count: dict[tuple, int] = {}
    for u, v, edata in G.edges(data=True):
        key = (min(u, v), max(u, v))
        idx = pair_count.get(key, 0)
        pair_count[key] = idx + 1
        total = drawn_pairs[key]
        # Arc rad: 0 for single edges, alternating ±0.25 for multiples
        if total == 1:
            rad = 0.0
        else:
            rad = 0.25 * (1 if idx % 2 == 0 else -1)

        nx.draw_networkx_edges(G, pos, edgelist=[(u, v)],
                               connectionstyle=f"arc3,rad={rad}",
                               edge_color="#555", width=1.8,
                               arrows=True, arrowsize=1, ax=ax)

    # Edge labels — one label per endpoint per edge, placed near each end_in_sec
    # We iterate edges and compute a point 30% along from each node
    ax.set_xlim([x for x in ax.get_xlim()])  # ensure transform is set

    for u, v, edata in G.edges(data=True):
        addr_u, addr_v = link_ipv6(u, v)
        mac_u,  mac_v  = link_mac(u, v)
        pu = pos[u]
        pv = pos[v]

        # Point 28% of the way from u toward v (and vice-versa)
        t = 0.28
        near_u = (pu[0] + t * (pv[0] - pu[0]), pu[1] + t * (pv[1] - pu[1]))
        near_v = (pv[0] + t * (pu[0] - pv[0]), pv[1] + t * (pu[1] - pv[1]))

        label_u = f"{strip_prefix(addr_u)}\n{mac_u}"
        label_v = f"{strip_prefix(addr_v)}\n{mac_v}"

        for pt, label in [(near_u, label_u), (near_v, label_v)]:
            ax.text(pt[0], pt[1], label,
                    fontsize=5.5, ha="center", va="center",
                    bbox=dict(boxstyle="round,pad=0.25", fc="white",
                              ec="#aaa", alpha=0.85),
                    zorder=5)

        # Contact window in the middle of the edge
        mid = ((pu[0] + pv[0]) / 2, (pu[1] + pv[1]) / 2)
        start_in_sec = edata.get("start_in_sec", "?")
        end_in_sec   = edata.get("end_in_sec",   "?")
        ax.text(mid[0], mid[1], f"t={start_in_sec}–{end_in_sec}",
                fontsize=6, ha="center", va="center", color="#333",
                bbox=dict(boxstyle="round,pad=0.2", fc="#ffffcc",
                          ec="#cccc00", alpha=0.9),
                zorder=6)

    # Legend
    legend_handles = [
        mpatches.Patch(color="#4a90d9", label="DTN node"),
        mpatches.Patch(color="#e07b39", label="Non-DTN node"),
    ]
    ax.legend(handles=legend_handles, loc="upper left", fontsize=9)

    ax.set_title("Contact Plan Topology", fontsize=14, fontweight="bold", pad=12)
    ax.axis("off")
    plt.tight_layout()
    plt.savefig(out_path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  wrote {out_path}")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    p = argparse.ArgumentParser(description="Generate per-node configs and docker-compose.yml")
    p.add_argument("plan", nargs="?",
                   default=str(Path(__file__).parent / "contact_plan_throughput" / "contact-plan.toml"),
                   metavar="CONTACT_PLAN",
                   help="path to contact-plan.toml (default: contact_plan_throughput/contact-plan.toml)")
    p.add_argument("--capture-interval", type=int, default=30, metavar="N",
                   help="log capture interval in seconds baked into docker-compose.yml (default: 30)")
    args = p.parse_args()

    plan_path = Path(args.plan)
    if not plan_path.is_absolute():
        plan_path = Path(__file__).parent / plan_path

    with open(plan_path, "rb") as f:
        data = tomllib.load(f)

    plan_name = data.get("contact_plan", {}).get("name", plan_path.parent.name)
    network_dir = Path(__file__).parent / plan_name

    contact_plan_raw = plan_path.read_text()
    node_data = build_node_data(data)

    out_dir = network_dir
    out_dir.mkdir(parents=True, exist_ok=True)

    for nid, node in node_data.items():
        toml = render_toml(node, contact_plan_raw)
        out_file = out_dir / f"node{nid}.toml"
        out_file.write_text(toml)
        print(f"  wrote {out_file}")

    generate_graph(data, node_data, out_dir / "topology.png")

    cpus   = str(data.get("contact_plan", {}).get("cpus",   "1.0"))
    memory = str(data.get("contact_plan", {}).get("memory", ""))
    generate_compose(node_data, out_dir, out_dir / "docker-compose.yml", cpus=cpus, memory=memory,
                     capture_interval=args.capture_interval)

    test_script = out_dir / "test.sh"
    if not test_script.exists():
        test_script.write_text('#!/bin/sh\necho "Tests not implemented"\n')
        test_script.chmod(0o755)
        print(f"  wrote {test_script}")
    else:
        print(f"  skipped {test_script} (already exists)")

    print(f"\nGenerated {len(node_data)} config(s) + docker-compose.yml in '{out_dir}/'")
    print(f"OUTPUT_DIR={out_dir}")


if __name__ == "__main__":
    main()