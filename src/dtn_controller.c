// dtn_controller.c: Implementation of the DTN Controller that processes incoming packets and manages store-and-forward operations
// Copyright (C) 2025 Michael Karpov & 2025 Cèlia Torras
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

#include "dtn_controller.h"
#include "dtn_routing.h"
#include "dtn_storage.h"
#include "dtn_icmpv6.h"
#include "lwip/ip6.h"
#include "lwip/ip6_addr.h"
#include "lwip/pbuf.h"
#include "lwip/err.h"
#include "lwip/netif.h"
#include "lwip/icmp6.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "raw_socket.h"
#include "lwip/sys.h"
#include "dtn_custody.h"

DTN_Controller *dtn_controller_create(DTN_Module *parent)
{
    DTN_Controller *controller = (DTN_Controller *)malloc(sizeof(DTN_Controller));
    if (controller)
    {
        controller->parent_module = parent;

        // Initialize forwarding attempts tracking
        for (int i = 0; i < MAX_DESTINATIONS; i++)
        {
            controller->forwarding_attempts[i].is_valid = false;
            memset(&controller->forwarding_attempts[i].destination, 0, sizeof(ip6_addr_t));
            controller->forwarding_attempts[i].last_attempt_time = 0;
            controller->forwarding_attempts[i].retry_count = 0;
        }

        printf("DTN Controller created.\n");
    }
    else
    {
        perror("Failed to allocate memory for DTN_Controller");
    }
    return controller;
}

void dtn_controller_destroy(DTN_Controller *controller)
{
    if (!controller)
        return;
    printf("Destroying DTN Controller...\n");
    free(controller);
}

