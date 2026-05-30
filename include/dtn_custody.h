// dtn_custody.h: Header file for custody transfer mechanisms using IPv6 hop-by-hop options
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

#ifndef DTN_CUSTODY_H
#define DTN_CUSTODY_H

#include <stdbool.h>

#include "lwip/ip6_addr.h"
#include "lwip/pbuf.h"

// TLV option types embedded in the IPv6 Hop-by-Hop extension header.
// Both use high bits 00 → kernel skips unknown options without error.
#define CUSTODY_OPTION_TYPE    0x1E  // TLV option 1: custodian IPv6 address (len=16)
#define PACKET_ID_OPTION_TYPE  0x1F  // TLV option 2: per-hop storage row ID  (len=4)

// Total HBH extension header length in bytes (must be a multiple of 8):
//   2  fixed header  (next_header + hdr_ext_len)
//  18  option 1      (type + len + 16-byte addr)
//   6  option 2      (type + len +  4-byte packet_id)
//   6  Pad1 × 6
//  ──
//  32
#define HBH_OPT_HDR_LEN 32

bool dtn_add_custodian_option(struct pbuf** p, const ip6_addr_t* custodian);

bool dtn_extract_custodian_option(const struct pbuf* p, ip6_addr_t* custodian_out);

bool dtn_strip_custodian_option(struct pbuf** p);

// Returns a newly-allocated pbuf with the custodian option added or updated
// and packet_id stamped into the packet-id TLV option (network byte order).
// packet_id == 0 → write zeros (packet was not stored on this hop).
// The original pbuf is left untouched. Returns NULL on allocation failure.
struct pbuf* dtn_update_or_add_custodian_option(const struct pbuf* orig,
                                                const ip6_addr_t* custodian,
                                                u32_t packet_id);

// Reads the packet_id from the packet-id TLV option in the HBH header.
// Returns false (and sets *packet_id_out = 0) if the option is absent.
bool dtn_extract_packet_id_from_hbh(const struct pbuf* p, u32_t* packet_id_out);

#endif
