# DTN-Enabled IPv6 Implementation

## PROJECT DESCRIPTION

This project implements the architecture proposed in "Leveraging IPv6 and ICMPv6 for Delay-Tolerant Networking in Deep Space" by Pirovano et al. It demonstrates how to integrate DTN (Delay-Tolerant Networking) functionalities directly into IPv6 using custom ICMPv6 messages and hop-by-hop extension headers, enabling store-and-forward capabilities while maintaining full IPv6 compatibility.

The implementation uses a modified version of the LwIP (Lightweight IP) stack in userspace with a TUN interface to intercept and process packets. DTN-aware nodes can store packets during network disruptions and forward them when connectivity is restored.

## KEY FEATURES

- **Store-and-Forward**: Persistent packet storage for handling network disruptions
- **Custom ICMPv6 Signaling**: DTN status reporting (RECEIVED, FORWARDED, DELIVERED, DELETED)
- **Contact Graph Routing**: Routing mechanism for intermittent connectivity
- **Custody Transfer**: Optional hop-by-hop reliability mechanism
- **Modular Architecture**: Separate controller, routing and storage functions

## ARCHITECTURE

The implementation consists of:
- **DTN Module**: Core functionality with controller, routing and storage functions
- **Custom LwIP**: Modified lightweight IP stack for userspace packet processing
- **TUN Interface**: Intercepts packets from kernel to userspace
- **Raw Sockets**: Direct packet transmission bypassing kernel routing

## COMPILING AND RUNNING THE PROJECT

Inside the lwip-tun-dtn directory

```bash     
make clean
make 
sudo ./lwip_tun
```

The current configuration is set to run on a node with the following characteristics:

- fd00:01::2 (enp0s9) — Interface connecting to a neighbor Node
- fd00:12::1 (enp0s8) — Interface connecting to another neighbor Node
- fd00::1 (tun0) — TUN interface for kernel-userspace communication
- fd00::2, fd00:01::2, fd00:12::1 — lwIP/DTN userspace address
- Two raw sockets connected to interfaces enp0s8 and enp0s9

For deployment on nodes/environments with other characteristics, all corresponding configurations in this project have to be adjusted. Namely:

- dtn_controller.h/c
- dtn_routing.h/c
- main.c
- raw_socket.h/c

For deployment, the interfaces accessed by the lwIP/DTN userpace module have to exist and be configured on the system. Moreover, the environment has to be configured to forward all traffic towards the address of the lwIP/DTN userpace module (fd00::2) over the tun interface fd00::1 (tun0).

## TESTING

The project ships with three layers of testing, all driven through Docker so no
host network configuration is required:

- **Unit tests** — run the DTN logic against a fixed config in a single container.
- **Network tests** — bring up a full multi-node topology and run a traffic scenario (ping/UDP).
- **Throughput tests** — measure end-to-end DTN throughput and generate metrics/plots.

### Prerequisites

Install the host dependencies once:

```bash
# Docker engine + compose plugin
sudo apt install docker.io docker-compose-plugin

# Python tooling used by the network generator and plotting scripts
sudo apt install python3 python3-pip
pip3 install matplotlib networkx
```

#### Big Constellations more than 16 nodes

