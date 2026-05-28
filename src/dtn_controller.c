// dtn_controller.c: Implementation of the DTN Controller that processes incoming packets and
// manages store-and-forward operations Copyright (C) 2025 Michael Karpov & 2025 Cèlia Torras
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

#include "dtn_controller.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dtn_config.h"
#include "dtn_custody.h"
#include "dtn_icmpv6.h"
#include "dtn_logger.h"
#include "dtn_routing.h"
#include "dtn_storage.h"
#include "lwip/err.h"
#include "lwip/icmp6.h"
#include "lwip/ip6.h"
#include "lwip/ip6_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/sys.h"
#include "raw_socket.h"

DTN_Controller* dtn_controller_create(DTN_Module* parent) {
    DTN_Controller* controller = (DTN_Controller*)malloc(sizeof(DTN_Controller));
    if (controller) {
        controller->parent_module = parent;
        DTN_INFO("Cratead DTN_Controller.");
    } else {
        DTN_ERROR("Failed to allocate memory for DTN_Controller");
    }
    return controller;
}

void dtn_controller_destroy(DTN_Controller* controller) {
    if (!controller)
        return;
    DTN_INFO("Destroying DTN Controller");
    free(controller);
}

int dtn_controller_process_icmpv6(DTN_Controller* controller, struct pbuf* p,
                                  ip6_addr_t* dest_addr) {
    if (!p || !controller || !controller->parent_module) {
        return 0;
    }

    return dtn_icmpv6_process(p, dest_addr);
}

bool is_local_address(const ip6_addr_t* dest_addr, const char* addr) {
    ip6_addr_t local_addr;

    if (ip6addr_aton(addr, &local_addr)) {
        ip6_addr_t dest_addr_nozone = *dest_addr;
#if LWIP_IPV6_SCOPES
        ip6_addr_set_zone(&dest_addr_nozone, IP6_NO_ZONE);
        ip6_addr_set_zone(&local_addr, IP6_NO_ZONE);
#endif

        if (ip6_addr_eq(&dest_addr_nozone, &local_addr)) {
            return true;
        }
    }
    return false;
}

