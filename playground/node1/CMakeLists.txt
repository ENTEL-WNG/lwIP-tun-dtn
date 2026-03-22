cmake_minimum_required(VERSION 3.16)
project(lwip_node1 C)

set(CMAKE_C_STANDARD 11)

set(LWIP_DIR    /lwip)
set(CONTRIB_DIR /lwip-contrib)

# ── Probe contrib layout ─────────────────────────────────────────
# Different contrib versions put sys_arch.c in different places.
# Try every known location; fall back to our own stub.
find_file(SYS_ARCH_C sys_arch.c
    PATHS
        ${CONTRIB_DIR}/ports/unix
        ${CONTRIB_DIR}/ports/unix/proj/minimal
        ${CONTRIB_DIR}/ports/unix/port
        ${CONTRIB_DIR}/ports/unix/port/sys_arch
    NO_DEFAULT_PATH
)

if(NOT SYS_ARCH_C)
    message(STATUS "sys_arch.c not found in contrib — using built-in stub")
    set(SYS_ARCH_C ${CMAKE_CURRENT_SOURCE_DIR}/sys_arch_stub.c)
endif()
message(STATUS "Using sys_arch: ${SYS_ARCH_C}")

# Same probe for tapif.c
find_file(TAPIF_C tapif.c
    PATHS
        ${CONTRIB_DIR}/ports/unix/netif
        ${CONTRIB_DIR}/ports/unix/port/netif
    NO_DEFAULT_PATH
)
if(NOT TAPIF_C)
    message(FATAL_ERROR
        "tapif.c not found under ${CONTRIB_DIR}. "
        "Run: find /lwip-contrib -name tapif.c to locate it, "
        "then add that path to the find_file() call above.")
endif()
message(STATUS "Using tapif:    ${TAPIF_C}")

# ── LwIP core sources ────────────────────────────────────────────
set(LWIP_SOURCES
    ${LWIP_DIR}/src/core/init.c
    ${LWIP_DIR}/src/core/def.c
    ${LWIP_DIR}/src/core/dns.c
    ${LWIP_DIR}/src/core/inet_chksum.c
    ${LWIP_DIR}/src/core/ip.c
    ${LWIP_DIR}/src/core/mem.c
    ${LWIP_DIR}/src/core/memp.c
    ${LWIP_DIR}/src/core/netif.c
    ${LWIP_DIR}/src/core/pbuf.c
    ${LWIP_DIR}/src/core/raw.c
    ${LWIP_DIR}/src/core/stats.c
    ${LWIP_DIR}/src/core/sys.c
    ${LWIP_DIR}/src/core/tcp.c
    ${LWIP_DIR}/src/core/tcp_in.c
    ${LWIP_DIR}/src/core/tcp_out.c
    ${LWIP_DIR}/src/core/timeouts.c
    ${LWIP_DIR}/src/core/udp.c

    # IPv6
    ${LWIP_DIR}/src/core/ipv6/dhcp6.c
    ${LWIP_DIR}/src/core/ipv6/ethip6.c
    ${LWIP_DIR}/src/core/ipv6/icmp6.c
    ${LWIP_DIR}/src/core/ipv6/inet6.c
    ${LWIP_DIR}/src/core/ipv6/ip6.c
    ${LWIP_DIR}/src/core/ipv6/ip6_addr.c
    ${LWIP_DIR}/src/core/ipv6/ip6_frag.c
    ${LWIP_DIR}/src/core/ipv6/mld6.c
    ${LWIP_DIR}/src/core/ipv6/nd6.c

    # Ethernet
    ${LWIP_DIR}/src/netif/ethernet.c

    # Located at configure time
    ${SYS_ARCH_C}
    ${TAPIF_C}
)

# ── Include dirs — probe both known contrib layouts ──────────────
set(LWIP_INCLUDE_DIRS
    ${LWIP_DIR}/src/include
    ${CONTRIB_DIR}/ports/unix/include         # older layout
    ${CONTRIB_DIR}/ports/unix/port/include    # newer layout
    ${CMAKE_CURRENT_SOURCE_DIR}                        # lwipopts.h + sys_arch_stub
)

# ── Target ───────────────────────────────────────────────────────
add_executable(lwip_node1 main.c ${LWIP_SOURCES})
target_include_directories(lwip_node1 PRIVATE ${LWIP_INCLUDE_DIRS})

find_library(PCAP_LIB pcap REQUIRED)
target_link_libraries(lwip_node1 PRIVATE ${PCAP_LIB})
target_compile_options(lwip_node1 PRIVATE -Wall -Wextra -O2)