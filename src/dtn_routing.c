// dtn_routing.c: Implementation of contact-based routing with time-variant contact management for DTN networks
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

#include "dtn_routing.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "lwip/ip6_addr.h"
#include "lwip/sys.h"
#include "dtn_logger.h"

#define TARGET_DTN_NODE_ADDR "fd00:55::2" // <- CHANGE

Routing_Function* dtn_routing_create(DTN_Module* parent) {
    Routing_Function* routing = (Routing_Function*)malloc(sizeof(Routing_Function));
    if (routing) {
        routing->parent_module = parent;
        routing->routing_algorithm_name = "Simple Contact-Based Routing";
        routing->contact_list_head = NULL;
        
        printf("DTN Routing Function created. Mode: %s\n", routing->routing_algorithm_name);
        
        ip6_addr_t target_node, next_hop;
        if (ip6addr_aton(TARGET_DTN_NODE_ADDR, &target_node)) {
            ip6_addr_copy(next_hop, target_node); 
            dtn_routing_add_contact(routing, &target_node, &next_hop, 
                                //   sys_now() + 15000, // Start in 15 seconds
                                  sys_now() + 30000, // Start in 15 seconds
                                  sys_now() + 3600000, // End in 1 hour
                                  true);
        }
    } else {
        perror("Failed to allocate memory for Routing_Function");
    }
    return routing;
}

bool dtn_routing_has_active_contact(Routing_Function* routing, const ip6_addr_t* dest_ip) {
    if (!routing || !dest_ip) {
        return false;
    }
    
    u32_t current_time = sys_now();
    
    // Iterate through all contacts
    Contact_Info* contact = routing->contact_list_head;
    while (contact != NULL) {
        if (contact->is_dtn_node) {
            ip6_addr_t contact_addr_nozone = contact->node_addr;
            ip6_addr_t dest_ip_nozone = *dest_ip;
            
#if LWIP_IPV6_SCOPES
            ip6_addr_set_zone(&contact_addr_nozone, IP6_NO_ZONE);
            ip6_addr_set_zone(&dest_ip_nozone, IP6_NO_ZONE);
#endif
            
            if (ip6_addr_cmp(&contact_addr_nozone, &dest_ip_nozone) &&
                current_time >= contact->start_time_ms && 
                current_time <= contact->end_time_ms) {
                return true;
            }
        }
        contact = contact->next;
    }
    
    return false;
}

void dtn_routing_destroy(Routing_Function* routing) {
    if (!routing) return;
    
    printf("Destroying DTN Routing Function...\n");
    
    // Free all contacts
    Contact_Info* current = routing->contact_list_head;
    Contact_Info* next;
    while (current != NULL) {
        next = current->next;
        free(current);
        current = next;
    }
    
    free(routing);
}

int dtn_routing_add_contact(Routing_Function* routing, 
                          const ip6_addr_t* node_addr, 
                          const ip6_addr_t* next_hop,
                          u32_t start_time_ms, 
                          u32_t end_time_ms,
                          bool is_dtn_node) {
    if (!routing || !node_addr || !next_hop) return 0;
    
    // Create new contact
    Contact_Info* new_contact = (Contact_Info*)malloc(sizeof(Contact_Info));
    if (!new_contact) {
        perror("Failed to allocate memory for Contact_Info");
        return 0;
    }
    
    // Fill in contact details
    ip6_addr_copy(new_contact->node_addr, *node_addr);
    ip6_addr_copy(new_contact->next_hop, *next_hop);
    new_contact->start_time_ms = start_time_ms;
    new_contact->end_time_ms = end_time_ms;
    new_contact->is_dtn_node = is_dtn_node;
    new_contact->next = NULL;
    
    // Add to list
    if (routing->contact_list_head == NULL) {
        routing->contact_list_head = new_contact;
    } else {
        Contact_Info* current = routing->contact_list_head;
        while (current->next != NULL) {
            current = current->next;
        }
        current->next = new_contact;
    }
    
    char node_addr_str[IP6ADDR_STRLEN_MAX];
    char next_hop_str[IP6ADDR_STRLEN_MAX];
    ip6addr_ntoa_r(node_addr, node_addr_str, sizeof(node_addr_str));
    ip6addr_ntoa_r(next_hop, next_hop_str, sizeof(next_hop_str));
    
    printf("DTN Routing: Added contact for %s via %s (%s), start: %u ms, end: %u ms\n",
            node_addr_str, next_hop_str, is_dtn_node ? "DTN" : "non-DTN", 
           start_time_ms, end_time_ms);
    
    return 1;
}

int dtn_routing_remove_contact(Routing_Function* routing, const ip6_addr_t* node_addr) {
    if (!routing || !node_addr || !routing->contact_list_head) return 0;
    
    Contact_Info* current = routing->contact_list_head;
    Contact_Info* prev = NULL;
    
    while (current != NULL) {
        if (ip6_addr_cmp(&current->node_addr, node_addr)) {
            // Found the contact to remove
            if (prev == NULL) {
                // First item
                routing->contact_list_head = current->next;
            } else {
                prev->next = current->next;
            }
            
            char node_addr_str[IP6ADDR_STRLEN_MAX];
            ip6addr_ntoa_r(node_addr, node_addr_str, sizeof(node_addr_str));
            printf("DTN Routing: Removed contact for %s\n", node_addr_str);
            
            free(current);
            return 1;
        }
        
        prev = current;
        current = current->next;
    }
    
    return 0; // Contact not found
}

