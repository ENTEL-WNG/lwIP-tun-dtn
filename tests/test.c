
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

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
    printf("ip6h->_plen                 : %u\n", ip6h->_plen);
    printf("ip6h->_plen hex             : 0x%04x \n", ip6h->_plen);

    u16_t _payload_length = lwip_ntohs(ip6h->_plen);
    printf("lwip_ntohs(ip6h->_plen)     : %u\n", _payload_length);
    printf("lwip_ntohs(ip6h->_plen) hex : 0x%04x \n", _payload_length);

    u16_t payload_length;
    memcpy(&payload_length, &ip6h->_plen, sizeof(u16_t));
    printf("memcpy                      : %u\n", payload_length);
    printf("memcpy hex                  : 0x%04x \n", payload_length);

    // Dump raw bytes to verify
    // uint8_t* raw = (uint8_t*)p->payload;
    // for (int i = 0; i < p->len; i++) {
    //     printf("%02X ", raw[i]);
    //     if ((i + 1) % 8 == 0)
    //         printf("\n");
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

static struct pbuf* make_test_packet(void) {
    struct pbuf* p = pbuf_alloc(PBUF_RAW, sizeof(struct ip6_hdr) + PAYLOAD_LEN, PBUF_RAM);
    if (!p)
        return NULL;
    memset(p->payload, 0, sizeof(struct ip6_hdr) + PAYLOAD_LEN);

    struct ip6_hdr* ip6h = (struct ip6_hdr*)p->payload;
    IP6H_VTCFL_SET(ip6h, 6, 0, 0);
    IP6H_PLEN_SET(ip6h, lwip_htons(PAYLOAD_LEN));
    IP6H_NEXTH_SET(ip6h, 59);
    IP6H_HOPLIM_SET(ip6h, 64);

    ip6_addr_t src, dst;
    ip6addr_aton("fd00:1:2::1", &src);
    ip6addr_aton("fd00:2:3::3", &dst);
    ip6_addr_copy_to_packed(ip6h->src, src);
    ip6_addr_copy_to_packed(ip6h->dest, dst);

    uint8_t* payload_ptr = (uint8_t*)p->payload + sizeof(struct ip6_hdr);
    memset(payload_ptr, 0xAB, PAYLOAD_LEN);
    return p;
}

