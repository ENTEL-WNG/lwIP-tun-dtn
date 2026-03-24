// raw_socket.c: Implementation of raw IPv6 socket operations for direct packet transmission bypassing kernel routing
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

#include "raw_socket.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <linux/if_packet.h>
#include <netinet/ip6.h>
#include <errno.h>
#include "lwip/pbuf.h"
#include "lwip/ip6_addr.h"

// Interface indices
static int if_index_1 = 0;  // enp0s8
static int if_index_2 = 0;  // enp0s9
static char if_name_1_global[IFNAMSIZ] = {0};
static char if_name_2_global[IFNAMSIZ] = {0};

// Socket handles
int raw_socket_enp0s8 = -1;
int raw_socket_enp0s9 = -1;

int raw_socket_init(const char* if_name_1, const char* if_name_2) {
    struct ifreq ifr;
    
    strncpy(if_name_1_global, if_name_1, IFNAMSIZ-1);
    strncpy(if_name_2_global, if_name_2, IFNAMSIZ-1);
    
    raw_socket_enp0s8 = socket(AF_INET6, SOCK_RAW, IPPROTO_RAW);
    if (raw_socket_enp0s8 < 0) {
        perror("Failed to create raw socket for first interface");
        return -1;
    }
    
    raw_socket_enp0s9 = socket(AF_INET6, SOCK_RAW, IPPROTO_RAW);
    if (raw_socket_enp0s9 < 0) {
        perror("Failed to create raw socket for second interface");
        close(raw_socket_enp0s8);
        return -1;
    }
    
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, if_name_1, IFNAMSIZ-1);
    if (ioctl(raw_socket_enp0s8, SIOCGIFINDEX, &ifr) < 0) {
        perror("Failed to get interface index for first interface");
        close(raw_socket_enp0s8);
        close(raw_socket_enp0s9);
        return -1;
    }
    if_index_1 = ifr.ifr_ifindex;
    
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, if_name_2, IFNAMSIZ-1);
    if (ioctl(raw_socket_enp0s9, SIOCGIFINDEX, &ifr) < 0) {
        perror("Failed to get interface index for second interface");
        close(raw_socket_enp0s8);
        close(raw_socket_enp0s9);
        return -1;
    }
    if_index_2 = ifr.ifr_ifindex;
    
    int on = 1;
    if (setsockopt(raw_socket_enp0s8, IPPROTO_IPV6, IPV6_HDRINCL, &on, sizeof(on)) < 0) {
        perror("Failed to set IPV6_HDRINCL option on first socket");
        close(raw_socket_enp0s8);
        close(raw_socket_enp0s9);
        return -1;
    }
    
    if (setsockopt(raw_socket_enp0s9, IPPROTO_IPV6, IPV6_HDRINCL, &on, sizeof(on)) < 0) {
        perror("Failed to set IPV6_HDRINCL option on second socket");
        close(raw_socket_enp0s8);
        close(raw_socket_enp0s9);
        return -1;
    }
    
    printf("Raw sockets initialized:\n");
    printf("  %s: socket %d, index %d\n", if_name_1, raw_socket_enp0s8, if_index_1);
    printf("  %s: socket %d, index %d\n", if_name_2, raw_socket_enp0s9, if_index_2);
    
    return 0;
}

int raw_socket_send_ipv6(struct pbuf *p, const ip6_addr_t *dest_addr) {
    struct sockaddr_in6 sin6;
    int socket_to_use;
    int if_index_to_use;
    char *if_name_to_use;
    int sent_bytes;
    char buf[2048];
    
    if (p->tot_len > sizeof(buf)) {
        fprintf(stderr, "Packet too large for raw socket buffer\n");
        return -1;
    }
    
    if (pbuf_copy_partial(p, buf, p->tot_len, 0) != p->tot_len) {
        fprintf(stderr, "Failed to copy pbuf data\n");
        return -1;
    }

    // Node1 --> If destination is in fd00:1::/64, use enp0s9, otherwise use enp0s8
    // Node2 --> If destination is in fd00:23::/64, use enp0s9, otherwise use enp0s8
    int use_second_interface = 0;
    if ((dest_addr->addr[0] == PP_HTONL(0xfd000023) || dest_addr->addr[0] == PP_HTONL(0xfd000033)) &&
        dest_addr->addr[1] == 0 &&
        dest_addr->addr[2] == 0) {
        use_second_interface = 1;
    }
    
    if (use_second_interface) {
        socket_to_use = raw_socket_enp0s9;
        if_index_to_use = if_index_2;
        if_name_to_use = if_name_2_global;
    } else {
        socket_to_use = raw_socket_enp0s8;
        if_index_to_use = if_index_1;
        if_name_to_use = if_name_1_global;
    }
    
    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;
    sin6.sin6_port = 0;
    sin6.sin6_flowinfo = 0;
    sin6.sin6_scope_id = if_index_to_use;
    
    memcpy(&sin6.sin6_addr, dest_addr, sizeof(struct in6_addr));
    
    if (setsockopt(socket_to_use, SOL_SOCKET, SO_BINDTODEVICE, 
                  if_name_to_use, strlen(if_name_to_use)) < 0) {
        perror("Failed to bind socket to interface");
        return -1;
    }
    
    sent_bytes = sendto(socket_to_use, buf, p->tot_len, 0, 
                       (struct sockaddr *)&sin6, sizeof(sin6));
                       
    if (sent_bytes < 0) {
        perror("Failed to send packet via raw socket"); //canvis al node 2
        return -1;
    } else if ((size_t)sent_bytes != p->tot_len) {
        fprintf(stderr, "Sent only %d bytes out of %d\n", sent_bytes, p->tot_len);
        return -1;
    }
    
    char addr_str[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &sin6.sin6_addr, addr_str, sizeof(addr_str));
    
    return 0;
}

void raw_socket_cleanup(void) {
    if (raw_socket_enp0s8 >= 0) {
        close(raw_socket_enp0s8);
        raw_socket_enp0s8 = -1;
    }
    if (raw_socket_enp0s9 >= 0) {
        close(raw_socket_enp0s9);
        raw_socket_enp0s9 = -1;
    }
    printf("Raw sockets closed\n");
}