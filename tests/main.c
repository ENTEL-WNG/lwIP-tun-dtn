
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

void test_init_config() {
    setenv(DTN_CONFIG_PATH, "networks/contact_plan_2/node2.toml", 1);

    if (dtn_config_load(&dtn_config) != 0)
        return;

    dtn_config_print(&dtn_config);

    printf("Node ID: %d, DTN: %s\n", dtn_config.id, dtn_config.is_dtn ? "yes" : "no");
    printf("Interfaces: %d\n", dtn_config.interface_count);
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
    ip6addr_aton("fd00:4:5::4", &dst);
    // ip6addr_aton("fd00:3:4::4", &dst);
    // ip6addr_aton("fe00:2:5::5", &dst);
    ip6_addr_copy_to_packed(ip6h->src, src);
    ip6_addr_copy_to_packed(ip6h->dest, dst);

    uint8_t* payload_ptr = (uint8_t*)p->payload + sizeof(struct ip6_hdr);
    memset(payload_ptr, 0xAB, PAYLOAD_LEN);

    printf("START TESTING\n");

    // test_config();

    // test_payload_length(ip6h);

    // ip6_addr_t edge_to_ipv6_addr;
    // edge_to_ipv6(3, 4, &edge_to_ipv6_addr);
    // edge_to_ipv6(10, 3, &edge_to_ipv6_addr);

    int next_hop_node_id;
    dtn_routing_get_next_hop_node_id(0, 0 * 1000, ip6h, &next_hop_node_id);
    printf("next_hop_node_id: %d\n", next_hop_node_id);
    dtn_routing_get_next_hop_node_id(0, 21 * 1000, ip6h, &next_hop_node_id);
    printf("next_hop_node_id: %d\n", next_hop_node_id);

    printf("STOP TESTING\n");

    pbuf_free(p);
    return 0;
}