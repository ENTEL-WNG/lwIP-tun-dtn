// dtn_storage.h: Header file for persistent packet storage functions supporting store-and-forward
// operations Copyright (C) 2025 Michael Karpov
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

#ifndef DTN_STORAGE_H
#define DTN_STORAGE_H

#include <sqlite3.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dtn_module.h"
#include "dtn_routing.h"
#include "lwip/ip6.h"
#include "lwip/ip6_addr.h"
#include "lwip/pbuf.h"

#define MAX_STORED_PACKETS 128
#define MAX_PATH_LENGTH 512

// A single loaded packet entry returned by dtn_storage_get_ready_entries.
// The caller owns `p` and must pbuf_free it when done.
typedef struct Stored_Packet_Entry {
    int64_t db_id;
    u32_t stored_time_ms;
    double delivery_time_in_sec;
    double max_delivery_time_in_sec;
    ip6_addr_t src_addr;
    ip6_addr_t original_dest;
    struct pbuf* p;  // caller-owned; pbuf_free when done
} Stored_Packet_Entry;

typedef struct Storage_Function {
    DTN_Module* parent_module;
    size_t max_storage_bytes;
    sqlite3* db;  // open DB handle; NULL until dtn_storage_create
} Storage_Function;

// Lifecycle
Storage_Function* dtn_storage_create(DTN_Module* parent);
void dtn_storage_destroy(Storage_Function* storage);

typedef enum {
    DTN_STORAGE_STORE_OK = 0,
    DTN_STORAGE_STORE_FULL = 1,
    DTN_STORAGE_STORE_ERR = -1,
} dtn_storage_store_packet_result_t;

// Write
dtn_storage_store_packet_result_t dtn_storage_store_packet(Storage_Function* storage,
                                                           struct pbuf* p,
                                                           const DtnRoutingResult* routing_result);

// Query
int dtn_storage_is_full(Storage_Function* storage);
int dtn_storage_count(Storage_Function* storage);

// Read packets whose delivery_time_in_sec <= now_sec into out[].
// Returns number of entries filled. Caller must pbuf_free each entry's p.
int dtn_storage_get_ready_entries(Storage_Function* storage, double now_sec,
                                  Stored_Packet_Entry out[], int max_count);

// Delete
void dtn_storage_delete_by_id(Storage_Function* storage, int64_t db_id);
void dtn_storage_delete_packet_by_ip_header(Storage_Function* storage, struct ip6_hdr* orig_ip6hdr);
void dtn_storage_delete_packet_by_icmp_data(Storage_Function* storage, struct pbuf* icmp_packet);

#endif
