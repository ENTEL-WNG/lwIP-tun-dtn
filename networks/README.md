# Network

## Contact Plan Throughput

sudo ./run_throughput_test.py --sender-id 1 --receiver-id 6

## Contact Plan Sateliot

### Test Paper

sudo ./run_throughput_test.py \
  --network contact_plan_throughput \
  --rate 128 \
  --size 1024 \
  --sender-id 1 \
  --receiver-id 6 \
  --duration 30 \
  --wait-after 10 \
  --capture-interval 5

python run_throughput_test.py \
  --network contact_plan_throughput \
  --rate 1 \
  --size 1024 \
  --sender-id 1 \
  --receiver-id 6 \
  --duration 30 \
  --wait-after 10 \
  --capture-interval 5

sudo ./run_throughput_test.py \
  --network contact_plan_throughput \
  --rate 8 \
  --size 1024 \
  --sender-id 1 \
  --receiver-id 6 \
  --duration 30 \
  --wait-after 10 \
  --capture-interval 5

sudo ./run_throughput_test.py \
  --network contact_plan_Iridium \
  --rate 1 \
  --duration 120 \
  --size 1024 \
  --sender-id 32 \
  --receiver-id 33 \
  --wait-after 10 \
  --capture-interval 10 

sudo ./run_throughput_test.py \
  --network contact_plan_sateliot \
  --rate 128 \
  --duration 120 \
  --size 1024 \
  --sender-id 7 \
  --receiver-id 6 \
  --wait-after 10 \
  --capture-interval 10 


systemd-inhibit --what=sleep --who="dtn-test" --why="24h experiment" \
  sudo python3 ./run_throughput_test.py \
    --network contact_plan_sateliot \
    --rate 1 \
    --duration 86400 \
    --size 1024 \
    --sender-id 7 \
    --receiver-id 6 \
    --wait-after 300 \
    --capture-interval 60

#### Contact Plan Storage

sudo ./run_throughput_test.py \
  --network contact_plan_storage \
  --rate 256 \
  --size 1024 \
  --sender-id 1 \
  --receiver-id 3 \
  --duration 80 \
  --wait-after 180 \
  --capture-interval 5

## `AF_PACKET` vs `AF_INET6` raw sockets

### How they differ

| | `AF_PACKET` | `AF_INET6 + IPV6_HDRINCL` |
|---|---|---|
| Layer | L2 — full Ethernet frame (dst MAC + src MAC + ethertype + IPv6 payload) | L3 — full IPv6 packet; kernel writes the Ethernet header |
| Kernel routing | **Bypassed entirely** — `sockaddr_ll` names the interface directly | **Goes through the routing stack** — kernel resolves outgoing interface and next-hop MAC via policy routing + routing table |
| Netfilter (ip6tables) | Bypassed — frames never enter the OUTPUT chain | Passes through OUTPUT chain (mangle, filter, etc.) |
| `SO_MARK` / fwmark | Irrelevant | Required — steers the packet into the right routing table |
| NDP / ARP | Not needed — destination MAC supplied by the caller | Kernel needs a resolved neighbour entry to write the L2 header |

### Why `AF_INET6` + table 200 doesn't work cleanly

When `lwip_tun` forwards a packet to a non-adjacent node (e.g. node 1 → node 6 via node 3), the IPv6
header destination is node 6's address on the **node3–node6** link. That address is not directly
reachable from node 1. Two problems arise with `AF_INET6 + IPV6_HDRINCL`:

1. **Wrong `sendto()` address** — passing the packet's final destination to `sendto()` causes the
   kernel to look it up in table 200, find no route, and return `ENETUNREACH` (errno 101).

2. **ECMP conflicts** — fixing (1) requires gateway routes for every reachable remote subnet in
   table 200, one per physical interface. A satellite topology with many contact windows between
   the same node pair generates duplicate routes for the same prefix on different devices. Linux
   ECMP + `SO_BINDTODEVICE` on a raw+`IPV6_HDRINCL` socket is unreliable — the kernel may pick
   the wrong interface despite `SO_BINDTODEVICE`.

The correct fix (in the `#ifdef USE_AF_INET6` block in `src/raw_socket.c`) is to pass the **direct
neighbour address** (`dtn_interface->remote_addr`) to `sendto()`. With `IPV6_HDRINCL` the kernel
uses the `sendto()` address only to resolve the next-hop; the IPv6 header in the buffer carries
the true destination unchanged.

### Why `AF_PACKET` is simpler

The Ethernet frame is constructed manually (dst MAC = `remote_mac` from config), then sent via
`sendto()` with a `sockaddr_ll` naming the interface index. No routing stack, no fwmark, no
table 200, no policy rules, no NDP. This is why `init_node.py` skips section 8 (fwmark 2 /
table 200 setup) unless `--af-inet6` is passed.
