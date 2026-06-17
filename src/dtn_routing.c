// dtn_routing.c: Implementation of Contact Graph Routing with time-variant contact management for
// DTN networks Copyright (C) 2025 Michael Karpov & 2026 Cèlia Torras
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "dtn_logger.h"
#include "lwip/ip6.h"
#include "lwip/ip6_addr.h"
#include "lwip/sys.h"
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <stdint.h>

#include "dtn_config.h"
#include "dtn_logger.h"
#include "dtn_routing.h"

#define MAX_LENGTH 5000
#define ROUTE_CACHE_SIZE 32
#define MAX_PY_OBJECTS 100

// Coarse bucket for the packet length in the route-cache key (~512 bytes). CGR
// may pick different routes by available volume, so we keep length in the key —
// but bucketed, so normal size variation reuses one entry instead of thrashing.
#define ROUTE_CACHE_LENGTH_BUCKET_BITS 4096
#define LENGTH_BUCKET(bits) ((long)((bits) / ROUTE_CACHE_LENGTH_BUCKET_BITS))

// Monotonic tick stamped on each cache hit/insert for LRU eviction.
static uint32_t route_cache_tick = 0;

static PyObject* py_registry[MAX_PY_OBJECTS];
static int py_registry_count = 0;

/* Route cache: avoid calling cgr_yen on every packet for the same destination
 * and packet size.  packet_length_in_bits is part of the key because CGR may
 * select different routes depending on available volume.
 * A cached entry is valid until its contact window expires (max_delivery_time). */

static RouteCacheEntry route_cache[ROUTE_CACHE_SIZE];
static int route_cache_count = 0;

/* Module-level pointer to the active Routing_Function so that
 * _dtn_routing_get_next_hop_node_id can reach the cached Python objects
 * without changing the public function signature. */
static Routing_Function* g_routing = NULL;

Routing_Function* dtn_routing_create(DTN_Module* parent) {
    Routing_Function* routing = (Routing_Function*)malloc(sizeof(Routing_Function));
    if (!routing) {
        DTN_ERROR("Failed to allocate memory for Routing_Function");
        return NULL;
    }
    routing->parent_module = parent;
    routing->routing_algorithm_name = "Contact Graph Routing";
    routing->base_time_in_ms = sys_now();
    routing->py_module = NULL;
    routing->py_cgr_yen = NULL;
    routing->py_fwd_candidate = NULL;
    routing->py_ipv6_packet = NULL;
    routing->py_contact_plan = NULL;

    /* --- Python interpreter (lives for the lifetime of the module) --- */
    Py_Initialize();
    if (!Py_IsInitialized()) {
        DTN_ERROR("Python not initialized");
        free(routing);
        return NULL;
    }

    PyObject* sys_path = PySys_GetObject("path");
    PyObject* py_pth = PyUnicode_FromString("py_cgr");
    PyList_Append(sys_path, py_pth);
    Py_DECREF(py_pth);

    PyObject* pModule = PyImport_ImportModule("py_cgr_lib.py_cgr_lib");
    if (!pModule) {
        DTN_ERROR("Cannot import py_cgr_lib.py_cgr_lib");
        PyErr_Print();
        Py_Finalize();
        free(routing);
        return NULL;
    }
    routing->py_module = pModule;

    /* Cache the four callables we use on every packet. */
    routing->py_cgr_yen = PyObject_GetAttrString(pModule, "cgr_yen");
    routing->py_fwd_candidate = PyObject_GetAttrString(pModule, "fwd_candidate");
    routing->py_ipv6_packet = PyObject_GetAttrString(pModule, "ipv6_packet");

    /* Load the contact plan once — cp_load is only needed here. */
    PyObject* py_cp_load = PyObject_GetAttrString(pModule, "cp_load");
    PyObject* args_load = PyTuple_New(3);
    PyTuple_SetItem(args_load, 0, PyUnicode_FromString(dtn_config.contact_plan_path));
    PyTuple_SetItem(args_load, 1, PyFloat_FromDouble(0.0)); /* session starts at t = 0 */
    PyTuple_SetItem(args_load, 2, PyLong_FromLong(MAX_LENGTH));
    PyObject* contact_plan = PyObject_CallObject(py_cp_load, args_load);
    Py_DECREF(args_load);
    Py_DECREF(py_cp_load);

    if (!contact_plan || contact_plan == Py_None) {
        DTN_ERROR("DTN Routing: cp_load returned NULL or None");
        PyErr_Print();
        Py_XDECREF((PyObject*)routing->py_ipv6_packet);
        Py_XDECREF((PyObject*)routing->py_fwd_candidate);
        Py_XDECREF((PyObject*)routing->py_cgr_yen);
        Py_XDECREF(pModule);
        Py_Finalize();
        free(routing);
        return NULL;
    }
    routing->py_contact_plan = contact_plan;

    g_routing = routing;
    DTN_INFO("DTN Routing Function created. Mode: %s", routing->routing_algorithm_name);
    return routing;
}

