// dtn_utils.h: Utility functions shared across DTN components
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

#ifndef DTN_UTILS_H
#define DTN_UTILS_H

#include <stdbool.h>

#include "lwip/arch.h"
#include "lwip/ip6_addr.h"
#include "lwip/pbuf.h"

// Returns true if dest_addr matches the IPv6 address string addr.
bool dtn_utils_is_addr_equal(const ip6_addr_t* dest_addr, const char* addr);
bool dtn_utils_is_local_addr(const ip6_addr_t* addr);

// Extracts the transport payload from p (skipping extension headers and the
// transport header).  If the payload is non-empty printable ASCII it logs it
// via DTN_INFO and returns a pointer to the string (backed by an internal
// static buffer — valid until the next call).  Returns NULL if p is NULL or
// the payload is binary / empty.
const char* dtn_utils_print_packet_payload(const struct pbuf* p);

// FNV-1a 32-bit hash over IPv6 src + dst + transport payload (HBH header skipped).
// Both the sender (stores packet without HBH) and the receiver (gets packet with HBH)
// skip the HBH extension header before hashing, so both sides produce the same value.
// Returns 0 only when p is NULL (0 is used as the "not stored" sentinel elsewhere).
u32_t dtn_utils_compute_packet_hash(const struct pbuf* p);

// If p is an ICMPv6 packet, writes its ICMPv6 type/code and returns true; returns
// false otherwise. Any IPv6 extension headers (e.g. the custodian Hop-by-Hop
// option) are skipped first, so this works on packets carrying a custodian.
// type_out and code_out may be NULL.
bool dtn_utils_get_icmpv6_type(const struct pbuf* p, u8_t* type_out, u8_t* code_out);

#endif
