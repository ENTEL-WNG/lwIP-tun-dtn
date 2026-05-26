// dtn_storage.c: Implementation of persistent packet storage with SQLite backend for DTN
// store-and-forward functionality Copyright (C) 2025 Michael Karpov
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

#include "dtn_storage.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sqlite3.h>

#include "dtn_config.h"
#include "dtn_custody.h"
#include "dtn_logger.h"
#include "lwip/ip6_addr.h"
#include "lwip/pbuf.h"
#include "lwip/sys.h"

static const char* CREATE_TABLE_SQL =
    "CREATE TABLE IF NOT EXISTS stored_packets ("
    "  id                       INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  stored_time_ms           INTEGER NOT NULL,"
    "  delivery_time_in_sec     REAL    NOT NULL,"
    "  max_delivery_time_in_sec REAL    NOT NULL,"
    "  original_dest            BLOB    NOT NULL,"
    "  packet_data              BLOB    NOT NULL"
    ");";

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Creates the storage directory tree and opens (or creates) the SQLite DB.
// Returns 1 on success, 0 on failure.
static int dtn_storage_init_db(Storage_Function* storage) {
    // Create directory hierarchy (unchanged logic from old init_directory).
    char tmp[MAX_PATH_LENGTH];
    strncpy(tmp, storage->storage_directory, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) == -1 && errno != EEXIST) {
                DTN_ERROR("Failed to create directory %s: %s", tmp, strerror(errno));
                return 0;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) == -1 && errno != EEXIST) {
        DTN_ERROR("Failed to create directory %s: %s", tmp, strerror(errno));
        return 0;
    }
    DTN_INFO("Directory ready: %s", storage->storage_directory);

    // Build DB path and open.
    char db_path[MAX_PATH_LENGTH];
    if (snprintf(db_path, sizeof(db_path), "%s/packets.db", storage->storage_directory) >=
        (int)sizeof(db_path)) {
        DTN_ERROR("DB path too long");
        return 0;
    }

    int rc = sqlite3_open(db_path, &storage->db);
    if (rc != SQLITE_OK) {
        DTN_ERROR("Failed to open SQLite DB at %s: %s", db_path, sqlite3_errmsg(storage->db));
        sqlite3_close(storage->db);
        storage->db = NULL;
        return 0;
    }

    char* errmsg = NULL;
    rc = sqlite3_exec(storage->db, CREATE_TABLE_SQL, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        DTN_ERROR("Failed to create packets table: %s", errmsg);
        sqlite3_free(errmsg);
        sqlite3_close(storage->db);
        storage->db = NULL;
        return 0;
    }

    DTN_INFO("SQLite DB ready: %s", db_path);
    return 1;
}

// Inserts a packet entry into the DB and sets entry->db_id.
// Returns 1 on success, 0 on failure.
static int dtn_storage_insert_packet(Storage_Function* storage, Stored_Packet_Entry* entry) {
    if (!storage || !storage->db || !entry || !entry->p)
        return 0;

    u16_t pkt_len = entry->p->tot_len;
    char* buf = malloc(pkt_len);
    if (!buf) {
        DTN_ERROR("Failed to allocate buffer for packet serialisation");
        return 0;
    }

    if (pbuf_copy_partial(entry->p, buf, pkt_len, 0) != pkt_len) {
        DTN_ERROR("pbuf_copy_partial failed");
        free(buf);
        return 0;
    }

    const char* sql =
        "INSERT INTO stored_packets"
        " (stored_time_ms, delivery_time_in_sec, max_delivery_time_in_sec, original_dest, packet_data)"
        " VALUES (?, ?, ?, ?, ?);";

    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(storage->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        DTN_ERROR("Failed to prepare INSERT: %s", sqlite3_errmsg(storage->db));
        free(buf);
        return 0;
    }

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)entry->stored_time_ms);
    sqlite3_bind_double(stmt, 2, entry->delivery_time_in_sec);
    sqlite3_bind_double(stmt, 3, entry->max_delivery_time_in_sec);
    sqlite3_bind_blob(stmt, 4, &entry->original_dest, sizeof(ip6_addr_t), SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 5, buf, pkt_len, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    int ok = (rc == SQLITE_DONE);
    if (!ok) {
        DTN_ERROR("INSERT failed: %s", sqlite3_errmsg(storage->db));
    } else {
        entry->db_id = (int64_t)sqlite3_last_insert_rowid(storage->db);
        DTN_INFO("Packet inserted into DB with rowid %" PRId64, entry->db_id);
    }

    sqlite3_finalize(stmt);
    free(buf);
    return ok ? 1 : 0;
}

