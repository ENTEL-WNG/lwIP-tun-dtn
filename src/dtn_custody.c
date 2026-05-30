// dtn_custody.c: Implementation of custody transfer using IPv6 hop-by-hop extension headers for DTN
// reliability Copyright (C) 2025 Michael Karpov
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

#include "dtn_custody.h"

#include <string.h>

#include "lwip/ip6.h"
#include "lwip/pbuf.h"

// Define IPv6 Hop-by-Hop next-header value for lwIP
#ifndef IP6_NEXTH_HOPOPTS
#define IP6_NEXTH_HOPOPTS 0
#endif

// Wire layout (32 bytes, hdr_ext_len = 3):
//   [next_header(1)][hdr_ext_len=3(1)]
//   [0x1E(1)][16(1)][custodian_addr×16]   ← TLV option 1: custody (18 bytes)
//   [0x1F(1)][ 4(1)][packet_id×4]         ← TLV option 2: packet ID (6 bytes)
//   [0x00×6]                               ← six Pad1 bytes to reach 32-byte alignment
#pragma pack(push, 1)
struct hbh_hdr {
    // Fixed HBH header (2 bytes)
    uint8_t next_header;
    uint8_t hdr_ext_len;         // = (HBH_OPT_HDR_LEN / 8) - 1 = 3

    // TLV Option 1: Custodian address (18 bytes)
    uint8_t cust_opt_type;       // = CUSTODY_OPTION_TYPE (0x1E)
    uint8_t cust_opt_data_len;   // = 16
    uint8_t addr[16];

    // TLV Option 2: Packet ID (6 bytes)
    uint8_t pkt_id_opt_type;     // = PACKET_ID_OPTION_TYPE (0x1F)
    uint8_t pkt_id_opt_data_len; // = 4
    uint8_t packet_id_bytes[4];  // per-hop storage row ID, network byte order

    // Padding to reach HBH_OPT_HDR_LEN = 32 bytes  (32 - 26 = 6)
    uint8_t pad[HBH_OPT_HDR_LEN - 26];
};
#pragma pack(pop)

// Helper: fill every field of an hbh_hdr in one place.
static void hbh_fill(struct hbh_hdr* hbh, uint8_t next_header,
                     const ip6_addr_t* custodian, u32_t packet_id) {
    hbh->next_header        = next_header;
    hbh->hdr_ext_len        = (HBH_OPT_HDR_LEN / 8) - 1;  // = 3
    hbh->cust_opt_type      = CUSTODY_OPTION_TYPE;
    hbh->cust_opt_data_len  = 16;
    memcpy(hbh->addr, custodian->addr, 16);
    hbh->pkt_id_opt_type      = PACKET_ID_OPTION_TYPE;
    hbh->pkt_id_opt_data_len  = 4;
    uint32_t pid_net = htonl(packet_id);
    memcpy(hbh->packet_id_bytes, &pid_net, 4);
    memset(hbh->pad, 0, sizeof(hbh->pad));
}

bool dtn_add_custodian_option(struct pbuf** p, const ip6_addr_t* custodian) {
    if (!p || !*p || !custodian)
        return false;
    struct pbuf* orig = *p;
    struct ip6_hdr* ip6hdr = (struct ip6_hdr*)orig->payload;

    uint8_t old_nexth   = IP6H_NEXTH(ip6hdr);
    uint16_t orig_plen  = IP6H_PLEN(ip6hdr);
    uint16_t new_plen   = orig_plen + HBH_OPT_HDR_LEN;

    struct pbuf* newp = pbuf_alloc(PBUF_RAW, IP6_HLEN + new_plen, PBUF_RAM);
    if (!newp)
        return false;

    // Copy and update IPv6 header
    memcpy(newp->payload, ip6hdr, IP6_HLEN);
    struct ip6_hdr* new_ip6 = (struct ip6_hdr*)newp->payload;
    IP6H_NEXTH_SET(new_ip6, IP6_NEXTH_HOPOPTS);
    IP6H_PLEN_SET(new_ip6, new_plen);

    // Build HBH extension header (packet_id = 0 on first hop)
    struct hbh_hdr* hbh = (struct hbh_hdr*)((uint8_t*)newp->payload + IP6_HLEN);
    hbh_fill(hbh, old_nexth, custodian, 0);

    // Copy original payload after HBH
    uint8_t*       dst = (uint8_t*)newp->payload + IP6_HLEN + HBH_OPT_HDR_LEN;
    const uint8_t* src = (const uint8_t*)orig->payload + IP6_HLEN;
    memcpy(dst, src, orig->tot_len - IP6_HLEN);

    pbuf_free(orig);
    *p = newp;
    return true;
}

