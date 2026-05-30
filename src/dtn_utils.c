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

#include <stddef.h>
#include <stdint.h>

#include "lwip/ip6.h"
#include "lwip/pbuf.h"

// IPv6 Hop-by-Hop next-header value
#ifndef IP6_NEXTH_HOPOPTS
#define IP6_NEXTH_HOPOPTS 0
#endif

// ---------------------------------------------------------------------------
// FNV-1a 32-bit hash
// ---------------------------------------------------------------------------

#define FNV1A_32_INIT  0x811c9dc5u
#define FNV1A_32_PRIME 0x01000193u

static uint32_t fnv1a_32(const uint8_t* data, size_t len, uint32_t hash) {
    for (size_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= FNV1A_32_PRIME;
    }
    return hash;
}

u32_t dtn_compute_packet_hash(const struct pbuf* p) {
    if (!p)
        return 0;

    const struct ip6_hdr* ip6hdr = (const struct ip6_hdr*)p->payload;

    uint32_t h = FNV1A_32_INIT;
    // Hash IPv6 src and dst — identical on both sender (no HBH) and receiver (has HBH).
    h = fnv1a_32((const uint8_t*)&ip6hdr->src,  sizeof(ip6hdr->src),  h);
    h = fnv1a_32((const uint8_t*)&ip6hdr->dest, sizeof(ip6hdr->dest), h);

    // Advance past the fixed IPv6 header.
    const uint8_t* payload = (const uint8_t*)ip6hdr + IP6_HLEN;
    uint16_t       plen    = IP6H_PLEN(ip6hdr);  // host byte order via macro

    // Skip HBH extension header if present so the hash is the same regardless
    // of whether the custodian HBH has been added or not.
    if (IP6H_NEXTH(ip6hdr) == IP6_NEXTH_HOPOPTS && plen >= 2) {
        uint16_t hbh_bytes = (uint16_t)((payload[1] + 1) * 8);
        if (hbh_bytes <= plen) {
            payload += hbh_bytes;
            plen    -= hbh_bytes;
        }
    }

    h = fnv1a_32(payload, plen, h);

    // Ensure 0 is never returned (0 is the "not stored" sentinel).
    return (h == 0) ? 1u : h;
}
