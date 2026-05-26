#include "dtn_config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dtn_logger.h"

/* tomlc99 – single-file library, expected at include/toml.h */
#include "toml.h"

DtnConfig dtn_config;

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/* Copy a toml string value into a fixed-size buffer; returns 0 on success. */
static int copy_str(toml_table_t* tbl, const char* key, char* dst, size_t dstsz) {
    toml_datum_t d = toml_string_in(tbl, key);
    if (!d.ok)
        return -1;
    snprintf(dst, dstsz, "%s", d.u.s);
    free(d.u.s);
    return 0;
}

/* Read an optional string; silently skip when the key is absent. */
static void copy_str_opt(toml_table_t* tbl, const char* key, char* dst, size_t dstsz) {
    copy_str(tbl, key, dst, dstsz); /* ignore return value */
}

/* Read an int64 value; returns 0 on success. */
static int read_int(toml_table_t* tbl, const char* key, int64_t* out) {
    toml_datum_t d = toml_int_in(tbl, key);
    if (!d.ok)
        return -1;
    *out = d.u.i;
    return 0;
}

/* Read a double value; returns 0 on success. */
static int read_double(toml_table_t* tbl, const char* key, double* out) {
    toml_datum_t d = toml_double_in(tbl, key);
    if (!d.ok)
        return -1;
    *out = d.u.d;
    return 0;
}

/* Read a bool value; returns 0 on success. */
static int read_bool(toml_table_t* tbl, const char* key, bool* out) {
    toml_datum_t d = toml_bool_in(tbl, key);
    if (!d.ok)
        return -1;
    *out = (bool)d.u.b;
    return 0;
}

