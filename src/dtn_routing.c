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
        routing->contact_list_head = NULL;
        routing->base_time = sys_now();

        DTN_INFO("DTN Routing Function created. Mode: %s", routing->routing_algorithm_name);

        // We save the contacts from the contact plan in the contact_list_head
        // const char* contacts_file = "py_cgr/contact_plans/cgr_tutorial_1.txt";
        // const char* contacts_file =  "/root/py_cgr/contact_plans/graph_01.txt";

        // int nloaded = dtn_routing_load_contacts(routing, dtn_config.CONTACT_PLAN_PATH);
        // if (nloaded < 0) {
        //     DTN_ERROR("DTN Routing: error loading contact plan %s",
        //     dtn_config.CONTACT_PLAN_PATH);
        // }

    } else {
        perror("Failed to allocate memory for Routing_Function");
    }
    return routing;
}

void dtn_routing_destroy(Routing_Function* routing) {
    if (!routing)
        return;

    printf("Destroying DTN Routing Function...\n");

    Contact_Info* current = routing->contact_list_head;
    Contact_Info* next;
    while (current != NULL) {
        next = current->next;
        free(current);
        current = next;
    }

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

int py_cgr_clean_all() {
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
    return 0;
}

int dtn_routing_is_node_id_dtn_node(int node_id, bool* is_dtn_node) {
    for (int i = 0; i < dtn_config.node_count; i++) {
        if (dtn_config.nodes[i].id == node_id) {
            *is_dtn_node = dtn_config.nodes[i].is_dtn_node;
            DTN_DEBUG("DTN Routing: node with id %d is %s a DTN node", node_id,
                      *is_dtn_node ? "" : "not");
            return 1;
        }
    }
    DTN_WARN("DTN Routing: node with id %d is not defined", node_id);
    return 0;
}

int dtn_routing_is_next_hop_active(double current_time_in_ms, int node_id,
                                   bool* is_next_hop_active) {
    if (current_time_in_ms < 0 || !is_next_hop_active) {
        DTN_ERROR("DTN Routing: Invalid arguments to get is_next_hop_active.");
        return 0;
    }

    double current_time_in_sec = current_time_in_ms / 1000;

    for (int i = 0; i < dtn_config.interface_count; i++) {
        if (dtn_config.interfaces[i].remote_node_id == node_id) {
            *is_next_hop_active = (current_time_in_sec >= dtn_config.interfaces[i].start_in_sec &&
                                   current_time_in_sec <= dtn_config.interfaces[i].end_in_sec);

            DTN_DEBUG("DTN Routing: Next hop to node %s is %s active", node_id,
                      *is_next_hop_active ? "" : "not");
            return 1;
        }
    }
    DTN_WARN("DTN Routing: node with id %d is not defined", node_id);
    return 0;
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
                                     struct ip6_hdr* ip6h, int* next_hop_node_id) {
    if (start_time_in_ms < 0 || current_time_in_ms < 0 || !ip6h || !next_hop_node_id) {
        DTN_ERROR("DTN Routing: Invalid arguments to get_next_dnt_hop.");
        return 0;
    }
    double start_time_in_sec = start_time_in_ms / 1000;
    double current_time_in_sec = current_time_in_ms / 1000;

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
        "DTN_ROUTING: Get next hop version: %u - traffic_class: %u - hoplim: %u - dscp: %u - "
        "payload_length: "
        "%u - package_length: %u",
        version, traffic_class, hoplim, dscp, payload_length, package_length);

    long deadline = hoplim * 1000;
    long curr_node_id = (long)dtn_config.id;
    long src_node_id = _ipv6_to_nodeid(src_string);
    long dest_node_id = _ipv6_to_nodeid(dest_string);

    DTN_DEBUG(
        "DTN Routing: src_node_id: %ld -> curr_node_id: %ld -> dest_node_id: %ld - deadline: %ld",
        src_node_id, curr_node_id, dest_node_id, deadline);

    return _dtn_routing_get_next_hop_node_id(
        dtn_config.contact_plan_path, start_time_in_sec, current_time_in_sec, curr_node_id,
        src_node_id, dest_node_id, deadline, package_length, dscp, next_hop_node_id);
}