bool dtn_extract_custodian_option(const struct pbuf* p, ip6_addr_t* custodian_out) {
    if (!p || !custodian_out)
        return false;
    const struct ip6_hdr* ip6hdr = (const struct ip6_hdr*)p->payload;
    uint8_t nexth = IP6H_NEXTH(ip6hdr);
    const uint8_t* ptr = (const uint8_t*)ip6hdr + IP6_HLEN;

    // Walk extension headers until we find Hop-by-Hop (type 0)
    while (nexth != IP6_NEXTH_HOPOPTS && nexth != IP6_NEXTH_NONE) {
        const uint8_t* ext = ptr;
        nexth = ext[0];
        uint8_t elen = (uint8_t)((ext[1] + 1) * 8);
        ptr += elen;
    }
    if (nexth != IP6_NEXTH_HOPOPTS)
        return false;

    const struct hbh_hdr* hbh = (const struct hbh_hdr*)ptr;
    if (hbh->cust_opt_type != CUSTODY_OPTION_TYPE || hbh->cust_opt_data_len != 16)
        return false;
    memcpy(custodian_out->addr, hbh->addr, 16);
    return true;
}

bool dtn_strip_custodian_option(struct pbuf** p) {
    if (!p || !*p)
        return false;
    struct pbuf* orig = *p;
    struct ip6_hdr* ip6hdr = (struct ip6_hdr*)orig->payload;

    if (IP6H_NEXTH(ip6hdr) != IP6_NEXTH_HOPOPTS)
        return false;

    const struct hbh_hdr* hbh = (const struct hbh_hdr*)((uint8_t*)orig->payload + IP6_HLEN);
    uint8_t  next_nexth = hbh->next_header;
    uint16_t hbh_len    = HBH_OPT_HDR_LEN;
    uint16_t orig_plen  = IP6H_PLEN(ip6hdr);
    uint16_t new_plen   = orig_plen - hbh_len;

    struct pbuf* newp = pbuf_alloc(PBUF_RAW, IP6_HLEN + new_plen, PBUF_RAM);
    if (!newp)
        return false;

    memcpy(newp->payload, ip6hdr, IP6_HLEN);
    struct ip6_hdr* new_ip6 = (struct ip6_hdr*)newp->payload;
    IP6H_NEXTH_SET(new_ip6, next_nexth);
    IP6H_PLEN_SET(new_ip6, new_plen);

    uint8_t*       dst = (uint8_t*)newp->payload + IP6_HLEN;
    const uint8_t* src = (const uint8_t*)orig->payload + IP6_HLEN + hbh_len;
    memcpy(dst, src, orig->tot_len - IP6_HLEN - hbh_len);

    pbuf_free(orig);
    *p = newp;
    return true;
}

