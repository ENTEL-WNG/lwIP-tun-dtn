#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dtn_config.h"

// Define the global instance
DTNConfig dtn_config;

void init_config(void) {
    char *HOST_TUN_IPV6_ADDR = getenv("HOST_TUN_IPV6_ADDR");
    strncpy(dtn_config.HOST_TUN_IPV6_ADDR, HOST_TUN_IPV6_ADDR ? HOST_TUN_IPV6_ADDR : "fd00::1", sizeof(dtn_config.HOST_TUN_IPV6_ADDR) - 1);

    // char *port_str = getenv("DB_PORT");
    // dtn_config.db_port = port_str ? atoi(port_str) : 5432;

    dtn_config.DEBUG = (getenv("DEBUG") != NULL);
    
    printf("DTN Config Loaded: HOST_TUN_IPV6_ADDR=%s,\n Port=%d\n", dtn_config.HOST_TUN_IPV6_ADDR, dtn_config.DEBUG);
}