// Deletes a single row by rowid.
// Returns 1 on success, 0 on failure.
static int dtn_storage_delete_row(Storage_Function* storage, int64_t db_id) {
    if (!storage || !storage->db || db_id < 0)
        return 0;

    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(storage->db, "DELETE FROM stored_packets WHERE id = ?;", -1, &stmt,
                                NULL);
    if (rc != SQLITE_OK) {
        DTN_ERROR("Failed to prepare DELETE: %s", sqlite3_errmsg(storage->db));
        return 0;
    }

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)db_id);
    rc = sqlite3_step(stmt);
    int ok = (rc == SQLITE_DONE);
    if (!ok) {
        DTN_ERROR("DELETE failed: %s", sqlite3_errmsg(storage->db));
    } else {
        DTN_INFO("Deleted DB row %" PRId64, db_id);
    }

    sqlite3_finalize(stmt);
    return ok ? 1 : 0;
}

// Reads all rows from the DB and reconstructs the in-memory linked list.
// Returns number of packets loaded.
static int dtn_storage_load_packets_from_db(Storage_Function* storage) {
    if (!storage || !storage->db)
        return 0;

    const char* sql =
        "SELECT id, stored_time_ms, delivery_time_in_sec, max_delivery_time_in_sec,"
        "       original_dest, packet_data"
        " FROM stored_packets ORDER BY id ASC;";

    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(storage->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        DTN_ERROR("Failed to prepare SELECT for load: %s", sqlite3_errmsg(storage->db));
        return 0;
    }

    int loaded = 0;
    Stored_Packet_Entry* tail = NULL;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (storage->stored_packets_count >= MAX_STORED_PACKETS) {
            DTN_INFO("Maximum packet count reached, stopping load");
            break;
        }

        int64_t db_id = (int64_t)sqlite3_column_int64(stmt, 0);
        u32_t stored_time_ms = (u32_t)sqlite3_column_int64(stmt, 1);
        double delivery_time = sqlite3_column_double(stmt, 2);
        double max_delivery_time = sqlite3_column_double(stmt, 3);

        const void* dest_blob = sqlite3_column_blob(stmt, 4);
        int dest_len = sqlite3_column_bytes(stmt, 4);
        const void* pkt_blob = sqlite3_column_blob(stmt, 5);
        int pkt_len = sqlite3_column_bytes(stmt, 5);

        if (!dest_blob || dest_len != sizeof(ip6_addr_t) || !pkt_blob || pkt_len <= 0) {
            DTN_WARN("Skipping malformed row %" PRId64, db_id);
            continue;
        }

        struct pbuf* p = pbuf_alloc(PBUF_RAW, (u16_t)pkt_len, PBUF_RAM);
        if (!p) {
            DTN_ERROR("Failed to allocate pbuf for loaded packet");
            continue;
        }
        if (pbuf_take(p, pkt_blob, (u16_t)pkt_len) != ERR_OK) {
            DTN_ERROR("pbuf_take failed for row %" PRId64, db_id);
            pbuf_free(p);
            continue;
        }

        Stored_Packet_Entry* entry = malloc(sizeof(Stored_Packet_Entry));
        if (!entry) {
            DTN_ERROR("Failed to allocate Stored_Packet_Entry");
            pbuf_free(p);
            continue;
        }

        entry->p = p;
        memcpy(&entry->original_dest, dest_blob, sizeof(ip6_addr_t));
        entry->stored_time_ms = stored_time_ms;
        entry->delivery_time_in_sec = delivery_time;
        entry->max_delivery_time_in_sec = max_delivery_time;
        entry->db_id = db_id;
        entry->next = NULL;

        // Append to linked list.
        if (storage->packet_list_head == NULL) {
            storage->packet_list_head = entry;
        } else {
            tail->next = entry;
        }
        tail = entry;

        storage->stored_packets_count++;
        loaded++;

        char addr_str[IP6ADDR_STRLEN_MAX];
        ip6addr_ntoa_r(&entry->original_dest, addr_str, sizeof(addr_str));
        DTN_INFO("Loaded packet for %s from DB (rowid %" PRId64 ")", addr_str, db_id);
    }

    sqlite3_finalize(stmt);
    DTN_INFO("Loaded %d packets from DB", loaded);
    return loaded;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