void dtn_routing_destroy(Routing_Function* routing) {
    if (!routing)
        return;

    DTN_INFO("Destroying DTN Routing Function...");
    route_cache_count = 0;
    Py_XDECREF((PyObject*)routing->py_contact_plan);
    Py_XDECREF((PyObject*)routing->py_ipv6_packet);
    Py_XDECREF((PyObject*)routing->py_fwd_candidate);
    Py_XDECREF((PyObject*)routing->py_cgr_yen);
    Py_XDECREF((PyObject*)routing->py_module);
    Py_Finalize();
    g_routing = NULL;
    free(routing);
}

PyObject* track_obj(PyObject* obj) {
    if (py_registry_count < MAX_PY_OBJECTS) {
        py_registry[py_registry_count++] = obj;
    } else {
        DTN_WARN("DTN ROUTING: Python registry is full");
    }
    return obj;
}

int py_cgr_clean_all(dtn_routing_result_t retcode) {
    if (PyErr_Occurred())
        PyErr_Print();
    for (int i = 0; i < py_registry_count; i++) {
        if (py_registry[i] != NULL) {
            Py_XDECREF(py_registry[i]);
            py_registry[i] = NULL;
        }
    }
    /* Do NOT call Py_Finalize() here — the interpreter is now owned by
     * Routing_Function and is shut down in dtn_routing_destroy(). */
    py_registry_count = 0;
    return retcode;
}

/* Fetch and track an attribute; handle NULL and Py_None.
 * Returns the PyObject* on success, NULL on failure. */
static PyObject* py_get_attr(PyObject* obj, const char* attr) {
    PyObject* val = track_obj(PyObject_GetAttrString(obj, attr));
    if (val == NULL) {
        DTN_ERROR("DTN Routing: object has no attribute '%s'", attr);
        return NULL;
    }
    if (val == Py_None) {
        DTN_DEBUG("DTN Routing: attribute '%s' is None", attr);
        return NULL;
    }
    return val;
}

/* Get an integer attribute from a Python object. */
static int py_get_long_attr(PyObject* obj, const char* attr, long* out) {
    PyObject* val = py_get_attr(obj, attr);
    if (val == NULL)
        return -1;
    if (!PyLong_Check(val)) {
        DTN_ERROR("DTN Routing: attribute '%s' is not an integer", attr);
        return -1;
    }
    *out = PyLong_AsLong(val);
    return 0;
}

/* Get a numeric attribute (int or float) as double. */
static int py_get_double_attr(PyObject* obj, const char* attr, double* out) {
    PyObject* val = py_get_attr(obj, attr);
    if (val == NULL)
        return -1;
    if (PyFloat_Check(val)) {
        *out = PyFloat_AsDouble(val);
    } else if (PyLong_Check(val)) {
        *out = (double)PyLong_AsLong(val);
    } else {
        DTN_ERROR("DTN Routing: attribute '%s' is not a number", attr);
        return -1;
    }
    return 0;
}

dtn_routing_result_t dtn_routing_is_node_id_dtn_node(int node_id, bool* is_dtn_node) {
    if (!is_dtn_node) {
        DTN_ERROR("Invalid arguments to is_node_id_dtn_node.");
        return DTN_ROUTING_ERR;
    }
    for (int i = 0; i < dtn_config.contact_plan.node_count; i++) {
        if (dtn_config.contact_plan.nodes[i].id == node_id) {
            *is_dtn_node = dtn_config.contact_plan.nodes[i].is_dtn_node;
            DTN_DEBUG("Node with id %d is %s a DTN node", node_id, *is_dtn_node ? "" : "not");
            return DTN_ROUTING_OK;
        }
    }
    DTN_WARN("Node with id %d is not defined", node_id);
    return DTN_ROUTING_ERR;
}

