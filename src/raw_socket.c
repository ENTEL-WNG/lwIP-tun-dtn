// raw_socket.c: Implementation of raw IPv6 socket operations for direct packet
// transmission bypassing kernel routing Copyright (C) 2025 Michael Karpov
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or any later
// version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "raw_socket.h"

#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <netinet/ip6.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "dtn_config.h"
#include "dtn_logger.h"
#include "lwip/ip6_addr.h"
#include "lwip/pbuf.h"

dtn_socket_result_t dtn_init_raw_socket(void) {
    DTN_DEBUG("Initializing raw sockets...");

    struct ifreq ifr;
    int on = 1;
    for (int i = 0; i < dtn_config.interface_count; i++) {
        char interface_name[IFNAMSIZ];
        strncpy(interface_name, dtn_config.interfaces[i].name, IFNAMSIZ - 1);
        interface_name[IFNAMSIZ - 1] = '\0';

        int raw_socket = socket(AF_INET6, SOCK_RAW, IPPROTO_RAW);
        if (raw_socket < 0) {
            DTN_ERROR("Failed to create raw socket for interface %s", interface_name);
            return DTN_SOCKET_ERR_CREATE;
        }

        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, interface_name, IFNAMSIZ - 1);
        if (ioctl(raw_socket, SIOCGIFINDEX, &ifr) < 0) {
            DTN_ERROR("Failed to get interface index for interface %s", interface_name);
            return DTN_SOCKET_ERR_IFINDEX;
        }

        if (setsockopt(raw_socket, IPPROTO_IPV6, IPV6_HDRINCL, &on, sizeof(on)) < 0) {
            DTN_ERROR("Failed to set IPV6_HDRINCL option on socket for interface %s",
                      interface_name);
            return DTN_SOCKET_ERR_SOCKOPT;
        }

        dtn_config.interfaces[i].socket = raw_socket;
        dtn_config.interfaces[i].socket_index = ifr.ifr_ifindex;
    }

    for (int i = 0; i < dtn_config.interface_count; i++) {
        DTN_INFO("Raw Socket: Initialized raw socket with name %s: socket %d, index %d",
                 dtn_config.interfaces[i].name, dtn_config.interfaces[i].socket,
                 dtn_config.interfaces[i].socket_index);
    }

    return DTN_SOCKET_OK;
}

dtn_socket_result_t dtn_raw_socket_send_to_node_id(struct pbuf* p, int node_id,
                                                   const ip6_addr_t* dest_addr) {
    for (int i = 0; i < dtn_config.interface_count; i++) {
        const DtnInterface* iface = &dtn_config.interfaces[i];
        if (iface->remote_node_id == node_id) {
            return dtn_raw_socket_send_via_interface(p, dest_addr, iface);
        }
    }
    DTN_WARN("Raw Socket: no interface configured for node id %d", node_id);
    return DTN_SOCKET_ERR_SEND;
}

dtn_socket_result_t dtn_raw_socket_send_to_ipv6_address(struct pbuf* p,
                                                        const ip6_addr_t* dest_addr) {
    int interface_to_use = -1;
    for (int i = 0; i < dtn_config.interface_count; i++) {
        const DtnInterface* iface = &dtn_config.interfaces[i];
        ip6_addr_t candidate;

        for (int j = 0; j < iface->dtn_address_count; j++) {
            if (ip6addr_aton(iface->dtn_addresses[j], &candidate) &&
                ip6_addr_zoneless_eq(dest_addr, &candidate)) {
                interface_to_use = i;
                break;
            }
        }
        if (interface_to_use != -1) {
            break;
        }

        for (int j = 0; j < iface->address_count; j++) {
            if (ip6addr_aton(iface->addresses[j], &candidate) &&
                ip6_addr_zoneless_eq(dest_addr, &candidate)) {
                interface_to_use = i;
                break;
            }
        }
        if (interface_to_use != -1) {
            break;
        }
    }

    char dest_str[IP6ADDR_STRLEN_MAX];
    ip6addr_ntoa_r(dest_addr, dest_str, sizeof(dest_str));
    if (interface_to_use == -1) {
        DTN_WARN("No route/interface defined for destination %s, using 0", dest_str);
        interface_to_use = 0;
    }

    const DtnInterface* iface = &dtn_config.interfaces[interface_to_use];
    DTN_INFO("Sending packet to dest: %s using interface %s (index %d, socket %d)", dest_str,
             iface->name, iface->socket_index, iface->socket);

    return dtn_raw_socket_send_via_interface(p, dest_addr, iface);
}

dtn_socket_result_t dtn_raw_socket_send_via_interface(struct pbuf* p, const ip6_addr_t* dest_addr,
                                                      const DtnInterface* dtn_interface) {
    struct sockaddr_in6 sin6;
    int sent_bytes;
    char buf[2048];

    if (p->tot_len > sizeof(buf)) {
        DTN_ERROR("Packet too large for raw socket buffer.");
        return DTN_SOCKET_ERR_PKT_TOO_LARGE;
    }

    if (pbuf_copy_partial(p, buf, p->tot_len, 0) != p->tot_len) {
        DTN_ERROR("Failed to copy pbuf data");
        return DTN_SOCKET_ERR_COPY;
    }

    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;
    sin6.sin6_port = 0;
    sin6.sin6_flowinfo = 0;
    sin6.sin6_scope_id = dtn_interface->socket_index;

    char remote_addr_bare[DTN_MAX_ADDR_LEN];
    strncpy(remote_addr_bare, dtn_interface->remote_addr, DTN_MAX_ADDR_LEN - 1);
    remote_addr_bare[DTN_MAX_ADDR_LEN - 1] = '\0';
    char* slash = strchr(remote_addr_bare, '/');
    if (slash)
        *slash = '\0';

    struct in6_addr next_hop;
    if (inet_pton(AF_INET6, remote_addr_bare, &next_hop) != 1) {
        DTN_ERROR("Failed to parse remote_addr %s for interface %s", dtn_interface->remote_addr,
                  dtn_interface->name);
        return DTN_SOCKET_ERR_SEND;
    }
    memcpy(&sin6.sin6_addr, &next_hop, sizeof(struct in6_addr));

    int setsockopt_result = setsockopt(dtn_interface->socket, SOL_SOCKET, SO_BINDTODEVICE,
                                       dtn_interface->name, strlen(dtn_interface->name));

    if (setsockopt_result != 0) {
        DTN_ERROR("Failed to bind socket to interface %s", dtn_interface->name);
        return DTN_SOCKET_ERR_BIND;
    }

    sent_bytes =
        sendto(dtn_interface->socket, buf, p->tot_len, 0, (struct sockaddr*)&sin6, sizeof(sin6));

    if (sent_bytes < 0) {
        DTN_ERROR("Failed to send packet via raw socket on interface %s", dtn_interface->name);
        return DTN_SOCKET_ERR_SEND;
    } else if ((size_t)sent_bytes != p->tot_len) {
        DTN_WARN("Sent only %d of %d bytes on interface %s", sent_bytes, p->tot_len,
                 dtn_interface->name);
        return DTN_SOCKET_ERR_PARTIAL;
    }

    return DTN_SOCKET_OK;
}

void dtn_raw_socket_cleanup(void) {
    for (int i = 0; i < dtn_config.interface_count; i++) {
        if (dtn_config.interfaces[i].socket >= 0) {
            close(dtn_config.interfaces[i].socket);
            dtn_config.interfaces[i].socket = -1;
        }
    }

    DTN_INFO("Raw sockets clean up complete.");
}