Storage_Function* dtn_storage_create(DTN_Module* parent) {
    Storage_Function* storage = (Storage_Function*)malloc(sizeof(Storage_Function));
    if (!storage) {
        DTN_ERROR("Failed to allocate memory for Storage_Function");
        return NULL;
    }

    storage->parent_module = parent;
    storage->stored_packets_count = 0;
    storage->max_storage_bytes = 1024 * 1024;  // 1 MB limit (informational)
    storage->packet_list_head = NULL;
    storage->db = NULL;

    strncpy(storage->storage_directory, dtn_config.storage_path, MAX_PATH_LENGTH - 1);
    storage->storage_directory[MAX_PATH_LENGTH - 1] = '\0';

    DTN_INFO("DTN Storage Function created (Max: %zu bytes, Max Packets: %d).",
             storage->max_storage_bytes, MAX_STORED_PACKETS);

    if (!dtn_storage_init_db(storage)) {
        DTN_ERROR("Failed to initialise storage DB");
        free(storage);
        return NULL;
    }

    dtn_storage_load_packets_from_db(storage);
    return storage;
}

void dtn_storage_destroy(Storage_Function* storage) {
    if (!storage)
        return;
    DTN_INFO("Destroying DTN Storage Function...");

    Stored_Packet_Entry* current = storage->packet_list_head;
    Stored_Packet_Entry* next_entry;
    while (current != NULL) {
        next_entry = current->next;
        char addr_str[IP6ADDR_STRLEN_MAX];
        ip6addr_ntoa_r(&current->original_dest, addr_str, sizeof(addr_str));
        DTN_INFO("Freeing stored pbuf (original dest: %s) during destroy.", addr_str);
        pbuf_free(current->p);
        free(current);
        current = next_entry;
    }
    storage->packet_list_head = NULL;
    storage->stored_packets_count = 0;

    if (storage->db) {
        sqlite3_close(storage->db);
        storage->db = NULL;
    }

    free(storage);
}

int dtn_storage_is_full(Storage_Function* storage) {
    if (!storage)
        return 1;
    return storage->stored_packets_count >= MAX_STORED_PACKETS;
}

void dtn_storage_remove_entry(Storage_Function* storage, Stored_Packet_Entry* entry) {
    if (!storage || !entry)
        return;

    /* Unlink from list — walk to find prev. */
    Stored_Packet_Entry* prev = NULL;
    Stored_Packet_Entry* cur = storage->packet_list_head;
    while (cur != NULL && cur != entry) {
        prev = cur;
        cur = cur->next;
    }
    if (cur == NULL) {
        DTN_WARN("DTN Storage: remove_entry called with entry not in list");
        return;
    }
    if (prev == NULL)
        storage->packet_list_head = entry->next;
    else
        prev->next = entry->next;

    storage->stored_packets_count--;

    /* Remove DB row. */
    dtn_storage_delete_row(storage, entry->db_id);

    /* Free pbuf and struct. */
    if (entry->p)
        pbuf_free(entry->p);
    free(entry);
}

