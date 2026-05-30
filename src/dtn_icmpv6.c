// dtn_icmpv6.c: Implementation of custom ICMPv6 messages for DTN packet status notifications
// (RECEIVED, FORWARDED, DELIVERED, DELETED) Copyright (C) 2025 Michael Karpov
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

#include "dtn_icmpv6.h"

#include <stdio.h>
#include <string.h>

#include "dtn_config.h"
#include "dtn_controller.h"
#include "dtn_custody.h"
#include "dtn_logger.h"
#include "dtn_module.h"
#include "dtn_storage.h"
#include "lwip/icmp6.h"
#include "lwip/inet_chksum.h"
#include "lwip/ip.h"
#include "lwip/ip6.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/prot/icmp6.h"
#include "lwip/prot/ip6.h"
#include "lwip/sys.h"
#include "raw_socket.h"

extern DTN_Module* global_dtn_module;
// extern void dtn_storage_delete_packet_by_ip_header(Storage_Function* storage,
//                                                    struct ip6_hdr* orig_ip6hdr);
// Structure for DTN custom ICMPv6 message payload
#pragma pack(1)
typedef struct {
    u32_t packet_id;       // per-hop storage ID echoed back for deletion; 0 = not stored
    u32_t timestamp;       // when this ICMPv6 message was created (ms)
    u16_t payload_length;  // original packet payload length
    u8_t reason_code;      // reason code (DELETED messages only)
} dtn_icmpv6_payload_t;
#pragma pack()

static dtn_icmpv6_send_message_result_t _dtn_icmpv6_send_message(const ip6_addr_t* src_addr, ip6_addr_t* dest_addr,
                                                                 DtnInterface* dtn_interface, const struct pbuf* p,
                                                                 dtn_icmpv6_msg_type_t type, u8_t code, u8_t reason) {
    const struct ip6_hdr* orig_ip6hdr = (const struct ip6_hdr*)p->payload;
    struct pbuf* q;
    struct icmp6_hdr* icmp6hdr;
    dtn_icmpv6_payload_t* dtn_payload;
    u16_t data_length;

    data_length = sizeof(dtn_icmpv6_payload_t) + IP6_HLEN + 8;

    q = pbuf_alloc(PBUF_IP, sizeof(struct icmp6_hdr) + data_length, PBUF_RAM);
    if (q == NULL) {
        DTN_ERROR("DTN ICMPv6: Failed to allocate pbuf for message");
        return DTN_ICMPV6_SEND_MESSAGE_ERR;
    }

    // Set up ICMP header
    icmp6hdr = (struct icmp6_hdr*)q->payload;
    icmp6hdr->type = type;
    icmp6hdr->code = code;
    icmp6hdr->data = 0;

    // Set up DTN payload
    dtn_payload = (dtn_icmpv6_payload_t*)(icmp6hdr + 1);
    u32_t pkt_id = 0;
    dtn_extract_packet_id_from_hbh(p, &pkt_id);
    dtn_payload->packet_id = lwip_htonl(pkt_id);
    dtn_payload->timestamp = sys_now();
    dtn_payload->payload_length = lwip_ntohs(IP6H_PLEN(orig_ip6hdr));
    dtn_payload->reason_code = reason;

    // Copy IPv6 header + first 8 bytes of payload from original packet
    pbuf_copy_partial(p, (u8_t*)(dtn_payload + 1), IP6_HLEN + 8, 0);

    // Calculate checksum
    icmp6hdr->chksum = 0;
    icmp6hdr->chksum = ip6_chksum_pseudo(q, IP6_NEXTH_ICMP6, q->tot_len, src_addr, dest_addr);

    // New Header
    struct pbuf* complete_pkt = pbuf_alloc(PBUF_IP, IP6_HLEN + q->tot_len, PBUF_RAM);
    if (complete_pkt == NULL) {
        DTN_ERROR("DTN ICMPv6: Failed to allocate pbuf for complete packet");
        pbuf_free(q);
        return DTN_ICMPV6_SEND_MESSAGE_ERR;
    }

    struct ip6_hdr* ip6hdr = (struct ip6_hdr*)complete_pkt->payload;
    IP6H_VTCFL_SET(ip6hdr, 6, 0, 0);
    IP6H_PLEN_SET(ip6hdr, q->tot_len);
    IP6H_NEXTH_SET(ip6hdr, IP6_NEXTH_ICMP6);
    IP6H_HOPLIM_SET(ip6hdr, 255);

    // Set source and destination
    ip6_addr_copy_to_packed(ip6hdr->src, *src_addr);
    ip6_addr_copy_to_packed(ip6hdr->dest, *dest_addr);

    // Copy ICMPv6 message after IPv6 header
    if (pbuf_copy_partial(q, (u8_t*)complete_pkt->payload + IP6_HLEN, q->tot_len, 0) != q->tot_len) {
        DTN_ERROR("DTN ICMPv6: Failed to copy ICMPv6 message to complete packet");
        pbuf_free(q);
        pbuf_free(complete_pkt);
        return DTN_ICMPV6_SEND_MESSAGE_ERR;
    }

    DtnRoutingResult routing_result;
    dtn_controller_process_outgoing_result_t outgoing_result = dtn_controller_process_outgoing(complete_pkt, &routing_result);

    dtn_icmpv6_send_message_result_t result;
    char src_str[IP6ADDR_STRLEN_MAX], dest_str[IP6ADDR_STRLEN_MAX];
    ip6addr_ntoa_r(src_addr, src_str, sizeof(src_str));
    ip6addr_ntoa_r(dest_addr, dest_str, sizeof(dest_str));
    switch (outgoing_result) {
        case DTN_CONTROLLER_PROCESS_OUTGOING_OK:
            dtn_socket_result_t socket_result = dtn_raw_socket_send_via_interface(complete_pkt, dtn_interface);

            if (socket_result == DTN_SOCKET_OK) {
                DTN_DEBUG("Successfully send ICMP6 message src: %s -> dest: %s | type %d | code %d", src_str, dest_str, type, code);
                result = DTN_ICMPV6_SEND_MESSAGE_OK;
                break;
            }
            DTN_ERROR(
                "Failed to send ICMP6 message src: %s -> dest: %s | type %d | code %d, error "
                "%d",
                src_str, dest_str, type, code, socket_result);
            result = DTN_ICMPV6_SEND_MESSAGE_ERR;
            break;
        case DTN_CONTROLLER_PROCESS_OUTGOING_STORE:
            dtn_storage_store_packet_result_t result = dtn_storage_store_packet(global_dtn_module->storage, complete_pkt, &routing_result);
            if (result == DTN_STORAGE_STORE_OK) {
                DTN_INFO("Successfully stored ICMP6 message src: %s -> dest: %s | type %d | code %d", src_str, dest_str, type, code);
                result = DTN_ICMPV6_SEND_MESSAGE_STORED;
                break;
            }
            DTN_ERROR("Failed stored ICMP6 message src: %s -> dest: %s | type %d | code %d", src_str, dest_str, type, code);
            result = DTN_ICMPV6_SEND_MESSAGE_STORED_ERR;
            break;
        default:
            result = DTN_ICMPV6_SEND_MESSAGE_ERR;
            break;
    }

    pbuf_free(complete_pkt);
    pbuf_free(q);
    return result;
}

