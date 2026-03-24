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

## ACKNOWLEDGEMENTS

This implementation is based on the paper "Leveraging IPv6 and ICMPv6 for Delay-Tolerant Networking in Deep Space" published in Technologies 2025, 13, 163. https://www.mdpi.com/2227-7080/13/4/163
