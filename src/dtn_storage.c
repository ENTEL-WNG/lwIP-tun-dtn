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
#include "dtn_utils.h"
#include "lwip/ip6_addr.h"
#include "lwip/pbuf.h"
#include "lwip/sys.h"

// Bump this whenever the schema changes. On mismatch the table is dropped and
// recreated so old DBs don't cause INSERT/SELECT failures.
#define SCHEMA_VERSION 1

static const char* CREATE_TABLE_SQL =
    "CREATE TABLE IF NOT EXISTS stored_packets ("
    "  id                        INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  packet_hash               INTEGER NOT NULL DEFAULT 0,"
    "  stored_time_ms            INTEGER NOT NULL,"
    "  next_hop_node_id          INTEGER NOT NULL,"
    "  best_delivery_time_in_sec REAL    NOT NULL,"
    "  max_delivery_time_in_sec  REAL    NOT NULL,"
    "  min_delivery_time_in_sec  REAL    NOT NULL,"
    "  number_of_forward_attempts INTEGER NOT NULL DEFAULT 0,"
    "  last_attempt_ms           INTEGER NOT NULL DEFAULT 0,"
    "  src_addr                  TEXT    NOT NULL,"
    "  dest_addr                 TEXT    NOT NULL,"
    "  custodian_addr            TEXT,"
    "  packet_data               BLOB    NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_delivery_time"
    " ON stored_packets(best_delivery_time_in_sec);"
    "CREATE INDEX IF NOT EXISTS idx_packet_hash"
    " ON stored_packets(packet_hash);";

