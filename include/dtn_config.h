#ifndef DTN_CONFIG_H
#define DTN_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Variables
 * ---------------------------------------------------------------------- */
#define PRINT_PAYLOAD 0
#define IS_DTN_ICMPV6_SEND_MESSAGE_DISABLED 0
#define IS_DTN_ICMPV6_RECEIVED_DISABLED 0
#define IS_DTN_ICMPV6_FORWARDED_DISABLED 1
#define IS_DTN_ICMPV6_DELIVERED_DISABLED 1
#define FORWARD_BEST_DELIVERY_TIME 0

#define MAX_STORED_PACKETS 1024 * 1024
#define MAX_STORED_PACKETS_FORWARD 128

// Stored-packet retransmission. A forwarded stored packet is normally deleted by
// the custody RECEIVED ACK; if that ACK (or the forward) is lost, re-forward the
// packet after DTN_FORWARD_RETRY_MS, up to DTN_MAX_FORWARD_ATTEMPTS, then drop it.
#define DTN_FORWARD_RETRY_MS 2000
#define DTN_MAX_FORWARD_ATTEMPTS 5

/* -------------------------------------------------------------------------
 * ENV Variables
 * ---------------------------------------------------------------------- */
#define DTN_CONFIG_PATH "DTN_CONFIG_PATH"
#define DTN_MAX_CONFIG_PATH 128

/* -------------------------------------------------------------------------
 * Limits
 * ---------------------------------------------------------------------- */
#define DTN_MAX_ADDRESSES 512
#define DTN_MAX_INTERFACES 512
#define DTN_MAX_NODES 512
#define DTN_MAX_EDGES 1024 * 8
#define DTN_MAX_ADDR_LEN 48 /* enough for a full IPv6 address + /prefix */
#define DTN_MAX_NAME_LEN 64
#define DTN_MAX_MAC_LEN 18 /* "xx:xx:xx:xx:xx:xx\0" */

/* -------------------------------------------------------------------------
 * Interface  (one [[interface]] entry)
 * ---------------------------------------------------------------------- */
typedef struct {
    char name[DTN_MAX_NAME_LEN];     /* logical name from TOML */
    char eth_name[DTN_MAX_NAME_LEN]; /* real kernel interface name (eth0, eth1, …) */
    char local_addr[DTN_MAX_ADDR_LEN];
    char local_mac[DTN_MAX_MAC_LEN];
    char remote_addr[DTN_MAX_ADDR_LEN];
    char remote_mac[DTN_MAX_MAC_LEN];
    int remote_node_id;

    char dtn_addresses[DTN_MAX_ADDRESSES][DTN_MAX_ADDR_LEN];
    int dtn_address_count;

    char addresses[DTN_MAX_ADDRESSES][DTN_MAX_ADDR_LEN];
    int address_count;

    // Socket assigned in raw_socket
    int socket;
    int socket_index;
} DtnInterface;

/* -------------------------------------------------------------------------
 * Node entry  (one [[nodes]] entry in the contact plan)
 * ---------------------------------------------------------------------- */
typedef struct {
    int id;
    char name[DTN_MAX_NAME_LEN]; /* optional – empty string if absent */
    bool is_dtn_node;
} DtnNodeEntry;

/* -------------------------------------------------------------------------
 * Edge entry  (one [[edges]] entry in the contact plan)
 * ---------------------------------------------------------------------- */
typedef struct {
    int from;
    int to;
    int64_t start;
    int64_t end;
    bool bidirected;
} DtnEdge;

/* -------------------------------------------------------------------------
 * Contact plan  ([contact_plan] + [[nodes]] + [[edges]])
 * ---------------------------------------------------------------------- */
typedef struct {
    char name[DTN_MAX_NAME_LEN];
    int64_t max_time_in_sec;

    /* [contact_plan.defaults] */
    double default_rate_in_bits_per_sec;
    double default_range;

    /* [[nodes]] */
    DtnNodeEntry nodes[DTN_MAX_NODES];
    int node_count;

    /* [[edges]] */
    DtnEdge edges[DTN_MAX_EDGES];
    int edge_count;
} DtnContactPlan;

/* -------------------------------------------------------------------------
 * Top-level config
 * ---------------------------------------------------------------------- */
typedef enum {
    DTN_ENV_PRODUCTION = 0,
    DTN_ENV_DEVELOPMENT = 1,
    DTN_ENV_DOCKER = 2,
} dtn_env_t;

/* -------------------------------------------------------------------------
 * Top-level config
 * ---------------------------------------------------------------------- */
typedef struct {
    /* env variables */
    dtn_env_t env;
    char contact_plan_path[DTN_MAX_CONFIG_PATH];
    char storage_path[DTN_MAX_CONFIG_PATH];

    /* [node] */
    int id;
    char name[DTN_MAX_NAME_LEN];
    bool is_dtn;

    char tun_ipv6_addr[DTN_MAX_ADDR_LEN];
    char lwip_ipv6_addr[DTN_MAX_ADDR_LEN];

    char dtn_addresses[DTN_MAX_ADDRESSES][DTN_MAX_ADDR_LEN];
    int dtn_address_count;

    char addresses[DTN_MAX_ADDRESSES][DTN_MAX_ADDR_LEN];
    int address_count;

    /* [[interface]] array */
    DtnInterface interfaces[DTN_MAX_INTERFACES];
    int interface_count;

    /* [contact_plan] + [[nodes]] + [[edges]] */
    DtnContactPlan contact_plan;
} DtnConfig;

/* -------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

/**
 * Parse a TOML config file into @cfg.
 *
 * @param cfg   Output struct – caller must provide storage.
 * @return      0 on success, -1 on error (message printed to stderr).
 */
int dtn_config_load(DtnConfig* cfg);

/**
 * Pretty-print the parsed config to stdout (useful for debugging).
 */
void dtn_config_print(const DtnConfig* cfg);

/**
 * Zero-initialise a DtnConfig struct.
 */
void dtn_config_init(DtnConfig* cfg);

extern DtnConfig dtn_config;

#endif /* DTN_CONFIG_H */