void dtn_controller_process_incoming(DTN_Controller* controller, struct pbuf* p,
                                     struct netif* inp_netif) {
    if (!p || !controller || !controller->parent_module || !controller->parent_module->routing ||
        !controller->parent_module->storage) {
        DTN_ERROR("DTN Controller: Invalid arguments or uninitialized components for incoming.");
        if (p) {
            pbuf_free(p);
        }
        return;
    }

    if (p->len < IP6_HLEN) {
        DTN_WARN("DTN Controller: Packet too small for IPv6 header.");
        pbuf_free(p);
        return;
    }

    const struct ip6_hdr* ip6hdr = (const struct ip6_hdr*)p->payload;
    if (IP6H_V(ip6hdr) != 6) {
        DTN_WARN("DTN Controller: Packet is not IPv6 (version %d).", IP6H_V(ip6hdr));
        pbuf_free(p);
        return;
    }

    char src_str[IP6ADDR_STRLEN_MAX], dest_str[IP6ADDR_STRLEN_MAX],
        custodian_str[IP6ADDR_STRLEN_MAX];
    ip6_addr_t src_addr, dest_addr, custodian_addr;
    ip6_addr_copy_from_packed(src_addr, ip6hdr->src);
    ip6_addr_copy_from_packed(dest_addr, ip6hdr->dest);
    ip6addr_ntoa_r(&src_addr, src_str, sizeof(src_str));
    ip6addr_ntoa_r(&dest_addr, dest_str, sizeof(dest_str));

    bool has_custodian = dtn_extract_custodian_option(p, &custodian_addr);
    if (has_custodian) {
        ip6addr_ntoa_r(&custodian_addr, custodian_str, sizeof(custodian_str));
    } else {
        strncpy(custodian_str, "NONE", sizeof(custodian_str));
    }

    DTN_INFO("Received package with src: %s -> dest: %s | custodian: %s", src_str, dest_str,
             custodian_str);

    if (PRINT_PAYLOAD) {
        uint8_t nexth = IP6H_NEXTH(ip6hdr);
        uint16_t offset = sizeof(struct ip6_hdr);

        /* Walk past extension headers (Hop-by-Hop=0, Routing=43,
         * Fragment=44, Dest Options=60) to reach the actual payload. */
        while (nexth == 0 || nexth == 43 || nexth == 44 || nexth == 60) {
            uint8_t ext[2];
            if (pbuf_copy_partial(p, ext, 2, offset) < 2)
                break;
            /* Fragment header is always 8 bytes; others: (len+1)*8 */
            uint16_t ext_len = (nexth == 44) ? 8 : ((uint16_t)(ext[1] + 1) * 8);
            nexth = ext[0];
            offset += ext_len;
            if (offset >= p->tot_len)
                break;
        }

        /* Skip transport header */
        uint16_t transport_hdr_len = 0;
        if (nexth == 17)
            transport_hdr_len = 8; /* UDP */
        else if (nexth == 6)
            transport_hdr_len = 20; /* TCP */

        // DTN_DEBUG("[debug] nexth=%u  plen=%u  payload_offset=%u  p->tot_len=%u\n", nexth, plen,
        //   offset + transport_hdr_len, p->tot_len);

        uint8_t buf[512];
        uint16_t copied = pbuf_copy_partial(p, buf, sizeof(buf) - 1, offset + transport_hdr_len);
        buf[copied] = '\0';

        /* Strip trailing whitespace (\n, \r, spaces, etc.) */
        while (copied > 0 && isspace(buf[copied - 1])) buf[--copied] = '\0';

        /* Only print if every byte is printable ASCII */
        bool is_printable = (copied > 0);
        for (uint16_t i = 0; i < copied && is_printable; i++) {
            if (!isprint(buf[i]) && !isspace(buf[i]))
                is_printable = false;
        }
        if (is_printable)
            DTN_INFO("[payload] %s", (char*)buf);
    }

    // Check if this is ICMPv6 and process it
    if (IP6H_NEXTH(ip6hdr) == IP6_NEXTH_ICMP6) {
        DTN_INFO("Received ICMPv6 message.");

        struct pbuf* q = pbuf_alloc(PBUF_RAW, p->tot_len, PBUF_RAM);
        if (!q) {
            DTN_ERROR("DTN Controller: Failed to allocate pbuf for ICMPv6 processing.");
            pbuf_free(p);
            return;
        }

        if (pbuf_copy(q, p) != ERR_OK) {
            DTN_ERROR("DTN Controller: Failed to copy pbuf for ICMPv6 processing.");
            pbuf_free(q);
            pbuf_free(p);
            return;
        }

        // Skip IPv6 header to get to ICMPv6 header
        if (pbuf_header(q, -IP6_HLEN) != 0) {
            DTN_ERROR("DTN Controller: Failed to adjust pbuf header for ICMPv6 processing.");
            pbuf_free(q);
            pbuf_free(p);
            return;
        }

        // Process the ICMPv6 message
        if (dtn_controller_process_icmpv6(controller, q, &src_addr)) {
            DTN_INFO("Processed ICMPv6 message.");
            pbuf_free(q);
            pbuf_free(p);
            return;
        }

        // Not a DTN ICMPv6 message, continue normal processing
        pbuf_free(q);
    }

    if (has_custodian) {
        dtn_icmpv6_send_pck_received(p, ICMP6_CODE_DTN_NO_INFO);
    }

    bool is_local = false;
    for (int i = 0; i < dtn_config.interface_count; i++) {
        if (is_local_address(&dest_addr, dtn_config.interfaces[i].local_addr)) {
            DTN_INFO("Destination is local interface %s with address %s.",
                     dtn_config.interfaces[i].name, dtn_config.interfaces[i].local_addr);
            is_local = true;
            break;
        }
    }

    if (is_local_address(&dest_addr, dtn_config.lwip_ipv6_addr)) {
        DTN_INFO("Destination is local lwIP addresse %s", dtn_config.lwip_ipv6_addr);
        is_local = true;
    }

    if (is_local_address(&dest_addr, dtn_config.tun_ipv6_addr)) {
        DTN_INFO("Destination is local TUN addresse %s", dtn_config.tun_ipv6_addr);
        is_local = true;
    }

    if (is_local) {
        // dtn_icmpv6_send_pck_delivered(p, ICMP6_CODE_DTN_NO_INFO, src_addr);

        // ip6_input takes ownership of p and frees it internally.
        err_t err = ip6_input(p, inp_netif);
        if (err != ERR_OK) {
            DTN_ERROR("ip6_input returned error %d for local stack packet.", err);
        }
        return;
    }

    dtn_controller_send_or_store(controller, p);
    // pbuf_free(p);
    return;
}