int dtn_storage_store_packet(Storage_Function* storage, struct pbuf* p,
                             const ip6_addr_t* original_dest,
                             const DtnRoutingResult* routing_result) {
    if (!storage || !p || !original_dest || !routing_result) {
        DTN_ERROR("Invalid arguments to store_packet.");
        return 0;
    }

    if (dtn_storage_is_full(storage)) {
        char addr_str[IP6ADDR_STRLEN_MAX];
        ip6addr_ntoa_r(original_dest, addr_str, sizeof(addr_str));
        DTN_INFO("Storage is full. Cannot store packet for %s.", addr_str);
        return 0;
    }

    // Create a copy of the packet to strip headers if needed.
    struct pbuf* p_to_store = pbuf_alloc(PBUF_RAW, p->tot_len, PBUF_RAM);
    if (!p_to_store) {
        DTN_ERROR("Failed to allocate pbuf for storage copy");
        return 0;
    }

    if (pbuf_copy(p_to_store, p) != ERR_OK) {
        DTN_ERROR("Failed to copy packet for storage");
        pbuf_free(p_to_store);
        return 0;
    }

    // Strip hop-by-hop header if present.
    dtn_strip_custodian_option(&p_to_store);

    Stored_Packet_Entry* new_entry = (Stored_Packet_Entry*)malloc(sizeof(Stored_Packet_Entry));
    if (!new_entry) {
        DTN_ERROR("Failed to allocate memory for Stored_Packet_Entry");
        pbuf_free(p_to_store);
        return 0;
    }

    new_entry->p = p_to_store;
    memcpy(&new_entry->original_dest, original_dest, sizeof(ip6_addr_t));
    new_entry->stored_time_ms = sys_now();
    new_entry->delivery_time_in_sec = routing_result->best_delivery_time;
    new_entry->max_delivery_time_in_sec = routing_result->to_time;
    new_entry->next = NULL;
    new_entry->db_id = -1;

    if (!dtn_storage_insert_packet(storage, new_entry)) {
        DTN_ERROR("Failed to persist packet to DB");
        pbuf_free(new_entry->p);
        free(new_entry);
        return 0;
    }

    if (storage->packet_list_head == NULL) {
        storage->packet_list_head = new_entry;
    } else {
        Stored_Packet_Entry* current_item = storage->packet_list_head;
        while (current_item->next != NULL) {
            current_item = current_item->next;
        }
        current_item->next = new_entry;
    }

    storage->stored_packets_count++;
    char addr_str_log[IP6ADDR_STRLEN_MAX];
    ip6addr_ntoa_r(original_dest, addr_str_log, sizeof(addr_str_log));
    DTN_INFO("Packet for %s stored successfully at time %u. Total stored: %zu", addr_str_log,
             new_entry->stored_time_ms, storage->stored_packets_count);

    return 1;
}

Stored_Packet_Entry* dtn_storage_retrieve_packet_for_dest(Storage_Function* storage,
                                                          const ip6_addr_t* target_dest) {
    if (!storage || !target_dest || storage->packet_list_head == NULL) {
        return NULL;
    }

    Stored_Packet_Entry* current = storage->packet_list_head;
    Stored_Packet_Entry* prev = NULL;
    Stored_Packet_Entry* match = NULL;
    Stored_Packet_Entry* prev_for_match = NULL;

    while (current != NULL) {
        ip6_addr_t current_dest_nozone;
        ip6_addr_t target_dest_nozone;

        memcpy(&current_dest_nozone, &current->original_dest, sizeof(ip6_addr_t));
        memcpy(&target_dest_nozone, target_dest, sizeof(ip6_addr_t));

#if LWIP_IPV6_SCOPES
        ip6_addr_set_zone(&current_dest_nozone, IP6_NO_ZONE);
        ip6_addr_set_zone(&target_dest_nozone, IP6_NO_ZONE);
#endif

        if (ip6_addr_cmp(&current_dest_nozone, &target_dest_nozone)) {
            match = current;
            prev_for_match = prev;
            break;
        }
        prev = current;
        current = current->next;
    }

    if (match) {
        if (prev_for_match == NULL) {
            storage->packet_list_head = match->next;
        } else {
            prev_for_match->next = match->next;
        }
        storage->stored_packets_count--;

        char addr_str[IP6ADDR_STRLEN_MAX];
        ip6addr_ntoa_r(&match->original_dest, addr_str, sizeof(addr_str));
        DTN_INFO("Retrieving packet for %s (stored at %u). Total stored now: %zu", addr_str,
                 match->stored_time_ms, storage->stored_packets_count);

        dtn_storage_delete_row(storage, match->db_id);

        match->next = NULL;
        return match;
    }
    return NULL;
}

