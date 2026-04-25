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

extern void dtn_storage_delete_packet_by_ip_header(Storage_Function* storage,
                                                   struct ip6_hdr* orig_ip6hdr);

// Structure for DTN custom ICMPv6 message payload
#pragma pack(1)
typedef struct {
    u32_t timestamp;
    u16_t fragment_offset;
    u16_t payload_length;
    u8_t reason_code;
} dtn_icmpv6_payload_t;
#pragma pack()

static err_t dtn_icmpv6_send_message(struct netif* netif, struct pbuf* p, u8_t type, u8_t code,
                                     u8_t reason) {
    struct ip6_hdr* orig_ip6hdr = (struct ip6_hdr*)p->payload;
    struct pbuf* q;
    struct icmp6_hdr* icmp6hdr;
    dtn_icmpv6_payload_t* dtn_payload;
    u16_t datalen;

    datalen = sizeof(dtn_icmpv6_payload_t) + IP6_HLEN + 8;

    q = pbuf_alloc(PBUF_IP, sizeof(struct icmp6_hdr) + datalen, PBUF_RAM);
    if (q == NULL) {
        printf("DTN ICMPv6: Failed to allocate pbuf for message\n");
        return ERR_MEM;
    }

    // Set up ICMP header
    icmp6hdr = (struct icmp6_hdr*)q->payload;
    icmp6hdr->type = type;
    icmp6hdr->code = code;
    icmp6hdr->data = 0;

    // Set up DTN payload
    dtn_payload = (dtn_icmpv6_payload_t*)(icmp6hdr + 1);
    dtn_payload->timestamp = sys_now();
    dtn_payload->fragment_offset = 0;
    dtn_payload->payload_length = lwip_ntohs(IP6H_PLEN(orig_ip6hdr));
    dtn_payload->reason_code = reason;

    // Copy IPv6 header + first 8 bytes of payload from original packet
    pbuf_copy_partial(p, (u8_t*)(dtn_payload + 1), IP6_HLEN + 8, 0);

    ip6_addr_t src_addr, dest_addr;

    ip6_addr_copy(src_addr, netif->ip6_addr[1]);

    if (!dtn_extract_custodian_option(p, &dest_addr)) {
        IP6_ADDR(&dest_addr, orig_ip6hdr->src.addr[0], orig_ip6hdr->src.addr[1],
                 orig_ip6hdr->src.addr[2], orig_ip6hdr->src.addr[3]);
    }

    // Calculate checksum
    icmp6hdr->chksum = 0;
    icmp6hdr->chksum = ip6_chksum_pseudo(q, IP6_NEXTH_ICMP6, q->tot_len, &src_addr, &dest_addr);

    // New Header
    struct pbuf* complete_pkt = pbuf_alloc(PBUF_IP, IP6_HLEN + q->tot_len, PBUF_RAM);
    if (complete_pkt == NULL) {
        printf("DTN ICMPv6: Failed to allocate pbuf for complete packet\n");
        pbuf_free(q);
        return ERR_MEM;
    }

    struct ip6_hdr* ip6hdr = (struct ip6_hdr*)complete_pkt->payload;
    IP6H_VTCFL_SET(ip6hdr, 6, 0, 0);
    IP6H_PLEN_SET(ip6hdr, q->tot_len);
    IP6H_NEXTH_SET(ip6hdr, IP6_NEXTH_ICMP6);
    IP6H_HOPLIM_SET(ip6hdr, 255);

    // Set source and destination
    ip6_addr_copy_to_packed(ip6hdr->src, src_addr);
    ip6_addr_copy_to_packed(ip6hdr->dest, dest_addr);

    // Copy ICMPv6 message after IPv6 header
    if (pbuf_copy_partial(q, (u8_t*)complete_pkt->payload + IP6_HLEN, q->tot_len, 0) !=
        q->tot_len) {
        printf("DTN ICMPv6: Failed to copy ICMPv6 message to complete packet\n");
        pbuf_free(q);
        pbuf_free(complete_pkt);
        return ERR_BUF;
    }

    pbuf_free(q);

    err_t err = raw_socket_send_ipv6(complete_pkt, &dest_addr) == 0 ? ERR_OK : ERR_IF;

    char dst_str[IP6ADDR_STRLEN_MAX];
    char src_addr_str[IP6ADDR_STRLEN_MAX];
    ip6addr_ntoa_r(&dest_addr, dst_str, sizeof(dst_str));
    ip6addr_ntoa_r(&src_addr, src_addr_str, sizeof(src_addr_str));

    if (err != ERR_OK) {
        DTN_ERROR("DTN ICMPv6: Failed to send message to %s via raw socket, err=%d", dst_str, err);
    } else {
        DTN_INFO("DTN ICMPv6: Sent type %d code %d from %s to %s via raw socket", type, code,
                 src_addr_str, dst_str);
    }

    pbuf_free(complete_pkt);
    return err;
}

