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
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dtn_config.h"
#include "dtn_custody.h"
#include "dtn_logger.h"
#include "lwip/ip6_addr.h"
#include "lwip/pbuf.h"
#include "lwip/sys.h"

// Addresses are stored as their raw 16-byte wire representation (no zone field)
// to make blob comparisons zone-independent.
#define IPV6_ADDR_BYTES 16

// Bump this whenever the schema changes. On mismatch the table is dropped and
// recreated so old DBs don't cause INSERT/SELECT failures.
#define SCHEMA_VERSION 1

static const char* CREATE_TABLE_SQL =
    "CREATE TABLE IF NOT EXISTS stored_packets ("
    "  id                       INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  stored_time_ms           INTEGER NOT NULL,"
    "  delivery_time_in_sec     REAL    NOT NULL,"
    "  max_delivery_time_in_sec REAL    NOT NULL,"
    "  src_addr                 BLOB    NOT NULL,"
    "  original_dest            BLOB    NOT NULL,"
    "  packet_data              BLOB    NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_delivery_time"
    " ON stored_packets(delivery_time_in_sec);";

static const char* DROP_TABLE_SQL = "DROP TABLE IF EXISTS stored_packets;";

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Bind the 16-byte wire address of an ip6_addr_t to a statement parameter.
static void bind_ip6_addr(sqlite3_stmt* stmt, int col, const ip6_addr_t* addr) {
    sqlite3_bind_blob(stmt, col, addr->addr, IPV6_ADDR_BYTES, SQLITE_TRANSIENT);
}

// Copy a 16-byte blob from a SELECT result into an ip6_addr_t (zone set to 0).
static void load_ip6_addr(const void* blob, ip6_addr_t* out) {
    memset(out, 0, sizeof(ip6_addr_t));
    memcpy(out->addr, blob, IPV6_ADDR_BYTES);
}