void dtn_storage_free_retrieved_entry_struct(Stored_Packet_Entry* entry) {
    if (entry) {
        char addr_str[IP6ADDR_STRLEN_MAX];
        ip6addr_ntoa_r(&entry->original_dest, addr_str, sizeof(addr_str));
        DTN_INFO(
            "Freeing Stored_Packet_Entry structure for %s (pbuf management is "
            "caller's responsibility).",
            addr_str);
        free(entry);
    }
}

Stored_Packet_Entry* dtn_storage_get_packet_copy_for_dest(Storage_Function* storage,
                                                          const ip6_addr_t* target_dest) {
    if (!storage || !target_dest || storage->packet_list_head == NULL) {
        return NULL;
    }

    Stored_Packet_Entry* current = storage->packet_list_head;

    while (current != NULL) {
        ip6_addr_t current_dest_nozone;
        ip6_addr_t target_dest_nozone;

        memcpy(&current_dest_nozone, &current->original_dest, sizeof(ip6_addr_t));
        memcpy(&target_dest_nozone, target_dest, sizeof(ip6_addr_t));

#if LWIP_IPV6_SCOPES
        ip6_addr_set_zone(&current_dest_nozone, IP6_NO_ZONE);
        ip6_addr_set_zone(&target_dest_nozone, IP6_NO_ZONE);
#endif

        if (ip6_addr_cmp(&current_dest_nozone, &target_dest_nozone)) {
            Stored_Packet_Entry* copy = (Stored_Packet_Entry*)malloc(sizeof(Stored_Packet_Entry));
            if (!copy) {
                DTN_INFO("Failed to allocate memory for packet copy");
                return NULL;
            }

            struct pbuf* p_copy = pbuf_alloc(PBUF_RAW, current->p->tot_len, PBUF_RAM);
            if (!p_copy) {
                DTN_INFO("Failed to allocate pbuf for packet copy");
                free(copy);
                return NULL;
            }

            if (pbuf_copy(p_copy, current->p) != ERR_OK) {
                DTN_INFO("Failed to copy packet data");
                pbuf_free(p_copy);
                free(copy);
                return NULL;
            }

            copy->p = p_copy;
            memcpy(&copy->original_dest, &current->original_dest, sizeof(ip6_addr_t));
            copy->stored_time_ms = current->stored_time_ms;
            copy->delivery_time_in_sec = current->delivery_time_in_sec;
            copy->max_delivery_time_in_sec = current->max_delivery_time_in_sec;
            copy->db_id = current->db_id;
            copy->next = NULL;

            char addr_str[IP6ADDR_STRLEN_MAX];
            ip6addr_ntoa_r(&copy->original_dest, addr_str, sizeof(addr_str));
            DTN_INFO("Created copy of packet for %s (original stored at %u)", addr_str,
                     copy->stored_time_ms);

            return copy;
        }

        current = current->next;
    }

    return NULL;
}

