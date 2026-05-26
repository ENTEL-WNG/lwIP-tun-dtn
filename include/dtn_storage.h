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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sqlite3.h>

#include "dtn_module.h"
#include "dtn_routing.h"
#include "lwip/ip6.h"
#include "lwip/ip6_addr.h"
#include "lwip/pbuf.h"

#define MAX_STORED_PACKETS 5
#define MAX_PATH_LENGTH 512

typedef struct Stored_Packet_Entry {
    struct pbuf* p;
    double delivery_time_in_sec;
    double max_delivery_time_in_sec;
    ip6_addr_t original_dest;
    u32_t stored_time_ms;
    struct Stored_Packet_Entry* next;
    int64_t db_id;  // SQLite rowid; -1 if not yet persisted
} Stored_Packet_Entry;

typedef struct Storage_Function {
    DTN_Module* parent_module;
    size_t stored_packets_count;
    size_t max_storage_bytes;
    Stored_Packet_Entry* packet_list_head;
    char storage_directory[MAX_PATH_LENGTH];
    sqlite3* db;  // open DB handle; NULL until dtn_storage_create
} Storage_Function;

Storage_Function* dtn_storage_create(DTN_Module* parent);
void dtn_storage_destroy(Storage_Function* storage);
int dtn_storage_store_packet(Storage_Function* storage, struct pbuf* p,
                             const ip6_addr_t* original_dest,
                             const DtnRoutingResult* routing_result);
int dtn_storage_is_full(Storage_Function* storage);
void dtn_storage_remove_entry(Storage_Function* storage, Stored_Packet_Entry* entry);
Stored_Packet_Entry* dtn_storage_retrieve_packet_for_dest(Storage_Function* storage,
                                                          const ip6_addr_t* target_dest);
void dtn_storage_free_retrieved_entry_struct(Stored_Packet_Entry* entry);
Stored_Packet_Entry* dtn_storage_get_packet_copy_for_dest(Storage_Function* storage,
                                                          const ip6_addr_t* target_dest);
void dtn_storage_delete_packet_by_ip_header(Storage_Function* storage, struct ip6_hdr* orig_ip6hdr);
void dtn_storage_delete_packet_by_icmp_data(Storage_Function* storage, struct pbuf* icmp_packet);

#endif
