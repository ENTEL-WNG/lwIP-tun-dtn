// dtn_controller.c: Implementation of the DTN Controller that processes
// incoming packets and manages store-and-forward operations Copyright (C) 2025
// Michael Karpov
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

        // Initialize forwarding attempts tracking
        for (int i = 0; i < MAX_DESTINATIONS; i++) {
            controller->forwarding_attempts[i].is_valid = false;
            memset(&controller->forwarding_attempts[i].destination, 0, sizeof(ip6_addr_t));
            controller->forwarding_attempts[i].last_attempt_time = 0;
            controller->forwarding_attempts[i].retry_count = 0;
        }

        printf("DTN Controller created.\n");
    } else {
        perror("Failed to allocate memory for DTN_Controller");
    }
    return controller;
}

void dtn_controller_destroy(DTN_Controller* controller) {
    if (!controller)
        return;
    printf("Destroying DTN Controller...\n");
    free(controller);
}

static bool should_attempt_forward(DTN_Controller* controller, const ip6_addr_t* dest_addr) {
    u32_t current_time = sys_now();

    // Check if this destination is in tracking list
    for (int i = 0; i < MAX_DESTINATIONS; i++) {
        if (controller->forwarding_attempts[i].is_valid) {
            ip6_addr_t tracking_addr = controller->forwarding_attempts[i].destination;

#if LWIP_IPV6_SCOPES
            ip6_addr_t dest_nozone = *dest_addr;
            ip6_addr_set_zone(&dest_nozone, IP6_NO_ZONE);
            ip6_addr_set_zone(&tracking_addr, IP6_NO_ZONE);

            if (ip6_addr_cmp(&dest_nozone, &tracking_addr)) {
#else
            if (ip6_addr_cmp(dest_addr, &tracking_addr)) {
#endif
                u32_t time_since_last_attempt =
                    current_time - controller->forwarding_attempts[i].last_attempt_time;

                if (time_since_last_attempt >= FORWARDING_RETRY_DELAY_MS) {
                    if (controller->forwarding_attempts[i].retry_count >= MAX_FORWARDING_RETRIES) {
                        // Max retries reached, delete packet from storage
                        Storage_Function* storage = controller->parent_module->storage;
                        if (storage) {
                            Stored_Packet_Entry* expired_packet =
                                dtn_storage_retrieve_packet_for_dest(storage, dest_addr);
                            if (expired_packet) {
                                char addr_str[IP6ADDR_STRLEN_MAX];
                                ip6addr_ntoa_r(dest_addr, addr_str, sizeof(addr_str));
                                printf(
                                    "DTN Controller: Deleting packet for %s after %d failed "
                                    "transmission attempts\n",
                                    addr_str, MAX_FORWARDING_RETRIES);

                                // Free the packet and entry
                                pbuf_free(expired_packet->p);
                                dtn_storage_free_retrieved_entry_struct(expired_packet);
                            }
                        }

                        // Remove from tracking list
                        controller->forwarding_attempts[i].is_valid = false;
                        return false;
                    }

                    controller->forwarding_attempts[i].last_attempt_time = current_time;
                    controller->forwarding_attempts[i].retry_count++;
                    return true;
                } else {
                    return false;
                }
            }
        }
    }

    for (int i = 0; i < MAX_DESTINATIONS; i++) {
        if (!controller->forwarding_attempts[i].is_valid) {
            controller->forwarding_attempts[i].is_valid = true;
            memcpy(&controller->forwarding_attempts[i].destination, dest_addr, sizeof(ip6_addr_t));
            controller->forwarding_attempts[i].last_attempt_time = current_time;
            controller->forwarding_attempts[i].retry_count = 1;
            return true;
        }
    }

    return true;
}

void dtn_controller_remove_tracking(DTN_Controller* controller, const ip6_addr_t* dest_addr) {
    if (!controller || !dest_addr) {
        return;
    }

    for (int i = 0; i < MAX_DESTINATIONS; i++) {
        if (controller->forwarding_attempts[i].is_valid) {
            ip6_addr_t tracking_addr = controller->forwarding_attempts[i].destination;

#if LWIP_IPV6_SCOPES
            ip6_addr_t dest_nozone = *dest_addr;
            ip6_addr_set_zone(&dest_nozone, IP6_NO_ZONE);
            ip6_addr_set_zone(&tracking_addr, IP6_NO_ZONE);

            if (ip6_addr_cmp(&dest_nozone, &tracking_addr)) {
#else
            if (ip6_addr_cmp(dest_addr, &tracking_addr)) {
#endif
                controller->forwarding_attempts[i].is_valid = false;
                return;
            }
        }
    }
}

int dtn_controller_process_icmpv6(DTN_Controller* controller, struct pbuf* p,
                                  struct netif* inp_netif) {
    if (!p || !controller || !controller->parent_module) {
        return 0;
    }

    return dtn_icmpv6_process(p, inp_netif);
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
        fprintf(stderr,
                "DTN Controller: Invalid arguments or uninitialized components for "
                "incoming.\n");
        if (p)
            pbuf_free(p);
        return;
    }

    if (p->len < IP6_HLEN) {
        fprintf(stderr, "DTN Controller: Packet too small for IPv6 header.\n");
        pbuf_free(p);
        return;
    }

    const struct ip6_hdr* ip6hdr = (const struct ip6_hdr*)p->payload;
    if (IP6H_V(ip6hdr) != 6) {
        fprintf(stderr, "DTN Controller: Packet is not IPv6 (version %d).\n", IP6H_V(ip6hdr));
        pbuf_free(p);
        return;
    }

    char src_str[IP6ADDR_STRLEN_MAX];
    char dest_str[IP6ADDR_STRLEN_MAX];
    ip6_addr_t temp_src_addr, temp_dest_addr;
    ip6_addr_copy_from_packed(temp_src_addr, ip6hdr->src);
    ip6_addr_copy_from_packed(temp_dest_addr, ip6hdr->dest);
    ip6addr_ntoa_r(&temp_src_addr, src_str, sizeof(src_str));
    ip6addr_ntoa_r(&temp_dest_addr, dest_str, sizeof(dest_str));
    DTN_INFO("Received package with Src: %s | Dest: %s | NextHdr: %d", src_str, dest_str,
             IP6H_NEXTH(ip6hdr));

    Routing_Function* routing = controller->parent_module->routing;
    Storage_Function* storage = controller->parent_module->storage;

    // Check if this is ICMPv6 and process it
    if (IP6H_NEXTH(ip6hdr) == IP6_NEXTH_ICMP6) {
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
            DTN_ERROR(
                "DTN Controller: Failed to adjust pbuf header for ICMPv6 "
                "processing.");
            pbuf_free(q);
            pbuf_free(p);
            return;
        }

        // Process the ICMPv6 message
        if (dtn_controller_process_icmpv6(controller, q, inp_netif)) {
            pbuf_free(q);
            pbuf_free(p);
            return;
        }

        // Not a DTN ICMPv6 message, continue normal processing
        pbuf_free(q);
    }

    bool is_local = false;
    for (int i = 0; i < dtn_config.interface_count; i++) {
        if (is_local_address(&temp_dest_addr, dtn_config.interfaces[i]->addr)) {
            DTN_DEBUG("Destination is local via %s", dtn_config.interfaces[i]->addr);
            is_local = true;
            break;
        }
    }

    if (is_local_address(&temp_dest_addr, dtn_config.HOST_LWIP_IPV6_ADDR)) {
        DTN_DEBUG("Destination is local lwIP addresse %s", dtn_config.HOST_LWIP_IPV6_ADDR);
        is_local = true;
    }

    if (is_local_address(&temp_dest_addr, dtn_config.HOST_TUN_IPV6_ADDR)) {
        DTN_DEBUG("Destination is local TUN addresse %s", dtn_config.HOST_TUN_IPV6_ADDR);
        is_local = true;
    }

    if (is_local) {
        // Create a copy of the packet for DTN-PCK-RECEIVED
        struct pbuf* p_copy = pbuf_alloc(PBUF_RAW, p->tot_len, PBUF_RAM);
        if (p_copy != NULL) {
            if (pbuf_copy(p_copy, p) == ERR_OK) {
                // Send DTN-PCK-RECEIVED message to acknowledge receipt
                dtn_icmpv6_send_pck_received(inp_netif, p_copy, ICMP6_CODE_DTN_NO_INFO);

                // Also send DTN-PCK-DELIVERED
                // dtn_icmpv6_send_pck_delivered(inp_netif, p_copy,
                // ICMP6_CODE_DTN_NO_INFO);
            }
            pbuf_free(p_copy);
        }

        // Process the packet locally
        err_t err = ip6_input(p, inp_netif);
        if (err != ERR_OK) {
            DTN_ERROR("DTN Controller: ip6_input returned error %d for local stack packet.", err);
        }
        return;
    }

    // Not for the local stack
    bool is_dtn_dest = dtn_routing_is_dtn_destination(routing, &temp_dest_addr);

    if (is_dtn_dest) {
        ip6_addr_t next_hop_ip;
        int contact_available =
            dtn_routing_get_dtn_next_hop(routing, &temp_dest_addr, &next_hop_ip);

        if (contact_available) {
            // Create a copy of the packet for DTN-PCK-FORWARDED message
            struct pbuf* p_copy = pbuf_alloc(PBUF_RAW, p->tot_len, PBUF_RAM);
            if (p_copy != NULL) {
                if (pbuf_copy(p_copy, p) == ERR_OK) {
                    // Send DTN-PCK-FORWARDED message
                    // dtn_icmpv6_send_pck_forwarded(inp_netif, p_copy,
                    // ICMP6_CODE_DTN_NO_INFO);
                }
                pbuf_free(p_copy);
            }

            ip6_addr_t my_addr = inp_netif->ip6_addr[1];
            dtn_add_custodian_option(&p, &my_addr);
            err_t err = raw_socket_send_ipv6(p, &temp_dest_addr) == 0 ? ERR_OK : ERR_IF;
            if (err != ERR_OK) {
                DTN_ERROR(
                    "DTS destination sending packet src: %s -> dest: %s  via raw "
                    "socket: %d.",
                    src_str, dest_str, err);
            }
            pbuf_free(p);
            return;
        }

        if (dtn_storage_store_packet(storage, p, &temp_dest_addr)) {
            // Create a copy for DTN-PCK-RECEIVED
            struct pbuf* p_copy = pbuf_alloc(PBUF_RAW, p->tot_len, PBUF_RAM);
            if (p_copy != NULL) {
                if (pbuf_copy(p_copy, p) == ERR_OK) {
                    // Send DTN-PCK-RECEIVED message
                    dtn_icmpv6_send_pck_received(inp_netif, p_copy, ICMP6_CODE_DTN_NO_CONTACT);
                }
                pbuf_free(p_copy);
            }
            return;
        }

        DTN_ERROR(
            "DTN Controller: Failed to store packet (e.g., storage full). "
            "Freeing.");

        // Create a copy of the packet for DTN-PCK-DELETED message
        struct pbuf* p_copy = pbuf_alloc(PBUF_RAW, p->tot_len, PBUF_RAM);
        if (p_copy != NULL) {
            if (pbuf_copy(p_copy, p) == ERR_OK) {
                // Send DTN-PCK-DELETED message
                // dtn_icmpv6_send_pck_deleted(inp_netif, p_copy,
                // ICMP6_CODE_DTN_DEPLETED_STORE, 0);
            }
            pbuf_free(p_copy);
        }

        pbuf_free(p);
        return;
    }

    err_t err = raw_socket_send_ipv6(p, &temp_dest_addr) == 0 ? ERR_OK : ERR_IF;
    if (err != ERR_OK) {
        DTN_ERROR("Error sending packet src: %s -> dest: %s  via raw socket: %d.", src_str,
                  dest_str, err);
    }
    pbuf_free(p);
    return;
}