static dtn_icmpv6_send_message_result_t dtn_icmpv6_send_message(const struct pbuf* p, dtn_icmpv6_msg_type_t type, u8_t code, u8_t reason) {
    ip6_addr_t src_addr, dest_addr, custodian_addr;

    bool has_custodian = dtn_extract_custodian_option(p, &custodian_addr);
    // We are assuming if no custodian than the previoys hop is not a dtn node and the we set
    // the src of the package as the custodian
    if (!has_custodian) {
        DTN_WARN("No custodian in dnt_icmpv6_send_pck_received");
        const struct ip6_hdr* ip6hdr = (const struct ip6_hdr*)p->payload;
        ip6_addr_copy_from_packed(custodian_addr, ip6hdr->src);
    }

    // Send to source of package
    if (type == ICMP6_TYPE_DTN_PCK_DELIVERED) {
        const struct ip6_hdr* ip6hdr = (const struct ip6_hdr*)p->payload;
        ip6_addr_copy_from_packed(dest_addr, ip6hdr->src);
    } else {
        ip6_addr_copy(dest_addr, custodian_addr);
    }

    // We are assuming that the destination node will be the one who send the message
    // so the id of the custodian address
    char custodian_addr_str[IP6ADDR_STRLEN_MAX];
    ip6addr_ntoa_r(&custodian_addr, custodian_addr_str, sizeof(custodian_addr_str));
    long dest_node_id = dtn_routing_ipv6_to_nodeid(custodian_addr_str);
    const DtnInterface* dtn_interface = dtn_raw_socket_get_interface_for_node((int)dest_node_id);
    if (!dtn_interface) {
        DTN_ERROR("No interface for custodian address %s", custodian_addr_str);
        return DTN_ICMPV6_SEND_MESSAGE_ERR;
    }

    ip6addr_aton(dtn_interface->local_addr, &src_addr);

    return _dtn_icmpv6_send_message(&src_addr, &dest_addr, dtn_interface, p, type, code, reason);
}

