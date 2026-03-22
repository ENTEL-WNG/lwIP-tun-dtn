/* node1/lwipopts.h */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H

/* Run without an OS */
#define NO_SYS                  1
#define SYS_LIGHTWEIGHT_PROT    0

/* Memory */
#define MEM_ALIGNMENT           4
#define MEM_SIZE                (16 * 1024)
#define MEMP_NUM_PBUF           32
#define MEMP_NUM_TCP_PCB        4
#define MEMP_NUM_UDP_PCB        4

/* IPv6 — the important ones */
#define LWIP_IPV6               1
#define LWIP_IPV6_NUM_ADDRESSES 3
#define LWIP_ICMP6              1
#define LWIP_IPV6_MLD           1
#define LWIP_IPV6_AUTOCONFIG    0   /* we assign the address manually */
#define LWIP_IPV6_DUP_DETECT_ATTEMPTS 0  /* skip DAD in sim */

/* Disable IPv4 to keep it clean */
#define LWIP_IPV4               0
#define LWIP_ARP                0

/* Ethernet */
#define LWIP_ETHERNET           1

/* Timers */
#define LWIP_TIMERS             1

/* Debug (optional — remove for production) */
#define LWIP_DEBUG              1
#define ICMP_DEBUG              LWIP_DBG_ON
#define IP6_DEBUG               LWIP_DBG_ON
#define NETIF_DEBUG             LWIP_DBG_ON

#endif /* LWIPOPTS_H */