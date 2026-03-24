#ifndef DTN_CONFIG_H
#define DTN_CONFIG_H

typedef struct {
    char *name;
    char *addr;
    char *addr_via;
    char **routes;    // Array of strings
    int route_count;  // To keep track of how many routes we found
} DTNInterfaceConfig;

typedef struct {
    int     DEBUG;
    char    APP_NAME[128];
    int     NODE;
    // char    INT_1[128];
    // char    INT_2[128];
    char    HOST_TUN_IPV6_ADDR[128];
    char    HOST_LWIP_IPV6_ADDR[128];
    DTNInterfaceConfig** interfaces;
    int interface_count;
} DTNConfig;



extern DTNConfig dtn_config;

void init_config(void);

#endif