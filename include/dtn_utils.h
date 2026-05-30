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

#include "lwip/pbuf.h"
#include "lwip/arch.h"

// FNV-1a 32-bit hash over IPv6 src + dst + transport payload (HBH header skipped).
// Both the sender (stores packet without HBH) and the receiver (gets packet with HBH)
// skip the HBH extension header before hashing, so both sides produce the same value.
// Returns 0 only when p is NULL (0 is used as the "not stored" sentinel elsewhere).
u32_t dtn_compute_packet_hash(const struct pbuf* p);

#endif
