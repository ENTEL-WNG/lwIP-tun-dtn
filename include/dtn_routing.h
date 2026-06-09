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
    /* Persistent Python CGR state — initialised once in dtn_routing_create,
     * released in dtn_routing_destroy.  Stored as void* to avoid pulling
     * Python.h into every translation unit that includes this header. */
    void* py_module;        /* PyObject*: py_cgr_lib module              */
    void* py_cgr_yen;       /* PyObject*: module.cgr_yen callable        */
    void* py_fwd_candidate; /* PyObject*: module.fwd_candidate callable  */
    void* py_ipv6_packet;   /* PyObject*: module.ipv6_packet callable    */
    void* py_contact_plan;  /* PyObject*: loaded contact-plan object     */
} Routing_Function;

typedef enum {
    DTN_ROUTING_OK = 0,
    DTN_ROUTING_NO_ROUTE = 1,
    DTN_ROUTING_ERR = -1,
} dtn_routing_result_t;

typedef struct {
    int next_hop_node_id;
    double min_delivery_time;
    double max_delivery_time;
    double best_delivery_time;
} DtnRoutingResult;

Routing_Function* dtn_routing_create(DTN_Module* parent);
void dtn_routing_destroy(Routing_Function* routing);
dtn_routing_result_t dtn_routing_is_node_id_dtn_node(int node_id, bool* is_dtn_node);
dtn_routing_result_t dtn_routing_is_next_hop_active(double current_time_in_ms, const DtnRoutingResult routing_result,
                                                    bool* is_next_hop_active);
dtn_routing_result_t dtn_routing_get_next_hop_node_id(double start_time_in_ms, double current_time_in_ms, struct ip6_hdr* ip6h,
                                                      DtnRoutingResult* result);
dtn_routing_result_t _dtn_routing_get_next_hop_node_id(double current_time_in_sec, long current_node_id, long src_node_id,
                                                       long dest_node_id, long deadline, long package_length_in_bits, long dscp,
                                                       DtnRoutingResult* result);
long dtn_routing_ipv6_to_nodeid(const char* ip6);

#endif