/* Read a TOML array-of-strings into a char[][LEN] buffer. */
static int read_str_array(toml_array_t* arr, char dst[][DTN_MAX_ADDR_LEN], int* count,
                          int max_count) {
    *count = 0;
    if (!arr)
        return 0;

    int n = toml_array_nelem(arr);
    for (int i = 0; i < n && i < max_count; i++) {
        toml_datum_t d = toml_string_at(arr, i);
        if (!d.ok)
            continue;
        snprintf(dst[*count], DTN_MAX_ADDR_LEN, "%s", d.u.s);
        free(d.u.s);
        (*count)++;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Parse [[interface]] entries
 * ---------------------------------------------------------------------- */
static int parse_interfaces(toml_table_t* root, DtnConfig* cfg) {
    toml_array_t* arr = toml_array_in(root, "interface");
    if (!arr)
        return 0; /* no interfaces is valid */

    int n = toml_array_nelem(arr);
    for (int i = 0; i < n && i < DTN_MAX_INTERFACES; i++) {
        toml_table_t* t = toml_table_at(arr, i);
        if (!t)
            continue;

        DtnInterface* iface = &cfg->interfaces[cfg->interface_count];

        copy_str_opt(t, "name", iface->name, DTN_MAX_NAME_LEN);
        copy_str_opt(t, "local_addr", iface->local_addr, DTN_MAX_ADDR_LEN);
        copy_str_opt(t, "local_mac", iface->local_mac, DTN_MAX_MAC_LEN);
        copy_str_opt(t, "remote_addr", iface->remote_addr, DTN_MAX_ADDR_LEN);
        copy_str_opt(t, "remote_mac", iface->remote_mac, DTN_MAX_MAC_LEN);

        int64_t tmp = 0;
        if (read_int(t, "remote_node_id", &tmp) == 0)
            iface->remote_node_id = (int)tmp;
        if (read_int(t, "start_in_sec", &tmp) == 0)
            iface->start_in_sec = tmp;
        if (read_int(t, "end_in_sec", &tmp) == 0)
            iface->end_in_sec = tmp;

        read_double(t, "rate", &iface->rate);
        read_double(t, "range", &iface->range);

        read_str_array(toml_array_in(t, "dtn_addresses"), iface->dtn_addresses,
                       &iface->dtn_address_count, DTN_MAX_ADDRESSES);

        read_str_array(toml_array_in(t, "addresses"), iface->addresses, &iface->address_count,
                       DTN_MAX_ADDRESSES);

        cfg->interface_count++;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Parse [[nodes]] entries
 * ---------------------------------------------------------------------- */
static int parse_nodes(toml_table_t* root, DtnConfig* cfg) {
    toml_array_t* arr = toml_array_in(root, "nodes");
    if (!arr)
        return 0;

    int n = toml_array_nelem(arr);
    for (int i = 0; i < n && i < DTN_MAX_NODES; i++) {
        toml_table_t* t = toml_table_at(arr, i);
        if (!t)
            continue;

        DtnNodeEntry* node = &cfg->contact_plan.nodes[cfg->contact_plan.node_count];

        int64_t id = 0;
        if (read_int(t, "id", &id) != 0)
            continue; /* id is mandatory */
        node->id = (int)id;

        copy_str_opt(t, "name", node->name, DTN_MAX_NAME_LEN);
        read_bool(t, "isDtnNode", &node->is_dtn_node);

        cfg->contact_plan.node_count++;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Parse [[edges]] entries
 * ---------------------------------------------------------------------- */
static int parse_edges(toml_table_t* root, DtnConfig* cfg) {
    toml_array_t* arr = toml_array_in(root, "edges");
    if (!arr)
        return 0;

    int n = toml_array_nelem(arr);
    for (int i = 0; i < n && i < DTN_MAX_EDGES; i++) {
        toml_table_t* t = toml_table_at(arr, i);
        if (!t)
            continue;

        DtnEdge* edge = &cfg->contact_plan.edges[cfg->contact_plan.edge_count];
        int64_t tmp = 0;

        if (read_int(t, "from", &tmp) == 0)
            edge->from = (int)tmp;
        if (read_int(t, "to", &tmp) == 0)
            edge->to = (int)tmp;
        if (read_int(t, "start_in_sec", &tmp) == 0)
            edge->start = tmp;
        if (read_int(t, "end_in_sec", &tmp) == 0)
            edge->end = tmp;
        read_bool(t, "bidirected", &edge->bidirected);

        cfg->contact_plan.edge_count++;
    }
    return 0;
}

/* =========================================================================
 * Public API
 * ====================================================================== */

void dtn_config_init(DtnConfig* cfg) { memset(cfg, 0, sizeof(*cfg)); }

int dtn_config_load(DtnConfig* cfg) {
    char* config_path = getenv(DTN_CONFIG_PATH);
    if (config_path == NULL || strlen(config_path) == 0) {
        DTN_ERROR("ERROR: Environment variable 'DTN_CONFIG_PATH' is not set or is empty.");
        // Exit with a failure code
        exit(EXIT_FAILURE);
    }

    DTN_INFO("DTN_CONFIG_PATH: %s", config_path);

    dtn_config_init(cfg);

    // Setup ENV
    char* env_val = getenv("DTN_ENV");
    cfg->env = DTN_ENV_DEVELOPMENT;
    if (env_val != NULL) {
        if (strcmp(env_val, "PRODUCTION") == 0) {
            cfg->env = DTN_ENV_PRODUCTION;
        } else if (strcmp(env_val, "DOCKER") == 0) {
            cfg->env = DTN_ENV_DOCKER;
        }
    }
    strncpy(cfg->contact_plan_path, config_path, DTN_MAX_CONFIG_PATH - 1);

    // Setup Node Configuration
    FILE* fp = fopen(config_path, "r");
    if (!fp) {
        DTN_ERROR("dtn_config: cannot open '%s': %s", config_path, strerror(errno));
        return -1;
    }

    char errbuf[256] = {0};
    toml_table_t* root = toml_parse_file(fp, errbuf, sizeof(errbuf));
    fclose(fp);

    if (!root) {
        DTN_ERROR("dtn_config: parse error in '%s': %s", config_path, errbuf);
        return -1;
    }

    /* ---- [node] ---- */
    toml_table_t* node_tbl = toml_table_in(root, "node");
    if (node_tbl) {
        int64_t id = 0;
        if (read_int(node_tbl, "id", &id) == 0)
            cfg->id = (int)id;
        copy_str_opt(node_tbl, "name", cfg->name, DTN_MAX_NAME_LEN);
        read_bool(node_tbl, "is_dtn", &cfg->is_dtn);
        copy_str(node_tbl, "tun_ipv6_addr", cfg->tun_ipv6_addr, DTN_MAX_ADDR_LEN);
        copy_str(node_tbl, "lwip_ipv6_addr", cfg->lwip_ipv6_addr, DTN_MAX_ADDR_LEN);

        read_str_array(toml_array_in(node_tbl, "dtn_addresses"), cfg->dtn_addresses,
                       &cfg->dtn_address_count, DTN_MAX_ADDRESSES);

        read_str_array(toml_array_in(node_tbl, "addresses"), cfg->addresses, &cfg->address_count,
                       DTN_MAX_ADDRESSES);
    }

    /* ---- [[interface]] ---- */
    parse_interfaces(root, cfg);

    /* ---- [contact_plan] ---- */
    toml_table_t* cp = toml_table_in(root, "contact_plan");
    if (cp) {
        copy_str_opt(cp, "name", cfg->contact_plan.name, DTN_MAX_NAME_LEN);
        int64_t tmp = 0;
        if (read_int(cp, "max_time_in_sec", &tmp) == 0)
            cfg->contact_plan.max_time_in_sec = tmp;

        /* ---- [contact_plan.defaults] ---- */
        toml_table_t* def = toml_table_in(cp, "defaults");
        if (def) {
            read_double(def, "rate", &cfg->contact_plan.default_rate);
            read_double(def, "range", &cfg->contact_plan.default_range);
        }
    }

    /* storage_path needs both node id and contact plan name */
    snprintf(cfg->storage_path, sizeof(cfg->storage_path), "./dtn_storage/%s",
             cfg->contact_plan.name[0] ? cfg->contact_plan.name : "default");

    /* ---- [[nodes]] ---- */
    parse_nodes(root, cfg);

    /* ---- [[edges]] ---- */
    parse_edges(root, cfg);

    toml_free(root);
    return 0;
}

void dtn_config_print(const DtnConfig* cfg) {
    DTN_INFO("=== DtnConfig ===");
    DTN_INFO("[node]");
    DTN_INFO("  id       = %d", cfg->id);
    DTN_INFO("  name     = \"%s\"", cfg->name);
    DTN_INFO("  is_dtn   = %s", cfg->is_dtn ? "true" : "false");
    DTN_INFO("  tun_ipv6_addr   = %s", cfg->tun_ipv6_addr);
    DTN_INFO("  lwip_ipv6_addr   = %s", cfg->lwip_ipv6_addr);
    DTN_INFO("  storage_path    = %s", cfg->storage_path);

    DTN_INFO("  dtn_addresses (%d):", cfg->dtn_address_count);
    for (int i = 0; i < cfg->dtn_address_count; i++)
        DTN_INFO("    [%d] %s", i, cfg->dtn_addresses[i]);

    DTN_INFO("  addresses (%d):", cfg->address_count);
    for (int i = 0; i < cfg->address_count; i++) DTN_INFO("    [%d] %s", i, cfg->addresses[i]);

    DTN_INFO("[[interface]] count = %d", cfg->interface_count);
    for (int i = 0; i < cfg->interface_count; i++) {
        const DtnInterface* iface = &cfg->interfaces[i];
        DTN_INFO("  [%d] name=%s  local=%s  remote=%s  node=%d  start=%lld  end=%lld", i,
                 iface->name, iface->local_addr, iface->remote_addr, iface->remote_node_id,
                 (long long)iface->start_in_sec, (long long)iface->end_in_sec);
        DTN_INFO("       dtn_addresses (%d):", iface->dtn_address_count);
        for (int j = 0; j < iface->dtn_address_count; j++)
            DTN_INFO("         %s", iface->dtn_addresses[j]);
    }

    DTN_INFO("[contact_plan] name = \"%s\"  max_time_in_sec = %lld", cfg->contact_plan.name,
             (long long)cfg->contact_plan.max_time_in_sec);
    DTN_INFO("  [defaults] rate=%.2f  range=%.2f", cfg->contact_plan.default_rate,
             cfg->contact_plan.default_range);

    DTN_INFO("  [[nodes]] count = %d", cfg->contact_plan.node_count);
    for (int i = 0; i < cfg->contact_plan.node_count; i++) {
        const DtnNodeEntry* n = &cfg->contact_plan.nodes[i];
        DTN_INFO("    id=%d  name=\"%s\"  isDtnNode=%s", n->id, n->name,
                 n->is_dtn_node ? "true" : "false");
    }

    DTN_INFO("  [[edges]] count = %d", cfg->contact_plan.edge_count);
    for (int i = 0; i < cfg->contact_plan.edge_count; i++) {
        const DtnEdge* e = &cfg->contact_plan.edges[i];
        DTN_INFO("    %d -> %d  start=%lld  end=%lld  bidirected=%s", e->from, e->to,
                 (long long)e->start, (long long)e->end, e->bidirected ? "true" : "false");
    }
}