static const char* DROP_TABLE_SQL = "DROP TABLE IF EXISTS stored_packets;";

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Return a cached prepared statement for @sql (a string literal — keyed by its
// stable address), preparing and caching it on first use. The statement is reset
// and its bindings cleared, ready for the caller to bind and step. Callers must
// sqlite3_reset() (not finalize) when done; statements are finalized in destroy.
static sqlite3_stmt* db_stmt(Storage_Function* storage, const char* sql) {
    for (int i = 0; i < storage->stmt_cache_count; i++) {
        if (storage->stmt_sql[i] == sql) {
            sqlite3_reset(storage->stmt_cache[i]);
            sqlite3_clear_bindings(storage->stmt_cache[i]);
            return storage->stmt_cache[i];
        }
    }

    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(storage->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        DTN_ERROR("Failed to prepare statement: %s", sqlite3_errmsg(storage->db));
        return NULL;
    }
    if (storage->stmt_cache_count < DB_STMT_CACHE_SIZE) {
        storage->stmt_sql[storage->stmt_cache_count] = sql;
        storage->stmt_cache[storage->stmt_cache_count] = stmt;
        storage->stmt_cache_count++;
    } else {
        // More distinct statements than the cache holds — should not happen.
        // Finalize on the caller's behalf is impossible here, so warn loudly.
        DTN_WARN("Statement cache full (%d) — leaking prepared statement", DB_STMT_CACHE_SIZE);
    }
    return stmt;
}

// Bind a human-readable IPv6 address string to a statement parameter.
static void bind_ip6_addr_text(sqlite3_stmt* stmt, int col, const ip6_addr_t* addr) {
    char str[IP6ADDR_STRLEN_MAX];
    ip6addr_ntoa_r(addr, str, sizeof(str));
    sqlite3_bind_text(stmt, col, str, -1, SQLITE_TRANSIENT);
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
    if (snprintf(db_path, sizeof(db_path), "%s/%s_dtn_packets.db", dtn_config.storage_path, dtn_config.name) >= (int)sizeof(db_path)) {
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

    // Throughput tuning. Stored packets are ephemeral per run, so we trade
    // crash-durability for speed: no per-write fsync and an in-memory journal.
    // Kept as a single DB file (no WAL -wal/-shm) so the read-only metrics
    // collector's "mode=ro&nolock=1" access keeps working.
    sqlite3_exec(storage->db, "PRAGMA synchronous=OFF;", NULL, NULL, NULL);
    sqlite3_exec(storage->db, "PRAGMA journal_mode=MEMORY;", NULL, NULL, NULL);
    sqlite3_exec(storage->db, "PRAGMA temp_store=MEMORY;", NULL, NULL, NULL);
    sqlite3_exec(storage->db, "PRAGMA cache_size=-8000;", NULL, NULL, NULL);  // ~8 MB page cache

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
    storage->stmt_cache_count = 0;

    DTN_INFO("DTN Storage Function created (Max: %zu bytes, Max Packets: %d).", storage->max_storage_bytes, MAX_STORED_PACKETS);

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

    for (int i = 0; i < storage->stmt_cache_count; i++) {
        sqlite3_finalize(storage->stmt_cache[i]);
    }
    storage->stmt_cache_count = 0;

    if (storage->db) {
        sqlite3_close(storage->db);
        storage->db = NULL;
    }

    free(storage);
}

int dtn_storage_count(Storage_Function* storage) {
    if (!storage || !storage->db)
        return 0;

    sqlite3_stmt* stmt = db_stmt(storage, "SELECT COUNT(*) FROM stored_packets;");
    if (!stmt)
        return 0;

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_reset(stmt);
    return count;
}

int dtn_storage_is_full(Storage_Function* storage) { return dtn_storage_count(storage) >= MAX_STORED_PACKETS; }

dtn_storage_store_packet_result_t dtn_storage_store_packet(Storage_Function* storage, struct pbuf* p,
                                                           const DtnRoutingResult* routing_result) {
    if (!storage || !p || !routing_result) {
        DTN_ERROR("Invalid arguments to store_packet.");
        return DTN_STORAGE_STORE_ERR;
    }

    if (dtn_storage_is_full(storage)) {
        DTN_INFO("Storage is full. Cannot store packet.");
        return DTN_STORAGE_STORE_FULL;
    }

    // Copy the packet so we can strip headers without touching the caller's pbuf.
    struct pbuf* p_to_store = pbuf_alloc(PBUF_RAW, p->tot_len, PBUF_RAM);
    if (!p_to_store) {
        DTN_ERROR("Failed to allocate pbuf for storage copy");
        return DTN_STORAGE_STORE_ERR;
    }
    if (pbuf_copy(p_to_store, p) != ERR_OK) {
        DTN_ERROR("Failed to copy packet for storage");
        pbuf_free(p_to_store);
        return DTN_STORAGE_STORE_ERR;
    }
    // dtn_strip_custodian_option(&p_to_store);

    // Extract source address from the IPv6 header.
    if (p_to_store->len < IP6_HLEN) {
        DTN_ERROR("Packet too short to extract src address");
        pbuf_free(p_to_store);
        return DTN_STORAGE_STORE_ERR;
    }
    struct ip6_hdr* ip6hdr = (struct ip6_hdr*)p_to_store->payload;
    ip6_addr_t src_addr, dest_addr;
    ip6_addr_copy_from_packed(src_addr, ip6hdr->src);
    ip6_addr_copy_from_packed(dest_addr, ip6hdr->dest);
#if LWIP_IPV6_SCOPES
    ip6_addr_set_zone(&src_addr, IP6_NO_ZONE);
    ip6_addr_set_zone(&dest_addr, IP6_NO_ZONE);
#endif

    ip6_addr_t custodian_addr;
    bool has_custodian = dtn_extract_custodian_option(p, &custodian_addr);
    char custodian_str[IP6ADDR_STRLEN_MAX];
    if (has_custodian)
        ip6addr_ntoa_r(&custodian_addr, custodian_str, sizeof(custodian_str));

    u16_t pkt_len = p_to_store->tot_len;
    char* buf = malloc(pkt_len);
    if (!buf) {
        DTN_ERROR("Failed to allocate serialisation buffer");
        pbuf_free(p_to_store);
        return DTN_STORAGE_STORE_ERR;
    }
    if (pbuf_copy_partial(p_to_store, buf, pkt_len, 0) != pkt_len) {
        DTN_ERROR("pbuf_copy_partial failed");
        free(buf);
        pbuf_free(p_to_store);
        return DTN_STORAGE_STORE_ERR;
    }
    pbuf_free(p_to_store);

    // Compute FNV-1a hash for per-packet identification (used to delete the stored
    // row when an ICMPv6 RECEIVED ACK arrives from the next hop).
    u32_t pkt_hash = dtn_utils_compute_packet_hash(p);

    const char* sql =
        "INSERT INTO stored_packets"
        " (packet_hash, stored_time_ms, next_hop_node_id,"
        "  best_delivery_time_in_sec, max_delivery_time_in_sec, min_delivery_time_in_sec,"
        "  src_addr, dest_addr, custodian_addr, packet_data)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";

    sqlite3_stmt* stmt = db_stmt(storage, sql);
    if (!stmt) {
        free(buf);
        return DTN_STORAGE_STORE_ERR;
    }
    int rc;

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)pkt_hash);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)sys_now());
    sqlite3_bind_int(stmt, 3, routing_result->next_hop_node_id);
    sqlite3_bind_double(stmt, 4, routing_result->best_delivery_time);
    sqlite3_bind_double(stmt, 5, routing_result->max_delivery_time);
    sqlite3_bind_double(stmt, 6, routing_result->min_delivery_time);
    bind_ip6_addr_text(stmt, 7, &src_addr);
    bind_ip6_addr_text(stmt, 8, &dest_addr);
    if (has_custodian)
        sqlite3_bind_text(stmt, 9, custodian_str, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(stmt, 9);
    sqlite3_bind_blob(stmt, 10, buf, pkt_len, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    int ok = (rc == SQLITE_DONE);
    if (!ok) {
        DTN_ERROR("INSERT failed: %s", sqlite3_errmsg(storage->db));
    } else {
        char addr_str[IP6ADDR_STRLEN_MAX];
        ip6addr_ntoa_r(&dest_addr, addr_str, sizeof(addr_str));
        DTN_INFO("Packet for %s stored (rowid %" PRId64 ", delivery_time=%.2f). Total stored: %d", addr_str,
                 (int64_t)sqlite3_last_insert_rowid(storage->db), routing_result->best_delivery_time, dtn_storage_count(storage));
    }

    sqlite3_reset(stmt);
    free(buf);
    return ok ? DTN_STORAGE_STORE_OK : DTN_STORAGE_STORE_ERR;
}

#if FORWARD_BEST_DELIVERY_TIME
#define READY_TIME_COL "best_delivery_time_in_sec"
#else
#define READY_TIME_COL "min_delivery_time_in_sec"
#endif

// A stored packet is (re)forwardable when its contact window is open (?1 = now_sec)
// AND it has not exhausted its retransmission budget (?2 = DTN_MAX_FORWARD_ATTEMPTS)
// AND it is either fresh or its retry window has elapsed (?3 = now_ms, ?4 = retry_ms).
#define RETRY_PREDICATE                    \
    " AND number_of_forward_attempts < ?2" \
    " AND (number_of_forward_attempts = 0 OR ?3 - last_attempt_ms >= ?4)"

// Bind the shared retry-predicate parameters (?2..?4) on a prepared statement
// whose ?1 is already the contact-window time.
static void bind_retry_params(sqlite3_stmt* stmt) {
    sqlite3_bind_int(stmt, 2, DTN_MAX_FORWARD_ATTEMPTS);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)sys_now());
    sqlite3_bind_int(stmt, 4, DTN_FORWARD_RETRY_MS);
}