// Send DTN-PCK-RECEIVED message
dtn_icmpv6_send_message_result_t dtn_icmpv6_send_pck_received(const struct pbuf* p, u8_t code) {
    return dtn_icmpv6_send_message(p, ICMP6_TYPE_DTN_PCK_RECEIVED, code, 0);
}

// Send DTN-PCK-FORWARDED message
dtn_icmpv6_send_message_result_t dtn_icmpv6_send_pck_forwarded(const struct pbuf* p, u8_t code) {
    return dtn_icmpv6_send_message(p, ICMP6_TYPE_DTN_PCK_FORWARDED, code, 0);
}

// Send DTN-PCK-DELIVERED message
dtn_icmpv6_send_message_result_t dtn_icmpv6_send_pck_delivered(const struct pbuf* p, u8_t code) {
    return dtn_icmpv6_send_message(p, ICMP6_TYPE_DTN_PCK_DELIVERED, code, 0);
}

// Send DTN-PCK-DELETED message with additional reason
dtn_icmpv6_send_message_result_t dtn_icmpv6_send_pck_deleted(const struct pbuf* p, u8_t code, u8_t reason) {
    return dtn_icmpv6_send_message(p, ICMP6_TYPE_DTN_PCK_DELETED, code, reason);
}

// Process incoming DTN ICMPv6 message
dtn_icmpv6_process_result_t dtn_icmpv6_process(struct pbuf* p) {
    if (!p || !p->payload || p->tot_len < IP6_HLEN || !global_dtn_module || !global_dtn_module->storage) {
        return DTN_ICMPV6_PROCESS_ERR;
    }

    const struct ip6_hdr* ip6hdr = (const struct ip6_hdr*)p->payload;
    ip6_addr_t src_addr, dest_addr;
    ip6_addr_copy_from_packed(src_addr, ip6hdr->src);
    ip6_addr_copy_from_packed(dest_addr, ip6hdr->dest);
    char src_addr_str[IP6ADDR_STRLEN_MAX], dest_addr_str[IP6ADDR_STRLEN_MAX];
    ip6addr_ntoa_r(&src_addr, src_addr_str, sizeof(src_addr_str));
    ip6addr_ntoa_r(&dest_addr, dest_addr_str, sizeof(dest_addr_str));

    struct icmp6_hdr* icmp6hdr = (struct icmp6_hdr*)((u8_t*)p->payload + IP6_HLEN);
    dtn_icmpv6_payload_t* dtn_payload;

    // Check if this is a DTN ICMPv6 message
    switch ((dtn_icmpv6_msg_type_t)icmp6hdr->type) {
        case ICMP6_TYPE_DTN_PCK_RECEIVED: {
            dtn_payload = (dtn_icmpv6_payload_t*)(icmp6hdr + 1);
            u32_t packet_id = lwip_ntohl(dtn_payload->packet_id);
            DTN_INFO("ICMPv6|| src: %s -> dest: %s | type %d | code %d | packet_id: %u || process PCK_RECEIVED", src_addr_str,
                     dest_addr_str, icmp6hdr->type, icmp6hdr->code, packet_id);
            // if (packet_id != 0)
            // dtn_storage_delete_by_packet_id(global_dtn_module->storage, packet_id);
            return DTN_ICMPV6_PROCESS_OK;
        }

        case ICMP6_TYPE_DTN_PCK_FORWARDED: {
            dtn_payload = (dtn_icmpv6_payload_t*)(icmp6hdr + 1);
            DTN_INFO("ICMPv6|| src: %s -> dest: %s | type %d | code %d || process PCK_FORWARDED", src_addr_str, dest_addr_str,
                     icmp6hdr->type, icmp6hdr->code);

            return DTN_ICMPV6_PROCESS_OK;
        }

        case ICMP6_TYPE_DTN_PCK_DELIVERED: {
            dtn_payload = (dtn_icmpv6_payload_t*)(icmp6hdr + 1);
            DTN_INFO("ICMPv6|| src: %s -> dest: %s | type %d | code %d || process PCK_DELIVERED", src_addr_str, dest_addr_str,
                     icmp6hdr->type, icmp6hdr->code);

            return DTN_ICMPV6_PROCESS_OK;
        }

        case ICMP6_TYPE_DTN_PCK_DELETED: {
            dtn_payload = (dtn_icmpv6_payload_t*)(icmp6hdr + 1);
            DTN_INFO("ICMPv6|| src: %s -> dest: %s | type %d | code %d || process PCK_DELETED", src_addr_str, dest_addr_str, icmp6hdr->type,
                     icmp6hdr->code);

            return DTN_ICMPV6_PROCESS_OK;
        }

        default:
            return DTN_ICMPV6_PROCESS_NOT_SUPPORTED;
    }

    return DTN_ICMPV6_PROCESS_ERR;
}