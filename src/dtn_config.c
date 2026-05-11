#include "dtn_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

DtnConfig dtn_config;;

static const char *env_str(const char *key, const char *fallback) {
    const char *val = getenv(key);
    return (val && *val) ? val : fallback;
}

static int env_int(const char *key, int fallback) {
    const char *val = getenv(key);
    if (!val || !*val) return fallback;
    char *end;
    long n = strtol(val, &end, 10);
    return (*end == '\0') ? (int)n : fallback;
}

void dtn_config_load(void) {
    dtn_config.tun_ipv6_addr    = env_str("HOST_TUN_IPV6_ADDR",   "fd00::1");
    dtn_config.lwip_ipv6_addr   = env_str("HOST_LWIP_IPV6_ADDR",  "fd00::2");
    dtn_config.enp0s9_ipv6_addr = env_str("HOST_enp0s9_IPV6_ADDR", "fd00:01::2");
    dtn_config.enp0s8_ipv6_addr = env_str("HOST_enp0s8_IPV6_ADDR", "fd00:12::1");
    dtn_config.curr_node_addr   = env_str("CURR_NODE_ADDR",        "fd00:12::1");
    dtn_config.node_id          = env_int("NODE_ID", 1);
    dtn_config.contact_plan_path = env_str("CONTACT_PLAN_PATH", "py_cgr/contact_plans/cgr_tutorial_1.txt");
}

void dtn_config_print(const DtnConfig *dtn_config) {
    printf("tun_ipv6_addr    : %s\n", dtn_config->tun_ipv6_addr);
    printf("lwip_ipv6_addr   : %s\n", dtn_config->lwip_ipv6_addr);
    printf("enp0s9_ipv6_addr : %s\n", dtn_config->enp0s9_ipv6_addr);
    printf("enp0s8_ipv6_addr : %s\n", dtn_config->enp0s8_ipv6_addr);
    printf("curr_node_addr   : %s\n", dtn_config->curr_node_addr);
    printf("node_id          : %d\n", dtn_config->node_id);
    printf("contact_plan_path: %s\n", dtn_config->contact_plan_path);
}