// Create the directory hierarchy and open (or create) the SQLite DB.
// Returns 1 on success, 0 on failure.
static int dtn_storage_init_db(Storage_Function* storage) {
    // Create directory hierarchy.
    char tmp[MAX_PATH_LENGTH];
    strncpy(tmp, dtn_config.storage_path, sizeof(tmp) - 1);
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
    DTN_INFO("Directory ready: %s", dtn_config.storage_path);

    // Open the DB.
    char db_path[MAX_PATH_LENGTH];
    if (snprintf(db_path, sizeof(db_path), "%s/%s_dtn_packets.db", dtn_config.storage_path,
                 dtn_config.name) >= (int)sizeof(db_path)) {
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

    // Check schema version; drop and recreate if stale.
    int version = 0;
    sqlite3_stmt* ver_stmt = NULL;
    if (sqlite3_prepare_v2(storage->db, "PRAGMA user_version;", -1, &ver_stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(ver_stmt) == SQLITE_ROW)
            version = sqlite3_column_int(ver_stmt, 0);
        sqlite3_finalize(ver_stmt);
    }

    if (version != SCHEMA_VERSION) {
        DTN_INFO("DB schema version %d != expected %d — recreating table", version, SCHEMA_VERSION);
        char* errmsg = NULL;
        rc = sqlite3_exec(storage->db, DROP_TABLE_SQL, NULL, NULL, &errmsg);
        if (rc != SQLITE_OK) {
            DTN_ERROR("Failed to drop old table: %s", errmsg);
            sqlite3_free(errmsg);
            sqlite3_close(storage->db);
            storage->db = NULL;
            return 0;
        }
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

    // Persist the schema version.
    if (version != SCHEMA_VERSION) {
        char pragma[64];
        snprintf(pragma, sizeof(pragma), "PRAGMA user_version = %d;", SCHEMA_VERSION);
        sqlite3_exec(storage->db, pragma, NULL, NULL, NULL);
    }

    DTN_INFO("SQLite DB ready: %s", db_path);
    return 1;
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
    storage->max_storage_bytes = 1024 * 1024;  // 1 MB limit (informational)
    storage->db = NULL;

    DTN_INFO("DTN Storage Function created (Max: %zu bytes, Max Packets: %d).",
             storage->max_storage_bytes, MAX_STORED_PACKETS);

    if (!dtn_storage_init_db(storage)) {
        DTN_ERROR("Failed to initialise storage DB");
        free(storage);
        return NULL;
    }

    return storage;
}

void dtn_storage_destroy(Storage_Function* storage) {
    if (!storage)
        return;
    DTN_INFO("Destroying DTN Storage Function...");

    if (storage->db) {
        sqlite3_close(storage->db);
        storage->db = NULL;
    }

    free(storage);
}

int dtn_storage_count(Storage_Function* storage) {
    if (!storage || !storage->db)
        return 0;

    sqlite3_stmt* stmt = NULL;
    int rc =
        sqlite3_prepare_v2(storage->db, "SELECT COUNT(*) FROM stored_packets;", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        DTN_ERROR("Failed to prepare COUNT: %s", sqlite3_errmsg(storage->db));
        return 0;
    }

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

int dtn_storage_is_full(Storage_Function* storage) {
    return dtn_storage_count(storage) >= MAX_STORED_PACKETS;
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

    // Copy the packet so we can strip headers without touching the caller's pbuf.
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
    dtn_strip_custodian_option(&p_to_store);

    // Extract source address from the IPv6 header.
    if (p_to_store->len < IP6_HLEN) {
        DTN_ERROR("Packet too short to extract src address");
        pbuf_free(p_to_store);
        return 0;
    }
    struct ip6_hdr* ip6hdr = (struct ip6_hdr*)p_to_store->payload;
    ip6_addr_t src_addr;
    ip6_addr_copy_from_packed(src_addr, ip6hdr->src);
#if LWIP_IPV6_SCOPES
    ip6_addr_set_zone(&src_addr, IP6_NO_ZONE);
#endif

    // Normalise dest zone too.
    ip6_addr_t dest_nozone = *original_dest;
#if LWIP_IPV6_SCOPES
    ip6_addr_set_zone(&dest_nozone, IP6_NO_ZONE);
#endif

    u16_t pkt_len = p_to_store->tot_len;
    char* buf = malloc(pkt_len);
    if (!buf) {
        DTN_ERROR("Failed to allocate serialisation buffer");
        pbuf_free(p_to_store);
        return 0;
    }
    if (pbuf_copy_partial(p_to_store, buf, pkt_len, 0) != pkt_len) {
        DTN_ERROR("pbuf_copy_partial failed");
        free(buf);
        pbuf_free(p_to_store);
        return 0;
    }
    pbuf_free(p_to_store);

    const char* sql =
        "INSERT INTO stored_packets"
        " (stored_time_ms, delivery_time_in_sec, max_delivery_time_in_sec,"
        "  src_addr, original_dest, packet_data)"
        " VALUES (?, ?, ?, ?, ?, ?);";

    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(storage->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        DTN_ERROR("Failed to prepare INSERT: %s", sqlite3_errmsg(storage->db));
        free(buf);
        return 0;
    }

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)sys_now());
    sqlite3_bind_double(stmt, 2, routing_result->best_delivery_time);
    sqlite3_bind_double(stmt, 3, routing_result->to_time);
    bind_ip6_addr(stmt, 4, &src_addr);
    bind_ip6_addr(stmt, 5, &dest_nozone);
    sqlite3_bind_blob(stmt, 6, buf, pkt_len, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    int ok = (rc == SQLITE_DONE);
    if (!ok) {
        DTN_ERROR("INSERT failed: %s", sqlite3_errmsg(storage->db));
    } else {
        char addr_str[IP6ADDR_STRLEN_MAX];
        ip6addr_ntoa_r(original_dest, addr_str, sizeof(addr_str));
        DTN_INFO("Packet for %s stored (rowid %" PRId64 ", delivery_time=%.2f). Total stored: %d",
                 addr_str, (int64_t)sqlite3_last_insert_rowid(storage->db),
                 routing_result->best_delivery_time, dtn_storage_count(storage));
    }

    sqlite3_finalize(stmt);
    free(buf);
    return ok ? 1 : 0;
}

int dtn_storage_get_ready_entries(Storage_Function* storage, double now_sec,
                                  Stored_Packet_Entry out[], int max_count) {
    if (!storage || !storage->db || !out || max_count <= 0)
        return 0;

    const char* sql =
        "SELECT id, stored_time_ms, delivery_time_in_sec, max_delivery_time_in_sec,"
        "       src_addr, original_dest, packet_data"
        " FROM stored_packets"
        " WHERE delivery_time_in_sec <= ?"
        " ORDER BY delivery_time_in_sec ASC;";

    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(storage->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        DTN_ERROR("Failed to prepare SELECT ready: %s", sqlite3_errmsg(storage->db));
        return 0;
    }

    sqlite3_bind_double(stmt, 1, now_sec);

    int n = 0;
    while (n < max_count && sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t db_id = sqlite3_column_int64(stmt, 0);
        u32_t stored_ms = (u32_t)sqlite3_column_int64(stmt, 1);
        double deliv = sqlite3_column_double(stmt, 2);
        double max_deliv = sqlite3_column_double(stmt, 3);

        const void* src_blob = sqlite3_column_blob(stmt, 4);
        int src_len = sqlite3_column_bytes(stmt, 4);
        const void* dest_blob = sqlite3_column_blob(stmt, 5);
        int dest_len = sqlite3_column_bytes(stmt, 5);
        const void* pkt_blob = sqlite3_column_blob(stmt, 6);
        int pkt_len = sqlite3_column_bytes(stmt, 6);

        if (!src_blob || src_len != IPV6_ADDR_BYTES || !dest_blob || dest_len != IPV6_ADDR_BYTES ||
            !pkt_blob || pkt_len <= 0) {
            DTN_WARN("Skipping malformed row %" PRId64, db_id);
            continue;
        }

        struct pbuf* p = pbuf_alloc(PBUF_RAW, (u16_t)pkt_len, PBUF_RAM);
        if (!p) {
            DTN_ERROR("Failed to allocate pbuf for row %" PRId64, db_id);
            continue;
        }
        if (pbuf_take(p, pkt_blob, (u16_t)pkt_len) != ERR_OK) {
            DTN_ERROR("pbuf_take failed for row %" PRId64, db_id);
            pbuf_free(p);
            continue;
        }

        Stored_Packet_Entry* e = &out[n];
        e->db_id = db_id;
        e->stored_time_ms = stored_ms;
        e->delivery_time_in_sec = deliv;
        e->max_delivery_time_in_sec = max_deliv;
        e->p = p;
        load_ip6_addr(src_blob, &e->src_addr);
        load_ip6_addr(dest_blob, &e->original_dest);

        char addr_str[IP6ADDR_STRLEN_MAX];
        ip6addr_ntoa_r(&e->original_dest, addr_str, sizeof(addr_str));
        DTN_INFO("Ready entry: dest=%s delivery_time=%.2f (rowid %" PRId64 ")", addr_str, deliv,
                 db_id);
        n++;
    }

    sqlite3_finalize(stmt);
    return n;
}

void dtn_storage_delete_by_id(Storage_Function* storage, int64_t db_id) {
    if (!storage || !storage->db || db_id < 0)
        return;

    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(storage->db, "DELETE FROM stored_packets WHERE id = ?;", -1, &stmt,
                                NULL);
    if (rc != SQLITE_OK) {
        DTN_ERROR("Failed to prepare DELETE by id: %s", sqlite3_errmsg(storage->db));
        return;
    }

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)db_id);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        DTN_ERROR("DELETE by id failed: %s", sqlite3_errmsg(storage->db));
    } else {
        DTN_INFO("Deleted DB row %" PRId64, db_id);
    }
    sqlite3_finalize(stmt);
}