int dtn_storage_get_ready_entries(Storage_Function* storage, double now_sec, Stored_Packet_Entry out[], int max_count) {
    if (!storage || !storage->db || !out || max_count <= 0)
        return 0;

    const char* sql =
        "SELECT id, stored_time_ms, next_hop_node_id,"
        "       best_delivery_time_in_sec, max_delivery_time_in_sec, min_delivery_time_in_sec,"
        "       src_addr, dest_addr, custodian_addr, packet_data"
        " FROM stored_packets"
        " WHERE " READY_TIME_COL " <= ?1" RETRY_PREDICATE " ORDER BY " READY_TIME_COL " ASC;";

    sqlite3_stmt* stmt = db_stmt(storage, sql);
    if (!stmt)
        return 0;

    sqlite3_bind_double(stmt, 1, now_sec);
    bind_retry_params(stmt);

    int n = 0;
    while (n < max_count && sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t db_id = sqlite3_column_int64(stmt, 0);
        u32_t stored_ms = (u32_t)sqlite3_column_int64(stmt, 1);
        int next_hop = sqlite3_column_int(stmt, 2);
        double deliv = sqlite3_column_double(stmt, 3);
        double max_deliv = sqlite3_column_double(stmt, 4);
        double min_deliv = sqlite3_column_double(stmt, 5);

        const char* src_text = (const char*)sqlite3_column_text(stmt, 6);
        const char* dest_text = (const char*)sqlite3_column_text(stmt, 7);
        const char* cust_text = (const char*)sqlite3_column_text(stmt, 8);  // NULL if no custodian
        const void* pkt_blob = sqlite3_column_blob(stmt, 9);
        int pkt_len = sqlite3_column_bytes(stmt, 9);

        if (!src_text || !dest_text || !pkt_blob || pkt_len <= 0) {
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
        e->routing_result.next_hop_node_id = next_hop;
        e->routing_result.best_delivery_time = deliv;
        e->routing_result.max_delivery_time = max_deliv;
        e->routing_result.min_delivery_time = min_deliv;
        e->p = p;
        strncpy(e->src_addr, src_text, sizeof(e->src_addr) - 1);
        e->src_addr[sizeof(e->src_addr) - 1] = '\0';
        strncpy(e->dest_addr, dest_text, sizeof(e->dest_addr) - 1);
        e->dest_addr[sizeof(e->dest_addr) - 1] = '\0';
        if (cust_text) {
            strncpy(e->custodian_addr, cust_text, sizeof(e->custodian_addr) - 1);
            e->custodian_addr[sizeof(e->custodian_addr) - 1] = '\0';
            e->has_custodian = true;
        } else {
            e->custodian_addr[0] = '\0';
            e->has_custodian = false;
        }

        DTN_INFO("Ready entry: dest=%s custodian=%s delivery_time=%.2f (rowid %" PRId64 ")", e->dest_addr,
                 e->has_custodian ? e->custodian_addr : "NONE", deliv, db_id);
        n++;
    }

    sqlite3_reset(stmt);
    return n;
}

int dtn_storage_any_ready_entries(Storage_Function* storage, double now_sec) {
    if (!storage || !storage->db)
        return 0;

    const char* sql =
        "SELECT 1 FROM stored_packets"
        " WHERE " READY_TIME_COL " <= ?1" RETRY_PREDICATE " LIMIT 1;";

    sqlite3_stmt* stmt = db_stmt(storage, sql);
    if (!stmt)
        return 0;

    sqlite3_bind_double(stmt, 1, now_sec);
    bind_retry_params(stmt);
    int has_ready = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_reset(stmt);
    return has_ready;
}

int dtn_storage_update_routing_result(Storage_Function* storage, int64_t db_id, const DtnRoutingResult* routing_result) {
    if (!storage || !storage->db || db_id < 0 || !routing_result)
        return 0;

    const char* sql =
        "UPDATE stored_packets"
        " SET next_hop_node_id = ?,"
        "     best_delivery_time_in_sec = ?,"
        "     max_delivery_time_in_sec = ?,"
        "     min_delivery_time_in_sec = ?"
        " WHERE id = ?;";

    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(storage->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        DTN_ERROR("Failed to prepare UPDATE routing result: %s", sqlite3_errmsg(storage->db));
        return 0;
    }

    sqlite3_bind_int(stmt, 1, routing_result->next_hop_node_id);
    sqlite3_bind_double(stmt, 2, routing_result->best_delivery_time);
    sqlite3_bind_double(stmt, 3, routing_result->max_delivery_time);
    sqlite3_bind_double(stmt, 4, routing_result->min_delivery_time);
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)db_id);

    rc = sqlite3_step(stmt);
    int ok = (rc == SQLITE_DONE);
    if (!ok) {
        DTN_ERROR("UPDATE routing result failed for row %" PRId64 ": %s", db_id, sqlite3_errmsg(storage->db));
    } else {
        DTN_INFO("Updated routing result for row %" PRId64 " (next_hop=%d, delivery_time=%.2f)", db_id, routing_result->next_hop_node_id,
                 routing_result->best_delivery_time);
    }

    sqlite3_finalize(stmt);
    return ok;
}