dtn_routing_result_t dtn_routing_is_next_hop_active(double current_time_in_ms, const DtnRoutingResult routing_result,
                                                    bool* is_next_hop_active) {
    if (current_time_in_ms < 0 || !is_next_hop_active) {
        DTN_ERROR("Invalid arguments to is_next_hop_active.");
        return DTN_ROUTING_ERR;
    }

    double current_time_in_sec = current_time_in_ms / 1000;
    *is_next_hop_active =
        (current_time_in_sec >= routing_result.min_delivery_time && current_time_in_sec <= routing_result.max_delivery_time);
    return DTN_ROUTING_OK;
}

// The last hex group after the final ':' encodes the node id.
// fd00:2:3::3  -> 3
// fd00:2:3::a  -> 10
// fd00:2:3::1f -> 31
long dtn_routing_ipv6_to_nodeid(const char* ip6) {
    if (!ip6 || ip6[0] == '\0') {
        DTN_WARN("ip6 is empty");
        return -1;
    }
    const char* last_colon = strrchr(ip6, ':');
    if (!last_colon || last_colon[1] == '\0') {
        DTN_WARN("ip6 has no node-id segment: %s", ip6);
        return -1;
    }
    char* end;
    long id = strtol(last_colon + 1, &end, 16);
    if (end == last_colon + 1) {
        DTN_WARN("ip6 node-id segment is not a valid hex number: %s", ip6);
        return -1;
    }
    return id;
}

dtn_routing_result_t dtn_routing_get_next_hop_node_id(double start_time_in_ms, double current_time_in_ms, struct ip6_hdr* ip6h,
                                                      DtnRoutingResult* result) {
    if (start_time_in_ms < 0 || current_time_in_ms < 0 || !ip6h || !result) {
        DTN_ERROR("Invalid arguments to get_next_dnt_hop.");
        return DTN_ROUTING_ERR;
    }

    double start_time_in_sec = start_time_in_ms / 1000;
    double current_time_in_sec = (current_time_in_ms / 1000) - start_time_in_sec;
    current_time_in_sec = fmod(current_time_in_sec, (double)dtn_config.contact_plan.max_time_in_sec);

    ip6_addr_t src, dest;
    char src_string[INET6_ADDRSTRLEN], dest_string[INET6_ADDRSTRLEN];
    ip6_addr_copy_from_packed(src, ip6h->src);
    ip6_addr_copy_from_packed(dest, ip6h->dest);
    ip6addr_ntoa_r(&src, src_string, sizeof(src_string));
    ip6addr_ntoa_r(&dest, dest_string, sizeof(dest_string));

    DTN_DEBUG("Packet|| src: %s - dest: %s || start_time_in_sec: %f - current_time_in_sec: %f", src_string, dest_string, start_time_in_sec,
              current_time_in_sec);

    u8_t version = IP6H_V(ip6h);
    u8_t traffic_class = IP6H_TC(ip6h);
    u8_t hoplim = IP6H_HOPLIM(ip6h);
    uint8_t dscp = (traffic_class >> 2) & 0x3F;

    u16_t payload_length = lwip_ntohs(ip6h->_plen);
    u16_t package_length = IP6_HLEN + payload_length;
    long package_length_in_bits = package_length * 8;

    DTN_DEBUG(
        "Packet|| src: %s - dest: %s || hop version: %u - traffic_class: %u - hoplim: %u - dscp: %u - "
        "payload_length: %u - package_length: %u - package_length in bits: %ld.",
        src_string, dest_string, version, traffic_class, hoplim, dscp, payload_length, package_length, package_length_in_bits);

    long deadline = hoplim * 1000;
    long curr_node_id = (long)dtn_config.id;
    long src_node_id = dtn_routing_ipv6_to_nodeid(src_string);
    long dest_node_id = dtn_routing_ipv6_to_nodeid(dest_string);

    DTN_DEBUG("Packet|| src: %s - dest: %s || src_node_id:%ld -> curr_node_id:%ld -> dest_node_id:%ld - deadline: %ld.", src_string,
              dest_string, src_node_id, curr_node_id, dest_node_id, deadline);

    return _dtn_routing_get_next_hop_node_id(current_time_in_sec, curr_node_id, src_node_id, dest_node_id, deadline, package_length_in_bits,
                                             dscp, result);
}