dtn_controller_send_or_store_result_t dtn_controller_send_or_store(DTN_Controller* controller,
                                                                   struct pbuf* p) {
    struct ip6_hdr* ip6hdr = (struct ip6_hdr*)p->payload;

    char src_str[IP6ADDR_STRLEN_MAX], dest_str[IP6ADDR_STRLEN_MAX],
        custodian_str[IP6ADDR_STRLEN_MAX];
    ip6_addr_t src_addr, dest_addr, custodian_addr;
    ip6_addr_copy_from_packed(src_addr, ip6hdr->src);
    ip6_addr_copy_from_packed(dest_addr, ip6hdr->dest);
    ip6addr_ntoa_r(&src_addr, src_str, sizeof(src_str));
    ip6addr_ntoa_r(&dest_addr, dest_str, sizeof(dest_str));
    bool has_custodian = dtn_extract_custodian_option(p, &custodian_addr);
    if (has_custodian) {
        ip6addr_ntoa_r(&custodian_addr, custodian_str, sizeof(custodian_str));
    } else {
        strncpy(custodian_str, "NONE", sizeof(custodian_str));
    }

    DTN_INFO("Attempting to send or store package with src: %s -> dest: %s | custodian: %s",
             src_str, dest_str, custodian_str);

    DtnRoutingResult routing_result;
    dtn_controller_send_result_t send_status = dtn_controller_send(controller, p, &routing_result);

    if (send_status == DTN_CONTROLLER_SEND_ERR) {
        return DTN_CONTROLLER_SEND_OR_STORE_ERR;
    }

    if (send_status == DTN_CONTROLLER_SEND_OK) {
        DTN_INFO("Successfully send package src: %s -> dest: %s", src_str, dest_str);
        return DTN_CONTROLLER_SEND_OR_STORE_OK;
    }

    if (send_status == DTN_CONTROLLER_SEND_NOT_ACTIVE) {
        Storage_Function* storage = controller->parent_module->storage;
        if (dtn_storage_store_packet(storage, p, &dest_addr, &routing_result)) {
            DTN_INFO("Successfully stored package src: %s -> dest: %s for later forwarding",
                     src_str, dest_str);
            return DTN_CONTROLLER_SEND_OR_STORE_OK;
        }

        DTN_ERROR("Failed to store package src: %s -> dest: %s for later forwarding", src_str,
                  dest_str);
        return DTN_CONTROLLER_SEND_OR_STORE_ERR;
    }

    DTN_WARN("No route for package src: %s -> dest: %s, falling back to direct send", src_str,
             dest_str);
    dtn_socket_result_t socket_result = dtn_raw_socket_send(p);
    if (socket_result == DTN_SOCKET_OK) {
        return DTN_CONTROLLER_SEND_OR_STORE_OK;
    }

    return DTN_CONTROLLER_SEND_OR_STORE_ERR;
}

dtn_controller_send_result_t dtn_controller_send(DTN_Controller* controller, struct pbuf* p,
                                                 DtnRoutingResult* routing_result) {
    if (!controller || !controller->parent_module || !p || !routing_result) {
        return DTN_CONTROLLER_SEND_ERR;
    }

    Routing_Function* routing = controller->parent_module->routing;

    const struct ip6_hdr* ip6hdr = (const struct ip6_hdr*)p->payload;

    char src_str[IP6ADDR_STRLEN_MAX], dest_str[IP6ADDR_STRLEN_MAX],
        custodian_str[IP6ADDR_STRLEN_MAX];
    ip6_addr_t src_addr, dest_addr, custodian_addr;
    ip6_addr_copy_from_packed(src_addr, ip6hdr->src);
    ip6_addr_copy_from_packed(dest_addr, ip6hdr->dest);
    ip6addr_ntoa_r(&src_addr, src_str, sizeof(src_str));
    ip6addr_ntoa_r(&dest_addr, dest_str, sizeof(dest_str));
    bool has_custodian = dtn_extract_custodian_option(p, &custodian_addr);
    if (has_custodian) {
        ip6addr_ntoa_r(&custodian_addr, custodian_str, sizeof(custodian_str));
    } else {
        strncpy(custodian_str, "NONE", sizeof(custodian_str));
    }

    DTN_INFO("Attempting to send package with src: %s -> dest: %s | custodian: %s", src_str,
             dest_str, custodian_str);

    dtn_routing_result_t routing_status = dtn_routing_get_next_hop_node_id(
        routing->base_time_in_ms, sys_now(), (struct ip6_hdr*)ip6hdr, routing_result);

    if (routing_status == DTN_ROUTING_NO_ROUTE) {
        return DTN_CONTROLLER_SEND_NO_ROUTE;
    }

    if (routing_status != DTN_ROUTING_OK) {
        return DTN_CONTROLLER_SEND_ERR;
    }

    bool is_next_hop_active;
    int active_result = dtn_routing_is_next_hop_active(sys_now(), routing_result->next_hop_node_id,
                                                       &is_next_hop_active);

    if (active_result == DTN_ROUTING_OK && !is_next_hop_active) {
        return DTN_CONTROLLER_SEND_NOT_ACTIVE;
    }

    const DtnInterface* dtn_interface =
        dtn_raw_socket_get_interface_for_node(routing_result->next_hop_node_id);

    if (!dtn_interface) {
        return DTN_CONTROLLER_SEND_ERR;
    }

    ip6_addr_t local_addr;
    ip6addr_aton(dtn_interface->local_addr, &local_addr);
    dtn_update_or_add_custodian_option(&p, &local_addr);

    dtn_socket_result_t socket_result = dtn_raw_socket_send_via_interface(p, dtn_interface);

    if (socket_result == DTN_SOCKET_OK) {
        // dtn_icmpv6_send_pck_forwarded(p, ICMP6_CODE_DTN_NO_INFO);
        return DTN_CONTROLLER_SEND_OK;
    }

    return DTN_CONTROLLER_SEND_ERR;
}