static bool should_attempt_forward(DTN_Controller *controller, const ip6_addr_t *dest_addr)
{
    u32_t current_time = sys_now();

    // Check if this destination is in tracking list
    for (int i = 0; i < MAX_DESTINATIONS; i++)
    {
        if (controller->forwarding_attempts[i].is_valid)
        {
            ip6_addr_t tracking_addr = controller->forwarding_attempts[i].destination;

#if LWIP_IPV6_SCOPES
            ip6_addr_t dest_nozone = *dest_addr;
            ip6_addr_set_zone(&dest_nozone, IP6_NO_ZONE);
            ip6_addr_set_zone(&tracking_addr, IP6_NO_ZONE);

            if (ip6_addr_cmp(&dest_nozone, &tracking_addr))
            {
#else
            if (ip6_addr_cmp(dest_addr, &tracking_addr))
            {
#endif
                u32_t time_since_last_attempt = current_time - controller->forwarding_attempts[i].last_attempt_time;

                if (time_since_last_attempt >= FORWARDING_RETRY_DELAY_MS)
                {
                    if (controller->forwarding_attempts[i].retry_count >= MAX_FORWARDING_RETRIES)
                    {
                        // Max retries reached, delete packet from storage
                        Storage_Function *storage = controller->parent_module->storage;
                        if (storage) {
                            Stored_Packet_Entry *expired_packet = dtn_storage_retrieve_packet_for_dest(storage, dest_addr);
                            if (expired_packet) {
                                char addr_str[IP6ADDR_STRLEN_MAX];
                                ip6addr_ntoa_r(dest_addr, addr_str, sizeof(addr_str));
                                printf("DTN Controller: Deleting packet for %s after %d failed transmission attempts\n", 
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
                }
                else
                {
                    return false;
                }
            }
        }
    }

    for (int i = 0; i < MAX_DESTINATIONS; i++)
    {
        if (!controller->forwarding_attempts[i].is_valid)
        {
            controller->forwarding_attempts[i].is_valid = true;
            memcpy(&controller->forwarding_attempts[i].destination, dest_addr, sizeof(ip6_addr_t));
            controller->forwarding_attempts[i].last_attempt_time = current_time;
            controller->forwarding_attempts[i].retry_count = 1;
            return true;
        }
    }

    return true;
}

void dtn_controller_remove_tracking(DTN_Controller *controller, const ip6_addr_t *dest_addr)
{
    if (!controller || !dest_addr)
    {
        return;
    }

    for (int i = 0; i < MAX_DESTINATIONS; i++)
    {
        if (controller->forwarding_attempts[i].is_valid)
        {
            ip6_addr_t tracking_addr = controller->forwarding_attempts[i].destination;

#if LWIP_IPV6_SCOPES
            ip6_addr_t dest_nozone = *dest_addr;
            ip6_addr_set_zone(&dest_nozone, IP6_NO_ZONE);
            ip6_addr_set_zone(&tracking_addr, IP6_NO_ZONE);

            if (ip6_addr_cmp(&dest_nozone, &tracking_addr))
            {
#else
            if (ip6_addr_cmp(dest_addr, &tracking_addr))
            {
#endif
                controller->forwarding_attempts[i].is_valid = false;
                return;
            }
        }
    }
}

int dtn_controller_process_icmpv6(DTN_Controller *controller, struct pbuf *p, struct netif *inp_netif)
{
    if (!p || !controller || !controller->parent_module)
    {
        return 0;
    }

    return dtn_icmpv6_process(p, inp_netif);
}

static bool is_next_hop_active_contact(Routing_Function *routing, ip6_addr_t *next_hop_ip)
{
    if (!routing || !next_hop_ip) return false;

    u32_t current_time = sys_now();
    Contact_Info *contact = routing->contact_list_head;

    while (contact != NULL)
    {
        bool ip_match = (next_hop_ip->addr[0] == contact->node_addr.addr[0] &&
                         next_hop_ip->addr[1] == contact->node_addr.addr[1] &&
                         next_hop_ip->addr[2] == contact->node_addr.addr[2] &&
                         next_hop_ip->addr[3] == contact->node_addr.addr[3]);

        if (ip_match)
        {
            bool time_ok = (current_time >= contact->start_time_ms && current_time <= contact->end_time_ms);
            if (time_ok) return true;
        }
        contact = contact->next;
    }
    return false;
}

void dtn_controller_process_incoming(DTN_Controller *controller, struct pbuf *p, struct netif *inp_netif)
{
    if (!p || !controller || !controller->parent_module ||
        !controller->parent_module->routing || !controller->parent_module->storage)
    {
        fprintf(stderr, "DTN Controller: Invalid arguments or uninitialized components for incoming.\n");
        if (p)
            pbuf_free(p);
        return;
    }

    if (p->len < IP6_HLEN)
    {
        fprintf(stderr, "DTN Controller: Packet too small for IPv6 header.\n");
        pbuf_free(p);
        return;
    }

    const struct ip6_hdr *ip6hdr = (const struct ip6_hdr *)p->payload;
    if (IP6H_V(ip6hdr) != 6)
    {
        fprintf(stderr, "DTN Controller: Packet is not IPv6 (version %d).\n", IP6H_V(ip6hdr));
        pbuf_free(p);
        return;
    }

    ip6_addr_t temp_src_addr, temp_dest_addr, temp_dest_sender;
    u32_t temp_v_tc_fl;
    u16_t temp_plen;
    u8_t  temp_hoplim;
    memcpy(&temp_src_addr, &ip6hdr->src, sizeof(ip6_addr_t));
    memcpy(&temp_dest_addr, &ip6hdr->dest, sizeof(ip6_addr_t));
    memcpy(&temp_v_tc_fl, &ip6hdr->_v_tc_fl, sizeof(u32_t));
    memcpy(&temp_plen, &ip6hdr->_plen, sizeof(u16_t));
    memcpy(&temp_hoplim, &ip6hdr->_hoplim, sizeof(u8_t));

    if (!dtn_extract_custodian_option(p, &temp_dest_sender)) {
        memcpy(&temp_dest_sender, &temp_src_addr, sizeof(ip6_addr_t));
    }

    Routing_Function *routing = controller->parent_module->routing;
    Storage_Function *storage = controller->parent_module->storage;

    // Check if this is ICMPv6 and process it
    if (IP6H_NEXTH(ip6hdr) == IP6_NEXTH_ICMP6)
    {
        struct pbuf *q = pbuf_alloc(PBUF_RAW, p->tot_len, PBUF_RAM);
        if (!q)
        {
            fprintf(stderr, "DTN Controller: Failed to allocate pbuf for ICMPv6 processing.\n");
            pbuf_free(p);
            return;
        }

        if (pbuf_copy(q, p) != ERR_OK)
        {
            fprintf(stderr, "DTN Controller: Failed to copy pbuf for ICMPv6 processing.\n");
            pbuf_free(q);
            pbuf_free(p);
            return;
        }

        // Skip IPv6 header to get to ICMPv6 header
        if (pbuf_header(q, -IP6_HLEN) != 0)
        {
            fprintf(stderr, "DTN Controller: Failed to adjust pbuf header for ICMPv6 processing.\n");
            pbuf_free(q);
            pbuf_free(p);
            return;
        }

        // Process the ICMPv6 message
        if (dtn_controller_process_icmpv6(controller, q, inp_netif))
        {
            pbuf_free(q);
            pbuf_free(p);
            return;
        }

        // Not a DTN ICMPv6 message, continue normal processing
        pbuf_free(q);
    }

    bool is_for_this_lwip_stack = false;
        ip6_addr_t local_lwip_addr_1, local_lwip_addr_2;
        if (ip6addr_aton("fd00:01::2", &local_lwip_addr_1)) {
            ip6_addr_t dest_nozone = temp_dest_addr;
#if LWIP_IPV6_SCOPES
            ip6_addr_set_zone(&dest_nozone, IP6_NO_ZONE);
            ip6_addr_set_zone(&local_lwip_addr_1, IP6_NO_ZONE);
#endif
            if (ip6_addr_cmp(&dest_nozone, &local_lwip_addr_1)) {
                is_for_this_lwip_stack = true;
            }
        }
        if (ip6addr_aton("fd00:12::1", &local_lwip_addr_2)) {
            ip6_addr_t dest_nozone = temp_dest_addr;
#if LWIP_IPV6_SCOPES
            ip6_addr_set_zone(&dest_nozone, IP6_NO_ZONE);
            ip6_addr_set_zone(&local_lwip_addr_2, IP6_NO_ZONE);
#endif
            if (ip6_addr_cmp(&dest_nozone, &local_lwip_addr_2)) {
                is_for_this_lwip_stack = true;
            }
        }
    if (is_for_this_lwip_stack)
    {
        // Create a copy of the packet for DTN-PCK-RECEIVED
        struct pbuf *p_copy = pbuf_alloc(PBUF_RAW, p->tot_len, PBUF_RAM);
        if (p_copy != NULL)
        {
            if (pbuf_copy(p_copy, p) == ERR_OK)
            {
                // Send DTN-PCK-RECEIVED message to acknowledge receipt
                dtn_icmpv6_send_pck_received(inp_netif, p_copy, ICMP6_CODE_DTN_NO_INFO);

                // Also send DTN-PCK-DELIVERED
                //dtn_icmpv6_send_pck_delivered(inp_netif, p_copy, ICMP6_CODE_DTN_NO_INFO);
            }
            pbuf_free(p_copy);
        }

        // Process the packet locally
        err_t err = ip6_input(p, inp_netif);
        if (err != ERR_OK)
        {
            fprintf(stderr, "DTN Controller: ip6_input returned error %d for local stack packet.\n", err);
        }
        return;
    }

    
    // Not for the local stack
    bool is_dtn_dest = dtn_routing_is_dtn_destination(routing, &temp_dest_addr);

    if (is_dtn_dest)
    {
        ip6_addr_t next_hop_ip;
        int contact_available = dtn_routing_get_dtn_next_hop(routing, &temp_v_tc_fl, &temp_plen, &temp_hoplim, &temp_dest_addr, &temp_dest_sender, &next_hop_ip);
        bool active = is_next_hop_active_contact(routing, &next_hop_ip);
        if (contact_available && active)
        {
            // Create a copy of the packet for DTN-PCK-FORWARDED message
            struct pbuf *p_copy = pbuf_alloc(PBUF_RAW, p->tot_len, PBUF_RAM);
            if (p_copy != NULL)
            {
                if (pbuf_copy(p_copy, p) == ERR_OK)
                {
                    // Send DTN-PCK-FORWARDED message
                    //dtn_icmpv6_send_pck_forwarded(inp_netif, p_copy, ICMP6_CODE_DTN_NO_INFO);
                }
                pbuf_free(p_copy);
            }

            ip6_addr_t my_addr = inp_netif->ip6_addr[1];
            dtn_update_or_add_custodian_option(&p, &my_addr);
            err_t err = raw_socket_send_ipv6(p, &next_hop_ip) == 0 ? ERR_OK : ERR_IF;
            if (err != ERR_OK)
            {
                fprintf(stderr, "DTN Controller: Error sending packet via raw socket: %d.\n", err);
            }
            pbuf_free(p);
            return;
        }
        else
        {
            if (dtn_storage_store_packet(storage, p, &temp_dest_addr))
            {
                // Create a copy for DTN-PCK-RECEIVED
                struct pbuf *p_copy = pbuf_alloc(PBUF_RAW, p->tot_len, PBUF_RAM);
                if (p_copy != NULL)
                {
                    if (pbuf_copy(p_copy, p) == ERR_OK)
                    {
                        // Send DTN-PCK-RECEIVED message
                        dtn_icmpv6_send_pck_received(inp_netif, p_copy, ICMP6_CODE_DTN_NO_CONTACT);
                    }
                    pbuf_free(p_copy);
                }
                return;
            }
            else
            {
                fprintf(stderr, "DTN Controller: Failed to store packet (e.g., storage full). Freeing.\n");

                // Create a copy of the packet for DTN-PCK-DELETED message
                struct pbuf *p_copy = pbuf_alloc(PBUF_RAW, p->tot_len, PBUF_RAM);
                if (p_copy != NULL)
                {
                    if (pbuf_copy(p_copy, p) == ERR_OK)
                    {
                        // Send DTN-PCK-DELETED message
                        //dtn_icmpv6_send_pck_deleted(inp_netif, p_copy, ICMP6_CODE_DTN_DEPLETED_STORE, 0);
                    }
                    pbuf_free(p_copy);
                }

                pbuf_free(p);
                return;
            }
        }
    }else
    {
        err_t err = raw_socket_send_ipv6(p, &temp_dest_addr) == 0 ? ERR_OK : ERR_IF;
        if (err != ERR_OK)
        {
            fprintf(stderr, "DTN Controller: Error sending packet via raw socket: %d.\n", err);
        }
        pbuf_free(p);
        return;
    }
}

void dtn_controller_attempt_forward_stored(DTN_Controller *controller, struct netif *netif_out)
{
    if (!controller || !controller->parent_module || !controller->parent_module->storage ||
        !controller->parent_module->routing || !netif_out)
    {
        return;
    }

    Storage_Function *storage = controller->parent_module->storage;
    Routing_Function *routing = controller->parent_module->routing;

    // Update routing contacts based on current time
    dtn_routing_update_contacts(routing);

    Stored_Packet_Entry *entry = storage->packet_list_head;

    while (entry != NULL)
    {
        Stored_Packet_Entry *next_entry = entry->next;

        // Check if enough time has passed since last attempt
        if (should_attempt_forward(controller, &entry->original_dest))
        {
            struct ip6_hdr *ip6hdr = (struct ip6_hdr *)entry->p->payload;
            u32_t v_tc_fl;
            u16_t plen;
            u8_t hoplim;
            ip6_addr_t src_addr, sender_ip, retrieved_dest_nozone;
            
            memcpy(&v_tc_fl, &ip6hdr->_v_tc_fl, sizeof(u32_t));
            memcpy(&plen, &ip6hdr->_plen, sizeof(u16_t));
            memcpy(&hoplim, &ip6hdr->_hoplim, sizeof(u8_t));
            memcpy(&src_addr, &ip6hdr->src, sizeof(ip6_addr_t));
            memcpy(&retrieved_dest_nozone, &ip6hdr->dest, sizeof(ip6_addr_t));

            if (!dtn_extract_custodian_option(entry->p, &sender_ip)) {
                memcpy(&sender_ip, &src_addr, sizeof(ip6_addr_t));
            }
            ip6_addr_t next_hop_ip;
            int contact_available = dtn_routing_get_dtn_next_hop(routing, &v_tc_fl, &plen, &hoplim, &retrieved_dest_nozone, &sender_ip, &next_hop_ip);
            if (contact_available && is_next_hop_active_contact(routing, &next_hop_ip))
            {
                char node_addr_str[IP6ADDR_STRLEN_MAX];
                ip6addr_ntoa_r(&next_hop_ip, node_addr_str, sizeof(node_addr_str));
                printf("DTN Controller: Forwarding to %s (via CGR)\n", node_addr_str);

                struct pbuf *p_to_fwd = pbuf_alloc(PBUF_RAW, entry->p->tot_len, PBUF_RAM);
                
                if (p_to_fwd && pbuf_copy(p_to_fwd, entry->p) == ERR_OK)
                {
                    bool is_for_this_lwip_stack = false;
                    ip6_addr_t local_lwip_addr_1, local_lwip_addr_2;
                    if (ip6addr_aton("fd00:01::2", &local_lwip_addr_1)) {
                        ip6_addr_t dest_nozone = retrieved_dest_nozone;
            #if LWIP_IPV6_SCOPES
                        ip6_addr_set_zone(&dest_nozone, IP6_NO_ZONE);
                        ip6_addr_set_zone(&local_lwip_addr_1, IP6_NO_ZONE);
            #endif
                        if (ip6_addr_cmp(&dest_nozone, &local_lwip_addr_1)) {
                            is_for_this_lwip_stack = true;
                        }
                    }
                    if (ip6addr_aton("fd00:12::1", &local_lwip_addr_2)) {
                        ip6_addr_t dest_nozone = retrieved_dest_nozone;
            #if LWIP_IPV6_SCOPES
                        ip6_addr_set_zone(&dest_nozone, IP6_NO_ZONE);
                        ip6_addr_set_zone(&local_lwip_addr_2, IP6_NO_ZONE);
            #endif
                        if (ip6_addr_cmp(&dest_nozone, &local_lwip_addr_2)) {
                            is_for_this_lwip_stack = true;
                        }
                    }

                    if (is_for_this_lwip_stack)
                    {
                        // Create a copy of the packet for DTN-PCK-RECEIVED message
                        struct pbuf *p_copy = pbuf_alloc(PBUF_RAW, p_to_fwd->tot_len, PBUF_RAM);
                        if (p_copy != NULL)
                        {
                            if (pbuf_copy(p_copy, p_to_fwd) == ERR_OK)
                            {
                                dtn_icmpv6_send_pck_received(netif_out, p_copy, ICMP6_CODE_DTN_NO_INFO);
                            }
                            pbuf_free(p_copy);
                        }

                        err_t err = ip6_input(p_to_fwd, netif_out);
                        if (err != ERR_OK)
                        {
                            pbuf_free(p_to_fwd);
                        }
                    }
                    else
                    {
                        struct pbuf *p_copy = pbuf_alloc(PBUF_RAW, p_to_fwd->tot_len, PBUF_RAM);
                        if (p_copy != NULL)
                        {
                            if (pbuf_copy(p_copy, p_to_fwd) == ERR_OK)
                            {
                                // Send DTN-PCK-FORWARDED message (comentat originalment)
                                //dtn_icmpv6_send_pck_forwarded(netif_out, p_copy, ICMP6_CODE_DTN_NO_INFO);
                            }
                            pbuf_free(p_copy);
                        }

                        ip6_addr_t my_addr = netif_out->ip6_addr[1];
                        dtn_add_custodian_option(&p_to_fwd, &my_addr);
                        
                        err_t err = raw_socket_send_ipv6(p_to_fwd, &next_hop_ip) == 0 ? ERR_OK : ERR_IF;
                        if (err != ERR_OK)
                        {
                            fprintf(stderr, "DTN Controller: Error sending stored packet via raw socket: %d.\n", err);
                        }
                        pbuf_free(p_to_fwd);
                    }
                }
                else if (p_to_fwd) {
                    pbuf_free(p_to_fwd); 
                }
            }
        }
        entry = next_entry;
    }
}