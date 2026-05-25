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

#define MAX_PY_OBJECTS 100
static PyObject* py_registry[MAX_PY_OBJECTS];
static int py_registry_count = 0;

Routing_Function* dtn_routing_create(DTN_Module* parent) {
    Routing_Function* routing = (Routing_Function*)malloc(sizeof(Routing_Function));
    if (routing) {
        routing->parent_module = parent;
        routing->routing_algorithm_name = "Contact Graph Routing";
        routing->base_time_in_ms = sys_now();
        DTN_INFO("DTN Routing Function created. Mode: %s", routing->routing_algorithm_name);

    } else {
        DTN_ERROR("Failed to allocate memory for Routing_Function");
    }
    return routing;
}

void dtn_routing_destroy(Routing_Function* routing) {
    if (!routing)
        return;

    DTN_INFO("Destroying DTN Routing Function...");
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
    Py_Finalize();
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

int dtn_routing_is_node_id_dtn_node(int node_id, bool* is_dtn_node) {
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

int dtn_routing_is_next_hop_active(double current_time_in_ms, int node_id,
                                   bool* is_next_hop_active) {
    if (current_time_in_ms < 0 || !is_next_hop_active) {
        DTN_ERROR("Invalid arguments to is_next_hop_active.");
        return DTN_ROUTING_ERR;
    }

    double current_time_in_sec = current_time_in_ms / 1000;

    for (int i = 0; i < dtn_config.interface_count; i++) {
        if (dtn_config.interfaces[i].remote_node_id == node_id) {
            *is_next_hop_active = (current_time_in_sec >= dtn_config.interfaces[i].start_in_sec &&
                                   current_time_in_sec <= dtn_config.interfaces[i].end_in_sec);
            DTN_DEBUG("Next hop to node %d is %sactive", node_id,
                      *is_next_hop_active ? "" : "not ");
            return DTN_ROUTING_OK;
        }
    }
    DTN_WARN("No interface configured for node id %d", node_id);
    return DTN_ROUTING_ERR;
}

// TODO:: Add to config
// The Last number in the address currently defines the node id
// fd00:2:3::3 -> node_id == 3
long _ipv6_to_nodeid(const char* ip6) {
    int length = strlen(ip6);
    if (length > 0) {
        char node_id = ip6[length - 1];
        return node_id - '0';
    } else {
        DTN_WARN("ip6 is empty");
    }
    return -1;
}

int dtn_routing_get_next_hop_node_id(double start_time_in_ms, double current_time_in_ms,
                                     struct ip6_hdr* ip6h, DtnRoutingResult* result) {
    if (start_time_in_ms < 0 || current_time_in_ms < 0 || !ip6h || !result) {
        DTN_ERROR("Invalid arguments to get_next_dnt_hop.");
        return DTN_ROUTING_ERR;
    }
    double start_time_in_sec = start_time_in_ms / 1000;
    double current_time_in_sec =
        fmod(current_time_in_ms / 1000.0, (double)dtn_config.contact_plan.max_time_in_sec);

    ip6_addr_t src, dest;
    char src_string[INET6_ADDRSTRLEN], dest_string[INET6_ADDRSTRLEN];
    ip6_addr_copy_from_packed(src, ip6h->src);
    ip6_addr_copy_from_packed(dest, ip6h->dest);
    ip6addr_ntoa_r(&src, src_string, sizeof(src_string));
    ip6addr_ntoa_r(&dest, dest_string, sizeof(dest_string));

    DTN_DEBUG(
        "DTN Routing: Get next hop start_time_in_sec: %f - current_time_in_sec: %f - src: %s - "
        "dest: %s",
        start_time_in_sec, current_time_in_sec, src_string, dest_string);

    u8_t version = IP6H_V(ip6h);
    u8_t traffic_class = IP6H_TC(ip6h);
    u8_t hoplim = IP6H_HOPLIM(ip6h);
    uint8_t dscp = (traffic_class >> 2) & 0x3F;

    u16_t payload_length = lwip_ntohs(ip6h->_plen);
    u16_t package_length = IP6_HLEN + payload_length;

    DTN_DEBUG(
        "Get next hop version: %u - traffic_class: %u - hoplim: %u - dscp: %u - "
        "payload_length: "
        "%u - package_length: %u",
        version, traffic_class, hoplim, dscp, payload_length, package_length);

    long deadline = hoplim * 1000;
    long curr_node_id = (long)dtn_config.id;
    long src_node_id = _ipv6_to_nodeid(src_string);
    long dest_node_id = _ipv6_to_nodeid(dest_string);

    DTN_DEBUG("src_node_id: %ld -> curr_node_id: %ld -> dest_node_id: %ld - deadline: %ld",
              src_node_id, curr_node_id, dest_node_id, deadline);

    return _dtn_routing_get_next_hop_node_id(dtn_config.contact_plan_path, start_time_in_sec,
                                             current_time_in_sec, curr_node_id, src_node_id,
                                             dest_node_id, deadline, package_length, dscp, result);
}

int _dtn_routing_get_next_hop_node_id(char* contact_plan_path, double start_time_in_sec,
                                      double current_time_in_sec, long current_node_id,
                                      long src_node_id, long dest_node_id, long deadline,
                                      long package_length, long dscp, DtnRoutingResult* result) {
    Py_Initialize();
    if (!Py_IsInitialized()) {
        DTN_ERROR("Python not initialized");
        return DTN_ROUTING_ERR;
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
        return DTN_ROUTING_ERR;
    }

    PyObject* py_cp_load = PyObject_GetAttrString(pModule, "cp_load");
    PyObject* py_cgr_yen = PyObject_GetAttrString(pModule, "cgr_yen");
    PyObject* py_fwd_candidate = PyObject_GetAttrString(pModule, "fwd_candidate");
    PyObject* py_ipv6_packet = PyObject_GetAttrString(pModule, "ipv6_packet");

    PyObject* args_load = track_obj(PyTuple_New(3));
    PyTuple_SetItem(args_load, 0, PyUnicode_FromString(contact_plan_path));
    PyTuple_SetItem(args_load, 1, PyFloat_FromDouble(start_time_in_sec));
    PyTuple_SetItem(args_load, 2, PyLong_FromLong(MAX_LENGTH));
    PyObject* contact_plan = track_obj(PyObject_CallObject(py_cp_load, args_load));
    if (contact_plan == NULL || contact_plan == Py_None) {
        DTN_ERROR("DTN Routing: cp_load returned NULL or None");
        return py_cgr_clean_all(DTN_ROUTING_ERR);
    }

    PyObject* args_yen = track_obj(PyTuple_New(6));
    PyTuple_SetItem(args_yen, 0, PyFloat_FromDouble(current_time_in_sec));
    PyTuple_SetItem(args_yen, 1, PyLong_FromLong(current_node_id));
    PyTuple_SetItem(args_yen, 2, PyLong_FromLong(dest_node_id));
    PyTuple_SetItem(args_yen, 3, PyFloat_FromDouble(current_time_in_sec));
    PyTuple_SetItem(args_yen, 4, contact_plan);
    PyTuple_SetItem(args_yen, 5, PyLong_FromLong(10));
    PyObject* routes = track_obj(PyObject_CallObject(py_cgr_yen, args_yen));
    if (routes == NULL || routes == Py_None) {
        DTN_ERROR("DTN Routing: cgr_yen returned NULL or None");
        return py_cgr_clean_all(DTN_ROUTING_ERR);
    }

    // DTN_DEBUG("DNT Routing: py_ipv6_packet");
    PyObject* args_pkt = track_obj(PyTuple_New(6));
    PyTuple_SetItem(args_pkt, 0, PyFloat_FromDouble(current_time_in_sec));
    PyTuple_SetItem(args_pkt, 1, PyLong_FromLong(dest_node_id));
    PyTuple_SetItem(args_pkt, 2, PyLong_FromLong(package_length));
    PyTuple_SetItem(args_pkt, 3, PyLong_FromLong(deadline));
    PyTuple_SetItem(args_pkt, 4, PyLong_FromLong(dscp));
    PyTuple_SetItem(args_pkt, 5, PyLong_FromLong(src_node_id));
    PyObject* ipv6pkt = track_obj(PyObject_CallObject(py_ipv6_packet, args_pkt));
    if (ipv6pkt == NULL || ipv6pkt == Py_None) {
        DTN_ERROR("DTN Routing: ipv6_packet constructor returned NULL or None");
        return py_cgr_clean_all(DTN_ROUTING_ERR);
    }

    // DTN_DEBUG("DNT Routing: py_fwd_candidate");
    PyObject* excluded_nodes = track_obj(PyList_New(0));
    PyObject* args_fwd = track_obj(PyTuple_New(6));
    PyTuple_SetItem(args_fwd, 0, PyFloat_FromDouble(current_time_in_sec));
    PyTuple_SetItem(args_fwd, 1, PyLong_FromLong(current_node_id));
    PyTuple_SetItem(args_fwd, 2, contact_plan);
    PyTuple_SetItem(args_fwd, 3, ipv6pkt);
    PyTuple_SetItem(args_fwd, 4, routes);
    PyTuple_SetItem(args_fwd, 5, excluded_nodes);
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
    if (py_get_double_attr(first, "to_time", &result->to_time) != 0)
        return py_cgr_clean_all(DTN_ROUTING_ERR);
    if (py_get_double_attr(first, "best_delivery_time", &result->best_delivery_time) != 0)
        return py_cgr_clean_all(DTN_ROUTING_ERR);

    result->next_hop_node_id = (int)next_hop_node_id;
    DTN_DEBUG("DTN Routing: next hop node id %d | best_delivery_time %f | max_delivery_time %f",
              result->next_hop_node_id, result->best_delivery_time, result->to_time);

    return py_cgr_clean_all(DTN_ROUTING_OK);
}