int _dtn_routing_get_next_hop_node_id(char* contact_plan_path, double start_time_in_sec,
                                      double current_time_in_sec, long current_node_id,
                                      long src_node_id, long dest_node_id, long deadline,
                                      long package_length, long dscp, int* next_hop_node_id) {
    // DTN_DEBUG("DNT Routing: Init python");
    Py_Initialize();
    if (!Py_IsInitialized()) {
        fprintf(stderr, "Python not initialized\n");
        return 0;
    }

    PyObject* sys_path = PySys_GetObject("path");
    PyObject* py_pth = PyUnicode_FromString("py_cgr");
    PyList_Append(sys_path, py_pth);
    Py_DECREF(py_pth);

    // DTN_DEBUG("DNT Routing: Init path");
    PyObject* pModule = PyImport_ImportModule("py_cgr_lib.py_cgr_lib");
    if (!pModule) {
        DTN_ERROR("Cannot import py_cgr_lib.py_cgr_lib");
        PyErr_Print();
        Py_Finalize();
        return 0;
    }

    // DTN_DEBUG("DNT Routing: Init modules");
    PyObject* py_cp_load = PyObject_GetAttrString(pModule, "cp_load");
    PyObject* py_cgr_yen = PyObject_GetAttrString(pModule, "cgr_yen");
    PyObject* py_fwd_candidate = PyObject_GetAttrString(pModule, "fwd_candidate");
    PyObject* py_ipv6_packet = PyObject_GetAttrString(pModule, "ipv6_packet");

    // DTN_DEBUG("DNT Routing: py_cp_load");
    PyObject* args_load = track_obj(PyTuple_New(3));
    PyTuple_SetItem(args_load, 0, PyUnicode_FromString(contact_plan_path));
    PyTuple_SetItem(args_load, 1, PyFloat_FromDouble(start_time_in_sec));
    PyTuple_SetItem(args_load, 2, PyLong_FromLong(MAX_LENGTH));
    PyObject* contact_plan = track_obj(PyObject_CallObject(py_cp_load, args_load));
    // Py_DECREF(args_load);
    if (contact_plan == NULL || contact_plan == Py_None) {
        DTN_ERROR("DTN Routing: ipv6_packet constructor returned NULL || None");
        return py_cgr_clean_all();
    }

    // DTN_DEBUG("DNT Routing: py_cgr_yen");
    PyObject* args_yen = track_obj(PyTuple_New(6));
    PyTuple_SetItem(args_yen, 0, PyFloat_FromDouble(current_time_in_sec));
    PyTuple_SetItem(args_yen, 1, PyLong_FromLong(current_node_id));
    PyTuple_SetItem(args_yen, 2, PyLong_FromLong(dest_node_id));
    PyTuple_SetItem(args_yen, 3, PyFloat_FromDouble(current_time_in_sec));
    PyTuple_SetItem(args_yen, 4, contact_plan);
    PyTuple_SetItem(args_yen, 5, PyLong_FromLong(10));
    PyObject* routes = track_obj(PyObject_CallObject(py_cgr_yen, args_yen));
    // Py_DECREF(args_yen);
    if (routes == NULL || routes == Py_None) {
        DTN_ERROR("DTN Routing: ipv6_packet constructor returned NULL || None");
        return py_cgr_clean_all();
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
    // Py_DECREF(args_pkt);
    if (ipv6pkt == NULL || ipv6pkt == Py_None) {
        DTN_ERROR("DTN Routing: ipv6_packet constructor returned NULL || None");
        return py_cgr_clean_all();
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
    // Py_XDECREF(excluded_nodes);
    // Py_XDECREF(args_fwd);

    if (candidates == NULL || !PyList_Check(candidates)) {
        DTN_ERROR("DTN Routing: candidates is not a list");
        return py_cgr_clean_all();
    }

    if (PyList_Size(candidates) < 1) {
        DTN_ERROR("DTN Routing: No candidate routes returned");
        return py_cgr_clean_all();
    }

    PyObject* first = PyList_GetItem(candidates, 0);
    PyObject* pNextNode = PyObject_GetAttrString(first, "next_node");
    if (pNextNode == NULL) {
        DTN_ERROR("DTN Routing: candidate object has no attribute next_node");
        return py_cgr_clean_all();
    }

    if (pNextNode == Py_None) {
        DTN_ERROR("DTN Routing: next node is None");
        return py_cgr_clean_all();
    }

    if (!PyLong_Check(pNextNode)) {
        DTN_ERROR("DTN Routing: next node is not an integer");
        return py_cgr_clean_all();
    }

    long _next_hop_node_id = PyLong_AsLong(pNextNode);
    *next_hop_node_id = (int)_next_hop_node_id;
    DTN_DEBUG("DTN Routing: next hope has node id %d", *next_hop_node_id);

    py_cgr_clean_all();
    return 1;
}

// int dtn_routing_get_dtn_next_hop(Routing_Function* routing, u32_t* v_tc_fl, u16_t* plen,
//                                  u8_t* hoplim, ip6_addr_t* dest_ip, ip6_addr_t* sender_ip,
//                                  ip6_addr_t* next_hop_ip) {
//     if (!routing || !v_tc_fl || !plen || !hoplim || !dest_ip || !next_hop_ip) {
//         fprintf(stderr, "DTN Routing: Invalid arguments to get_dtn_next_hop.\n");
//         return 0;
//     }

//     if (!dtn_routing_is_dtn_destination(routing, dest_ip)) {
//         char dest_addr_str_err[IP6ADDR_STRLEN_MAX];
//         ip6addr_ntoa_r(dest_ip, dest_addr_str_err, sizeof(dest_addr_str_err));
//         fprintf(stderr, "DTN Routing ERROR: get_dtn_next_hop called for non-DTN dest %s\n",
//                 dest_addr_str_err);
//         ip6_addr_set_any(next_hop_ip);
//         return 0;
//     }

//     ip6_addr_t local;
//     unsigned char tmpbuf[16];
//     // if (inet_pton(AF_INET6, dtn_config.interfaces[0]->addr, tmpbuf) != 1) {
//     //     fprintf(stderr, "inet_pton local address failed\n");
//     //     return 0;
//     // }
//     for (int i = 0; i < 4; i++) {
//         uint32_t w = (tmpbuf[i * 4 + 0] << 24) | (tmpbuf[i * 4 + 1] << 16) |
//                      (tmpbuf[i * 4 + 2] << 8) | (tmpbuf[i * 4 + 3]);
//         local.addr[i] = ntohl(w);
//     }

//     char dst_s[INET6_ADDRSTRLEN];
//     char sender_s[INET6_ADDRSTRLEN];

//     if (ip6_addr_to_str(dest_ip, dst_s, sizeof(dst_s)) != 0) {
//         fprintf(stderr, "ip6_addr_to_str dest failed\n");
//         return 1;
//     }
//     if (ip6_addr_to_str(sender_ip, sender_s, sizeof(sender_s)) != 0) {
//         fprintf(stderr, "ip6_addr_to_str sender failed\n");
//         return 1;
//     }

//     Py_Initialize();
//     if (!Py_IsInitialized()) {
//         fprintf(stderr, "Python not initialized\n");
//         return 0;
//     }

//     // PyRun_SimpleString(
//     // "import sys; sys.stdout = open('/captures/cgr_debug.log', 'w'); sys.stderr = sys.stdout");

//     PyObject* sys_path = PySys_GetObject("path");
//     PyObject* py_pth = PyUnicode_FromString("py_cgr");
//     PyList_Append(sys_path, py_pth);
//     Py_DECREF(py_pth);

//     PyObject* pModule = PyImport_ImportModule("py_cgr_lib.py_cgr_lib");
//     if (!pModule) {
//         DTN_ERROR("Cannot import py_cgr_lib.py_cgr_lib");
//         PyErr_Print();
//         Py_Finalize();
//         return 0;
//     }

//     double curr_time_load = ((double)routing->base_time) / 1000;

//     PyObject* py_cp_load = PyObject_GetAttrString(pModule, "cp_load");
//     PyObject* py_cgr_yen = PyObject_GetAttrString(pModule, "cgr_yen");
//     PyObject* py_fwd_candidate = PyObject_GetAttrString(pModule, "fwd_candidate");
//     PyObject* py_ipv6_packet = PyObject_GetAttrString(pModule, "ipv6_packet");

//     // cp_load
//     PyObject* args_load = PyTuple_New(3);
//     // PyTuple_SetItem(args_load, 0, PyUnicode_FromString(dtn_config.CONTACT_PLAN_PATH));
//     PyTuple_SetItem(args_load, 1, PyFloat_FromDouble(curr_time_load));
//     PyTuple_SetItem(args_load, 2, PyLong_FromLong(MAX_LENGTH));
//     PyObject* contact_plan = PyObject_CallObject(py_cp_load, args_load);
//     if (!contact_plan) {
//         fprintf(stderr, "[ERR] cp_load returned NULL\n");
//         PyErr_Print();
//         Py_DECREF(pModule);
//         Py_Finalize();
//         return 0;
//     }
//     Py_DECREF(args_load);

//     // PyObject_Print(contact_plan, stdout, 0);
//     // printf("\n");

//     // cgr_yen
//     long curr_node_id = (long)1;
//     long dest_node_id = ipv6_to_nodeid(dst_s);
//     double curr_time = ((double)sys_now()) / 1000;

//     // curr_time = curr_time_load;

//     PyObject* args_yen = PyTuple_New(6);
//     PyTuple_SetItem(args_yen, 0, PyFloat_FromDouble(curr_time));
//     PyTuple_SetItem(args_yen, 1, PyLong_FromLong(curr_node_id));
//     PyTuple_SetItem(args_yen, 2, PyLong_FromLong(dest_node_id));
//     PyTuple_SetItem(args_yen, 3, PyFloat_FromDouble(curr_time));
//     PyTuple_SetItem(args_yen, 4, contact_plan);
//     PyTuple_SetItem(args_yen, 5, PyLong_FromLong(10));
//     PyObject* routes = PyObject_CallObject(py_cgr_yen, args_yen);
//     if (!routes) {
//         fprintf(stderr, "[ERR] cgr_yen returned NULL\n");
//         PyErr_Print();
//         Py_DECREF(contact_plan);
//         Py_DECREF(pModule);
//         Py_Finalize();
//         return 0;
//     }
//     Py_DECREF(args_yen);

//     // PyObject_Print(routes, stdout, 0);
//     // printf("\n");

//     // ipv6_packet
//     uint8_t hoplim_val = 0;
//     uint32_t v_tc_fl_val = 0;
//     uint16_t plen_val = 0;

//     if (hoplim != NULL)
//         hoplim_val = *hoplim;
//     if (v_tc_fl != NULL)
//         v_tc_fl_val = *v_tc_fl;
//     if (plen != NULL)
//         plen_val = *plen;

//     long deadline = hoplim_val * 10000;                  // multiplying factor
//     uint8_t tc = (uint8_t)((v_tc_fl_val >> 20) & 0xFF);  // traffic class (8 bits)
//     uint8_t dscp = (uint8_t)(tc >> 2);                   // DSCP = TC[7:2] (6 bits)

//     long sender_node_id = ipv6_to_nodeid(sender_s);

//     DTN_INFO("hoplim: %d - v_tc_fl: %d - plen: %d", hoplim_val, v_tc_fl_val, plen_val);
//     DTN_INFO("deadline: %ld - tc: %d - dscp: %d", deadline, tc, dscp);
//     DTN_INFO("base_time: %f - current_time: %f", curr_time_load, curr_time);
//     // DTN_INFO("CGR [Sender: %ld - %s] [Current Node: %ld - %s] [Target: %ld - %s]",
//     // sender_node_id,
//     //  sender_s, curr_node_id, dtn_config.interfaces[0]->addr, dest_node_id, dst_s);

//     PyObject* args_pkt = PyTuple_New(6);
//     PyTuple_SetItem(args_pkt, 0, PyFloat_FromDouble(curr_time));
//     PyTuple_SetItem(args_pkt, 1, PyLong_FromLong(dest_node_id));
//     PyTuple_SetItem(args_pkt, 2, PyLong_FromLong(plen_val));
//     PyTuple_SetItem(args_pkt, 3, PyLong_FromLong(deadline));
//     PyTuple_SetItem(args_pkt, 4, PyLong_FromLong(dscp));
//     PyTuple_SetItem(args_pkt, 5, PyLong_FromLong(sender_node_id));
//     PyObject* ipv6pkt = PyObject_CallObject(py_ipv6_packet, args_pkt);
//     if (!ipv6pkt) {
//         DTN_ERROR("[ERR] ipv6_packet constructor returned NULL");
//         PyErr_Print();
//         Py_DECREF(routes);
//         Py_DECREF(contact_plan);
//         Py_DECREF(pModule);
//         Py_Finalize();
//         return 0;
//     }
//     Py_DECREF(args_pkt);

//     // PyObject* repr_pkt = PyObject_Repr(ipv6pkt);
//     // if (repr_pkt) {
//     //     const char* sp = PyUnicode_AsUTF8(repr_pkt);
//     //     DTN_INFO("ipv6pkt repr: %s\n", sp ? sp : "<NULL>");
//     //     Py_DECREF(repr_pkt);
//     // } else {
//     //     DTN_INFO("ipv6pkt repr failed");
//     // }

//     // fwd_candidate
//     PyObject* excluded_nodes = PyList_New(0);
//     PyObject* args_fwd = PyTuple_New(6);
//     PyTuple_SetItem(args_fwd, 0, PyFloat_FromDouble(curr_time));
//     PyTuple_SetItem(args_fwd, 1, PyLong_FromLong(curr_node_id));
//     PyTuple_SetItem(args_fwd, 2, contact_plan);
//     PyTuple_SetItem(args_fwd, 3, ipv6pkt);
//     PyTuple_SetItem(args_fwd, 4, routes);
//     PyTuple_SetItem(args_fwd, 5, excluded_nodes);
//     PyObject* candidates = PyObject_CallObject(py_fwd_candidate, args_fwd);
//     Py_DECREF(args_fwd);

//     // PyObject_Print(candidates, stdout, 0);
//     // printf("\n");

//     // we check the next hop for the best route
//     if (PyList_Check(candidates) && PyList_Size(candidates) > 0) {
//         PyObject* first = PyList_GetItem(candidates, 0);
//         PyObject* pNextNode = PyObject_GetAttrString(first, "next_node");
//         if (pNextNode) {
//             if (pNextNode == Py_None) {
//                 printf("Next hop: None\n");
//             } else if (PyLong_Check(pNextNode)) {
//                 long next_node = PyLong_AsLong(pNextNode);
//                 ip6_addr_t next_ip;
//                 if (edge_to_ipv6(curr_node_id, next_node, &next_ip) == 0) {
//                     // if (nodeid_to_ipv6(next_node, &next_ip) == 0) {

//                     // ip6_addr_copy(next_hop_ip, &next_ip);

//                     memcpy(next_hop_ip, &next_ip, sizeof(ip6_addr_t));
//                     char next_ip_s[INET6_ADDRSTRLEN];
//                     if (ip6_addr_to_str(&next_ip, next_ip_s, sizeof(next_ip_s)) == 0) {
//                         DTN_INFO("Next hop ipv6: %s", next_ip_s);
//                     } else {
//                         DTN_ERROR("Failed to stringify next_ip for node %ld", next_node);
//                         Py_DECREF(candidates);
//                         Py_DECREF(pModule);
//                         Py_Finalize();
//                         return 0;
//                     }
//                 } else {
//                     DTN_ERROR("No mapping nodeid->ipv6 for node %ld", next_node);
//                     Py_DECREF(candidates);
//                     Py_DECREF(pModule);
//                     Py_Finalize();
//                     return 0;
//                 }

//             } else {
//                 DTN_INFO("Next hop: (non-int)");
//             }
//             Py_DECREF(pNextNode);
//         } else {
//             PyErr_Clear();
//             DTN_INFO("Candidate object has no attribute next_node");
//             Py_DECREF(candidates);
//             Py_DECREF(pModule);
//             Py_Finalize();
//             return 0;
//         }
//     } else {
//         DTN_INFO("No candidate routes returned (list empty or not a list)");
//         Py_DECREF(candidates);
//         Py_DECREF(pModule);
//         Py_Finalize();
//         return 0;
//     }

//     Py_DECREF(candidates);
//     Py_DECREF(pModule);
//     Py_Finalize();
//     return 1;
// }

// int ip6_addr_to_str(const ip6_addr_t* a, char* buf, size_t buflen) {
//     if (!a || !buf)
//         return -1;
//     unsigned char tmp[16];
//     for (int i = 0; i < 4; ++i) {
//         uint32_t w = ntohl(a->addr[i]);
//         tmp[i * 4 + 0] = (w >> 24) & 0xFF;
//         tmp[i * 4 + 1] = (w >> 16) & 0xFF;
//         tmp[i * 4 + 2] = (w >> 8) & 0xFF;
//         tmp[i * 4 + 3] = (w >> 0) & 0xFF;
//     }
//     if (!inet_ntop(AF_INET6, tmp, buf, (socklen_t)buflen))
//         return -1;
//     return 0;
// }

// long ipv6_to_nodeid(const char* ip6) {
//     // fd00:2:3::3
//     // 02 03

//     int length = strlen(ip6);
//     if (length > 0) {
//         char node_id = ip6[length - 1];
//         return node_id - '0';
//     } else {
//         DTN_WARN("ip6 is empty");
//     }
//     return -1;
// }

// int edge_to_ipv6(long from_node_id, long to_node_id, ip6_addr_t* ip6_addr_out) {
//     long smaller_node_id = (from_node_id < to_node_id) ? from_node_id : to_node_id;
//     long bigger_node_id = (from_node_id > to_node_id) ? from_node_id : to_node_id;

//     uint32_t block0 = 0xfd000000 | (smaller_node_id & 0xFFFF);
//     uint32_t block1 = ((uint32_t)bigger_node_id & 0xFFFF) << 16;
//     uint32_t block2 = 0;
//     uint32_t block3 = ((uint32_t)to_node_id & 0xFFFF);
//     IP6_ADDR(ip6_addr_out, lwip_htonl(block0), lwip_htonl(block1), lwip_htonl(block2),
//              lwip_htonl(block3));

//     char ip6_addr_str[IP6ADDR_STRLEN_MAX];
//     ip6addr_ntoa_r(ip6_addr_out, ip6_addr_str, sizeof(ip6_addr_str));
//     DTN_DEBUG("DTN Routing: Created addresses from %ld to %ld: %s", from_node_id, to_node_id,
//               ip6_addr_str);

//     return 0;
// }

// int dtn_routing_load_contacts(Routing_Function* routing, const char* filename) {
//     if (!routing || !filename)
//         return -1;

//     FILE* f = fopen(filename, "r");
//     if (!f) {
//         DTN_INFO("DTN Routing: failed to open contact file '%s': %s", filename, strerror(errno));
//         return -1;
//     }

//     char line[512];
//     int loaded = 0;

//     while (fgets(line, sizeof(line), f)) {
//         // trim leading spaces
//         char* p = line;
//         while (*p && isspace((unsigned char)*p)) p++;

//         if (*p == '\0' || *p == '#')
//             continue;

//         char tok[8][64];
//         int ntok = 0;
//         char* s = p;
//         while (ntok < 8) {
//             // skip spaces
//             while (*s && isspace((unsigned char)*s)) s++;
//             if (!*s || *s == '\n' || *s == '\r')
//                 break;
//             // read token
//             int i = 0;
//             while (*s && !isspace((unsigned char)*s) && i < 63) {
//                 tok[ntok][i++] = *s++;
//             }
//             tok[ntok][i] = '\0';
//             ntok++;
//         }

//         if (ntok < 5)
//             continue;

//         char *start_tok = NULL, *end_tok = NULL;
//         for (int i = 0; i < ntok; ++i) {
//             if (tok[i][0] == '+') {
//                 if (!start_tok)
//                     start_tok = tok[i];
//                 else if (!end_tok)
//                     end_tok = tok[i];
//             }
//         }

//         char *from_tok = NULL, *to_tok = NULL;
//         for (int i = 0; i < ntok; ++i) {
//             bool all_digits = true;
//             size_t L = strlen(tok[i]);
//             if (L == 0 || L > 3)
//                 continue;
//             for (size_t j = 0; j < L; ++j)
//                 if (!isdigit((unsigned char)tok[i][j])) {
//                     all_digits = false;
//                     break;
//                 }
//             if (all_digits) {
//                 if (!from_tok)
//                     from_tok = tok[i];
//                 else if (!to_tok)
//                     to_tok = tok[i];
//             }
//         }

//         if (!start_tok || !end_tok || !from_tok || !to_tok) {
//             continue;
//         }

//         int start_sec = 0, end_sec = 0;
//         if (sscanf(start_tok, "+%d", &start_sec) != 1)
//             continue;
//         if (sscanf(end_tok, "+%d", &end_sec) != 1)
//             continue;

//         u32_t start_ms = (u32_t)start_sec * 1000;
//         u32_t end_ms = (u32_t)end_sec * 1000;

//         long from_node = (long)atoi(from_tok);
//         long to_node = (long)atoi(to_tok);

//         if (from_node < 0 || to_node < 0) {
//             DTN_WARN("DTN Routing: bad node token from='%s' to='%s' (skipping)", from_tok,
//             to_tok); continue;
//         }

//         ip6_addr_t from_ip6, to_ip6;
//         if (edge_to_ipv6(from_node, to_node, &from_ip6) != 0) {
//             DTN_WARN("DTN Routing: edge_to_ipv6 failed for edge %ld -> %ld", from_node, to_node);
//             continue;
//         }

//         if (edge_to_ipv6(to_node, from_node, &to_ip6) != 0) {
//             DTN_WARN("DTN Routing: edge_to_ipv6 failed for edge %ld -> %ld", to_node, from_node);
//             continue;
//         }

//         // if (nodeid_to_ipv6(from_node, &from_ip6) != 0) {
//         //     DTN_WARN("DTN Routing: nodeid_to_ipv6 failed for node %ld (from token '%s'),
//         //     skipping",
//         //              from_node, from_tok);
//         //     continue;
//         // }
//         // if (nodeid_to_ipv6(to_node, &to_ip6) != 0) {
//         //     DTN_WARN("DTN Routing: nodeid_to_ipv6 failed for node %ld (to token '%s'),
//         skipping",
//         //              to_node, to_tok);
//         //     continue;
//         // }

// #if LWIP_IPV6_SCOPES
//         ip6_addr_set_zone(&from_ip6, IP6_NO_ZONE);
//         ip6_addr_set_zone(&to_ip6, IP6_NO_ZONE);
// #endif

//         int added =
//             dtn_routing_add_contact(routing, &to_ip6, &from_ip6, start_ms + routing->base_time,
//                                     end_ms + routing->base_time, true);
//         if (added)
//             loaded++;
//     }

//     fclose(f);
//     DTN_INFO("DTN Routing: Loaded %d contacts from %s", loaded, filename);
//     return loaded;
// }

// int dtn_routing_add_contact(Routing_Function* routing, const ip6_addr_t* node_addr,
//                             const ip6_addr_t* next_hop, u32_t start_time_ms, u32_t end_time_ms,
//                             bool is_dtn_node) {
//     if (!routing || !node_addr || !next_hop)
//         return 0;

//     Contact_Info* new_contact = (Contact_Info*)malloc(sizeof(Contact_Info));
//     if (!new_contact) {
//         perror("Failed to allocate memory for Contact_Info");
//         return 0;
//     }

//     ip6_addr_copy(new_contact->node_addr, *node_addr);
//     ip6_addr_copy(new_contact->next_hop, *next_hop);
//     new_contact->start_time_ms = start_time_ms;
//     new_contact->end_time_ms = end_time_ms;
//     new_contact->is_dtn_node = is_dtn_node;
//     new_contact->next = NULL;

//     if (routing->contact_list_head == NULL) {
//         routing->contact_list_head = new_contact;
//     } else {
//         Contact_Info* current = routing->contact_list_head;
//         while (current->next != NULL) {
//             current = current->next;
//         }
//         current->next = new_contact;
//     }

//     char node_addr_str[IP6ADDR_STRLEN_MAX];
//     char next_hop_str[IP6ADDR_STRLEN_MAX];
//     ip6addr_ntoa_r(node_addr, node_addr_str, sizeof(node_addr_str));
//     ip6addr_ntoa_r(next_hop, next_hop_str, sizeof(next_hop_str));

//     printf("DTN Routing: Added contact for %s via %s (%s), start: %u ms, end: %u ms\n",
//            node_addr_str, next_hop_str, is_dtn_node ? "DTN" : "non-DTN", start_time_ms,
//            end_time_ms);

//     return 1;
// }

// // not used
// int dtn_routing_remove_contact(Routing_Function* routing, const ip6_addr_t* node_addr) {
//     if (!routing || !node_addr || !routing->contact_list_head)
//         return 0;

//     Contact_Info* current = routing->contact_list_head;
//     Contact_Info* prev = NULL;

//     while (current != NULL) {
//         if (ip6_addr_cmp(&current->node_addr, node_addr)) {
//             // Found the contact to remove
//             if (prev == NULL) {
//                 // First item
//                 routing->contact_list_head = current->next;
//             } else {
//                 prev->next = current->next;
//             }

//             char node_addr_str[IP6ADDR_STRLEN_MAX];
//             ip6addr_ntoa_r(node_addr, node_addr_str, sizeof(node_addr_str));
//             printf("DTN Routing: Removed contact for %s\n", node_addr_str);

//             free(current);
//             return 1;
//         }

//         prev = current;
//         current = current->next;
//     }

//     return 0;  // Contact not found
// }

// // no chanches needed, funciton used only to print any changes in the contacts' state
// bool dtn_routing_update_contacts(Routing_Function* routing) {
//     if (!routing)
//         return false;

//     bool ret = false;
//     static u32_t last_check_time = 0;
//     static bool last_active_states[100] = {false};
//     static int contact_index = 0;

//     u32_t current_time = sys_now();  // time when the computer has started

//     if (last_check_time == 0) {
//         last_check_time = current_time;
//     }

//     // Iterate through all contacts
//     Contact_Info* contact = routing->contact_list_head;
//     contact_index = 0;

//     while (contact != NULL && contact_index < 100) {
//         bool is_active =
//             (current_time >= contact->start_time_ms && current_time <= contact->end_time_ms);

//         if (is_active != last_active_states[contact_index]) {
//             char node_addr_str[IP6ADDR_STRLEN_MAX], next_hop_str[IP6ADDR_STRLEN_MAX];
//             ip6addr_ntoa_r(&contact->node_addr, node_addr_str, sizeof(node_addr_str));
//             ip6addr_ntoa_r(&contact->next_hop, next_hop_str, sizeof(next_hop_str));
//             u32_t secs = current_time / 1000;
//             u32_t msecs = current_time % 1000;

//             if (is_active) {
//                 DTN_INFO("DTN Routing: Contact from %s to %s became ACTIVE at time %5u.%03u ms",
//                          next_hop_str, node_addr_str, secs, msecs);
//                 ret = true;

//             } else {
//                 DTN_INFO("DTN Routing: Contact for %s became IN-ACTIVE at time %5u.%03u ",
//                          node_addr_str, secs, msecs);
//             }

//             last_active_states[contact_index] = is_active;
//         }

//         contact_index++;
//         contact = contact->next;
//     }

//     last_check_time = current_time;
//     return ret;
// }

// bool dtn_routing_is_dtn_destination(Routing_Function* routing, const ip6_addr_t* dest_ip_in) {
//     if (!routing || !dest_ip_in) {
//         return false;
//     }

//     ip6_addr_t local_dest_ip;
//     memset(&local_dest_ip, 0, sizeof(ip6_addr_t));
//     memcpy(&local_dest_ip, dest_ip_in, sizeof(ip6_addr_t));

//     // Check the contact list
//     Contact_Info* contact = routing->contact_list_head;
//     while (contact != NULL) {
//         if (contact->is_dtn_node) {
//             ip6_addr_t contact_addr_nozone = contact->node_addr;
//             ip6_addr_t local_dest_nozone = local_dest_ip;

// #if LWIP_IPV6_SCOPES
//             ip6_addr_set_zone(&contact_addr_nozone, IP6_NO_ZONE);
//             ip6_addr_set_zone(&local_dest_nozone, IP6_NO_ZONE);
// #endif

//             if (ip6_addr_eq(&local_dest_nozone, &contact_addr_nozone)) {
//                 char local_dest_nozone_str[IP6ADDR_STRLEN_MAX];
//                 ip6addr_ntoa_r(&local_dest_nozone, local_dest_nozone_str,
//                                sizeof(local_dest_nozone_str));
//                 DTN_INFO("Dest: %s is a DTN destination", local_dest_nozone_str);
//                 return true;
//             }
//         }
//         contact = contact->next;
//     }
//     return false;
// }