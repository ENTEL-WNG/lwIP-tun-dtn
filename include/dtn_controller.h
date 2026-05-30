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

void dtn_controller_process_incoming(struct pbuf* p, struct netif* inp_netif);
void dtn_controller_attempt_forward_stored(struct netif* netif_out);
dtn_controller_process_outgoing_result_t dtn_controller_process_outgoing(
    struct pbuf* p, DtnRoutingResult* out_routing_result);

#endif
