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

#pragma pack(push, 1)
struct hbh_hdr {
    uint8_t next_header;
    uint8_t hdr_ext_len;
    uint8_t opt_type;
    uint8_t opt_data_len;
    uint8_t addr[16];
    uint8_t packet_id_bytes[4];  // per-hop storage ID in network byte order (was: pad)
};
#pragma pack(pop)

bool dtn_add_custodian_option(struct pbuf** p, const ip6_addr_t* custodian) {
    if (!p || !*p || !custodian)
        return false;
    struct pbuf* orig = *p;
    struct ip6_hdr* ip6hdr = (struct ip6_hdr*)orig->payload;

    uint8_t old_nexth = IP6H_NEXTH(ip6hdr);
    uint16_t orig_len = IP6H_PLEN(ip6hdr);
    uint16_t new_len = orig_len + HBH_OPT_HDR_LEN;

    // Allocate new pbuf for rebuilt packet
    struct pbuf* newp = pbuf_alloc(PBUF_RAW, IP6_HLEN + new_len, PBUF_RAM);
    if (!newp)
        return false;

    // 1. Copy IPv6 header and update
    memcpy(newp->payload, ip6hdr, IP6_HLEN);
    struct ip6_hdr* new_ip6 = newp->payload;
    IP6H_NEXTH_SET(new_ip6, IP6_NEXTH_HOPOPTS);
    IP6H_PLEN_SET(new_ip6, new_len);

    // 2. Build Hop-by-Hop header
    struct hbh_hdr* hbh = (struct hbh_hdr*)((uint8_t*)newp->payload + IP6_HLEN);
    hbh->next_header = old_nexth;
    hbh->hdr_ext_len = (HBH_OPT_HDR_LEN / 8) - 1;
    hbh->opt_type = CUSTODY_OPTION_TYPE;
    hbh->opt_data_len = 20;  // 16 addr + 4 packet_id; all 20 bytes inside this option
    memcpy(hbh->addr, custodian->addr, 16);
    memset(hbh->packet_id_bytes, 0, sizeof(hbh->packet_id_bytes));

    // 3. Copy original payload after HBH
    uint8_t* dst = (uint8_t*)newp->payload + IP6_HLEN + HBH_OPT_HDR_LEN;
    uint8_t* src = (uint8_t*)orig->payload + IP6_HLEN;
    memcpy(dst, src, orig->tot_len - IP6_HLEN);

    // 4. Replace old packet
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

    // Skip extension headers until Hop-by-Hop
    while (nexth != IP6_NEXTH_HOPOPTS && nexth != IP6_NEXTH_NONE) {
        const uint8_t* ext = ptr;
        nexth = ext[0];
        uint8_t elen = (ext[1] + 1) * 8;
        ptr += elen;
    }
    if (nexth != IP6_NEXTH_HOPOPTS)
        return false;

    // ptr points at HBH header start
    const struct hbh_hdr* hbh = (const struct hbh_hdr*)ptr;
    if (hbh->opt_type != CUSTODY_OPTION_TYPE || hbh->opt_data_len != 20)
        return false;
    memcpy(custodian_out->addr, hbh->addr, 16);
    return true;
}

bool dtn_strip_custodian_option(struct pbuf** p) {
    if (!p || !*p)
        return false;
    struct pbuf* orig = *p;
    struct ip6_hdr* ip6hdr = (struct ip6_hdr*)orig->payload;

    // Check if packet has hop-by-hop header
    if (IP6H_NEXTH(ip6hdr) != IP6_NEXTH_HOPOPTS) {
        return false;  // No hop-by-hop header to strip
    }

    // Get the hop-by-hop header
    struct hbh_hdr* hbh = (struct hbh_hdr*)((uint8_t*)orig->payload + IP6_HLEN);
    uint8_t next_nexth = hbh->next_header;
    uint16_t hbh_len = HBH_OPT_HDR_LEN;

    // Calculate new packet length
    uint16_t orig_len = IP6H_PLEN(ip6hdr);
    uint16_t new_len = orig_len - hbh_len;

    // Allocate new pbuf without hop-by-hop header
    struct pbuf* newp = pbuf_alloc(PBUF_RAW, IP6_HLEN + new_len, PBUF_RAM);
    if (!newp)
        return false;

    // Copy IPv6 header and update
    memcpy(newp->payload, ip6hdr, IP6_HLEN);
    struct ip6_hdr* new_ip6 = newp->payload;
    IP6H_NEXTH_SET(new_ip6, next_nexth);  // Set next header to what HBH pointed to
    IP6H_PLEN_SET(new_ip6, new_len);

    // Copy remaining payload (skip HBH header)
    uint8_t* dst = (uint8_t*)newp->payload + IP6_HLEN;
    uint8_t* src = (uint8_t*)orig->payload + IP6_HLEN + hbh_len;
    memcpy(dst, src, orig->tot_len - IP6_HLEN - hbh_len);

    // Replace old packet
    pbuf_free(orig);
    *p = newp;
    return true;
}

