// lwipopts.h: LwIP configuration file customized for IPv6-only DTN operations with disabled IPv4 support
// Copyright (C) 2025 Michael Karpov
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#ifndef LWIPOPTS_H
#define LWIPOPTS_H

// Core System
#define NO_SYS 1
#define LWIP_TIMERS 1
#define LWIP_TIMEVAL_PRIVATE 0
#define SYS_LIGHTWEIGHT_PROT 1

// Ipv4 Configuration
#define LWIP_IPV4 0                      
#define LWIP_ARP 0                       
#define LWIP_ETHERNET 0              
#define LWIP_TCP 0          
#define LWIP_UDP 0             
#define LWIP_ICMP 0                    

// IPv6 Configuration
#define LWIP_IPV6 1
#define LWIP_ICMP6 1                    
#define LWIP_IPV6_REASS 0
#define LWIP_ND6_QUEUEING 0
#define LWIP_NETIF_IPV6_ADDR_GEN_AUTO 0
#define LWIP_IPV6_NUM_ADDRESSES 5 
#define LWIP_IPV6_FORWARD 1
#define IP_FORWARD 1
#define IP_FORWARD_ALLOW_TX_ON_RX_NETIF 1

// Memory Configuration
#define MEM_SIZE (16 * 2048)                     
#define MEMP_NUM_PBUF 10                 
#define PBUF_POOL_SIZE 100                 
#define PBUF_POOL_BUFSIZE 1536
#define MEM_LIBC_MALLOC                  0
#define MEM_ALIGNMENT                    4

// Network Interface Callbacks
#define LWIP_NETIF_LINK_CALLBACK 1
#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_TCPIP_CORE_LOCKING 0

// API Support
#define LWIP_SOCKET 0
#define LWIP_NETCONN 0

// Debugging
//#define LWIP_DEBUG
//#define IP6_DEBUG       LWIP_DBG_ON

#endif