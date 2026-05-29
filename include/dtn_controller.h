// dtn_controller.h: Header file for the DTN Controller that manages packet forwarding and storage
// decisions in delay-tolerant networks Copyright (C) 2025 Michael Karpov
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

#ifndef DTN_CONTROLLER_H
#define DTN_CONTROLLER_H

#include <stdbool.h>

#include "dtn_module.h"
#include "dtn_routing.h"
#include "dtn_storage.h"
#include "lwip/netif.h"
#include "raw_socket.h"

// #define MAX_DESTINATIONS 10
// #define FORWARDING_RETRY_DELAY_MS 30000  // 30 seconds delay between retransmissions
// #define MAX_FORWARDING_RETRIES 10        // Max retries

// typedef struct {
//     ip6_addr_t destination;
//     u32_t last_attempt_time;
//     int retry_count;
//     bool is_valid;
// } ForwardingAttempt;

typedef struct DTN_Controller {
    DTN_Module* parent_module;
} DTN_Controller;

typedef enum {
    DTN_CONTROLLER_PROCESS_OUTGOING_OK = 0,
    DTN_CONTROLLER_PROCESS_OUTGOING_SEND = 1,
    DTN_CONTROLLER_PROCESS_OUTGOING_STORE = 2,
    DTN_CONTROLLER_PROCESS_OUTGOING_ERR = -1,
} dtn_controller_process_outgoing_result_t;

typedef enum {
    DTN_CONTROLLER_STORE_OK = 0,
    DTN_CONTROLLER_STORE_ERR = -1,
} dtn_controller_store_result_t;

DTN_Controller* dtn_controller_create(DTN_Module* parent);
void dtn_controller_destroy(DTN_Controller* controller);
void dtn_controller_process_incoming(DTN_Controller* controller, struct pbuf* p,
                                     struct netif* inp_netif);
void dtn_controller_attempt_forward_stored(DTN_Controller* controller, struct netif* netif_out);
int dtn_controller_process_icmpv6(DTN_Controller* controller, struct pbuf* p, ip6_addr_t* src_addr);

dtn_controller_process_outgoing_result_t dtn_controller_process_outgoing(
    DTN_Controller* controller, struct pbuf* p, DtnRoutingResult* out_routing_result);

dtn_storage_store_packet_result_t dtn_controller_store(Storage_Function* storage, struct pbuf* p,
                                                       const DtnRoutingResult* routing_result);
dtn_socket_result_t dtn_controller_send(struct pbuf* p, DtnRoutingResult routing_result);

#endif