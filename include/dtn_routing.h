// dtn_routing.h: Header file for DTN routing functions implementing contact-based and
// schedule-aware routing Copyright (C) 2025 Michael Karpov
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

#include <stdbool.h>
#include <time.h>

#include "dtn_module.h"
#include "lwip/ip6.h"
#include "lwip/ip6_addr.h"

typedef struct Routing_Function {
    DTN_Module* parent_module;
    char* routing_algorithm_name;
    u32_t base_time_in_ms;
} Routing_Function;

typedef enum {
    DTN_ROUTING_OK = 0,
    DTN_ROUTING_NO_ROUTE = 1,
    DTN_ROUTING_ERR = -1,
} dtn_routing_result_t;

typedef struct {
    int next_hop_node_id;
    double to_time;
    double best_delivery_time;
} DtnRoutingResult;

Routing_Function* dtn_routing_create(DTN_Module* parent);

void dtn_routing_destroy(Routing_Function* routing);

dtn_routing_result_t dtn_routing_is_node_id_dtn_node(int node_id, bool* is_dtn_node);

dtn_routing_result_t dtn_routing_is_next_hop_active(double current_time_in_ms, int node_id,
                                                    bool* is_next_hop_active);

dtn_routing_result_t dtn_routing_get_next_hop_node_id(double start_time_in_ms,
                                                      double current_time_in_ms,
                                                      struct ip6_hdr* ip6h,
                                                      DtnRoutingResult* result);

dtn_routing_result_t _dtn_routing_get_next_hop_node_id(
    char* contact_plan_path, double start_time_in_sec, double current_time_in_sec,
    long current_node_id, long src_node_id, long dest_node_id, long deadline, long package_length,
    long dscp, DtnRoutingResult* result);

// bool dtn_routing_is_dtn_destination(Routing_Function* routing, const ip6_addr_t* dest_ip);

// int dtn_routing_get_dtn_next_hop(Routing_Function* routing, u32_t* v_tc_fl, u16_t* plen,
//                                  u8_t* hoplim, ip6_addr_t* dest_ip, ip6_addr_t* sender,
//                                  ip6_addr_t* next_hop_ip);
// int dtn_routing_get_next_dnt_hop_v2(double start_time, double current_time,
//                                     struct ip6_hdr* ip6_header, ip6_addr_t* next_hop_ip);

// int dtn_routing_add_contact(Routing_Function* routing, const ip6_addr_t* node_addr,
//                             const ip6_addr_t* next_hop, u32_t start_time_ms, u32_t end_time_ms,
//                             bool is_dtn_node);

// int dtn_routing_remove_contact(Routing_Function* routing, const ip6_addr_t* node_addr);

// bool dtn_routing_update_contacts(Routing_Function* routing);

// bool dtn_routing_has_active_contact(Routing_Function* routing, const ip6_addr_t* dest_ip);

// int ip6_addr_to_str(const ip6_addr_t* a, char* buf, size_t buflen);

// long ipv6_to_nodeid(const char* ip6);

// int nodeid_to_ipv6(long node_id, ip6_addr_t* out);

// int dtn_routing_load_contacts(Routing_Function* routing, const char* filename);

#endif