void dtn_controller_attempt_forward_stored(DTN_Controller* controller, struct netif* netif_out) {
    if (!controller || !controller->parent_module || !controller->parent_module->storage ||
        !controller->parent_module->routing || !netif_out) {
        return;
    }

    Storage_Function* storage = controller->parent_module->storage;
    Routing_Function* routing = controller->parent_module->routing;

    // Update routing contacts based on current time
    dtn_routing_update_contacts(routing);

    // Process all contacts with available storage
    Contact_Info* contact = routing->contact_list_head;
    u32_t current_time = sys_now();

    while (contact != NULL) {
        // If contact is active
        if (contact->is_dtn_node && current_time >= contact->start_time_ms &&
            current_time <= contact->end_time_ms) {
            // Check if enough time has passed since last attempt
            if (should_attempt_forward(controller, &contact->node_addr)) {
                // Get a copy of the packet while keeping it in storage
                Stored_Packet_Entry* packet_copy =
                    dtn_storage_get_packet_copy_for_dest(storage, &contact->node_addr);

                if (packet_copy && packet_copy->p) {
                    char node_addr_str[IP6ADDR_STRLEN_MAX];
                    ip6addr_ntoa_r(&contact->node_addr, node_addr_str, sizeof(node_addr_str));
                    printf("DTN Controller: Forwarding to %s\n", node_addr_str);

                    char retrieved_dest_str[IP6ADDR_STRLEN_MAX];
                    ip6addr_ntoa_r(&packet_copy->original_dest, retrieved_dest_str,
                                   sizeof(retrieved_dest_str));
                    struct pbuf* p_to_fwd = packet_copy->p;

                    bool is_for_this_lwip_stack = false;
                    ip6_addr_t local_lwip_addr;
                    if (ip6addr_aton(dtn_config.HOST_LWIP_IPV6_ADDR,
                                     &local_lwip_addr))  // <- CHANGE
                    {
                        ip6_addr_t retrieved_dest_nozone;
                        memcpy(&retrieved_dest_nozone, &packet_copy->original_dest,
                               sizeof(ip6_addr_t));
#if LWIP_IPV6_SCOPES
                        ip6_addr_set_zone(&retrieved_dest_nozone, IP6_NO_ZONE);
                        ip6_addr_set_zone(&local_lwip_addr, IP6_NO_ZONE);
#endif
                        if (ip6_addr_cmp(&retrieved_dest_nozone, &local_lwip_addr)) {
                            is_for_this_lwip_stack = true;
                        }
                    }

                    if (is_for_this_lwip_stack) {
                        // Create a copy of the packet for DTN-PCK-RECEIVED message
                        struct pbuf* p_copy = pbuf_alloc(PBUF_RAW, p_to_fwd->tot_len, PBUF_RAM);
                        if (p_copy != NULL) {
                            if (pbuf_copy(p_copy, p_to_fwd) == ERR_OK) {
                                dtn_icmpv6_send_pck_received(netif_out, p_copy,
                                                             ICMP6_CODE_DTN_NO_INFO);
                                // dtn_icmpv6_send_pck_delivered(netif_out, p_copy,
                                // ICMP6_CODE_DTN_NO_INFO);
                            }
                            pbuf_free(p_copy);
                        }

                        err_t err = ip6_input(p_to_fwd, netif_out);
                        if (err != ERR_OK) {
                            pbuf_free(p_to_fwd);
                        }
                    } else {
                        // Create a copy of the packet for DTN-PCK-FORWARDED message
                        struct pbuf* p_copy = pbuf_alloc(PBUF_RAW, p_to_fwd->tot_len, PBUF_RAM);
                        if (p_copy != NULL) {
                            if (pbuf_copy(p_copy, p_to_fwd) == ERR_OK) {
                                // Send DTN-PCK-FORWARDED message
                                // dtn_icmpv6_send_pck_forwarded(netif_out, p_copy,
                                // ICMP6_CODE_DTN_NO_INFO);
                            }
                            pbuf_free(p_copy);
                        }

                        ip6_addr_t my_addr = netif_out->ip6_addr[1];
                        dtn_add_custodian_option(&p_to_fwd, &my_addr);
                        err_t err = raw_socket_send_ipv6(p_to_fwd, &contact->node_addr) == 0
                                        ? ERR_OK
                                        : ERR_IF;
                        if (err != ERR_OK) {
                            fprintf(stderr,
                                    "DTN Controller: Error sending stored packet via raw "
                                    "socket: %d.\n",
                                    err);
                        }
                        pbuf_free(p_to_fwd);
                    }
                    dtn_storage_free_retrieved_entry_struct(packet_copy);
                }
            }
        }
        contact = contact->next;
    }
}