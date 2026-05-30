
#include <inttypes.h>
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

// ---------------------------------------------------------------------------
// Test framework
// ---------------------------------------------------------------------------

// Evaluates cond; logs PASS/FAIL and returns false on failure so the caller
// can propagate it with  return TEST_ASSERT(...);  or accumulate with  ok &= ...
#define TEST_ASSERT(cond, fmt, ...)                                            \
    ({                                                                         \
        bool _ok = (bool)(cond);                                               \
        if (_ok)                                                               \
            DTN_TEST("PASS: " fmt, ##__VA_ARGS__);                             \
        else                                                                   \
            DTN_TEST("FAIL [%s:%d]: " fmt, __FILE__, __LINE__, ##__VA_ARGS__); \
        _ok;                                                                   \
    })

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

#define PAYLOAD_LEN 64
#define TEST_STORAGE_DIR "/tmp/dtn_test_storage"

// Minimal DtnRoutingResult — used by storage tests.
static DtnRoutingResult make_routing_result(double delivery, double to) {
    DtnRoutingResult r;
    memset(&r, 0, sizeof(r));
    r.best_delivery_time = delivery;
    r.to_time = to;
    return r;
}

static struct pbuf* make_test_packet(uint16_t payload_length, const char* src, const char* dest, uint8_t payload_value) {
    struct pbuf* p = pbuf_alloc(PBUF_RAW, sizeof(struct ip6_hdr) + payload_length, PBUF_RAM);
    if (!p)
        return NULL;
    memset(p->payload, 0, sizeof(struct ip6_hdr) + payload_length);

    struct ip6_hdr* ip6h = (struct ip6_hdr*)p->payload;
    IP6H_VTCFL_SET(ip6h, 6, 0, 0);
    IP6H_PLEN_SET(ip6h, payload_length);  // IP6H_PLEN_SET already applies htons internally
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

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

bool test_payload_length(void) {
    struct pbuf* p = make_test_packet(PAYLOAD_LEN, "fd00:1:2::1", "fd00:4:5::5", 0xAB);
    struct ip6_hdr* ip6h = (struct ip6_hdr*)p->payload;

    DTN_TEST("ip6h->_plen                 : %u", ip6h->_plen);
    DTN_TEST("ip6h->_plen hex             : 0x%04x", ip6h->_plen);

    u16_t _payload_length = lwip_ntohs(ip6h->_plen);
    DTN_TEST("lwip_ntohs(ip6h->_plen)     : %u", _payload_length);
    DTN_TEST("lwip_ntohs(ip6h->_plen) hex : 0x%04x", _payload_length);

    u16_t payload_length;
    memcpy(&payload_length, &ip6h->_plen, sizeof(u16_t));
    DTN_TEST("memcpy                      : %u", payload_length);
    DTN_TEST("memcpy hex                  : 0x%04x", payload_length);

    bool ok = true;
    ok &= TEST_ASSERT(_payload_length == PAYLOAD_LEN, "plen round-trip: expected %u, got %u", PAYLOAD_LEN, _payload_length);

    pbuf_free(p);
    return ok;
}

bool test_custodian(void) {
    bool ok = true;

    struct pbuf* p = make_test_packet(PAYLOAD_LEN, "fd00:1:2::1", "fd00:4:5::5", 0xAB);

    ip6_addr_t custodian;
    bool has_custodian = dtn_extract_custodian_option(p, &custodian);
    ok &= TEST_ASSERT(!has_custodian, "no custodian before add");

    // -----------------------------------------------------------------
    // 1. Add custodian with packet_id = 0 (fresh packet, not stored)
    // -----------------------------------------------------------------
    ip6_addr_t custodian_addr;
    ip6addr_aton("fd00:2:5::2", &custodian_addr);
    struct pbuf* p_custodian = dtn_update_or_add_custodian_option(p, &custodian_addr, 0);
    pbuf_free(p);
    ok &= TEST_ASSERT(p_custodian != NULL, "dtn_update_or_add_custodian_option returned NULL");

    has_custodian = dtn_extract_custodian_option(p_custodian, &custodian);
    ok &= TEST_ASSERT(has_custodian, "custodian present after add");

    char exp[IP6ADDR_STRLEN_MAX], got[IP6ADDR_STRLEN_MAX];
    ip6addr_ntoa_r(&custodian_addr, exp, sizeof(exp));
    ip6addr_ntoa_r(&custodian, got, sizeof(got));
    ok &= TEST_ASSERT(strcmp(exp, got) == 0, "custodian addr: expected '%s', got '%s'", exp, got);

    // packet_id = 0 → extract should return false and 0
    u32_t extracted_id = 99;
    bool has_id = dtn_extract_packet_id_from_hbh(p_custodian, &extracted_id);
    ok &= TEST_ASSERT(!has_id, "packet_id not present when set to 0");
    ok &= TEST_ASSERT(extracted_id == 0, "extracted packet_id is 0 when not set");

    // -----------------------------------------------------------------
    // 2. Update custodian with a real packet_id (simulates forwarding a
    //    stored packet whose DB row id is e.g. 42)
    // -----------------------------------------------------------------
    const u32_t TEST_PACKET_ID = 42;
    ip6_addr_t custodian_addr2;
    ip6addr_aton("fd00:3:6::3", &custodian_addr2);
    struct pbuf* p_with_id = dtn_update_or_add_custodian_option(p_custodian, &custodian_addr2, TEST_PACKET_ID);
    pbuf_free(p_custodian);
    ok &= TEST_ASSERT(p_with_id != NULL, "dtn_update_or_add_custodian_option (with packet_id) returned NULL");

    u32_t round_trip_id = 0;
    has_id = dtn_extract_packet_id_from_hbh(p_with_id, &round_trip_id);
    ok &= TEST_ASSERT(has_id, "packet_id present after setting to 42");
    ok &= TEST_ASSERT(round_trip_id == TEST_PACKET_ID, "packet_id round-trip: expected %u, got %u", TEST_PACKET_ID, round_trip_id);

    // custodian address must also survive the update
    ip6_addr_t custodian2;
    dtn_extract_custodian_option(p_with_id, &custodian2);
    char exp2[IP6ADDR_STRLEN_MAX], got2[IP6ADDR_STRLEN_MAX];
    ip6addr_ntoa_r(&custodian_addr2, exp2, sizeof(exp2));
    ip6addr_ntoa_r(&custodian2, got2, sizeof(got2));
    ok &= TEST_ASSERT(strcmp(exp2, got2) == 0, "custodian addr preserved after packet_id update: expected '%s', got '%s'", exp2, got2);

    // -----------------------------------------------------------------
    // 3. Packet with no HBH → extract returns false
    // -----------------------------------------------------------------
    struct pbuf* p_plain = make_test_packet(PAYLOAD_LEN, "fd00:1::1", "fd00:2::2", 0x00);
    u32_t no_id = 99;
    ok &= TEST_ASSERT(!dtn_extract_packet_id_from_hbh(p_plain, &no_id), "no packet_id on plain packet");
    ok &= TEST_ASSERT(no_id == 0, "extracted id is 0 on plain packet");
    pbuf_free(p_plain);

    pbuf_free(p_with_id);
    return ok;
}

bool test_delete_by_packet_id(void) {
    bool ok = true;

    strncpy(dtn_config.storage_path, TEST_STORAGE_DIR, sizeof(dtn_config.storage_path) - 1);
    dtn_config.storage_path[sizeof(dtn_config.storage_path) - 1] = '\0';
    strncpy(dtn_config.name, "test_del_pid", sizeof(dtn_config.name) - 1);
    dtn_config.name[sizeof(dtn_config.name) - 1] = '\0';
    remove(TEST_STORAGE_DIR "/test_del_pid_dtn_packets.db");

    Storage_Function* storage = dtn_storage_create(NULL);
    ok &= TEST_ASSERT(storage != NULL, "storage created for delete_by_packet_id test");
    if (!storage)
        return false;

    // Store two packets with the same src/dest — this is the problematic case
    // that delete-by-src/dest cannot handle correctly.
    struct pbuf* pa = make_test_packet(32, "fd00:a::1", "fd00:b::2", 0x11);
    struct pbuf* pb = make_test_packet(32, "fd00:a::1", "fd00:b::2", 0x22);
    DtnRoutingResult rr = make_routing_result(0.0, 100.0);

    ok &= TEST_ASSERT(dtn_storage_store_packet(storage, pa, &rr) == DTN_STORAGE_STORE_OK, "store pa");
    ok &= TEST_ASSERT(dtn_storage_store_packet(storage, pb, &rr) == DTN_STORAGE_STORE_OK, "store pb");
    pbuf_free(pa);
    pbuf_free(pb);
    ok &= TEST_ASSERT(dtn_storage_count(storage) == 2, "two packets stored");

    // Read back entries; the row IDs are the packet_ids used for deletion.
    Stored_Packet_Entry entries[4];
    int n = dtn_storage_get_ready_entries(storage, 1000.0, entries, 4);
    ok &= TEST_ASSERT(n == 2, "two entries ready");
    if (n < 2) {
        dtn_storage_destroy(storage);
        return false;
    }

    int64_t id_a = entries[0].db_id;
    int64_t id_b = entries[1].db_id;
    pbuf_free(entries[0].p);
    pbuf_free(entries[1].p);

    // Delete only the first by its row-id-as-packet_id
    dtn_storage_delete_by_packet_id(storage, (u32_t)id_a);
    ok &= TEST_ASSERT(dtn_storage_count(storage) == 1, "one packet left after delete_by_packet_id");

    // Remaining packet must be the second one
    Stored_Packet_Entry remaining[2];
    int nr = dtn_storage_get_ready_entries(storage, 1000.0, remaining, 2);
    ok &= TEST_ASSERT(nr == 1, "one ready entry remains");
    if (nr == 1) {
        ok &= TEST_ASSERT(remaining[0].db_id == id_b, "surviving packet is id_b (%" PRId64 "), got %" PRId64, id_b, remaining[0].db_id);
        pbuf_free(remaining[0].p);
    }

    // Deleting with packet_id = 0 must be a no-op
    dtn_storage_delete_by_packet_id(storage, 0);
    ok &= TEST_ASSERT(dtn_storage_count(storage) == 1, "count unchanged after delete_by_packet_id(0)");

    dtn_storage_destroy(storage);
    return ok;
}

// Own node_id is defined in node_test.toml which is 2
bool test_routing(void) {
    bool ok = true;

    struct pbuf* p = make_test_packet(PAYLOAD_LEN, "fd00:1:2::1", "fd00:3:4::4", 0xAB);
    struct ip6_hdr* ip6h = (struct ip6_hdr*)p->payload;

    DtnRoutingResult routing_result;
    dtn_routing_get_next_hop_node_id(0, 0 * 1000, ip6h, &routing_result);
    DTN_TEST("t=0 | next_hop_node_id: %d | %f", routing_result.next_hop_node_id, routing_result.best_delivery_time);

    dtn_routing_get_next_hop_node_id(0, 21 * 1000, ip6h, &routing_result);
    DTN_TEST("t=21 | next_hop_node_id: %d | %f", routing_result.next_hop_node_id, routing_result.best_delivery_time);

    pbuf_free(p);
    return ok;
}

bool test_storage(void) {
    bool ok = true;

    // -----------------------------------------------------------------------
    // Setup: redirect storage to a temp dir and wipe any leftover DB so every
    // run starts from a clean slate.
    // -----------------------------------------------------------------------
    strncpy(dtn_config.storage_path, TEST_STORAGE_DIR, sizeof(dtn_config.storage_path) - 1);
    dtn_config.storage_path[sizeof(dtn_config.storage_path) - 1] = '\0';
    strncpy(dtn_config.name, "test_node", sizeof(dtn_config.name) - 1);
    dtn_config.name[sizeof(dtn_config.name) - 1] = '\0';
    remove(TEST_STORAGE_DIR "/test_node_dtn_packets.db");

    // -----------------------------------------------------------------------
    // 1. Create / destroy
    // -----------------------------------------------------------------------
    Storage_Function* storage = dtn_storage_create(NULL);
    ok &= TEST_ASSERT(storage != NULL, "storage_create returns non-NULL");
    if (!storage)
        return false;

    // -----------------------------------------------------------------------
    // 2. Empty-state queries
    // -----------------------------------------------------------------------
    ok &= TEST_ASSERT(dtn_storage_count(storage) == 0, "count is 0 on empty DB");
    ok &= TEST_ASSERT(dtn_storage_is_full(storage) == 0, "is_full is 0 on empty DB");

    // -----------------------------------------------------------------------
    // 3. Store one packet → count increases
    // -----------------------------------------------------------------------
    struct pbuf* p1 = make_test_packet(64, "fd00:1:2::1", "fd00:4:5::5", 0xAA);
    ok &= TEST_ASSERT(p1 != NULL, "make_test_packet p1 non-NULL");
    DtnRoutingResult rr1 = make_routing_result(0.0, 100.0);
    dtn_storage_store_packet_result_t store_res = dtn_storage_store_packet(storage, p1, &rr1);
    ok &= TEST_ASSERT(store_res == DTN_STORAGE_STORE_OK, "store_packet returns OK");
    ok &= TEST_ASSERT(dtn_storage_count(storage) == 1, "count is 1 after storing one packet");
    pbuf_free(p1);

    // -----------------------------------------------------------------------
    // 4. Get ready entries — packet IS ready (delivery_time 0.0 <= now 1000.0)
    // -----------------------------------------------------------------------
    Stored_Packet_Entry entries[10];
    int n = dtn_storage_get_ready_entries(storage, 1000.0, entries, 10);
    ok &= TEST_ASSERT(n == 1, "get_ready_entries returns 1 for a past-due packet");
    if (n >= 1) {
        ok &= TEST_ASSERT(entries[0].delivery_time_in_sec == 0.0, "delivery_time round-trips: expected 0.0, got %.2f",
                          entries[0].delivery_time_in_sec);
        ok &= TEST_ASSERT(strcmp(entries[0].dest_addr, "FD00:4:5::5") == 0, "dest_addr: expected 'FD00:4:5::5', got '%s'",
                          entries[0].dest_addr);
        ok &=
            TEST_ASSERT(strcmp(entries[0].src_addr, "FD00:1:2::1") == 0, "src_addr: expected 'FD00:1:2::1', got '%s'", entries[0].src_addr);
        ok &= TEST_ASSERT(!entries[0].has_custodian, "has_custodian is false for plain packet");
        ok &= TEST_ASSERT(entries[0].p != NULL, "entry pbuf is non-NULL");
        pbuf_free(entries[0].p);
    }

    // -----------------------------------------------------------------------
    // 5. Get ready entries — packet NOT yet ready (now -1.0 < delivery_time 0.0)
    // -----------------------------------------------------------------------
    int n2 = dtn_storage_get_ready_entries(storage, -1.0, entries, 10);
    ok &= TEST_ASSERT(n2 == 0, "get_ready_entries returns 0 when no packet is due yet");

    // -----------------------------------------------------------------------
    // 6. Delete by ID
    // -----------------------------------------------------------------------
    int n3 = dtn_storage_get_ready_entries(storage, 1000.0, entries, 10);
    ok &= TEST_ASSERT(n3 == 1, "one ready entry available before delete_by_id");
    if (n3 >= 1) {
        int64_t del_id = entries[0].db_id;
        pbuf_free(entries[0].p);
        dtn_storage_delete_by_id(storage, del_id);
        ok &= TEST_ASSERT(dtn_storage_count(storage) == 0, "count is 0 after delete_by_id");
    }

    // -----------------------------------------------------------------------
    // 7. Delete by packet_id (= row db_id cast to u32_t)
    // -----------------------------------------------------------------------
    struct pbuf* p2 = make_test_packet(32, "fd00:a:b::1", "fd00:c:d::2", 0xBB);
    ok &= TEST_ASSERT(p2 != NULL, "make_test_packet p2 non-NULL");
    DtnRoutingResult rr2 = make_routing_result(0.0, 200.0);
    ok &= TEST_ASSERT(dtn_storage_store_packet(storage, p2, &rr2) == DTN_STORAGE_STORE_OK, "store p2 returns OK");
    pbuf_free(p2);
    ok &= TEST_ASSERT(dtn_storage_count(storage) == 1, "count is 1 after storing p2");

    // Retrieve the entry to obtain the db_id, which doubles as the packet_id.
    Stored_Packet_Entry entries2[2];
    int n_p2 = dtn_storage_get_ready_entries(storage, 1000.0, entries2, 2);
    ok &= TEST_ASSERT(n_p2 == 1, "one ready entry for p2");
    if (n_p2 >= 1) {
        u32_t pkt_id2 = (u32_t)entries2[0].db_id;
        pbuf_free(entries2[0].p);
        dtn_storage_delete_by_packet_id(storage, pkt_id2);
        ok &= TEST_ASSERT(dtn_storage_count(storage) == 0, "count is 0 after delete_by_packet_id");
    }

    // -----------------------------------------------------------------------
    // 8. NULL-safety — must not crash; must return safe/error values
    // -----------------------------------------------------------------------
    ok &= TEST_ASSERT(dtn_storage_count(NULL) == 0, "count(NULL) returns 0");
    ok &= TEST_ASSERT(dtn_storage_is_full(NULL) == 0, "is_full(NULL) returns 0");
    ok &= TEST_ASSERT(dtn_storage_get_ready_entries(NULL, 0.0, entries, 10) == 0, "get_ready_entries(NULL, ...) returns 0");

    struct pbuf* p3 = make_test_packet(16, "fd00:1::1", "fd00:2::2", 0x00);
    ok &= TEST_ASSERT(p3 != NULL, "make_test_packet p3 non-NULL");
    DtnRoutingResult rr3 = make_routing_result(0.0, 0.0);
    ok &= TEST_ASSERT(dtn_storage_store_packet(NULL, p3, &rr3) == DTN_STORAGE_STORE_ERR, "store_packet(NULL storage) returns ERR");
    ok &= TEST_ASSERT(dtn_storage_store_packet(storage, NULL, &rr3) == DTN_STORAGE_STORE_ERR, "store_packet(NULL pbuf) returns ERR");
    pbuf_free(p3);

    dtn_storage_delete_by_id(NULL, 0);  // must not crash
    DTN_TEST("PASS: delete_by_id(NULL, 0) did not crash");

    // -----------------------------------------------------------------------
    // Teardown
    // -----------------------------------------------------------------------
    dtn_storage_destroy(storage);
    return ok;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    lwip_init();
    if (dtn_config_load(&dtn_config) != 0) {
        DTN_ERROR("Failed to load config");
        return -1;
    }

    dtn_config_print(&dtn_config);
    dtn_log_init(DTN_LOG_LEVEL_TEST);

    DTN_TEST("START TESTING");

    bool ok = true;

    DTN_TEST("CUSTODIAN / PACKET-ID TESTS");
    ok &= test_custodian();
    ok &= test_payload_length();
    DTN_TEST("%s", ok ? "CUSTODIAN / PACKET-ID TESTS PASSED" : "CUSTODIAN / PACKET-ID TESTS FAILED");

    DTN_TEST("ROUTING TESTS");
    ok &= test_routing();
    DTN_TEST("%s", ok ? "ROUTING TESTS PASSED" : "ROUTING TESTS FAILED");

    DTN_TEST("STORAGE TESTS");
    ok &= test_storage();
    DTN_TEST("%s", ok ? "STORAGE TESTS PASSED" : "STORAGE TESTS FAILED");

    DTN_TEST("DELETE-BY-PACKET-ID TESTS");
    ok &= test_delete_by_packet_id();
    DTN_TEST("%s", ok ? "DELETE-BY-PACKET-ID TESTS PASSED" : "DELETE-BY-PACKET-ID TESTS FAILED");

    DTN_TEST("%s", ok ? "ALL TESTS PASSED" : "SOME TESTS FAILED");

    return ok ? 0 : 1;
}
