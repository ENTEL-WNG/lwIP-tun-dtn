#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dtn_config.h"
#include "dtn_logger.h"

// Define the global instance
DTNConfig dtn_config;

char* get_required_env(const char *env_name) {
    char *val = getenv(env_name);
    if (val == NULL || strlen(val) == 0) {
        DTN_FATAL("Missing required environment variable: %s", env_name);
        exit(EXIT_FAILURE); 
    }

    return val;
}

void fetch_env_string(const char *env_name, char *dest, size_t dest_size) {
    char *val = get_required_env(env_name);
    strncpy(dest, val, dest_size - 1);
    dest[dest_size - 1] = '\0';
}

int fetch_env_int(const char *env_name) {
    char *val = get_required_env(env_name);
    return atoi(val);
}

void init_config(void) {
    fetch_env_string("INT_1", dtn_config.INT_1, sizeof(dtn_config.INT_1));
    fetch_env_string("INT_2", dtn_config.INT_2, sizeof(dtn_config.INT_2));
    fetch_env_string("HOST_TUN_IPV6_ADDR", dtn_config.HOST_TUN_IPV6_ADDR, sizeof(dtn_config.HOST_TUN_IPV6_ADDR));
    fetch_env_string("HOST_LWIP_IPV6_ADDR", dtn_config.HOST_LWIP_IPV6_ADDR, sizeof(dtn_config.HOST_LWIP_IPV6_ADDR));

    dtn_config.NODE = fetch_env_int("NODE");

    dtn_config.DEBUG = (getenv("DEBUG") != NULL);

    DTN_INFO("Loaded config:");
    DTN_INFO("NODE: %d", dtn_config.NODE);
    DTN_INFO("INT_1: %s", dtn_config.INT_1);
    DTN_INFO("INT_2: %s", dtn_config.INT_2);
    DTN_INFO("HOST_TUN_IPV6_ADDR: %s", dtn_config.HOST_TUN_IPV6_ADDR);
    DTN_INFO("HOST_LWIP_IPV6_ADDR: %s", dtn_config.HOST_LWIP_IPV6_ADDR);
}