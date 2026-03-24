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

#include "lwip/ip6_addr.h"
#include "lwip/pbuf.h"
#include <stdbool.h>

// Option type for custody transfer header (choose unassigned value)
#define CUSTODY_OPTION_TYPE 0x1E
// Hop-by-Hop header length in bytes (must be multiple of 8)
#define HBH_OPT_HDR_LEN 24

bool dtn_add_custodian_option(struct pbuf **p, const ip6_addr_t *custodian);

bool dtn_extract_custodian_option(const struct pbuf *p, ip6_addr_t *custodian_out);

bool dtn_strip_custodian_option(struct pbuf **p);

bool dtn_update_or_add_custodian_option(struct pbuf **p, const ip6_addr_t *custodian);

#endif
