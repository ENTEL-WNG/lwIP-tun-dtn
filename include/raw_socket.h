// raw_socket.h: Header file for raw socket interface managing IPv6 packet transmission on multiple
// network interfaces Copyright (C) 2025 Michael Karpov
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

#ifndef RAW_SOCKET_H
#define RAW_SOCKET_H

#include <netinet/in.h>

#include "lwip/ip6_addr.h"
#include "lwip/pbuf.h"

// extern int raw_socket_enp0s8;
// extern int raw_socket_enp0s9;

int dtn_init_raw_socket(void);

// Send an IPv6 packet through a raw socket on the appropriate interface
// Returns 0 on success, -1 on failure
int dtn_raw_socket_send_ipv6(struct pbuf* p, const ip6_addr_t* dest_addr);

void dtn_raw_socket_cleanup(void);

#endif