struct pbuf* dtn_update_or_add_custodian_option(const struct pbuf* orig, const ip6_addr_t* custodian, u32_t packet_id) {
    if (!orig || !custodian)
        return NULL;

    // Encode packet_id in network byte order for wire storage.
    uint32_t pid_net = htonl(packet_id);

    const struct ip6_hdr* ip6hdr = (const struct ip6_hdr*)orig->payload;

    if (IP6H_NEXTH(ip6hdr) == IP6_NEXTH_HOPOPTS) {
        const struct hbh_hdr* hbh = (const struct hbh_hdr*)((const uint8_t*)orig->payload + IP6_HLEN);

        if (hbh->opt_type == CUSTODY_OPTION_TYPE && hbh->opt_data_len == 20) {
            // Already has our custody option — copy the packet, update address and packet_id.
            struct pbuf* newp = pbuf_alloc(PBUF_RAW, orig->tot_len, PBUF_RAM);
            if (!newp)
                return NULL;
            memcpy(newp->payload, orig->payload, orig->tot_len);
            struct hbh_hdr* new_hbh = (struct hbh_hdr*)((uint8_t*)newp->payload + IP6_HLEN);
            memcpy(new_hbh->addr, custodian->addr, 16);
            memcpy(new_hbh->packet_id_bytes, &pid_net, 4);
            return newp;
        }

        // Has a different HBH option — replace it with a fresh custody HBH.
        // The existing HBH is assumed to be exactly HBH_OPT_HDR_LEN bytes (our format),
        // so the new packet is the same total size.
        uint8_t next_nexth = hbh->next_header;
        uint16_t orig_plen = IP6H_PLEN(ip6hdr);

        struct pbuf* newp = pbuf_alloc(PBUF_RAW, IP6_HLEN + orig_plen, PBUF_RAM);
        if (!newp)
            return NULL;

        memcpy(newp->payload, ip6hdr, IP6_HLEN);
        struct ip6_hdr* new_ip6 = newp->payload;
        IP6H_NEXTH_SET(new_ip6, IP6_NEXTH_HOPOPTS);
        IP6H_PLEN_SET(new_ip6, orig_plen);

        struct hbh_hdr* new_hbh = (struct hbh_hdr*)((uint8_t*)newp->payload + IP6_HLEN);
        new_hbh->next_header = next_nexth;
        new_hbh->hdr_ext_len = (HBH_OPT_HDR_LEN / 8) - 1;
        new_hbh->opt_type = CUSTODY_OPTION_TYPE;
        new_hbh->opt_data_len = 20;  // 16 addr + 4 packet_id; all inside this option
        memcpy(new_hbh->addr, custodian->addr, 16);
        memcpy(new_hbh->packet_id_bytes, &pid_net, 4);

        // Copy payload that came after the old HBH header.
        uint8_t* dst = (uint8_t*)newp->payload + IP6_HLEN + HBH_OPT_HDR_LEN;
        const uint8_t* src = (const uint8_t*)orig->payload + IP6_HLEN + HBH_OPT_HDR_LEN;
        memcpy(dst, src, orig->tot_len - IP6_HLEN - HBH_OPT_HDR_LEN);

        return newp;
    }

    // No HBH header — add one.
    uint8_t old_nexth = IP6H_NEXTH(ip6hdr);
    uint16_t orig_plen = IP6H_PLEN(ip6hdr);
    uint16_t new_plen = orig_plen + HBH_OPT_HDR_LEN;

    struct pbuf* newp = pbuf_alloc(PBUF_RAW, IP6_HLEN + new_plen, PBUF_RAM);
    if (!newp)
        return NULL;

    memcpy(newp->payload, ip6hdr, IP6_HLEN);
    struct ip6_hdr* new_ip6 = newp->payload;
    IP6H_NEXTH_SET(new_ip6, IP6_NEXTH_HOPOPTS);
    IP6H_PLEN_SET(new_ip6, new_plen);

    struct hbh_hdr* new_hbh = (struct hbh_hdr*)((uint8_t*)newp->payload + IP6_HLEN);
    new_hbh->next_header = old_nexth;
    new_hbh->hdr_ext_len = (HBH_OPT_HDR_LEN / 8) - 1;
    new_hbh->opt_type = CUSTODY_OPTION_TYPE;
    new_hbh->opt_data_len = 16;
    memcpy(new_hbh->addr, custodian->addr, 16);
    memcpy(new_hbh->packet_id_bytes, &pid_net, 4);

    uint8_t* dst = (uint8_t*)newp->payload + IP6_HLEN + HBH_OPT_HDR_LEN;
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
    const struct hbh_hdr* hbh = (const struct hbh_hdr*)((const uint8_t*)ip6hdr + IP6_HLEN);
    if (hbh->opt_type != CUSTODY_OPTION_TYPE || hbh->opt_data_len != 20)
        return false;
    uint32_t pid_net;
    memcpy(&pid_net, hbh->packet_id_bytes, 4);
    *packet_id_out = ntohl(pid_net);
    return (*packet_id_out != 0);
}
