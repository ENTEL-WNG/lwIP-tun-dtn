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

// Option type for custody transfer header (choose unassigned value)
#define CUSTODY_OPTION_TYPE 0x1E
// Hop-by-Hop header length in bytes (must be multiple of 8).
// Layout: next_header(1) + hdr_ext_len(1) + opt_type(1) + opt_data_len(1) + addr(16) + packet_id(4) = 24.
// opt_data_len is 20 (covers both addr and packet_id) so the kernel skips the entire
// option as an unknown type (0x1E, high bits 00) without mis-parsing the packet_id bytes.
#define HBH_OPT_HDR_LEN 24

bool dtn_add_custodian_option(struct pbuf** p, const ip6_addr_t* custodian);

bool dtn_extract_custodian_option(const struct pbuf* p, ip6_addr_t* custodian_out);

bool dtn_strip_custodian_option(struct pbuf** p);

// Returns a newly-allocated pbuf with the custodian option added or updated,
// and packet_id embedded in the last 4 bytes of the HBH option (network byte order).
// packet_id == 0 writes zeros (sentinel: packet was not stored on this hop).
// The original pbuf is left untouched. Returns NULL on allocation failure.
struct pbuf* dtn_update_or_add_custodian_option(const struct pbuf* orig,
                                                const ip6_addr_t* custodian,
                                                u32_t packet_id);

// Reads the packet_id embedded in the custody HBH option (network → host order).
// Returns false and sets *packet_id_out = 0 if no custody HBH is present.
bool dtn_extract_packet_id_from_hbh(const struct pbuf* p, u32_t* packet_id_out);

#endif