void dtn_storage_delete_packet_by_ip_header(Storage_Function* storage,
                                            struct ip6_hdr* orig_ip6hdr) {
    if (!storage || !storage->db || !orig_ip6hdr)
        return;

    ip6_addr_t src, dest;
    IP6_ADDR(&src, orig_ip6hdr->src.addr[0], orig_ip6hdr->src.addr[1], orig_ip6hdr->src.addr[2],
             orig_ip6hdr->src.addr[3]);
    IP6_ADDR(&dest, orig_ip6hdr->dest.addr[0], orig_ip6hdr->dest.addr[1], orig_ip6hdr->dest.addr[2],
             orig_ip6hdr->dest.addr[3]);

    char src_str[IP6ADDR_STRLEN_MAX], dest_str[IP6ADDR_STRLEN_MAX];
    ip6addr_ntoa_r(&src, src_str, sizeof(src_str));
    ip6addr_ntoa_r(&dest, dest_str, sizeof(dest_str));
    DTN_INFO("Deleting stored packet matching src=%s dest=%s", src_str, dest_str);

    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(
        storage->db, "DELETE FROM stored_packets WHERE src_addr = ? AND original_dest = ?;", -1,
        &stmt, NULL);
    if (rc != SQLITE_OK) {
        DTN_ERROR("Failed to prepare DELETE by ip header: %s", sqlite3_errmsg(storage->db));
        return;
    }

    bind_ip6_addr(stmt, 1, &src);
    bind_ip6_addr(stmt, 2, &dest);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        DTN_ERROR("DELETE by ip header failed: %s", sqlite3_errmsg(storage->db));
    } else {
        int deleted = sqlite3_changes(storage->db);
        DTN_INFO("Deleted %d row(s) matching src=%s dest=%s", deleted, src_str, dest_str);
    }
    sqlite3_finalize(stmt);
}

