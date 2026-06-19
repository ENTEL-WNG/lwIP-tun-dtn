// dtn_utils.c: Utility functions shared across DTN components
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

#include "dtn_utils.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dtn_config.h"
#include "dtn_logger.h"
#include "lwip/ip6.h"
#include "lwip/ip6_addr.h"
#include "lwip/pbuf.h"

// ---------------------------------------------------------------------------
// Address helpers
// ---------------------------------------------------------------------------

bool dtn_utils_is_addr_equal(const ip6_addr_t* dest_addr, const char* addr) {
    ip6_addr_t local_addr;
    if (ip6addr_aton(addr, &local_addr)) {
        ip6_addr_t dest_nozone = *dest_addr;
#if LWIP_IPV6_SCOPES
        ip6_addr_set_zone(&dest_nozone, IP6_NO_ZONE);
        ip6_addr_set_zone(&local_addr, IP6_NO_ZONE);
#endif
        return ip6_addr_eq(&dest_nozone, &local_addr);
    }
    return false;
}

bool dtn_utils_is_local_addr(const ip6_addr_t* addr) {
    for (int i = 0; i < dtn_config.interface_count; i++) {
        if (dtn_utils_is_addr_equal(addr, dtn_config.interfaces[i].local_addr)) {
            return true;
        }
    }

    if (dtn_utils_is_addr_equal(addr, dtn_config.lwip_ipv6_addr)) {
        return true;
    }

    if (dtn_utils_is_addr_equal(addr, dtn_config.tun_ipv6_addr)) {
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Payload printing
// ---------------------------------------------------------------------------

const char* dtn_utils_print_packet_payload(const struct pbuf* p) {
    if (!p)
        return NULL;

    const struct ip6_hdr* ip6hdr = (const struct ip6_hdr*)p->payload;
    uint8_t nexth = IP6H_NEXTH(ip6hdr);
    uint16_t offset = sizeof(struct ip6_hdr);

    /* Walk past extension headers (Hop-by-Hop=0, Routing=43,
     * Fragment=44, Dest Options=60) to reach the transport header. */
    while (nexth == 0 || nexth == 43 || nexth == 44 || nexth == 60) {
        uint8_t ext[2];
        if (pbuf_copy_partial(p, ext, 2, offset) < 2)
            break;
        /* Fragment header is always 8 bytes; others: (len+1)*8 */
        uint16_t ext_len = (nexth == 44) ? 8 : ((uint16_t)(ext[1] + 1) * 8);
        nexth = ext[0];
        offset += ext_len;
        if (offset >= p->tot_len)
            break;
    }

    /* Skip transport header */
    uint16_t transport_hdr_len = 0;
    if (nexth == 17)
        transport_hdr_len = 8; /* UDP */
    else if (nexth == 6)
        transport_hdr_len = 20; /* TCP */

    static char buf[512];
    uint16_t copied = pbuf_copy_partial(p, (uint8_t*)buf, sizeof(buf) - 1, offset + transport_hdr_len);
    buf[copied] = '\0';

    /* Strip trailing whitespace */
    while (copied > 0 && isspace((unsigned char)buf[copied - 1])) buf[--copied] = '\0';

    /* Return NULL if every byte is not printable ASCII */
    if (copied == 0)
        return NULL;
    for (uint16_t i = 0; i < copied; i++) {
        if (!isprint((unsigned char)buf[i]) && !isspace((unsigned char)buf[i]))
            return NULL;
    }

    return buf;
}

bool dtn_utils_get_icmpv6_type(const struct pbuf* p, u8_t* type_out, u8_t* code_out) {
    if (!p || p->len < sizeof(struct ip6_hdr))
        return false;

    const struct ip6_hdr* ip6hdr = (const struct ip6_hdr*)p->payload;
    uint8_t nexth = IP6H_NEXTH(ip6hdr);
    uint16_t offset = sizeof(struct ip6_hdr);

    /* Skip extension headers (Hop-by-Hop=0, Routing=43, Fragment=44,
     * Dest Options=60) — same walk as dtn_utils_print_packet_payload. */
    while (nexth == 0 || nexth == 43 || nexth == 44 || nexth == 60) {
        uint8_t ext[2];
        if (pbuf_copy_partial(p, ext, 2, offset) < 2)
            return false;
        uint16_t ext_len = (nexth == 44) ? 8 : ((uint16_t)(ext[1] + 1) * 8);
        nexth = ext[0];
        offset += ext_len;
        if (offset >= p->tot_len)
            return false;
    }

    if (nexth != IP6_NEXTH_ICMP6)
        return false;

    uint8_t hdr[2]; /* ICMPv6 type, code */
    if (pbuf_copy_partial(p, hdr, 2, offset) < 2)
        return false;
    if (type_out)
        *type_out = hdr[0];
    if (code_out)
        *code_out = hdr[1];
    return true;
}

// IPv6 Hop-by-Hop next-header value
#ifndef IP6_NEXTH_HOPOPTS
#define IP6_NEXTH_HOPOPTS 0
#endif

// ---------------------------------------------------------------------------
// FNV-1a 32-bit hash
// ---------------------------------------------------------------------------

#define FNV1A_32_INIT 0x811c9dc5u
#define FNV1A_32_PRIME 0x01000193u

static uint32_t dtn_utils_fnv1a_32(const uint8_t* data, size_t len, uint32_t hash) {
    for (size_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= FNV1A_32_PRIME;
    }
    return hash;
}

u32_t dtn_utils_compute_packet_hash(const struct pbuf* p) {
    if (!p)
        return 0;

    const struct ip6_hdr* ip6hdr = (const struct ip6_hdr*)p->payload;

    uint32_t h = FNV1A_32_INIT;
    // Hash IPv6 src and dst — identical on both sender (no HBH) and receiver (has HBH).
    h = dtn_utils_fnv1a_32((const uint8_t*)&ip6hdr->src, sizeof(ip6hdr->src), h);
    h = dtn_utils_fnv1a_32((const uint8_t*)&ip6hdr->dest, sizeof(ip6hdr->dest), h);

    // Advance past the fixed IPv6 header.
    const uint8_t* payload = (const uint8_t*)ip6hdr + IP6_HLEN;
    uint16_t plen = IP6H_PLEN(ip6hdr);  // host byte order via macro

    // Skip HBH extension header if present so the hash is the same regardless
    // of whether the custodian HBH has been added or not.
    if (IP6H_NEXTH(ip6hdr) == IP6_NEXTH_HOPOPTS && plen >= 2) {
        uint16_t hbh_bytes = (uint16_t)((payload[1] + 1) * 8);
        if (hbh_bytes <= plen) {
            payload += hbh_bytes;
            plen -= hbh_bytes;
        }
    }

    h = dtn_utils_fnv1a_32(payload, plen, h);

    // Ensure 0 is never returned (0 is the "not stored" sentinel).
    return (h == 0) ? 1u : h;
}