void dtn_routing_update_contacts(Routing_Function* routing) {
    if (!routing) return;
    
    static u32_t last_check_time = 0;
    static bool last_active_states[10] = {false};
    static int contact_index = 0;
    
    u32_t current_time = sys_now();
    
    if (last_check_time == 0) {
        last_check_time = current_time;
    }
    
    // Iterate through all contacts
    Contact_Info* contact = routing->contact_list_head;
    contact_index = 0;
    
    while (contact != NULL && contact_index < 10) { 
        // Contact has changed state?
        bool is_active = (current_time >= contact->start_time_ms && 
                          current_time <= contact->end_time_ms);
        
        if (is_active != last_active_states[contact_index]) {
            char node_addr_str[IP6ADDR_STRLEN_MAX];
            ip6addr_ntoa_r(&contact->node_addr, node_addr_str, sizeof(node_addr_str));
            
            if (is_active) {
                DTN_INFO("DTN Routing: Contact for %s became AVAILABLE at time %u ms", 
                       node_addr_str, current_time);
            } else {
                DTN_INFO("DTN Routing: Contact for %s became UNAVAILABLE at time %u ms", 
                       node_addr_str, current_time);
            }
            
            last_active_states[contact_index] = is_active;
        }
        
        contact_index++;
        contact = contact->next;
    }
    
    last_check_time = current_time;
}

bool dtn_routing_is_dtn_destination(Routing_Function* routing, const ip6_addr_t* dest_ip_in) {
    if (!routing || !dest_ip_in) {
        return false;
    }
    
    ip6_addr_t local_dest_ip;
    memset(&local_dest_ip, 0, sizeof(ip6_addr_t));
    memcpy(&local_dest_ip, dest_ip_in, sizeof(ip6_addr_t));
    
    // Check the contact list
    Contact_Info* contact = routing->contact_list_head;
    while (contact != NULL) {
        if (contact->is_dtn_node) {
            ip6_addr_t contact_addr_nozone = contact->node_addr;
            ip6_addr_t local_dest_nozone = local_dest_ip;
            
#if LWIP_IPV6_SCOPES
            ip6_addr_set_zone(&contact_addr_nozone, IP6_NO_ZONE);
            ip6_addr_set_zone(&local_dest_nozone, IP6_NO_ZONE);
#endif
            
            if (ip6_addr_cmp(&local_dest_nozone, &contact_addr_nozone)) {
                return true;
            }
        }
        contact = contact->next;
    }
    
    ip6_addr_t target_dtn_node;
    if (!ip6addr_aton(TARGET_DTN_NODE_ADDR, &target_dtn_node)) {
        fprintf(stderr, "DTN Routing: Failed to parse TARGET_DTN_NODE_ADDR %s!\n", TARGET_DTN_NODE_ADDR);
        return false;
    }
    
#if LWIP_IPV6_SCOPES
    ip6_addr_set_zone(&local_dest_ip, IP6_NO_ZONE);
    ip6_addr_set_zone(&target_dtn_node, IP6_NO_ZONE);
#endif
    
    return ip6_addr_cmp(&local_dest_ip, &target_dtn_node);
}

int dtn_routing_get_dtn_next_hop(Routing_Function* routing, const ip6_addr_t* dest_ip, ip6_addr_t* next_hop_ip) {
    if (!routing || !dest_ip || !next_hop_ip) {
        fprintf(stderr, "DTN Routing: Invalid arguments to get_dtn_next_hop.\n");
        return 0;
    }
    
    if (!dtn_routing_is_dtn_destination(routing, dest_ip)) {
        char dest_addr_str_err[IP6ADDR_STRLEN_MAX];
        ip6addr_ntoa_r(dest_ip, dest_addr_str_err, sizeof(dest_addr_str_err));
        fprintf(stderr, "DTN Routing ERROR: get_dtn_next_hop called for non-DTN dest %s\n", dest_addr_str_err);
        ip6_addr_set_any(next_hop_ip);
        return 0;
    }
    
    // Find contact
    Contact_Info* contact = routing->contact_list_head;
    u32_t current_time = sys_now();
    
    while (contact != NULL) {
        ip6_addr_t contact_addr_nozone = contact->node_addr;
        ip6_addr_t dest_addr_nozone = *dest_ip;
        
#if LWIP_IPV6_SCOPES
        ip6_addr_set_zone(&contact_addr_nozone, IP6_NO_ZONE);
        ip6_addr_set_zone(&dest_addr_nozone, IP6_NO_ZONE);
#endif
        
        if (ip6_addr_cmp(&contact_addr_nozone, &dest_addr_nozone)) {
            // Contact is currently active?
            if (current_time >= contact->start_time_ms && current_time <= contact->end_time_ms) {
                char dest_addr_str[IP6ADDR_STRLEN_MAX];
                ip6addr_ntoa_r(dest_ip, dest_addr_str, sizeof(dest_addr_str));
                DTN_INFO("DTN Routing: Contact AVAILABLE for DTN destination %s. Providing next hop.", dest_addr_str);
                ip6_addr_copy(*next_hop_ip, contact->next_hop);
                return 1;
            } else {
                char dest_addr_str[IP6ADDR_STRLEN_MAX];
                ip6addr_ntoa_r(dest_ip, dest_addr_str, sizeof(dest_addr_str));
                DTN_INFO("DTN Routing: Contact EXISTS but NOT ACTIVE for DTN destination %s.", dest_addr_str);
                ip6_addr_set_any(next_hop_ip);
                return 0;
            }
        }
        contact = contact->next;
    }
    
    // No contact found
    char dest_addr_str[IP6ADDR_STRLEN_MAX];
    ip6addr_ntoa_r(dest_ip, dest_addr_str, sizeof(dest_addr_str));
    DTN_WARN("DTN Routing: No contact found for %s\n", dest_addr_str);
    ip6_addr_set_any(next_hop_ip);
    return 0;
}