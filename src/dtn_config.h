#ifndef CONFIG_H
#define CONFIG_H

typedef struct {
    char    HOST_TUN_IPV6_ADDR[128];
    int     DEBUG;
} DTNConfig;

// Declare the global config instance
extern DTNConfig dtn_config;

// Function to populate the struct from environment variables
void init_config(void);

#endif