dtn_routing_result_t _dtn_routing_get_next_hop_node_id(double current_time_in_sec, long current_node_id, long src_node_id,
                                                       long dest_node_id, long deadline, long package_length_in_bits, long dscp,
                                                       DtnRoutingResult* result) {
    if (!g_routing || !g_routing->py_module) {
        DTN_ERROR("DTN Routing: Python not initialised — call dtn_routing_create first");
        return DTN_ROUTING_ERR;
    }

    /* --- Route cache: skip Python CGR if we already know the answer --- */
    const long length_bucket = LENGTH_BUCKET(package_length_in_bits);
    for (int i = 0; i < route_cache_count; i++) {
        RouteCacheEntry* cache_entry = &route_cache[i];
        if (cache_entry->dest_node_id == dest_node_id && cache_entry->length_bucket == length_bucket &&
            current_time_in_sec < cache_entry->result.max_delivery_time) {
            cache_entry->last_used = ++route_cache_tick;
            *result = cache_entry->result;
            return DTN_ROUTING_OK;
        }
    }

    PyObject* py_cgr_yen = (PyObject*)g_routing->py_cgr_yen;
    PyObject* py_fwd_candidate = (PyObject*)g_routing->py_fwd_candidate;
    PyObject* py_ipv6_packet = (PyObject*)g_routing->py_ipv6_packet;
    PyObject* contact_plan = (PyObject*)g_routing->py_contact_plan;

    PyObject* args_yen = track_obj(PyTuple_New(6));
    PyTuple_SetItem(args_yen, 0, PyFloat_FromDouble(current_time_in_sec));
    PyTuple_SetItem(args_yen, 1, PyLong_FromLong(current_node_id));
    PyTuple_SetItem(args_yen, 2, PyLong_FromLong(dest_node_id));
    PyTuple_SetItem(args_yen, 3, PyFloat_FromDouble(current_time_in_sec));
    Py_INCREF(contact_plan); /* tuple steals; struct keeps its ref */
    PyTuple_SetItem(args_yen, 4, contact_plan);
    PyTuple_SetItem(args_yen, 5, PyLong_FromLong(10));
    PyObject* routes = PyObject_CallObject(py_cgr_yen, args_yen); /* NOT tracked; args_fwd will own */
    if (routes == NULL || routes == Py_None) {
        DTN_ERROR("DTN Routing: cgr_yen returned NULL or None");
        Py_XDECREF(routes);
        return py_cgr_clean_all(DTN_ROUTING_ERR);
    }

    /* --- ipv6_packet: describe this packet to the CGR --- */
    PyObject* args_pkt = track_obj(PyTuple_New(6));
    PyTuple_SetItem(args_pkt, 0, PyFloat_FromDouble(current_time_in_sec));
    PyTuple_SetItem(args_pkt, 1, PyLong_FromLong(dest_node_id));
    PyTuple_SetItem(args_pkt, 2, PyLong_FromLong(package_length_in_bits));
    PyTuple_SetItem(args_pkt, 3, PyLong_FromLong(deadline));
    PyTuple_SetItem(args_pkt, 4, PyLong_FromLong(dscp));
    PyTuple_SetItem(args_pkt, 5, PyLong_FromLong(src_node_id));
    PyObject* ipv6pkt = PyObject_CallObject(py_ipv6_packet, args_pkt); /* NOT tracked; args_fwd will own */
    if (ipv6pkt == NULL || ipv6pkt == Py_None) {
        DTN_ERROR("DTN Routing: ipv6_packet constructor returned NULL or None");
        Py_XDECREF(ipv6pkt);
        Py_XDECREF(routes); /* args_fwd not yet created; free routes ourselves */
        return py_cgr_clean_all(DTN_ROUTING_ERR);
    }

    /* --- fwd_candidate: pick the best next hop ---
     * args_fwd takes ownership of routes, ipv6pkt and excluded_nodes via
     * PyTuple_SetItem (steals references).  Tracking args_fwd in the registry
     * is sufficient to free all three when py_cgr_clean_all runs. */
    PyObject* excluded_nodes = PyList_New(0); /* NOT tracked; args_fwd will own */
    PyObject* args_fwd = track_obj(PyTuple_New(6));
    PyTuple_SetItem(args_fwd, 0, PyFloat_FromDouble(current_time_in_sec));
    PyTuple_SetItem(args_fwd, 1, PyLong_FromLong(current_node_id));
    Py_INCREF(contact_plan); /* tuple steals; struct keeps its ref */
    PyTuple_SetItem(args_fwd, 2, contact_plan);
    PyTuple_SetItem(args_fwd, 3, ipv6pkt);        /* args_fwd takes ownership */
    PyTuple_SetItem(args_fwd, 4, routes);         /* args_fwd takes ownership */
    PyTuple_SetItem(args_fwd, 5, excluded_nodes); /* args_fwd takes ownership */
    PyObject* candidates = track_obj(PyObject_CallObject(py_fwd_candidate, args_fwd));

    if (candidates == NULL || !PyList_Check(candidates)) {
        DTN_ERROR("DTN Routing: fwd_candidate did not return a list");
        return py_cgr_clean_all(DTN_ROUTING_ERR);
    }

    if (PyList_Size(candidates) < 1) {
        DTN_DEBUG("DTN Routing: No candidate routes returned");
        return py_cgr_clean_all(DTN_ROUTING_NO_ROUTE);
    }

    PyObject* first = PyList_GetItem(candidates, 0);

    long next_hop_node_id;
    if (py_get_long_attr(first, "next_node", &next_hop_node_id) != 0)
        return py_cgr_clean_all(DTN_ROUTING_NO_ROUTE);
    if (py_get_double_attr(first, "from_time", &result->min_delivery_time) != 0)
        return py_cgr_clean_all(DTN_ROUTING_ERR);
    if (py_get_double_attr(first, "to_time", &result->max_delivery_time) != 0)
        return py_cgr_clean_all(DTN_ROUTING_ERR);
    if (py_get_double_attr(first, "best_delivery_time", &result->best_delivery_time) != 0)
        return py_cgr_clean_all(DTN_ROUTING_ERR);

    result->next_hop_node_id = (int)next_hop_node_id;
    DTN_DEBUG(
        "Packet|| src_id: %ld -> dest_id: %ld | length_in_bits: %ld | min_delivery_time: %.2f | max_delivery_time: %.2f | "
        "best_delivery_time: "
        "%.2f || SUCCESSFULLY found next_hop_node_id: %d",
        src_node_id, dest_node_id, package_length_in_bits, result->min_delivery_time, result->max_delivery_time, result->best_delivery_time,
        result->next_hop_node_id);

    int cache_idx = -1;
    for (int i = 0; i < route_cache_count; i++) {
        if (route_cache[i].dest_node_id == dest_node_id && route_cache[i].length_bucket == length_bucket) {
            cache_idx = i;
            break;
        }
    }
    if (cache_idx < 0 && route_cache_count < ROUTE_CACHE_SIZE) {
        cache_idx = route_cache_count++;
    }
    if (cache_idx < 0) {
        // Cache full and no matching entry — evict the least-recently-used slot
        // instead of giving up (which previously forced a CGR run every packet).
        cache_idx = 0;
        for (int i = 1; i < route_cache_count; i++) {
            if (route_cache[i].last_used < route_cache[cache_idx].last_used)
                cache_idx = i;
        }
    }
    if (cache_idx >= 0) {
        route_cache[cache_idx].dest_node_id = (int)dest_node_id;
        route_cache[cache_idx].length_bucket = length_bucket;
        route_cache[cache_idx].last_used = ++route_cache_tick;
        route_cache[cache_idx].result = *result;
        DTN_DEBUG(
            "Packet|| src_id: %ld -> dest_id: %ld | length_in_bits: %ld | min_delivery_time: %.2f | max_delivery_time: %.2f | "
            "best_delivery_time: "
            "%.2f || SUCCESSFULLY cached",
            src_node_id, dest_node_id, package_length_in_bits, result->min_delivery_time, result->max_delivery_time,
            result->best_delivery_time);
    }

    return py_cgr_clean_all(DTN_ROUTING_OK);
}