#!/usr/bin/env python3
"""
gen_node_configs.py - Generate per-node TOML config files from a contact-plan JSON.

Usage:
    python3 gen_node_configs.py [contact-plan.json]

If no argument is given, defaults to 'contact-plan.json' in the current directory.

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

import sys
import tomllib
import random
from collections import deque
from pathlib import Path

COLORS = [
    "Red", "Blue", "Green", "Yellow", "Purple", "Orange", "Pink", "Cyan",
    "Magenta", "Amber", "Crimson", "Indigo", "Violet", "Teal", "Scarlet",
    "Silver", "Golden", "Ivory", "Cobalt", "Jade",
]

ANIMALS = [
    "Fox", "Wolf", "Bear", "Eagle", "Hawk", "Falcon", "Tiger", "Lion",
    "Panther", "Lynx", "Otter", "Badger", "Raven", "Owl", "Shark",
    "Dolphin", "Bison", "Moose", "Puma", "Viper",
]


def random_name() -> str:
    return f"{random.choice(COLORS)}{random.choice(ANIMALS)}"

# import matplotlib
# matplotlib.use("Agg")
# import matplotlib.pyplot as plt
# import matplotlib.patches as mpatches
# import networkx as nx


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
    return f"{prefix}::{a}/64", f"{prefix}::{b}/64"


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
    default_rate = defaults.get("rate", 1)
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
        start_in_sec  = edge.get("start_in_sec")
        end_in_sec    = edge.get("end_in_sec")
        rate   = edge.get("rate", default_rate)
        range  = edge.get("range", default_range)

        addr_a, addr_b = link_ipv6(a, b)
        mac_a,  mac_b  = link_mac(a, b)

        node_addrs[a].append(strip_prefix(addr_a))
        node_addrs[b].append(strip_prefix(addr_b))

        for local, remote, local_addr, remote_addr, local_mac, neighbour_mac in [
            (a, b, addr_a, addr_b, mac_a, mac_b),
            (b, a, addr_b, addr_a, mac_b, mac_a),
        ]:
            iface_map[local].append({
                "iface":          iface_name(local, remote),
                "local_addr":     local_addr,
                "local_mac":      local_mac,
                "remote_addr":    remote_addr,
                "neighbour_mac":  neighbour_mac,
                "remote_node_id": remote,
                "start_in_sec":   start_in_sec,
                "end_in_sec":     end_in_sec,
                "rate":           rate,
                "range":          range,
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
        name = meta["name"] if meta["name"] is not None else random_name()
        result[nid] = {
            "id":                  nid,
            "name":                name,
            "is_dtn":              meta["is_dtn"],
            "interfaces":          iface_map[nid],
            "dtn_addresses":       dtn_addrs,
            "addresses":           plain_addrs,
            "tun_ipv6_addr":  f"fd00:ffff:{nid_hex}::1/64",
            "lwip_ipv6_addr": f"fd00:ffff:{nid_hex}::{nid_hex}/64",
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
    lines.append(f"id     = {nid}")
    lines.append(f'name   = "{node["name"]}"')
    lines.append(f"is_dtn        = {str(node['is_dtn']).lower()}")
    lines.append(f"dtn_addresses = {toml_str_list(node['dtn_addresses'])}")
    lines.append(f"addresses     = {toml_str_list(node['addresses'])}")
    lines.append(f'tun_ipv6_addr  = "{node["tun_ipv6_addr"]}"')
    lines.append(f'lwip_ipv6_addr = "{node["lwip_ipv6_addr"]}"')
    lines.append("")

    for iface in node["interfaces"]:
        lines.append("[[interface]]")
        lines.append(f'name           = "{iface["iface"]}"')
        lines.append(f'local_addr     = "{iface["local_addr"]}"')
        lines.append(f'local_mac      = "{iface["local_mac"]}"')
        lines.append(f'remote_addr    = "{iface["remote_addr"]}"')
        lines.append(f'neighbour_mac  = "{iface["neighbour_mac"]}"')
        lines.append(f"remote_node_id = {iface['remote_node_id']}")
        lines.append(f"start_in_sec   = {iface['start_in_sec']}")
        lines.append(f"end_in_sec     = {iface['end_in_sec']}")
        lines.append(f"rate           = {iface['rate']}")
        lines.append(f"range          = {iface['range']}")
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

def generate_compose(node_data: dict, config_dir: Path, out_path: Path) -> None:
    """
    Write a docker-compose.yml that models the contact-plan topology.

    Per-service rules:
      - DTN nodes  → build context uses Dockerfile.dtn
      - Plain nodes → build context uses Dockerfile.reg
      - Each service receives a CONFIG_PATH env-var pointing to its TOML
        config file inside the container (mounted from the host config dir).
      - Every pair of nodes that share a link is placed on a dedicated
        Docker network named  net-nodeA-nodeB  (lo < hi).
      - Each service's network entry carries the MAC address derived from
        the contact-plan addressing scheme (local_mac for that interface).
    """
    import yaml  # requires PyYAML

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

    services: dict[str, dict] = {}
    for nid in sorted(node_data):
        node = node_data[nid]
        svc_name   = f"node{nid}"
        is_dtn     = node["is_dtn"]
        dockerfile = "Dockerfile.dtn" if is_dtn else "Dockerfile.reg"

        # Config file path inside the container
        repo_config = f"/node-configs/{svc_name}.toml"
        container_config = f"/configs/{svc_name}.toml"

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
                "context": ".",
                "dockerfile": dockerfile,
            },
            "container_name": svc_name,
            "hostname": node["name"],
            "environment": {
                "CONFIG_PATH": container_config,
            },
            "volumes": [
                f"{repo_config}:{container_config}:ro"
            ],
            "cap_add": ["NET_ADMIN"],
        }

        if svc_networks:
            service["networks"] = svc_networks

        services[svc_name] = service

    compose = {
        # "version": "3.9",
        "services": services,
        "networks": networks,
    }

    out_path.write_text(yaml.dump(compose, default_flow_style=False, sort_keys=False))
    print(f"  wrote {out_path}")


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
    plan_path = sys.argv[1] if len(sys.argv) > 1 else "contact-plan.toml"

    with open(plan_path, "rb") as f:
        data = tomllib.load(f)

    contact_plan_raw = Path(plan_path).read_text()
    node_data = build_node_data(data)

    out_dir = Path("node-configs")
    out_dir.mkdir(exist_ok=True)

    for nid, node in node_data.items():
        toml = render_toml(node, contact_plan_raw)
        out_file = out_dir / f"node{nid}.toml"
        out_file.write_text(toml)
        print(f"  wrote {out_file}")

    # generate_graph(data, node_data, out_dir / "topology.png")

    generate_compose(node_data, out_dir, out_dir / "docker-compose.yml")

    print(f"\nGenerated {len(node_data)} config(s) + docker-compose.yml in '{out_dir}/'")


if __name__ == "__main__":
    main()