void test_storage_sqlite(void) {
    DTN_TEST("=== test_storage_sqlite ===\n");

    // Point config at a temporary directory so we don't touch real storage.
    strncpy(dtn_config.storage_path, TEST_STORAGE_DIR, sizeof(dtn_config.storage_path) - 1);
    dtn_config.storage_path[sizeof(dtn_config.storage_path) - 1] = '\0';

    // --- 1. Create storage ---
    Storage_Function* storage = dtn_storage_create(NULL);
    if (!storage) {
        DTN_TEST("FAIL: dtn_storage_create returned NULL\n");
        return;
    }
    if (!storage->db) {
        DTN_TEST("FAIL: storage->db is NULL after create\n");
        dtn_storage_destroy(storage);
        return;
    }
    DTN_TEST("PASS: storage created, DB handle is non-NULL\n");

    // --- 2. Store a packet ---
    struct pbuf* p = make_test_packet();
    if (!p) {
        DTN_TEST("FAIL: make_test_packet returned NULL\n");
        dtn_storage_destroy(storage);
        return;
    }

    ip6_addr_t dest;
    ip6addr_aton("fd00:2:3::3", &dest);
    DtnRoutingResult rr = make_routing_result(42.5, 100.0);

    int stored = dtn_storage_store_packet(storage, p, &dest, &rr);
    pbuf_free(p);  // storage keeps its own copy

    if (!stored) {
        DTN_TEST("FAIL: dtn_storage_store_packet returned 0\n");
        dtn_storage_destroy(storage);
        return;
    }
    if (storage->stored_packets_count != 1) {
        DTN_TEST("FAIL: expected stored_packets_count == 1, got %zu\n",
                 storage->stored_packets_count);
        dtn_storage_destroy(storage);
        return;
    }
    if (storage->packet_list_head->db_id <= 0) {
        DTN_TEST("FAIL: db_id not set (got %" PRId64 ")\n",
                 storage->packet_list_head->db_id);
        dtn_storage_destroy(storage);
        return;
    }
    DTN_TEST("PASS: packet stored, count=1, db_id=%" PRId64 "\n",
             storage->packet_list_head->db_id);

    // --- 3. Confirm row exists in DB ---
    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(storage->db,
                                "SELECT COUNT(*) FROM stored_packets;", -1, &stmt, NULL);
    int db_count = 0;
    if (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
        db_count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    if (db_count != 1) {
        DTN_TEST("FAIL: expected 1 row in DB, found %d\n", db_count);
        dtn_storage_destroy(storage);
        return;
    }
    DTN_TEST("PASS: DB contains 1 row\n");

    // --- 4. Remove the entry ---
    dtn_storage_remove_entry(storage, storage->packet_list_head);

    if (storage->stored_packets_count != 0) {
        DTN_TEST("FAIL: expected stored_packets_count == 0 after remove, got %zu\n",
                 storage->stored_packets_count);
        dtn_storage_destroy(storage);
        return;
    }

    // Confirm row gone from DB.
    rc = sqlite3_prepare_v2(storage->db,
                            "SELECT COUNT(*) FROM stored_packets;", -1, &stmt, NULL);
    db_count = 0;
    if (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
        db_count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    if (db_count != 0) {
        DTN_TEST("FAIL: expected 0 rows in DB after remove, found %d\n", db_count);
        dtn_storage_destroy(storage);
        return;
    }
    DTN_TEST("PASS: entry removed, DB has 0 rows\n");

    dtn_storage_destroy(storage);

    // --- 5. Persistence round-trip ---
    DTN_TEST("--- persistence round-trip ---\n");

    // Store a new packet in a fresh storage instance.
    storage = dtn_storage_create(NULL);
    if (!storage) {
        DTN_TEST("FAIL: second dtn_storage_create returned NULL\n");
        return;
    }

    p = make_test_packet();
    rr = make_routing_result(77.25, 200.0);
    stored = dtn_storage_store_packet(storage, p, &dest, &rr);
    pbuf_free(p);

    if (!stored) {
        DTN_TEST("FAIL: store in round-trip phase returned 0\n");
        dtn_storage_destroy(storage);
        return;
    }

    int64_t saved_db_id = storage->packet_list_head->db_id;
    dtn_storage_destroy(storage);  // closes DB

    // Reopen — load_packets_from_db must reconstruct the list.
    storage = dtn_storage_create(NULL);
    if (!storage) {
        DTN_TEST("FAIL: third dtn_storage_create returned NULL\n");
        return;
    }

    if (storage->stored_packets_count != 1) {
        DTN_TEST("FAIL: expected 1 packet after reload, got %zu\n",
                 storage->stored_packets_count);
        dtn_storage_destroy(storage);
        return;
    }

    Stored_Packet_Entry* loaded = storage->packet_list_head;
    if (loaded->db_id != saved_db_id) {
        DTN_TEST("FAIL: db_id mismatch after reload (expected %" PRId64 ", got %" PRId64 ")\n",
                 saved_db_id, loaded->db_id);
        dtn_storage_destroy(storage);
        return;
    }
    if (loaded->delivery_time_in_sec != 77.25) {
        DTN_TEST("FAIL: delivery_time_in_sec not preserved (got %f)\n",
                 loaded->delivery_time_in_sec);
        dtn_storage_destroy(storage);
        return;
    }
    if (loaded->max_delivery_time_in_sec != 200.0) {
        DTN_TEST("FAIL: max_delivery_time_in_sec not preserved (got %f)\n",
                 loaded->max_delivery_time_in_sec);
        dtn_storage_destroy(storage);
        return;
    }
    DTN_TEST("PASS: round-trip OK — db_id=%" PRId64
             ", delivery_time=%.2f, max_delivery_time=%.2f\n",
             loaded->db_id, loaded->delivery_time_in_sec, loaded->max_delivery_time_in_sec);

    // Clean up.
    dtn_storage_destroy(storage);
    DTN_TEST("=== test_storage_sqlite PASSED ===\n");
}

void test_init_config() {
    // setenv(DTN_CONFIG_PATH, "tests/node_test.toml", 1);

    if (dtn_config_load(&dtn_config) != 0)
        return;

    dtn_config_print(&dtn_config);

    DTN_TEST("Node ID: %d, DTN: %s\n", dtn_config.id, dtn_config.is_dtn ? "yes" : "no");
    DTN_TEST("Interfaces: %d\n", dtn_config.interface_count);
}

int main() {
    lwip_init();
    test_init_config();
    dtn_log_init();

    struct pbuf* p = pbuf_alloc(PBUF_RAW, sizeof(struct ip6_hdr) + PAYLOAD_LEN, PBUF_RAM);
    if (p == NULL) {
        printf("pbuf_alloc returned NULL - fix lwipopts.h memory config\n");
        return 1;
    }

    memset(p->payload, 0, sizeof(struct ip6_hdr) + PAYLOAD_LEN);

    struct ip6_hdr* ip6h = (struct ip6_hdr*)p->payload;

    IP6H_VTCFL_SET(ip6h, 6, 0x04, 0x12345);
    IP6H_PLEN_SET(ip6h, PAYLOAD_LEN);
    IP6H_NEXTH_SET(ip6h, 59);
    IP6H_HOPLIM_SET(ip6h, 64);

    ip6_addr_t src, dst;
    ip6addr_aton("fd00:1:2::1", &src);
    ip6addr_aton("fd00:2:3::3", &dst);
    // ip6addr_aton("fd00:3:4::4", &dst);
    // ip6addr_aton("fe00:2:5::5", &dst);
    ip6_addr_copy_to_packed(ip6h->src, src);
    ip6_addr_copy_to_packed(ip6h->dest, dst);

    uint8_t* payload_ptr = (uint8_t*)p->payload + sizeof(struct ip6_hdr);
    memset(payload_ptr, 0xAB, PAYLOAD_LEN);

    DTN_TEST("START TESTING\n");

    int next_hop_node_id;
    dtn_routing_get_next_hop_node_id(0, 0 * 1000, ip6h, &next_hop_node_id);
    DTN_TEST("next_hop_node_id: %d\n", next_hop_node_id);
    dtn_routing_get_next_hop_node_id(0, 21 * 1000, ip6h, &next_hop_node_id);
    DTN_TEST("next_hop_node_id: %d\n", next_hop_node_id);

    test_storage_sqlite();

    DTN_TEST("STOP TESTING\n");

    pbuf_free(p);
    return 0;
}