The Docker default address pool must be widened, otherwise large topologies run
out of subnets. Edit `/etc/docker/daemon.json` (create it if it doesn't exist):

```json
{
  "default-address-pools": [
    { "base": "10.0.0.0/8", "size": 28 }
  ]
}
```

Then restart Docker:

```bash
sudo systemctl restart docker
```

### Network Generation

A complete test network is described by a single `contact-plan.toml` topology
file (nodes, which of them are DTN-aware, and the time-bounded contact edges
between them). `networks/generate_network.py` expands it into a runnable testbed:

```bash
cd networks
# Default plan (contact_plan_throughput/contact-plan.toml)
python3 generate_network.py

# Or pass an explicit contact plan
python3 generate_network.py contact_plan_ping/contact-plan.toml

# Optional: override the in-container log capture interval (seconds)
python3 generate_network.py contact_plan_ping/contact-plan.toml --capture-interval 10
```

For each plan it writes, into `networks/<plan_name>/`:

- `nodeN.toml` — per-node config (interfaces, IPv6/MAC addresses, embedded contact plan)
- `docker-compose.yml` — one service per node
- `topology.png` — a rendered graph of the topology

Addressing is derived automatically from the contact plan:

- **IPv6 (ULA):** for a link between nodes A and B (`lo = min`, `hi = max`),
  each endpoint gets `fd00:<lo_hex>:<hi_hex>::<node_id>`.
- **MAC:** each endpoint gets `00:<lo_hex>:00:<hi_hex>:00:<node_id_hex>`.

`init_node.py` is the container entrypoint: it configures kernel
interfaces/routes/ip6tables, then either execs `lwip_tun` (for DTN nodes,
`isDtnNode = true`) or keeps the container alive (for plain IPv6 nodes).

### Unit Test

Unit tests build `tests/lwip_tun_test` from `tests/test.c` and exercise the DTN
logic against the fixed config in `tests/node_test.toml` (loaded via
`DTN_CONFIG_PATH`). They run entirely inside one container — no host network or
TUN device needed:

```bash
./run_unit_test.sh
```

This is a thin wrapper around:

```bash
docker compose -f tests/docker-compose.test.yml up --build --abort-on-container-exit
docker compose -f tests/docker-compose.test.yml down --volumes --remove-orphans
```

The container exits with the test process's status code, so the run fails fast
if any assertion fails.

### Network Test

Network tests run a full multi-node simulation end to end. Pass a contact plan,
or omit it to use the default (`networks/contact_plan_ping/contact-plan.toml`):

```bash
# Default plan (contact_plan_ping)
./run_network_test.sh

# Or pass an explicit contact plan
./run_network_test.sh networks/contact_plan_udp/contact-plan.toml
```

The script orchestrates the whole run:

1. Generate per-node configs and `docker-compose.yml` from the contact plan.
2. Build the images (build time recorded in `build_time.json`) and `docker compose up -d`.
3. Wait until every container is running, then snapshot `container_inspect.json`.
4. Run the plan's `networks/<plan>/test.sh` (e.g. issuing pings at scheduled times).
5. Collect time-sorted `docker compose logs`.
6. `docker compose down`.
7. Merge the per-node tcpdump captures into a single time-sorted `tcpdump.txt`.

Artifacts land in `networks/<plan>/captures/<run>/`:

- `logs.txt` — merged, time-sorted container logs
- `tcpdump.txt` — merged per-node traffic (human-readable)
- `node*.pcap` / `node*.txt` — per-node binary and text captures
- `build_time.json`, `container_inspect.json` — run metadata

Available test networks under `networks/` include `contact_plan_ping`,
`contact_plan_ping_cgr`, `contact_plan_icmpv6`, `contact_plan_udp`,
`contact_plan_udp_cgr`, `contact_plan_throughput`, and `contact_plan_sateliot`.

### Throughput Test

`networks/run_throughput_test.py` automates an end-to-end throughput experiment:
it generates the network, brings the containers up, drives UDP traffic from a
sender to a receiver, drains the in-flight DTN packets, tears everything down,
and renders metric plots.

```bash
cd networks
sudo ./run_throughput_test.py --sender-id 1 --receiver-id 6
```

Common options (all forwarded to the traffic generator):

| Option | Default | Meaning |
|---|---|---|
| `--network DIR` | `contact_plan_throughput` | Network directory containing the contact plan |
| `--rate N` | `100` | Packets per second |
| `--duration N` | `30` | Sender run time (seconds) |
| `--size N` | `512` | Payload size (bytes) |
| `--sender-id N` / `--receiver-id N` | — | Override source/destination node IDs |
| `--wait-after N` | `15` | Drain time after the sender stops (seconds) |
| `--capture-interval N` | `30` | In-container log capture interval (seconds) |
| `--no-plots` / `--no-analyze` | off | Skip plotting / analysis |
| `--keep-up` | off | Leave containers running (skips plots) |

Example (full evaluation run):

```bash
sudo ./run_throughput_test.py \
  --network contact_plan_throughput \
  --rate 128 --size 1024 \
  --sender-id 1 --receiver-id 6 \
  --duration 60 --wait-after 40 --capture-interval 5
```

Plots can also be regenerated from a previous run's captures:

```bash
python3 plot_metrics.py --captures contact_plan_throughput/captures/1
```

See `networks/README.md` for more ready-made command lines (Sateliot, Iridium,
storage, and 24h experiments).

## PROJECT STRUCTURE

```
.
inside src/ and include/
├── main.c                 # Main entry point and TUN interface setup
├── dtn_module.[ch]        # DTN module initialization
├── dtn_controller.[ch]    # Packet processing and forwarding logic
├── dtn_routing.[ch]       # Contact-based routing implementation
├── dtn_storage.[ch]       # Persistent packet storage
├── dtn_custody.[ch]       # Custody transfer mechanisms
├── dtn_icmpv6.[ch]        # Custom ICMPv6 messages
├── raw_socket.[ch]        # Raw socket interface
inside py_cgr/
├── contact_plans/         # Contact Plan examples              
├── py_cgr_lib.py          # CGR functions library
others
├── lwipopts.h             # LwIP configuration
├── lwip/                  # Modified LwIP library
├── Makefile               # Build configuration
├── LICENSE                # AGPLv3 license
└── dtn_storage/           # Packet storage directory
```

### TODO:

[ ] lwip as git submodule

## CUSTOM ICMPV6 MESSAGES

- | 200 | DTN-PCK-RECEIVED | Packet received by DTN node |
- | 201 | DTN-PCK-FORWARDED | Packet forwarded to next hop |
- | 202 | DTN-PCK-DELIVERED | Packet reached final destination |
- | 203 | DTN-PCK-DELETED | Packet deleted from storage |

## AUTHORS

- Michael Karpov <michael.karpov@estudiantat.upc.edu> — Initial author and main developer
- Cèlia Torras <celia.torras@estudiantat.upc.edu> — CGR integrator and further developer
- Anna Calveras <anna.calveras@upc.edu> — Project supervisor

## FUNDING

This research was funded in part by the Spanish MCIU/AEI/10.13039/501100011033/ FEDER/UE through project PID2023-146378NB-I00, and by Secretaria d'Universitats i Recerca del departament d'Empresa i Coneixement de la Generalitat de Catalunya with the grant number 2021 SGR 00330

## LICENSE

This project is licensed under the GNU Affero General Public License Version 3 (AGPLv3). See the `LICENSE` file for details.

### Third-Party Components

**Modified LwIP Library:**
- This project includes a modified version of the LwIP (Lightweight IP) library
- Original LwIP is Copyright (c) 2001, 2002 Swedish Institute of Computer Science under a BSD License
- Modification adds the configurable option (IP FORWARD ALLOW TX ON RX NETIF) to ip6.c.

**Modified CGR Library:**
- This project includes a modifies version of the CGR funcion library
- Copiright (c) 2023 Juan Fraire
- Modifications allow IPv6 functionalities instead of BPv7

#### tomlc99

#### SQLite

To change/update version download [sqlite-amalgamation-XXXXXXX.zip](https://www.sqlite.org/download.html).
Replace with [](./third_party/sqlite/sqlite3.c) and sqlite3.h

## ACKNOWLEDGEMENTS

This implementation is based on the paper "Leveraging IPv6 and ICMPv6 for Delay-Tolerant Networking in Deep Space" published in Technologies 2025, 13, 163. https://www.mdpi.com/2227-7080/13/4/163