struct pbuf* dtn_update_or_add_custodian_option(const struct pbuf* orig,
                                                const ip6_addr_t* custodian,
                                                u32_t packet_id) {
    if (!orig || !custodian)
        return NULL;

    const struct ip6_hdr* ip6hdr = (const struct ip6_hdr*)orig->payload;

    if (IP6H_NEXTH(ip6hdr) == IP6_NEXTH_HOPOPTS) {
        const struct hbh_hdr* hbh =
            (const struct hbh_hdr*)((const uint8_t*)orig->payload + IP6_HLEN);

        if (hbh->cust_opt_type == CUSTODY_OPTION_TYPE && hbh->cust_opt_data_len == 16) {
            // Already has our custody option — copy packet and update address + packet_id.
            struct pbuf* newp = pbuf_alloc(PBUF_RAW, orig->tot_len, PBUF_RAM);
            if (!newp)
                return NULL;
            memcpy(newp->payload, orig->payload, orig->tot_len);
            struct hbh_hdr* new_hbh =
                (struct hbh_hdr*)((uint8_t*)newp->payload + IP6_HLEN);
            memcpy(new_hbh->addr, custodian->addr, 16);
            uint32_t pid_net = htonl(packet_id);
            memcpy(new_hbh->packet_id_bytes, &pid_net, 4);
            return newp;
        }

        // Has a different HBH — replace it in-place (same total size assumed).
        uint8_t  next_nexth = hbh->next_header;
        uint16_t orig_plen  = IP6H_PLEN(ip6hdr);

        struct pbuf* newp = pbuf_alloc(PBUF_RAW, IP6_HLEN + orig_plen, PBUF_RAM);
        if (!newp)
            return NULL;

        memcpy(newp->payload, ip6hdr, IP6_HLEN);
        struct ip6_hdr* new_ip6 = (struct ip6_hdr*)newp->payload;
        IP6H_NEXTH_SET(new_ip6, IP6_NEXTH_HOPOPTS);
        IP6H_PLEN_SET(new_ip6, orig_plen);

        struct hbh_hdr* new_hbh =
            (struct hbh_hdr*)((uint8_t*)newp->payload + IP6_HLEN);
        hbh_fill(new_hbh, next_nexth, custodian, packet_id);

        uint8_t*       dst = (uint8_t*)newp->payload + IP6_HLEN + HBH_OPT_HDR_LEN;
        const uint8_t* src = (const uint8_t*)orig->payload + IP6_HLEN + HBH_OPT_HDR_LEN;
        memcpy(dst, src, orig->tot_len - IP6_HLEN - HBH_OPT_HDR_LEN);

        return newp;
    }

    // No HBH header — prepend one.
    uint8_t  old_nexth = IP6H_NEXTH(ip6hdr);
    uint16_t orig_plen = IP6H_PLEN(ip6hdr);
    uint16_t new_plen  = orig_plen + HBH_OPT_HDR_LEN;

    struct pbuf* newp = pbuf_alloc(PBUF_RAW, IP6_HLEN + new_plen, PBUF_RAM);
    if (!newp)
        return NULL;

    memcpy(newp->payload, ip6hdr, IP6_HLEN);
    struct ip6_hdr* new_ip6 = (struct ip6_hdr*)newp->payload;
    IP6H_NEXTH_SET(new_ip6, IP6_NEXTH_HOPOPTS);
    IP6H_PLEN_SET(new_ip6, new_plen);

    struct hbh_hdr* new_hbh =
        (struct hbh_hdr*)((uint8_t*)newp->payload + IP6_HLEN);
    hbh_fill(new_hbh, old_nexth, custodian, packet_id);

    uint8_t*       dst = (uint8_t*)newp->payload + IP6_HLEN + HBH_OPT_HDR_LEN;
    const uint8_t* src = (const uint8_t*)orig->payload + IP6_HLEN;
    memcpy(dst, src, orig->tot_len - IP6_HLEN);

    return newp;
}

bool dtn_extract_packet_id_from_hbh(const struct pbuf* p, u32_t* packet_id_out) {
    if (!p || !packet_id_out)
        return false;
    *packet_id_out = 0;
    const struct ip6_hdr* ip6hdr = (const struct ip6_hdr*)p->payload;
    if (IP6H_NEXTH(ip6hdr) != IP6_NEXTH_HOPOPTS)
        return false;
    const struct hbh_hdr* hbh =
        (const struct hbh_hdr*)((const uint8_t*)ip6hdr + IP6_HLEN);
    if (hbh->cust_opt_type    != CUSTODY_OPTION_TYPE   || hbh->cust_opt_data_len    != 16)
        return false;
    if (hbh->pkt_id_opt_type  != PACKET_ID_OPTION_TYPE || hbh->pkt_id_opt_data_len  != 4)
        return false;
    uint32_t pid_net;
    memcpy(&pid_net, hbh->packet_id_bytes, 4);
    *packet_id_out = ntohl(pid_net);
    return (*packet_id_out != 0);
}
