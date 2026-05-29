
#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dtn_config.h"
#include "dtn_controller.h"
#include "dtn_custody.h"
#include "dtn_icmpv6.h"
#include "dtn_logger.h"
#include "dtn_routing.h"
#include "dtn_storage.h"
#include "lwip/err.h"
#include "lwip/icmp6.h"
#include "lwip/init.h"
#include "lwip/ip6.h"
#include "lwip/ip6_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/sys.h"
#include "raw_socket.h"

DTN_Module* global_dtn_module = NULL;

#define PAYLOAD_LEN 64

void test_payload_length(struct ip6_hdr* ip6h) {
    DTN_TEST("ip6h->_plen                 : %u", ip6h->_plen);
    DTN_TEST("ip6h->_plen hex             : 0x%04x", ip6h->_plen);

    u16_t _payload_length = lwip_ntohs(ip6h->_plen);
    DTN_TEST("lwip_ntohs(ip6h->_plen)     : %u", _payload_length);
    DTN_TEST("lwip_ntohs(ip6h->_plen) hex : 0x%04x", _payload_length);

    u16_t payload_length;
    memcpy(&payload_length, &ip6h->_plen, sizeof(u16_t));
    DTN_TEST("memcpy                      : %u", payload_length);
    DTN_TEST("memcpy hex                  : 0x%04x", payload_length);

    // Dump raw bytes to verify
    // uint8_t* raw = (uint8_t*)p->payload;
    // for (int i = 0; i < p->len; i++) {
    //     printf("%02X ", raw[i]);
    //     if ((i + 1) % 8 == 0)
    //         printf("");
    // }

    return;
}

// ---------------------------------------------------------------------------
// Storage tests
// ---------------------------------------------------------------------------

#define TEST_STORAGE_DIR "/tmp/dtn_test_storage"

// Minimal DtnRoutingResult with timing values we can verify after round-trip.
static DtnRoutingResult make_routing_result(double delivery, double to) {
    DtnRoutingResult r;
    memset(&r, 0, sizeof(r));
    r.best_delivery_time = delivery;
    r.to_time = to;
    return r;
}

static struct pbuf* make_test_packet(uint16_t payload_length, const char* src, const char* dest,
                                     uint8_t payload_value) {
    struct pbuf* p = pbuf_alloc(PBUF_RAW, sizeof(struct ip6_hdr) + payload_length, PBUF_RAM);
    if (!p) {
        return NULL;
    }
    memset(p->payload, 0, sizeof(struct ip6_hdr) + payload_length);

    struct ip6_hdr* ip6h = (struct ip6_hdr*)p->payload;
    IP6H_VTCFL_SET(ip6h, 6, 0, 0);
    IP6H_PLEN_SET(ip6h, lwip_htons(payload_length));
    IP6H_NEXTH_SET(ip6h, 59);
    IP6H_HOPLIM_SET(ip6h, 64);

    ip6_addr_t src_addr, dst_addr;
    ip6addr_aton(src, &src_addr);
    ip6addr_aton(dest, &dst_addr);
    ip6_addr_copy_to_packed(ip6h->src, src_addr);
    ip6_addr_copy_to_packed(ip6h->dest, dst_addr);

    uint8_t* payload_ptr = (uint8_t*)p->payload + sizeof(struct ip6_hdr);
    memset(payload_ptr, payload_value, payload_length);
    return p;
}

// Adds a DTN custody Hop-by-Hop header to p, stamping custodian_addr as the
// current holder. Returns the (possibly reallocated) pbuf on success, NULL on
// failure — in the failure case the original pbuf is left untouched.
static struct pbuf* add_custodian(struct pbuf* p, const char* custodian_addr) {
    if (!p || !custodian_addr)
        return NULL;

    ip6_addr_t custodian;
    if (!ip6addr_aton(custodian_addr, &custodian))
        return NULL;

    struct pbuf* result = dtn_update_or_add_custodian_option(p, &custodian);
    if (!result)
        return NULL;

    return result;
}

