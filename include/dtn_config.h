#ifndef CONFIG_H
#define CONFIG_H

typedef struct {
    const char *tun_ipv6_addr;
    const char *lwip_ipv6_addr;
    const char *enp0s9_ipv6_addr;
    const char *enp0s8_ipv6_addr;
    const char *curr_node_addr;
    int         node_id;
    const char *contact_plan_path;
} DtnConfig;

extern DtnConfig dtn_config;  /* global instance, defined in config.c */

void dtn_config_load(void);
void dtn_config_print(const DtnConfig *cfg);

#endif