void dtn_storage_delete_packet_by_ip_header(Storage_Function* storage,
                                            struct ip6_hdr* orig_ip6hdr) {
    if (!storage || !orig_ip6hdr || !storage->packet_list_head) {
        return;
    }

    ip6_addr_t orig_src, orig_dest;

    IP6_ADDR(&orig_src, orig_ip6hdr->src.addr[0], orig_ip6hdr->src.addr[1],
             orig_ip6hdr->src.addr[2], orig_ip6hdr->src.addr[3]);

    IP6_ADDR(&orig_dest, orig_ip6hdr->dest.addr[0], orig_ip6hdr->dest.addr[1],
             orig_ip6hdr->dest.addr[2], orig_ip6hdr->dest.addr[3]);

    char orig_src_str[IP6ADDR_STRLEN_MAX] = {0};
    char orig_dest_str[IP6ADDR_STRLEN_MAX] = {0};
    ip6addr_ntoa_r(&orig_src, orig_src_str, sizeof(orig_src_str));
    ip6addr_ntoa_r(&orig_dest, orig_dest_str, sizeof(orig_dest_str));

    DTN_INFO("Looking for stored packet matching src=%s, dest=%s", orig_src_str, orig_dest_str);

    Stored_Packet_Entry* current = storage->packet_list_head;
    Stored_Packet_Entry* prev = NULL;
    bool found = false;

    while (current != NULL) {
        if (current->p && current->p->len >= IP6_HLEN) {
            struct ip6_hdr* stored_ip6hdr = (struct ip6_hdr*)current->p->payload;

            if (memcmp(&stored_ip6hdr->src, &orig_ip6hdr->src, 16) == 0 &&
                memcmp(&stored_ip6hdr->dest, &orig_ip6hdr->dest, 16) == 0) {
                found = true;

                if (prev == NULL) {
                    storage->packet_list_head = current->next;
                } else {
                    prev->next = current->next;
                }

                DTN_INFO(
                    "Deleting stored packet for %s (src=%s) as next hop confirmed "
                    "reception",
                    orig_dest_str, orig_src_str);

                dtn_storage_delete_row(storage, current->db_id);

                pbuf_free(current->p);
                free(current);

                storage->stored_packets_count--;

                break;
            }
        }

        prev = current;
        current = current->next;
    }

    if (!found) {
        DTN_INFO("No matching stored packet found for %s (src=%s)", orig_dest_str, orig_src_str);
    }
}