// Send DTN-PCK-RECEIVED message
void dtn_icmpv6_send_pck_received(struct netif* netif, struct pbuf* p, u8_t code) {
    dtn_icmpv6_send_message(netif, p, ICMP6_TYPE_DTN_PCK_RECEIVED, code, 0);
}

// Send DTN-PCK-FORWARDED message
void dtn_icmpv6_send_pck_forwarded(struct netif* netif, struct pbuf* p, u8_t code) {
    dtn_icmpv6_send_message(netif, p, ICMP6_TYPE_DTN_PCK_FORWARDED, code, 0);
}

// Send DTN-PCK-DELIVERED message
void dtn_icmpv6_send_pck_delivered(struct netif* netif, struct pbuf* p, u8_t code) {
    dtn_icmpv6_send_message(netif, p, ICMP6_TYPE_DTN_PCK_DELIVERED, code, 0);
}

// Send DTN-PCK-DELETED message with additional reason
void dtn_icmpv6_send_pck_deleted(struct netif* netif, struct pbuf* p, u8_t code, u8_t reason) {
    dtn_icmpv6_send_message(netif, p, ICMP6_TYPE_DTN_PCK_DELETED, code, reason);
}

// Extract original IPv6 header from an ICMPv6 message
static struct ip6_hdr* extract_original_header(struct icmp6_hdr* icmp6hdr) {
    // Skip ICMP header and DTN payload to get to the original IPv6 header
    dtn_icmpv6_payload_t* dtn_payload = (dtn_icmpv6_payload_t*)(icmp6hdr + 1);
    return (struct ip6_hdr*)((u8_t*)dtn_payload + sizeof(dtn_icmpv6_payload_t));
}

// Convert a packed IPv6 address to an ip6_addr_t structure
static void packed_ip6_addr_to_ip6_addr_t(const u32_t packed_addr[4], ip6_addr_t* ip6_addr) {
    IP6_ADDR(ip6_addr, packed_addr[0], packed_addr[1], packed_addr[2], packed_addr[3]);
}

