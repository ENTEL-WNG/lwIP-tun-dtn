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

#include "dtn_routing.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"

typedef enum {
    DTN_CONTROLLER_PROCESS_OUTGOING_OK = 0,
    DTN_CONTROLLER_PROCESS_OUTGOING_STORE = 1,
    DTN_CONTROLLER_PROCESS_OUTGOING_ERR = -1,
} dtn_controller_process_outgoing_result_t;

typedef enum {
    DTN_CONTROLLER_PROCESS_INCOMING_FORWARDED = 0,  // packet sent to next hop
    DTN_CONTROLLER_PROCESS_INCOMING_STORED = 1,     // packet stored for later forwarding
    DTN_CONTROLLER_PROCESS_INCOMING_LOCAL = 2,      // packet delivered to local stack
    DTN_CONTROLLER_PROCESS_INCOMING_ICMPV6 = 3,     // packet handled as DTN ICMPv6 control
    DTN_CONTROLLER_PROCESS_INCOMING_ERR = -1,       // invalid input or unrecoverable failure
} dtn_controller_process_incoming_result_t;

dtn_controller_process_incoming_result_t dtn_controller_process_incoming(struct pbuf* p, struct netif* inp_netif);
dtn_controller_process_outgoing_result_t dtn_controller_process_outgoing(struct pbuf* p, DtnRoutingResult* out_routing_result);
int dtn_controller_process_stored(void);
void dtn_controller_stats_timer_start(void);

#endif
