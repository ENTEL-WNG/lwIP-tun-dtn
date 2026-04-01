#ifndef DTN_CONFIG_H
#define DTN_CONFIG_H

typedef struct {
    char* name;
    char* addr;
    char* addr_via;
    char** routes;
    int route_count;
    // assigned during raw_socket_init
    int socket;
    int socket_index;

} DTNInterfaceConfig;

typedef struct {
    int DEBUG;
    char APP_NAME[128];
    int NODE;
    char HOST_TUN_IPV6_ADDR[128];
    char HOST_LWIP_IPV6_ADDR[128];
    DTNInterfaceConfig** interfaces;
    int interface_count;
} DTNConfig;

extern DTNConfig dtn_config;

void init_config(void);

#endif