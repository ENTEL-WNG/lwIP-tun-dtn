// dtn_routing.h: Header file for DTN routing functions implementing contact-based and schedule-aware routing
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

#ifndef DTN_ROUTING_H
#define DTN_ROUTING_H

#include "dtn_module.h"
#include "lwip/ip6_addr.h"
#include <stdbool.h>
#include <time.h>

// Contact opportunity
typedef struct Contact_Info {
    ip6_addr_t node_addr;        // contact address (destination)
    ip6_addr_t next_hop;         // address of the node to reach the destination
    u32_t start_time_ms;         // start of the contact window
    u32_t end_time_ms;           // end of the contact window
    bool is_dtn_node;            // whether the node is DTN-capable
    struct Contact_Info *next;   // pointer to the next contact in the list
} Contact_Info;

typedef struct Routing_Function {
    DTN_Module* parent_module;
    char* routing_algorithm_name;
    u32_t base_time;
    
    Contact_Info* contact_list_head;
} Routing_Function;

Routing_Function* dtn_routing_create(DTN_Module* parent);

void dtn_routing_destroy(Routing_Function* routing);

bool dtn_routing_is_dtn_destination(Routing_Function* routing, const ip6_addr_t* dest_ip);

int dtn_routing_get_dtn_next_hop(Routing_Function* routing, u32_t* v_tc_fl, u16_t* plen, u8_t* hoplim, ip6_addr_t* dest_ip, ip6_addr_t* sender, ip6_addr_t* next_hop_ip);

int dtn_routing_add_contact(Routing_Function* routing, 
                          const ip6_addr_t* node_addr, 
                          const ip6_addr_t* next_hop,
                          u32_t start_time_ms, 
                          u32_t end_time_ms,
                          bool is_dtn_node);

int dtn_routing_remove_contact(Routing_Function* routing, const ip6_addr_t* node_addr);

bool dtn_routing_update_contacts(Routing_Function* routing);

bool dtn_routing_has_active_contact(Routing_Function* routing, const ip6_addr_t* dest_ip);

int ip6_addr_to_str(const ip6_addr_t *a, char *buf, size_t buflen);

long ipv6_to_nodeid(const char *ip6);

int nodeid_to_ipv6(long node_id, ip6_addr_t *out);

int dtn_routing_load_contacts(Routing_Function* routing, const char* filename);

#endif