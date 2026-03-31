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
#include <stdbool.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <linux/if_packet.h>
#include <netinet/ip6.h>
#include <errno.h>
#include "lwip/pbuf.h"
#include "lwip/ip6_addr.h"
#include "dtn_logger.h"
#include "dtn_config.h"

int raw_socket_init(void) {
    struct ifreq ifr;
    int error = 0;
    int on = 1;
    for (int i = 0; i < dtn_config.interface_count; i++) {
        char interface_name[IFNAMSIZ];
        strncpy(interface_name, dtn_config.interfaces[i]->name, IFNAMSIZ - 1);
        interface_name[IFNAMSIZ - 1] = '\0';

        // dtn_config.interfaces[i]->socket = i;

        int raw_socket = socket(AF_INET6, SOCK_RAW, IPPROTO_RAW);
        if (raw_socket < 0) {
            DTN_ERROR("Failed to create raw socket for interface %s", interface_name);
            error = -1;
            break;
        }

        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, interface_name, IFNAMSIZ - 1);
        if (ioctl(raw_socket, SIOCGIFINDEX, &ifr) < 0) {
            DTN_ERROR("Failed to get interface index for interface %s", interface_name);
            error = -2;
            break;
        }

        if (setsockopt(raw_socket, IPPROTO_IPV6, IPV6_HDRINCL, &on, sizeof(on)) < 0) {
           DTN_ERROR("Failed to set IPV6_HDRINCL option on socket for interface %s", interface_name);
           error = -3;
           break;
        }

        if (error != 0) {
            break;
        }

        dtn_config.interfaces[i]->socket = raw_socket;
        dtn_config.interfaces[i]->socket_index = ifr.ifr_ifindex;
    }

    if (error != 0) {
        raw_socket_cleanup();
        return error;
    }

    for (int i = 0; i < dtn_config.interface_count; i++) {
        DTN_INFO("Initialized raw socket with name %s: socket %d, index %d",
            dtn_config.interfaces[i]->name, dtn_config.interfaces[i]->socket,
            dtn_config.interfaces[i]->socket_index);
    }

    return error;
}

bool is_dest_addresse_for_interface(const ip6_addr_t *dest_addr, const char *addr) {
    ip6_addr_t route_addr;

    if (ip6addr_aton(addr, &route_addr)) {
        if (dest_addr->addr[0] == route_addr.addr[0] &&
            dest_addr->addr[1] == route_addr.addr[1]) {
                return 1;
        }
    }
    return 0;
}

int raw_socket_send_ipv6(struct pbuf *p, const ip6_addr_t *dest_addr) {
    struct sockaddr_in6 sin6;
    int sent_bytes;
    char buf[2048];
    
    if (p->tot_len > sizeof(buf)) {
        DTN_ERROR("Packet too large for raw socket buffer.");
        return -1;
    }
    
    if (pbuf_copy_partial(p, buf, p->tot_len, 0) != p->tot_len) {
        DTN_ERROR("Failed to copy pbuf data");
        return -1;
    }
    
    // TODO:: more addresses
    int interface_to_use = -1;
    for (int i = 0; i < dtn_config.interface_count; i++) {
        // if (dtn_config.interfaces[i]->route_count > 0) {
        for (int j = 0; j < dtn_config.interfaces[i]->route_count; j++) {
            if (is_dest_addresse_for_interface(dest_addr, dtn_config.interfaces[i]->routes[j])) {
                interface_to_use = i;
                break;
            }

            // ip6_addr_t route_addr;
            // if (ip6addr_aton(dtn_config.interfaces[i]->routes[j], &route_addr)) {
            //     // Check if destination matches the route prefix (simplified check for /64)
            //     if (dest_addr->addr[0] == route_addr.addr[0] &&
            //         dest_addr->addr[1] == route_addr.addr[1]) {
            //         interface_to_use = i;
            //         break;
            //     }
            // }
        }

        if (is_dest_addresse_for_interface(dest_addr, dtn_config.interfaces[i]->addr_via)) {
            interface_to_use = i;
        }
        
        if (interface_to_use != -1) {
            break;
        }
    }

    char dest_str_log[IP6ADDR_STRLEN_MAX];
    ip6addr_ntoa_r(dest_addr, dest_str_log, sizeof(dest_str_log));
    if (interface_to_use == -1) {
        DTN_WARN("No route/interface defined for destination %s, using default", dest_str_log);
        interface_to_use = 0;
    }

    DTNInterfaceConfig* interface_config = dtn_config.interfaces[interface_to_use];
    char dest_str[IP6ADDR_STRLEN_MAX];
    ip6addr_ntoa_r(dest_addr, dest_str, sizeof(dest_str));
    DTN_INFO("Sending packet to %s using socket_to_use %d socket %d (interface %s)",
        dest_str_log, interface_config->socket, interface_config->socket_index, interface_config->name);
    
    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;
    sin6.sin6_port = 0;
    sin6.sin6_flowinfo = 0;
    sin6.sin6_scope_id = interface_config->socket_index;
    
    memcpy(&sin6.sin6_addr, dest_addr, sizeof(struct in6_addr));
    
    if (setsockopt(interface_config->socket, SOL_SOCKET, SO_BINDTODEVICE, 
                  interface_config->name, strlen(interface_config->name)) < 0) {
        DTN_ERROR("Failed to bind socket to interface");
        return -1;
    }
    
    sent_bytes = sendto(interface_config->socket, buf, p->tot_len, 0, 
                       (struct sockaddr *)&sin6, sizeof(sin6));
                       
    if (sent_bytes < 0) {
        // DTN_ERROR("Failed to send packet via raw socket");
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
     for (int i = 0; i < dtn_config.interface_count; i++) {
        if (dtn_config.interfaces[i]->socket >= 0) {
            close(dtn_config.interfaces[i]->socket);
            dtn_config.interfaces[i]->socket = -1;
        }
    }

    DTN_INFO("Raw sockets clean up complete.");
}