void dtn_storage_delete_by_id(Storage_Function* storage, int64_t db_id) {
    if (!storage || !storage->db || db_id < 0)
        return;

    sqlite3_stmt* stmt = db_stmt(storage, "DELETE FROM stored_packets WHERE id = ?;");
    if (!stmt)
        return;

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)db_id);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        DTN_ERROR("DELETE by id failed: %s", sqlite3_errmsg(storage->db));
    } else {
        DTN_INFO("Deleted DB row %" PRId64, db_id);
    }
    sqlite3_reset(stmt);
}

void dtn_storage_delete_by_hash(Storage_Function* storage, u32_t packet_hash) {
    if (!storage || !storage->db || packet_hash == 0)
        return;

    sqlite3_stmt* stmt = db_stmt(storage, "DELETE FROM stored_packets WHERE packet_hash = ?;");
    if (!stmt)
        return;

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)packet_hash);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        DTN_ERROR("DELETE by hash failed: %s", sqlite3_errmsg(storage->db));
    } else {
        DTN_INFO("Deleted stored packet(s) with hash 0x%08x (%d rows)", packet_hash, sqlite3_changes(storage->db));
    }
    sqlite3_reset(stmt);
}

void dtn_storage_increment_forward_attempts(Storage_Function* storage, int64_t db_id) {
    if (!storage || !storage->db || db_id < 0)
        return;

    sqlite3_stmt* stmt = db_stmt(storage,
                                 "UPDATE stored_packets SET number_of_forward_attempts = number_of_forward_attempts + 1,"
                                 " last_attempt_ms = ? WHERE id = ?;");
    if (!stmt)
        return;

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)sys_now());
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)db_id);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        DTN_ERROR("UPDATE forward attempts failed: %s", sqlite3_errmsg(storage->db));
    }
    sqlite3_reset(stmt);
}

int dtn_storage_purge_exhausted(Storage_Function* storage) {
    if (!storage || !storage->db)
        return 0;

    sqlite3_stmt* stmt = db_stmt(storage, "DELETE FROM stored_packets WHERE number_of_forward_attempts >= ?;");
    if (!stmt)
        return 0;

    sqlite3_bind_int(stmt, 1, DTN_MAX_FORWARD_ATTEMPTS);
    int rc = sqlite3_step(stmt);
    int purged = (rc == SQLITE_DONE) ? sqlite3_changes(storage->db) : 0;
    if (rc != SQLITE_DONE) {
        DTN_ERROR("purge-exhausted failed: %s", sqlite3_errmsg(storage->db));
    } else if (purged > 0) {
        DTN_WARN("Purged %d stored packet(s) that exhausted %d forward attempts (custody ACK never arrived)", purged,
                 DTN_MAX_FORWARD_ATTEMPTS);
    }
    sqlite3_reset(stmt);
    return purged;
}