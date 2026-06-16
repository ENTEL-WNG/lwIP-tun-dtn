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

// Wire layout (24 bytes, hdr_ext_len = 2):
//   [next_header(1)][hdr_ext_len=2(1)]
//   [0x1E(1)][16(1)][custodian_addr×16]   ← TLV option 1: custody (18 bytes)
//   [0x00×4]                               ← four Pad1 bytes to reach 24-byte alignment
#pragma pack(push, 1)
struct hbh_hdr {
    uint8_t next_header;
    uint8_t hdr_ext_len;        // = (HBH_OPT_HDR_LEN / 8) - 1 = 2
    uint8_t cust_opt_type;      // = CUSTODY_OPTION_TYPE (0x1E)
    uint8_t cust_opt_data_len;  // = 16
    uint8_t addr[16];           // custodian IPv6 address
    uint8_t pad[4];             // Pad1 × 4 to reach 24 bytes
};
#pragma pack(pop)

// Fill every field of hbh_hdr in one place.
static void hbh_fill(struct hbh_hdr* hbh, uint8_t next_header, const ip6_addr_t* custodian) {
    hbh->next_header       = next_header;
    hbh->hdr_ext_len       = (HBH_OPT_HDR_LEN / 8) - 1;  // = 2
    hbh->cust_opt_type     = CUSTODY_OPTION_TYPE;
    hbh->cust_opt_data_len = 16;
    memcpy(hbh->addr, custodian->addr, 16);
    memset(hbh->pad, 0, sizeof(hbh->pad));
}

// True only if the first (contiguous) pbuf holds at least @min_len bytes.
// Guards both out-of-bounds header reads and the unsigned `tot_len - N`
// length math below from underflowing on runt / malformed packets.
static bool pbuf_has_bytes(const struct pbuf* p, size_t min_len) {
    return p->len >= min_len && p->tot_len >= min_len;
}

// ---------------------------------------------------------------------------
// Public custody-option API
// ---------------------------------------------------------------------------

bool dtn_add_custodian_option(struct pbuf** p, const ip6_addr_t* custodian) {
    if (!p || !*p || !custodian)
        return false;
    struct pbuf* orig = *p;
    if (!pbuf_has_bytes(orig, IP6_HLEN))
        return false;
    struct ip6_hdr* ip6hdr = (struct ip6_hdr*)orig->payload;

    uint8_t  old_nexth  = IP6H_NEXTH(ip6hdr);
    uint16_t orig_plen  = IP6H_PLEN(ip6hdr);
    uint16_t new_plen   = orig_plen + HBH_OPT_HDR_LEN;

    struct pbuf* newp = pbuf_alloc(PBUF_RAW, IP6_HLEN + new_plen, PBUF_RAM);
    if (!newp)
        return false;

    // Copy and update IPv6 header.
    memcpy(newp->payload, ip6hdr, IP6_HLEN);
    struct ip6_hdr* new_ip6 = (struct ip6_hdr*)newp->payload;
    IP6H_NEXTH_SET(new_ip6, IP6_NEXTH_HOPOPTS);
    IP6H_PLEN_SET(new_ip6, new_plen);

    // Build HBH extension header.
    struct hbh_hdr* hbh = (struct hbh_hdr*)((uint8_t*)newp->payload + IP6_HLEN);
    hbh_fill(hbh, old_nexth, custodian);

    // Copy original payload after HBH.
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

    // The Hop-by-Hop Options header, if present, MUST immediately follow the
    // IPv6 header (RFC 8200) — and that is exactly where dtn_add_custodian_option
    // inserts it. So there is no extension-header chain to walk: just check the
    // next-header field directly. (The previous walk had no bounds check and
    // could spin forever when a length byte truncated to 0.)
    if (p->len < IP6_HLEN + sizeof(struct hbh_hdr))
        return false;

    const struct ip6_hdr* ip6hdr = (const struct ip6_hdr*)p->payload;
    if (IP6H_NEXTH(ip6hdr) != IP6_NEXTH_HOPOPTS)
        return false;

    const struct hbh_hdr* hbh = (const struct hbh_hdr*)((const uint8_t*)ip6hdr + IP6_HLEN);
    if (hbh->cust_opt_type != CUSTODY_OPTION_TYPE || hbh->cust_opt_data_len != 16)
        return false;

    memcpy(custodian_out->addr, hbh->addr, 16);
    return true;
}

bool dtn_strip_custodian_option(struct pbuf** p) {
    if (!p || !*p)
        return false;
    struct pbuf* orig = *p;
    if (!pbuf_has_bytes(orig, IP6_HLEN + sizeof(struct hbh_hdr)))
        return false;
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
                                                const ip6_addr_t* custodian) {
    if (!orig || !custodian)
        return NULL;
    if (!pbuf_has_bytes(orig, IP6_HLEN))
        return NULL;

    const struct ip6_hdr* ip6hdr = (const struct ip6_hdr*)orig->payload;

    if (IP6H_NEXTH(ip6hdr) == IP6_NEXTH_HOPOPTS) {
        if (!pbuf_has_bytes(orig, IP6_HLEN + sizeof(struct hbh_hdr)))
            return NULL;
        const struct hbh_hdr* hbh =
            (const struct hbh_hdr*)((const uint8_t*)orig->payload + IP6_HLEN);

        if (hbh->cust_opt_type == CUSTODY_OPTION_TYPE && hbh->cust_opt_data_len == 16) {
            // Already has our custody option — copy packet and update custodian address.
            struct pbuf* newp = pbuf_alloc(PBUF_RAW, orig->tot_len, PBUF_RAM);
            if (!newp)
                return NULL;
            memcpy(newp->payload, orig->payload, orig->tot_len);
            struct hbh_hdr* new_hbh =
                (struct hbh_hdr*)((uint8_t*)newp->payload + IP6_HLEN);
            memcpy(new_hbh->addr, custodian->addr, 16);
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
        hbh_fill(new_hbh, next_nexth, custodian);

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
    hbh_fill(new_hbh, old_nexth, custodian);

    uint8_t*       dst = (uint8_t*)newp->payload + IP6_HLEN + HBH_OPT_HDR_LEN;
    const uint8_t* src = (const uint8_t*)orig->payload + IP6_HLEN;
    memcpy(dst, src, orig->tot_len - IP6_HLEN);

    return newp;
}
