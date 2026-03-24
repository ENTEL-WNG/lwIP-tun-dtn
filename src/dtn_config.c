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

void load_interfaces() {
    dtn_config.interface_count = 0;
    dtn_config.interfaces = malloc(10 * sizeof(DTNInterfaceConfig*));

    int i = 1;
    char env_name[16];

    while (1) {
        sprintf(env_name, "INT_%d", i);
        if (getenv(env_name) == NULL) {
            break; 
        }

        if (dtn_config.interface_count >= 10) {
            DTN_FATAL("dtn_config.interface_count is too small");
        }

        char *raw_val = getenv(env_name);
        char *val = strdup(raw_val);
        DTNInterfaceConfig *dtn_interface_config = malloc(sizeof(DTNInterfaceConfig));

        char *token = strtok(val, ", ");
        int index = 0;

        // TODO:: optimize
        dtn_interface_config->routes = malloc(10 * sizeof(char*)); 
        dtn_interface_config->route_count = 0;

        while (token != NULL) {
            if (index == 0) {
                dtn_interface_config->name = strdup(token);
            } else if (index == 1) {
                dtn_interface_config->addr = strdup(token);
            } else if (index == 2) {
                dtn_interface_config->addr_via = strdup(token);
            } else {
                dtn_interface_config->routes[dtn_interface_config->route_count++] = strdup(token);
            }
            
            token = strtok(NULL, ", ");
            index++;
        }

        dtn_config.interfaces[dtn_config.interface_count] = dtn_interface_config;
        dtn_config.interface_count++;

        free(val);
        i++;
    }
}

void init_config(void) {
    dtn_config.NODE = fetch_env_int("NODE");
    dtn_config.DEBUG = (getenv("DEBUG") != NULL);
    fetch_env_string("HOST_TUN_IPV6_ADDR", dtn_config.HOST_TUN_IPV6_ADDR, sizeof(dtn_config.HOST_TUN_IPV6_ADDR));
    fetch_env_string("HOST_LWIP_IPV6_ADDR", dtn_config.HOST_LWIP_IPV6_ADDR, sizeof(dtn_config.HOST_LWIP_IPV6_ADDR));
    load_interfaces();
    

    
    DTN_INFO(" ==== CONFIG ====");
    DTN_INFO("NODE: %d", dtn_config.NODE);

    for (int i = 0; i < dtn_config.interface_count; i++) {
        DTNInterfaceConfig* iface = dtn_config.interfaces[i];
        
        DTN_INFO("Interface %d: %s", i + 1, iface->name);
        DTN_INFO("Addr: %s", iface->addr);
        
        for (int r = 0; r < iface->route_count; r++) {
            DTN_INFO("Route %d: %s via %s", r, iface->routes[r], iface->addr_via);
        }
    }
    // DTN_INFO("INT_1: %s", dtn_config.INT_1);
    // DTN_INFO("INT_2: %s", dtn_config.INT_2);
    DTN_INFO("HOST_TUN_IPV6_ADDR: %s", dtn_config.HOST_TUN_IPV6_ADDR);
    DTN_INFO("HOST_LWIP_IPV6_ADDR: %s", dtn_config.HOST_LWIP_IPV6_ADDR);
    DTN_INFO("=================");
}