void dtn_storage_delete_packet_by_icmp_data(Storage_Function* storage, struct pbuf* icmp_packet) {
    if (!storage || !icmp_packet || !storage->packet_list_head) {
        return;
    }

    // Parse ICMP packet structure:
    // IPv6 Header (40 bytes) + ICMPv6 Header (8 bytes) + Payload + Original IPv6 Header + 8 bytes
    // of original payload

    if (icmp_packet->len < IP6_HLEN + 8 + 9 + IP6_HLEN + 8) {
        DTN_INFO("ICMP packet too small for proper parsing");
        return;
    }

    u8_t* icmp_data = (u8_t*)icmp_packet->payload + IP6_HLEN + 8;  // Skip outer IPv6 + ICMP headers

    // Skip DTN payload structure (9 bytes: 4+2+2+1)
    icmp_data += 9;

    // Now icmp_data points to the original IPv6 header
    struct ip6_hdr* orig_ip6hdr = (struct ip6_hdr*)icmp_data;
    u8_t* orig_first_8_bytes = icmp_data + IP6_HLEN;  // First 8 bytes after IPv6 header

    ip6_addr_t orig_src, orig_dest;
    IP6_ADDR(&orig_src, orig_ip6hdr->src.addr[0], orig_ip6hdr->src.addr[1],
             orig_ip6hdr->src.addr[2], orig_ip6hdr->src.addr[3]);
    IP6_ADDR(&orig_dest, orig_ip6hdr->dest.addr[0], orig_ip6hdr->dest.addr[1],
             orig_ip6hdr->dest.addr[2], orig_ip6hdr->dest.addr[3]);

    char orig_src_str[IP6ADDR_STRLEN_MAX] = {0};
    char orig_dest_str[IP6ADDR_STRLEN_MAX] = {0};
    ip6addr_ntoa_r(&orig_src, orig_src_str, sizeof(orig_src_str));
    ip6addr_ntoa_r(&orig_dest, orig_dest_str, sizeof(orig_dest_str));

    DTN_INFO(
        "Looking for stored packet matching src=%s, dest=%s with payload "
        "verification",
        orig_src_str, orig_dest_str);

    // Parse the original packet to skip hop-by-hop headers in the ICMP data
    u8_t orig_nexth = orig_ip6hdr->_nexth;
    u16_t orig_payload_offset = 0;

    if (orig_nexth == IP6_NEXTH_HOPBYHOP) {
        // Skip hop-by-hop header to get to actual payload
        u8_t hbh_hdr_len = orig_first_8_bytes[1];
        orig_payload_offset = (hbh_hdr_len + 1) * 8;
        orig_nexth = orig_first_8_bytes[0];

        DTN_INFO("Skipping hop-by-hop header (%d bytes) in ICMP data", orig_payload_offset);
    }

    // Get pointer to actual payload (after any headers)
    u8_t* orig_payload_start = orig_first_8_bytes + orig_payload_offset;
    u8_t actual_payload_bytes = 8 - orig_payload_offset;

    if (actual_payload_bytes <= 0) {
        DTN_WARN("Not enough payload data after headers, falling back to basic matching");
        dtn_storage_delete_packet_by_ip_header(storage, orig_ip6hdr);
        return;
    }

    Stored_Packet_Entry* current = storage->packet_list_head;
    Stored_Packet_Entry* prev = NULL;
    bool found = false;

    while (current != NULL) {
        if (current->p && current->p->len >= IP6_HLEN) {
            struct ip6_hdr* stored_ip6hdr = (struct ip6_hdr*)current->p->payload;

            // 1. Check src address
            if (memcmp(&stored_ip6hdr->src, &orig_ip6hdr->src, 16) == 0) {
                // 2. Check next header protocol
                u8_t stored_nexth = stored_ip6hdr->_nexth;
                u16_t stored_payload_offset = IP6_HLEN;

                // Stored packets shouldn't have hop-by-hop headers, but check anyway
                if (stored_nexth == IP6_NEXTH_HOPBYHOP) {
                    DTN_INFO(
                        "Warning - stored packet has hop-by-hop header "
                        "(unexpected)");
                    prev = current;
                    current = current->next;
                    continue;
                }

                // 3. Compare actual payload bytes
                bool payload_matches = true;
                if (actual_payload_bytes > 0 &&
                    current->p->len >= stored_payload_offset + actual_payload_bytes) {
                    u8_t* stored_payload = (u8_t*)current->p->payload + stored_payload_offset;

                    if (memcmp(stored_payload, orig_payload_start, actual_payload_bytes) != 0) {
                        DTN_INFO("Payload mismatch for src=%s, dest=%s", orig_src_str,
                                 orig_dest_str);
                        payload_matches = false;
                    }
                }

                // 4. Check if protocols match (after stripping HBH from original)
                if (stored_nexth != orig_nexth) {
                    DTN_INFO("Protocol mismatch: stored=%d, orig=%d", stored_nexth, orig_nexth);
                    payload_matches = false;
                }

                if (payload_matches) {
                    found = true;

                    if (prev == NULL) {
                        storage->packet_list_head = current->next;
                    } else {
                        prev->next = current->next;
                    }

                    DTN_INFO(
                        "Deleting stored packet for %s (src=%s) with payload "
                        "verification",
                        orig_dest_str, orig_src_str);

                    dtn_storage_delete_row(storage, current->db_id);

                    pbuf_free(current->p);
                    free(current);

                    storage->stored_packets_count--;

                    break;
                }
            }
        }

        prev = current;
        current = current->next;
    }

    if (!found) {
        DTN_INFO(
            "No matching stored packet found for %s (src=%s) with payload "
            "verification\n",
            orig_dest_str, orig_src_str);
    }
}