void dtn_controller_attempt_forward_stored(DTN_Controller* controller, struct netif* netif_out) {
    if (!controller || !controller->parent_module || !controller->parent_module->storage ||
        !controller->parent_module->routing || !netif_out) {
        return;
    }

    Storage_Function* storage = controller->parent_module->storage;

    double now_sec = sys_now() / 1000.0;
    int number_of_stored_packages = dtn_storage_count(storage);
    if (number_of_stored_packages == 0) {
        return;
    }

    // Query only the packets whose contact window has arrived.
    Stored_Packet_Entry entries[MAX_STORED_PACKETS];
    int n = dtn_storage_get_ready_entries(storage, now_sec, entries, MAX_STORED_PACKETS);

    DTN_INFO("%d packekts stored / %d packets ready for forwarding at %f",
             number_of_stored_packages, n, now_sec);

    for (int i = 0; i < n; i++) {
        Stored_Packet_Entry* entry = &entries[i];
        struct pbuf* p = entry->p;

        struct ip6_hdr* ip6hdr = (struct ip6_hdr*)p->payload;

        char src_str[IP6ADDR_STRLEN_MAX], dest_str[IP6ADDR_STRLEN_MAX],
            custodian_str[IP6ADDR_STRLEN_MAX];
        ip6_addr_t src_addr, dest_addr, custodian_addr;
        ip6_addr_copy_from_packed(src_addr, ip6hdr->src);
        ip6_addr_copy_from_packed(dest_addr, ip6hdr->dest);
        ip6addr_ntoa_r(&src_addr, src_str, sizeof(src_str));
        ip6addr_ntoa_r(&dest_addr, dest_str, sizeof(dest_str));

        bool has_custodian = dtn_extract_custodian_option(p, &custodian_addr);
        if (has_custodian) {
            ip6addr_ntoa_r(&custodian_addr, custodian_str, sizeof(custodian_str));
        } else {
            strncpy(custodian_str, "NONE", sizeof(custodian_str));
        }
        DTN_INFO("Attempting to send stored package with src: %s -> dest: %s | custodian: %s",
                 src_str, dest_str, custodian_str);

        DtnRoutingResult routing_result;
        dtn_controller_send_result_t send_result =
            dtn_controller_send(controller, p, &routing_result);
        switch (send_result) {
            case DTN_CONTROLLER_SEND_OK:
                DTN_INFO("Successfully send stored package src: %s -> dest: %s", src_str, dest_str);
                break;
            case DTN_CONTROLLER_SEND_NOT_ACTIVE:
                DTN_ERROR("No route for stored package src: %s -> dest: %s", src_str, dest_str);
                break;
            case DTN_CONTROLLER_SEND_NO_ROUTE:
                DTN_ERROR("No route for stored package src: %s -> dest: %s", src_str, dest_str);
                break;
            case DTN_CONTROLLER_SEND_ERR:
                DTN_ERROR("Failed to send stored package src: %s -> dest: %s", src_str, dest_str);
                break;
        }

        // Free the pbuf if it wasn't consumed by dtn_update_or_add_custodian_option.
        if (entry->p != NULL) {
            pbuf_free(entry->p);
            entry->p = NULL;
        }
    }
}