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

#include <errno.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <stdbool.h>
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
#include "lwip/sys.h"

#define ETH_HDR_LEN 14

dtn_socket_result_t dtn_init_raw_socket(void) {
    DTN_DEBUG("Initializing raw sockets...");

    struct ifreq ifr;
    for (int i = 0; i < dtn_config.interface_count; i++) {
        char interface_name[IFNAMSIZ];
        strncpy(interface_name, dtn_config.interfaces[i].name, IFNAMSIZ - 1);
        interface_name[IFNAMSIZ - 1] = '\0';

        /* AF_PACKET bypasses ip6tables/policy routing entirely — operating at L2. */
        // int raw_socket = socket(AF_INET6, SOCK_RAW, IPPROTO_RAW);
        int raw_socket = socket(AF_PACKET, SOCK_RAW, htons(0x86DD /* ETH_P_IPV6 */));
        if (raw_socket < 0) {
            DTN_ERROR("Failed to create AF_PACKET socket for interface %s", interface_name);
            return DTN_SOCKET_ERR_CREATE;
        }

        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, interface_name, IFNAMSIZ - 1);
        if (ioctl(raw_socket, SIOCGIFINDEX, &ifr) < 0) {
            DTN_ERROR("Failed to get interface index for interface %s", interface_name);
            return DTN_SOCKET_ERR_IFINDEX;
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
    DTN_INFO("Sending packet to dest: %s using interface %s (index %d, socket %d)", dest_str,
             iface->name, iface->socket_index, iface->socket);

    return dtn_raw_socket_send_via_interface(p, dest_addr, iface);
}

static int parse_mac(const char* mac_str, uint8_t mac[6]) {
    return sscanf(mac_str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &mac[0], &mac[1], &mac[2], &mac[3],
                  &mac[4], &mac[5]) == 6
               ? 0
               : -1;
}

dtn_socket_result_t dtn_raw_socket_send_via_interface(struct pbuf* p, const ip6_addr_t* dest_addr,
                                                      const DtnInterface* dtn_interface) {
    uint8_t buf[ETH_HDR_LEN + 2048];

    if (p->tot_len > 2048) {
        DTN_ERROR("Packet too large for raw socket buffer.");
        return DTN_SOCKET_ERR_PKT_TOO_LARGE;
    }

    uint8_t dst_mac[6], src_mac[6];
    if (parse_mac(dtn_interface->remote_mac, dst_mac) < 0) {
        DTN_ERROR("Failed to parse remote_mac '%s' for interface %s", dtn_interface->remote_mac,
                  dtn_interface->name);
        return DTN_SOCKET_ERR_SEND;
    }
    if (parse_mac(dtn_interface->local_mac, src_mac) < 0) {
        DTN_ERROR("Failed to parse local_mac '%s' for interface %s", dtn_interface->local_mac,
                  dtn_interface->name);
        return DTN_SOCKET_ERR_SEND;
    }

    /* Ethernet header: dst(6) + src(6) + ethertype(2) */
    memcpy(buf, dst_mac, 6);
    memcpy(buf + 6, src_mac, 6);
    buf[12] = 0x86;
    buf[13] = 0xDD; /* ETH_P_IPV6 */

    if (pbuf_copy_partial(p, buf + ETH_HDR_LEN, p->tot_len, 0) != p->tot_len) {
        DTN_ERROR("Failed to copy pbuf data");
        return DTN_SOCKET_ERR_COPY;
    }

    struct sockaddr_ll sll = {0};
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(0x86DD);
    sll.sll_ifindex = dtn_interface->socket_index;
    sll.sll_halen = 6;
    memcpy(sll.sll_addr, dst_mac, 6);

    int frame_len = ETH_HDR_LEN + p->tot_len;
    int sent_bytes =
        sendto(dtn_interface->socket, buf, frame_len, 0, (struct sockaddr*)&sll, sizeof(sll));

    if (sent_bytes < 0) {
        DTN_ERROR("Failed to send frame via interface %s: errno=%d (%s)", dtn_interface->name,
                  errno, strerror(errno));
        return DTN_SOCKET_ERR_SEND;
    } else if (sent_bytes != frame_len) {
        DTN_WARN("Sent only %d of %d bytes on interface %s", sent_bytes, frame_len,
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