void test_storage_sqlite(void) {
    DTN_TEST("=== test_storage_sqlite ===");

    // Point config at a temporary directory so we don't touch real storage.
    strncpy(dtn_config.storage_path, TEST_STORAGE_DIR, sizeof(dtn_config.storage_path) - 1);
    dtn_config.storage_path[sizeof(dtn_config.storage_path) - 1] = '\0';

    // --- 1. Create storage ---
    Storage_Function* storage = dtn_storage_create(NULL);
    if (!storage) {
        DTN_TEST("FAIL: dtn_storage_create returned NULL");
        return;
    }
    if (!storage->db) {
        DTN_TEST("FAIL: storage->db is NULL after create");
        dtn_storage_destroy(storage);
        return;
    }
    DTN_TEST("PASS: storage created, DB handle is non-NULL");

    // --- 2. Store a packet ---
    struct pbuf* p = make_test_packet(PAYLOAD_LEN, "fd00:1:2::1", "fd00:2:3::3", 0xAB);
    // if (!p) {
    //     DTN_TEST("FAIL: make_test_packet returned NULL");
    //     dtn_storage_destroy(storage);
    //     return;
    // }

    ip6_addr_t dest;
    ip6addr_aton("fd00:2:3::3", &dest);
    DtnRoutingResult rr = make_routing_result(42.5, 100.0);

    dtn_storage_store_packet_result_t stored = dtn_storage_store_packet(storage, p, &rr);
    pbuf_free(p);  // storage keeps its own copy

    if (stored != DTN_STORAGE_STORE_OK) {
        DTN_TEST("FAIL: dtn_storage_store_packet returned error");
        dtn_storage_destroy(storage);
        return;
    }
    if (dtn_storage_count(storage) != 1) {
        DTN_TEST("FAIL: expected count == 1, got %d", dtn_storage_count(storage));
        dtn_storage_destroy(storage);
        return;
    }

    // Retrieve the db_id of the stored row directly from the DB.
    sqlite3_stmt* id_stmt = NULL;
    int64_t stored_db_id = -1;
    sqlite3_prepare_v2(storage->db, "SELECT id FROM stored_packets LIMIT 1;", -1, &id_stmt, NULL);
    if (sqlite3_step(id_stmt) == SQLITE_ROW)
        stored_db_id = sqlite3_column_int64(id_stmt, 0);
    sqlite3_finalize(id_stmt);

    if (stored_db_id <= 0) {
        DTN_TEST("FAIL: db_id not set (got %" PRId64 ")", stored_db_id);
        dtn_storage_destroy(storage);
        return;
    }
    DTN_TEST("PASS: packet stored, count=1, db_id=%" PRId64 "", stored_db_id);

    // --- 3. Confirm row exists in DB ---
    sqlite3_stmt* stmt = NULL;
    int rc =
        sqlite3_prepare_v2(storage->db, "SELECT COUNT(*) FROM stored_packets;", -1, &stmt, NULL);
    int db_count = 0;
    if (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
        db_count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    if (db_count != 1) {
        DTN_TEST("FAIL: expected 1 row in DB, found %d", db_count);
        dtn_storage_destroy(storage);
        return;
    }
    DTN_TEST("PASS: DB contains 1 row");

    // --- 4. Remove the entry ---
    dtn_storage_delete_by_id(storage, stored_db_id);

    if (dtn_storage_count(storage) != 0) {
        DTN_TEST("FAIL: expected count == 0 after delete, got %d", dtn_storage_count(storage));
        dtn_storage_destroy(storage);
        return;
    }

    // Confirm row gone from DB.
    rc = sqlite3_prepare_v2(storage->db, "SELECT COUNT(*) FROM stored_packets;", -1, &stmt, NULL);
    db_count = 0;
    if (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
        db_count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    if (db_count != 0) {
        DTN_TEST("FAIL: expected 0 rows in DB after remove, found %d", db_count);
        dtn_storage_destroy(storage);
        return;
    }
    DTN_TEST("PASS: entry removed, DB has 0 rows");

    dtn_storage_destroy(storage);

    // --- 5. Persistence round-trip ---
    DTN_TEST("--- persistence round-trip ---");

    // Store a new packet in a fresh storage instance.
    storage = dtn_storage_create(NULL);
    if (!storage) {
        DTN_TEST("FAIL: second dtn_storage_create returned NULL");
        return;
    }

    p = make_test_packet(PAYLOAD_LEN, "fd00:1:2::1", "fd00:2:3::3", 0xAB);
    rr = make_routing_result(77.25, 200.0);
    stored = dtn_storage_store_packet(storage, p, &rr);
    pbuf_free(p);

    if (stored != DTN_STORAGE_STORE_OK) {
        DTN_TEST("FAIL: store in round-trip phase returned error");
        dtn_storage_destroy(storage);
        return;
    }

    // Get the db_id before closing.
    sqlite3_stmt* rt_stmt = NULL;
    int64_t saved_db_id = -1;
    sqlite3_prepare_v2(storage->db, "SELECT id FROM stored_packets LIMIT 1;", -1, &rt_stmt, NULL);
    if (sqlite3_step(rt_stmt) == SQLITE_ROW)
        saved_db_id = sqlite3_column_int64(rt_stmt, 0);
    sqlite3_finalize(rt_stmt);

    dtn_storage_destroy(storage);  // closes DB

    // Reopen — data must survive across open/close.
    storage = dtn_storage_create(NULL);
    if (!storage) {
        DTN_TEST("FAIL: third dtn_storage_create returned NULL");
        return;
    }

    if (dtn_storage_count(storage) != 1) {
        DTN_TEST("FAIL: expected 1 packet after reload, got %d", dtn_storage_count(storage));
        dtn_storage_destroy(storage);
        return;
    }

    // Load the entry using a large now_sec so it's always "ready".
    Stored_Packet_Entry entries[MAX_STORED_PACKETS];
    int n = dtn_storage_get_ready_entries(storage, 1e18, entries, MAX_STORED_PACKETS);
    if (n != 1) {
        DTN_TEST("FAIL: expected 1 ready entry after reload, got %d", n);
        dtn_storage_destroy(storage);
        return;
    }

    Stored_Packet_Entry loaded = entries[0];
    pbuf_free(loaded.p);  // we only need the metadata

    if (loaded.db_id != saved_db_id) {
        DTN_TEST("FAIL: db_id mismatch after reload (expected %" PRId64 ", got %" PRId64 ")",
                 saved_db_id, loaded.db_id);
        dtn_storage_destroy(storage);
        return;
    }
    if (loaded.delivery_time_in_sec != 77.25) {
        DTN_TEST("FAIL: delivery_time_in_sec not preserved (got %f)", loaded.delivery_time_in_sec);
        dtn_storage_destroy(storage);
        return;
    }
    if (loaded.max_delivery_time_in_sec != 200.0) {
        DTN_TEST("FAIL: max_delivery_time_in_sec not preserved (got %f)",
                 loaded.max_delivery_time_in_sec);
        dtn_storage_destroy(storage);
        return;
    }
    DTN_TEST("PASS: round-trip OK — db_id=%" PRId64 ", delivery_time=%.2f, max_delivery_time=%.2f",
             loaded.db_id, loaded.delivery_time_in_sec, loaded.max_delivery_time_in_sec);

    // Clean up.
    dtn_storage_destroy(storage);
    DTN_TEST("=== test_storage_sqlite PASSED ===");
}

int main() {
    lwip_init();
    if (dtn_config_load(&dtn_config) != 0) {
        DTN_ERROR("Failed to load config");
        return -1;
    }

    dtn_config_print(&dtn_config);
    dtn_log_init();

    DTN_TEST("START TESTING");

    struct pbuf* p = make_test_packet(PAYLOAD_LEN, "fd00:1:2::1", "fd00:2:3::3", 0xAB);
    struct ip6_hdr* ip6h = (struct ip6_hdr*)p->payload;

    struct pbuf* p_custodian = add_custodian(p, "fd00:5:4::4");
    ip6_addr_t custodian;
    dtn_extract_custodian_option(p_custodian, &custodian);
    DTN_TEST("custodian: %s", ip6addr_ntoa(&custodian));

    DtnRoutingResult routing_result;
    dtn_routing_get_next_hop_node_id(0, 0 * 1000, ip6h, &routing_result);
    DTN_TEST("next_hop_node_id: %d", routing_result.next_hop_node_id);
    dtn_routing_get_next_hop_node_id(0, 21 * 1000, ip6h, &routing_result);
    DTN_TEST("next_hop_node_id: %d", routing_result.next_hop_node_id);

    test_storage_sqlite();

    DTN_TEST("STOP TESTING");

    // pbuf_free(p);
    return 0;
}