void dtn_storage_delete_packet_by_icmp_data(Storage_Function* storage, struct pbuf* icmp_packet) {
    if (!storage || !storage->db || !icmp_packet)
        return;

    // ICMP packet layout:
    //   IPv6 Header (40 bytes) + ICMPv6 Header (8 bytes) + DTN payload (9 bytes)
    //   + Original IPv6 Header + 8 bytes of original payload
    if (icmp_packet->len < IP6_HLEN + 8 + 9 + IP6_HLEN) {
        DTN_WARN("ICMP packet too small to parse original IPv6 header");
        return;
    }

    // Skip outer IPv6 (40) + ICMPv6 (8) + DTN payload struct (9).
    const u8_t* orig_ip6_raw = (const u8_t*)icmp_packet->payload + IP6_HLEN + 8 + 9;
    const struct ip6_hdr* orig_ip6hdr = (const struct ip6_hdr*)orig_ip6_raw;

    ip6_addr_t orig_src, orig_dest;
    IP6_ADDR(&orig_src, orig_ip6hdr->src.addr[0], orig_ip6hdr->src.addr[1],
             orig_ip6hdr->src.addr[2], orig_ip6hdr->src.addr[3]);
    IP6_ADDR(&orig_dest, orig_ip6hdr->dest.addr[0], orig_ip6hdr->dest.addr[1],
             orig_ip6hdr->dest.addr[2], orig_ip6hdr->dest.addr[3]);

    char src_str[IP6ADDR_STRLEN_MAX], dest_str[IP6ADDR_STRLEN_MAX];
    ip6addr_ntoa_r(&orig_src, src_str, sizeof(src_str));
    ip6addr_ntoa_r(&orig_dest, dest_str, sizeof(dest_str));
    DTN_INFO("Deleting stored packet via ICMP data: src=%s dest=%s", src_str, dest_str);

    // Delegate to the ip-header path — we have what we need.
    dtn_storage_delete_packet_by_ip_header(storage, (struct ip6_hdr*)orig_ip6hdr);
}
