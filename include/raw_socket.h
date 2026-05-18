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

#include "dtn_config.h"
#include "lwip/ip6_addr.h"
#include "lwip/pbuf.h"

typedef enum {
    DTN_SOCKET_OK = 0,
    DTN_SOCKET_ERR_CREATE = -1,        /* socket() failed                  */
    DTN_SOCKET_ERR_IFINDEX = -2,       /* ioctl(SIOCGIFINDEX) failed        */
    DTN_SOCKET_ERR_SOCKOPT = -3,       /* setsockopt(IPV6_HDRINCL) failed   */
    DTN_SOCKET_ERR_PKT_TOO_LARGE = -4, /* packet exceeds internal buffer    */
    DTN_SOCKET_ERR_COPY = -5,          /* pbuf_copy_partial failed          */
    DTN_SOCKET_ERR_BIND = -6,          /* SO_BINDTODEVICE failed            */
    DTN_SOCKET_ERR_SEND = -7,          /* sendto() failed                   */
    DTN_SOCKET_ERR_PARTIAL = -8,       /* sendto() sent fewer bytes         */
} dtn_socket_result_t;

dtn_socket_result_t dtn_init_raw_socket(void);

dtn_socket_result_t dtn_raw_socket_send_to_node_id(struct pbuf* p, int node_id,
                                                   const ip6_addr_t* dest_addr);
dtn_socket_result_t dtn_raw_socket_send_to_ipv6_address(struct pbuf* p,
                                                        const ip6_addr_t* dest_addr);
dtn_socket_result_t dtn_raw_socket_send_via_interface(struct pbuf* p, const ip6_addr_t* dest_addr,
                                                      const DtnInterface* dtn_interface);

void dtn_raw_socket_cleanup(void);

#endif