// Process incoming DTN ICMPv6 message
u8_t dtn_icmpv6_process(struct pbuf* p, ip6_addr_t* src_addr) {
    extern DTN_Module* global_dtn_module;

    if (!p || !p->payload || !global_dtn_module || !global_dtn_module->storage ||
        !global_dtn_module->controller) {
        return 0;
    }

    struct icmp6_hdr* icmp6hdr = (struct icmp6_hdr*)p->payload;
    dtn_icmpv6_payload_t* dtn_payload;

    // Check if this is a DTN ICMPv6 message
    switch (icmp6hdr->type) {
        case ICMP6_TYPE_DTN_PCK_RECEIVED: {
            dtn_payload = (dtn_icmpv6_payload_t*)(icmp6hdr + 1);

            char src_addr_str[IP6ADDR_STRLEN_MAX];
            ip6addr_ntoa_r(&src_addr, src_addr_str, sizeof(src_addr_str));

            DTN_INFO(
                "DTN ICMPv6: Received PCK-RECEIVED type %d code %d from %s, timestamp %u, reason "
                "%d",
                icmp6hdr->type, icmp6hdr->code, src_addr_str, dtn_payload->timestamp,
                dtn_payload->reason_code);

            // Extract original IPv6 header
            struct ip6_hdr* orig_ip6hdr = extract_original_header(icmp6hdr);

            // Delete the stored packet
            struct pbuf* icmp_with_ipv6 = pbuf_alloc(PBUF_RAW, IP6_HLEN + p->tot_len, PBUF_RAM);
            if (icmp_with_ipv6 != NULL) {
                // Copy the outer IPv6 header
                struct ip6_hdr* outer_ipv6 = (struct ip6_hdr*)icmp_with_ipv6->payload;
                // Set basic IPv6 header fields
                IP6H_VTCFL_SET(outer_ipv6, 6, 0, 0);
                IP6H_PLEN_SET(outer_ipv6, p->tot_len);
                IP6H_NEXTH_SET(outer_ipv6, IP6_NEXTH_ICMP6);
                IP6H_HOPLIM_SET(outer_ipv6, 255);
                // Copy source and destination from current IP context
                memcpy(&outer_ipv6->src, ip6_current_src_addr(), sizeof(ip6_addr_p_t));
                memcpy(&outer_ipv6->dest, ip6_current_dest_addr(), sizeof(ip6_addr_p_t));

                // Copy the ICMP payload
                if (pbuf_copy_partial(p, (u8_t*)icmp_with_ipv6->payload + IP6_HLEN, p->tot_len,
                                      0) == p->tot_len) {
                    dtn_storage_delete_packet_by_icmp_data(global_dtn_module->storage,
                                                           icmp_with_ipv6);
                } else {
                    dtn_storage_delete_packet_by_ip_header(global_dtn_module->storage, orig_ip6hdr);
                }
                pbuf_free(icmp_with_ipv6);
            } else {
                dtn_storage_delete_packet_by_ip_header(global_dtn_module->storage, orig_ip6hdr);
            }

            // Remove tracking for this destination
            ip6_addr_t dest_addr;

            u32_t temp_addr[4];
            memcpy(temp_addr, orig_ip6hdr->dest.addr, sizeof(temp_addr));
            packed_ip6_addr_to_ip6_addr_t(temp_addr, &dest_addr);

            // Remove destination from forwarding tracking list
            dtn_controller_remove_tracking(global_dtn_module->controller, &dest_addr);

            return 1;
        }

        case ICMP6_TYPE_DTN_PCK_FORWARDED: {
            dtn_payload = (dtn_icmpv6_payload_t*)(icmp6hdr + 1);

            char src_addr_str[IP6ADDR_STRLEN_MAX] = {0};
            ip6addr_ntoa_r(ip6_current_src_addr(), src_addr_str, sizeof(src_addr_str));

            printf(
                "DTN ICMPv6: Received PCK-FORWARDED type %d code %d from %s, timestamp %u, reason "
                "%d\n",
                icmp6hdr->type, icmp6hdr->code, src_addr_str, dtn_payload->timestamp,
                dtn_payload->reason_code);

            return 1;
        }

        case ICMP6_TYPE_DTN_PCK_DELIVERED: {
            dtn_payload = (dtn_icmpv6_payload_t*)(icmp6hdr + 1);

            char src_addr_str[IP6ADDR_STRLEN_MAX] = {0};
            ip6addr_ntoa_r(ip6_current_src_addr(), src_addr_str, sizeof(src_addr_str));

            printf(
                "DTN ICMPv6: Received PCK-DELIVERED type %d code %d from %s, timestamp %u, reason "
                "%d\n",
                icmp6hdr->type, icmp6hdr->code, src_addr_str, dtn_payload->timestamp,
                dtn_payload->reason_code);

            return 1;
        }

        case ICMP6_TYPE_DTN_PCK_DELETED: {
            dtn_payload = (dtn_icmpv6_payload_t*)(icmp6hdr + 1);

            char src_addr_str[IP6ADDR_STRLEN_MAX] = {0};
            ip6addr_ntoa_r(ip6_current_src_addr(), src_addr_str, sizeof(src_addr_str));

            printf(
                "DTN ICMPv6: Received PCK-DELETED type %d code %d from %s, timestamp %u, reason "
                "%d\n",
                icmp6hdr->type, icmp6hdr->code, src_addr_str, dtn_payload->timestamp,
                dtn_payload->reason_code);

            return 1;
        }
    }

    return 0;
}