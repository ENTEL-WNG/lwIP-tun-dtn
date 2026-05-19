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
#include <netinet/in.h>
#include <netinet/ip6.h>
#include <stdint.h>
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
#include "raw_socket.h"

dtn_socket_result_t dtn_init_raw_socket(void) {
    DTN_DEBUG("Initializing raw sockets...");

    for (int i = 0; i < dtn_config.interface_count; i++) {
        const char* ifname = dtn_config.interfaces[i].name;

        int raw_socket = socket(AF_INET6, SOCK_RAW, IPPROTO_RAW);
        if (raw_socket < 0) {
            DTN_ERROR("Failed to create AF_INET6 raw socket for interface %s: errno=%d (%s)",
                      ifname, errno, strerror(errno));
            return DTN_SOCKET_ERR_CREATE;
        }

        /* We supply a complete IPv6 packet (header + payload) from lwIP. */
        int one = 1;
        if (setsockopt(raw_socket, IPPROTO_IPV6, IPV6_HDRINCL, &one, sizeof(one)) < 0) {
            DTN_ERROR("Failed to set IPV6_HDRINCL for interface %s: errno=%d (%s)", ifname, errno,
                      strerror(errno));
            close(raw_socket);
            return DTN_SOCKET_ERR_SOCKOPT;
        }

        /* Force egress on the correct physical interface. */
        if (setsockopt(raw_socket, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname) + 1) < 0) {
            DTN_ERROR("Failed to SO_BINDTODEVICE to %s: errno=%d (%s)", ifname, errno,
                      strerror(errno));
            close(raw_socket);
            return DTN_SOCKET_ERR_BIND;
        }

        /* Mark outgoing packets with fwmark 2 so the OUTPUT mangle rule
         * (--set-mark 1 → table 100 → tun0) does not redirect them back
         * through the TUN interface, causing a forwarding loop. */
        int mark = 2;
        if (setsockopt(raw_socket, SOL_SOCKET, SO_MARK, &mark, sizeof(mark)) < 0) {
            DTN_ERROR("Failed to set SO_MARK for interface %s: errno=%d (%s)", ifname, errno,
                      strerror(errno));
            close(raw_socket);
            return DTN_SOCKET_ERR_SOCKOPT;
        }

        dtn_config.interfaces[i].socket = raw_socket;
    }

    for (int i = 0; i < dtn_config.interface_count; i++) {
        DTN_INFO("Raw Socket: Initialized socket for interface %s: fd=%d",
                 dtn_config.interfaces[i].name, dtn_config.interfaces[i].socket);
    }

    return DTN_SOCKET_OK;
}

dtn_socket_result_t dtn_raw_socket_send_to_node_id(struct pbuf* p, int node_id,
                                                   const ip6_addr_t* dest_addr) {
    for (int i = 0; i < dtn_config.interface_count; i++) {
        const DtnInterface* iface = &dtn_config.interfaces[i];
        if (iface->remote_node_id != node_id) {
            continue;
        }

        DTN_DEBUG("Raw Socket: sending to node %d via interface %s", node_id, iface->name);
        return dtn_raw_socket_send_via_interface(p, dest_addr, iface);
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
    DTN_INFO("Sending packet to dest: %s using interface %s (fd=%d)", dest_str, iface->name,
             iface->socket);

    return dtn_raw_socket_send_via_interface(p, dest_addr, iface);
}

dtn_socket_result_t dtn_raw_socket_send_via_interface(struct pbuf* p, const ip6_addr_t* dest_addr,
                                                      const DtnInterface* dtn_interface) {
    if (p->tot_len > 2048) {
        DTN_ERROR("Packet too large for raw socket buffer.");
        return DTN_SOCKET_ERR_PKT_TOO_LARGE;
    }

    uint8_t buf[2048];
    if (pbuf_copy_partial(p, buf, p->tot_len, 0) != p->tot_len) {
        DTN_ERROR("Failed to copy pbuf data");
        return DTN_SOCKET_ERR_COPY;
    }

    struct sockaddr_in6 sa6 = {0};
    sa6.sin6_family = AF_INET6;
    /* lwIP stores addr[4] in network byte order — same layout as in6_addr. */
    memcpy(&sa6.sin6_addr, dest_addr->addr, sizeof(sa6.sin6_addr));
    /* Link-local destinations need a scope id to select the outgoing interface. */
    if (IN6_IS_ADDR_LINKLOCAL(&sa6.sin6_addr)) {
        sa6.sin6_scope_id = if_nametoindex(dtn_interface->name);
    }

    int sent_bytes =
        sendto(dtn_interface->socket, buf, p->tot_len, 0, (struct sockaddr*)&sa6, sizeof(sa6));
    if (sent_bytes < 0) {
        DTN_ERROR("Failed to send packet via interface %s: errno=%d (%s)", dtn_interface->name,
                  errno, strerror(errno));
        return DTN_SOCKET_ERR_SEND;
    } else if ((uint16_t)sent_bytes != p->tot_len) {
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