// dtn_icmpv6.h: Header file for custom ICMPv6 messages implementing DTN status reporting
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

#ifndef DTN_ICMPV6_H
#define DTN_ICMPV6_H

#include "dtn_module.h"
#include "lwip/icmp6.h"
#include "lwip/ip6_addr.h"
#include "lwip/pbuf.h"

// DTN ICMPv6 message types
typedef enum {
    ICMP6_TYPE_DTN_PCK_RECEIVED = 200,
    ICMP6_TYPE_DTN_PCK_FORWARDED = 201,
    ICMP6_TYPE_DTN_PCK_DELIVERED = 202,
    ICMP6_TYPE_DTN_PCK_DELETED = 203,
} dtn_icmpv6_msg_type_t;

// DTN ICMPv6 message codes (taken from BPv7)
#define ICMP6_CODE_DTN_NO_INFO 0
#define ICMP6_CODE_DTN_LIFETIME_EXP 1
#define ICMP6_CODE_DTN_UNI_LINK 2
#define ICMP6_CODE_DTN_TX_CANCELLED 3
#define ICMP6_CODE_DTN_DEPLETED_STORE 4
#define ICMP6_CODE_DTN_NO_DEST 5
#define ICMP6_CODE_DTN_NO_ROUTE 6
#define ICMP6_CODE_DTN_NO_CONTACT 7
#define ICMP6_CODE_DTN_HOP_LIMIT 9
#define ICMP6_CODE_DTN_TRAFFIC_PARED 10

typedef enum {
    DTN_ICMPV6_SEND_MESSAGE_OK = 0,
    DTN_ICMPV6_SEND_MESSAGE_STORED = 1,
    DTN_ICMPV6_SEND_MESSAGE_ERR = -1,
    DTN_ICMPV6_SEND_MESSAGE_STORED_ERR = -2,
} dtn_icmpv6_send_message_result_t;

typedef enum {
    DTN_ICMPV6_PROCESS_OK = 0,
    DTN_ICMPV6_PROCESS_NOT_SUPPORTED = 1,
    DTN_ICMPV6_PROCESS_ERR = -1,
} dtn_icmpv6_process_result_t;

dtn_icmpv6_send_message_result_t dtn_icmpv6_send_pck_received(const struct pbuf* p, u8_t code);
dtn_icmpv6_send_message_result_t dtn_icmpv6_send_pck_forwarded(const struct pbuf* p, u8_t code);
dtn_icmpv6_send_message_result_t dtn_icmpv6_send_pck_delivered(const struct pbuf* p, u8_t code);
dtn_icmpv6_send_message_result_t dtn_icmpv6_send_pck_deleted(const struct pbuf* p, u8_t code,
                                                             u8_t reason);

dtn_icmpv6_process_result_t dtn_icmpv6_process(struct pbuf* p);

#endif