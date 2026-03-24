// dtn_routing.c: Implementation of Contact Graph Routing with time-variant contact management for DTN networks
// Copyright (C) 2025 Michael Karpov & 2026 Cèlia Torras
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

#include "dtn_routing.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "lwip/ip6_addr.h"
#include "lwip/sys.h"
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <ctype.h>

#define CURR_NODE_ADDR "fd00:01::2"
#define MAX_LENGTH 5000

Routing_Function* dtn_routing_create(DTN_Module* parent) {
    Routing_Function* routing = (Routing_Function*)malloc(sizeof(Routing_Function));
    if (routing) {
        routing->parent_module = parent;
        routing->routing_algorithm_name = "Contact Graph Routing";
        routing->contact_list_head = NULL; 
        routing->base_time = sys_now();
        
        printf("DTN Routing Function created. Mode: %s\n", routing->routing_algorithm_name);
        
        //We save the contacts from the contact plan in the contact_list_head
        const char *contacts_file = "py_cgr/contact_plans/cgr_tutorial_1.txt";
        int nloaded = dtn_routing_load_contacts(routing, contacts_file);
        if (nloaded < 0) {
            fprintf(stderr, "DTN Routing: error loading contact plan %s\n", contacts_file);
        }

    } else {
        perror("Failed to allocate memory for Routing_Function");
    }
    return routing;
}

void dtn_routing_destroy(Routing_Function* routing) {
    if (!routing) return;
    
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

int dtn_routing_add_contact(Routing_Function* routing, 
                          const ip6_addr_t* node_addr, 
                          const ip6_addr_t* next_hop,
                          u32_t start_time_ms, 
                          u32_t end_time_ms,
                          bool is_dtn_node) {
    if (!routing || !node_addr || !next_hop) return 0;
    
    Contact_Info* new_contact = (Contact_Info*)malloc(sizeof(Contact_Info));
    if (!new_contact) {
        perror("Failed to allocate memory for Contact_Info");
        return 0;
    }
    
    ip6_addr_copy(new_contact->node_addr, *node_addr);
    ip6_addr_copy(new_contact->next_hop, *next_hop);
    new_contact->start_time_ms = start_time_ms;
    new_contact->end_time_ms = end_time_ms;
    new_contact->is_dtn_node = is_dtn_node;
    new_contact->next = NULL;
    
    if (routing->contact_list_head == NULL) {
        routing->contact_list_head = new_contact;
    } else {
        Contact_Info* current = routing->contact_list_head;
        while (current->next != NULL) {
            current = current->next;
        }
        current->next = new_contact;
    }
    
    char node_addr_str[IP6ADDR_STRLEN_MAX];
    char next_hop_str[IP6ADDR_STRLEN_MAX];
    ip6addr_ntoa_r(node_addr, node_addr_str, sizeof(node_addr_str));
    ip6addr_ntoa_r(next_hop, next_hop_str, sizeof(next_hop_str));
    
    printf("DTN Routing: Added contact for %s via %s (%s), start: %u ms, end: %u ms\n",
            node_addr_str, next_hop_str, is_dtn_node ? "DTN" : "non-DTN", 
           start_time_ms, end_time_ms);
    
    return 1;
}

// not used
int dtn_routing_remove_contact(Routing_Function* routing, const ip6_addr_t* node_addr) {
    if (!routing || !node_addr || !routing->contact_list_head) return 0;
    
    Contact_Info* current = routing->contact_list_head;
    Contact_Info* prev = NULL;
    
    while (current != NULL) {
        if (ip6_addr_cmp(&current->node_addr, node_addr)) {
            // Found the contact to remove
            if (prev == NULL) {
                // First item
                routing->contact_list_head = current->next;
            } else {
                prev->next = current->next;
            }
            
            char node_addr_str[IP6ADDR_STRLEN_MAX];
            ip6addr_ntoa_r(node_addr, node_addr_str, sizeof(node_addr_str));
            printf("DTN Routing: Removed contact for %s\n", node_addr_str);
            
            free(current);
            return 1;
        }
        
        prev = current;
        current = current->next;
    }
    
    return 0; // Contact not found
}

// no chanches needed, funciton used only to print any changes in the contacts' state
bool dtn_routing_update_contacts(Routing_Function* routing) {
    if (!routing) return false;
    
    bool ret = false;
    static u32_t last_check_time = 0;
    static bool last_active_states[100] = {false};
    static int contact_index = 0;
    
    u32_t current_time = sys_now(); //time when the computer has started
    
    if (last_check_time == 0) {
        last_check_time = current_time;
    }
    
    // Iterate through all contacts
    Contact_Info* contact = routing->contact_list_head;
    contact_index = 0;
    
    while (contact != NULL && contact_index < 100) { 
        bool is_active = (current_time >= contact->start_time_ms && 
                          current_time <= contact->end_time_ms);
                          
        if (is_active != last_active_states[contact_index]) {
            char node_addr_str[IP6ADDR_STRLEN_MAX], next_hop_str[IP6ADDR_STRLEN_MAX];
            ip6addr_ntoa_r(&contact->node_addr, node_addr_str, sizeof(node_addr_str));
            ip6addr_ntoa_r(&contact->next_hop, next_hop_str, sizeof(next_hop_str));
            
            if (is_active) {
                printf("DTN Routing: Contact from %s to %s became AVAILABLE at time %u ms\n", 
                       next_hop_str, node_addr_str, current_time);
                ret = true;

            } else {
                printf("DTN Routing: Contact for %s became UNAVAILABLE at time %u ms\n", 
                       node_addr_str, current_time);
            }
            
            last_active_states[contact_index] = is_active;
        }
        
        contact_index++;
        contact = contact->next;
    }
    
    last_check_time = current_time;
    return ret;
}

bool dtn_routing_is_dtn_destination(Routing_Function* routing, const ip6_addr_t* dest_ip_in) {
    if (!routing || !dest_ip_in) {
        return false;
    }
    
    ip6_addr_t local_dest_ip;
    memset(&local_dest_ip, 0, sizeof(ip6_addr_t));
    memcpy(&local_dest_ip, dest_ip_in, sizeof(ip6_addr_t));
    
    // Check the contact list
    Contact_Info* contact = routing->contact_list_head;
    while (contact != NULL) {
        if (contact->is_dtn_node) {
            ip6_addr_t contact_addr_nozone = contact->node_addr;
            ip6_addr_t local_dest_nozone = local_dest_ip;
            
#if LWIP_IPV6_SCOPES
            ip6_addr_set_zone(&contact_addr_nozone, IP6_NO_ZONE);
            ip6_addr_set_zone(&local_dest_nozone, IP6_NO_ZONE);
#endif
            
            if (ip6_addr_cmp(&local_dest_nozone, &contact_addr_nozone)) {
                return true;
            }
        }
        contact = contact->next;
    }
    return false;
}
   
int dtn_routing_get_dtn_next_hop(Routing_Function* routing, u32_t* v_tc_fl, u16_t* plen, u8_t* hoplim, ip6_addr_t* dest_ip, ip6_addr_t* sender_ip, ip6_addr_t* next_hop_ip) {
    if (!routing || !v_tc_fl || !plen || !hoplim || !dest_ip || !next_hop_ip) {
        fprintf(stderr, "DTN Routing: Invalid arguments to get_dtn_next_hop.\n");
        return 0;
    }
    
    if (!dtn_routing_is_dtn_destination(routing, dest_ip)) {
        char dest_addr_str_err[IP6ADDR_STRLEN_MAX];
        ip6addr_ntoa_r(dest_ip, dest_addr_str_err, sizeof(dest_addr_str_err));
        fprintf(stderr, "DTN Routing ERROR: get_dtn_next_hop called for non-DTN dest %s\n", dest_addr_str_err);
        ip6_addr_set_any(next_hop_ip);
        return 0;
    }

    ip6_addr_t local;
    unsigned char tmpbuf[16];
    if (inet_pton(AF_INET6, CURR_NODE_ADDR , tmpbuf) != 1) {
        fprintf(stderr, "inet_pton local address failed\n");
        return 0;
    }
    for (int i=0;i<4;i++) {
        uint32_t w = (tmpbuf[i*4+0] << 24) | (tmpbuf[i*4+1] << 16) | (tmpbuf[i*4+2] << 8) | (tmpbuf[i*4+3]);
        local.addr[i] = ntohl(w);
    }

    char dst_s[INET6_ADDRSTRLEN]; 
    char sender_s[INET6_ADDRSTRLEN];

    if (ip6_addr_to_str(dest_ip, dst_s, sizeof(dst_s)) != 0) { 
        fprintf(stderr, "ip6_addr_to_str dest failed\n"); return 1; 
    }
    if (ip6_addr_to_str(sender_ip, sender_s, sizeof(sender_s)) != 0) { 
        fprintf(stderr, "ip6_addr_to_str sender failed\n"); return 1; 
    }

    Py_Initialize();
    if (!Py_IsInitialized()) {
        fprintf(stderr, "Python not initialized\n");
        return 0;
    }

    PyObject *sys_path = PySys_GetObject("path");
    PyObject *py_pth = PyUnicode_FromString("py_cgr");
    PyList_Append(sys_path, py_pth);
    Py_DECREF(py_pth);

    PyObject *pModule = PyImport_ImportModule("py_cgr_lib.py_cgr_lib");
    if (!pModule) {
        fprintf(stderr, "[ERR] ERROR: cannot import py_cgr_lib.py_cgr_lib\n");
        PyErr_Print();
        Py_Finalize();
        return 0;
    }
    
    double curr_time_load = ((double)routing->base_time)/1000;

    PyObject *py_cp_load = PyObject_GetAttrString(pModule, "cp_load");
    PyObject *py_cgr_yen = PyObject_GetAttrString(pModule, "cgr_yen");
    PyObject *py_fwd_candidate = PyObject_GetAttrString(pModule, "fwd_candidate");
    PyObject *py_ipv6_packet = PyObject_GetAttrString(pModule, "ipv6_packet");

    // cp_load
    PyObject *args_load = PyTuple_New(3);
    PyTuple_SetItem(args_load, 0, PyUnicode_FromString("py_cgr/contact_plans/cgr_tutorial_Simulation.txt"));
    PyTuple_SetItem(args_load, 1, PyFloat_FromDouble(curr_time_load));
    PyTuple_SetItem(args_load, 2, PyLong_FromLong(MAX_LENGTH));
    PyObject *contact_plan = PyObject_CallObject(py_cp_load, args_load);
    if (!contact_plan) {
        fprintf(stderr, "[ERR] cp_load returned NULL\n");
        PyErr_Print();
        Py_DECREF(pModule);
        Py_Finalize();
        return 0;
    }
    Py_DECREF(args_load);

    // cgr_yen
    long curr_node_id = ipv6_to_nodeid(CURR_NODE_ADDR);
    long dest_node_id = ipv6_to_nodeid(dst_s);
    double curr_time = ((double)sys_now())/1000;

    PyObject *args_yen = PyTuple_New(6);
    PyTuple_SetItem(args_yen, 0, PyFloat_FromDouble(curr_time));
    PyTuple_SetItem(args_yen, 1, PyLong_FromLong(curr_node_id));
    PyTuple_SetItem(args_yen, 2, PyLong_FromLong(dest_node_id));
    PyTuple_SetItem(args_yen, 3, PyFloat_FromDouble(curr_time));
    PyTuple_SetItem(args_yen, 4, contact_plan);
    PyTuple_SetItem(args_yen, 5, PyLong_FromLong(10)); 
    PyObject *routes = PyObject_CallObject(py_cgr_yen, args_yen);
    if (!routes) {
        fprintf(stderr, "[ERR] cgr_yen returned NULL\n");
        PyErr_Print();
        Py_DECREF(contact_plan);
        Py_DECREF(pModule);
        Py_Finalize();
        return 0;
    }
    Py_DECREF(args_yen);

    // ipv6_packet
    uint8_t hoplim_val = 0;
    uint32_t v_tc_fl_val = 0;
    uint16_t plen_val = 0;

    if (hoplim != NULL) hoplim_val = *hoplim;
    if (v_tc_fl != NULL) v_tc_fl_val = *v_tc_fl;
    if (plen != NULL) plen_val = *plen;

    long deadline = hoplim_val*10000;                 //multiplying factor
    uint8_t tc = (uint8_t)((v_tc_fl_val >> 20) & 0xFF); // traffic class (8 bits) 
    uint8_t dscp = (uint8_t)(tc >> 2);              // DSCP = TC[7:2] (6 bits)

    long sender_node_id = ipv6_to_nodeid(sender_s);
    
    PyObject *args_pkt = PyTuple_New(6);
    PyTuple_SetItem(args_pkt, 0, PyFloat_FromDouble(curr_time));
    PyTuple_SetItem(args_pkt, 1, PyLong_FromLong(dest_node_id));
    PyTuple_SetItem(args_pkt, 2, PyLong_FromLong(plen_val));
    PyTuple_SetItem(args_pkt, 3, PyLong_FromLong(deadline));
    PyTuple_SetItem(args_pkt, 4, PyLong_FromLong(dscp));
    PyTuple_SetItem(args_pkt, 5, PyLong_FromLong(sender_node_id));
    PyObject *ipv6pkt = PyObject_CallObject(py_ipv6_packet, args_pkt);
    if (!ipv6pkt) {
        fprintf(stderr, "[ERR] ipv6_packet constructor returned NULL\n");
        PyErr_Print();
        Py_DECREF(routes);
        Py_DECREF(contact_plan);
        Py_DECREF(pModule);
        Py_Finalize();
    return 0;
    }
    Py_DECREF(args_pkt);

    // fwd_candidate
    PyObject *excluded_nodes = PyList_New(0);
    PyObject *args_fwd = PyTuple_New(6);
    PyTuple_SetItem(args_fwd, 0, PyFloat_FromDouble(curr_time));
    PyTuple_SetItem(args_fwd, 1, PyLong_FromLong(curr_node_id));
    PyTuple_SetItem(args_fwd, 2, contact_plan);
    PyTuple_SetItem(args_fwd, 3, ipv6pkt);
    PyTuple_SetItem(args_fwd, 4, routes);
    PyTuple_SetItem(args_fwd, 5, excluded_nodes);
    PyObject *candidates = PyObject_CallObject(py_fwd_candidate, args_fwd);
    Py_DECREF(args_fwd);

    //we check the next hop for the best route
    if (PyList_Check(candidates) && PyList_Size(candidates) > 0) {
        PyObject *first = PyList_GetItem(candidates, 0);
        PyObject *pNextNode = PyObject_GetAttrString(first, "next_node");
        if (pNextNode) {
            if (pNextNode == Py_None) {
                printf("Next hop: None\n");
            } else if (PyLong_Check(pNextNode)) {
                long next_node = PyLong_AsLong(pNextNode);
                ip6_addr_t next_ip;
                if (nodeid_to_ipv6(next_node, &next_ip) == 0) {
                    memcpy(next_hop_ip, &next_ip, sizeof(ip6_addr_t));
                    char next_ip_s[INET6_ADDRSTRLEN];
                    if (ip6_addr_to_str(&next_ip, next_ip_s, sizeof(next_ip_s)) == 0) {
                        printf("Next hop ipv6: %s\n", next_ip_s);
                    } else {
                        fprintf(stderr, "Failed to stringify next_ip for node %ld\n", next_node);
                        Py_DECREF(candidates);
                        Py_DECREF(pModule);
                        Py_Finalize();
                        return 0;
                    }
                } else {
                    fprintf(stderr, "No mapping nodeid->ipv6 for node %ld\n", next_node);
                    Py_DECREF(candidates);
                    Py_DECREF(pModule);
                    Py_Finalize();
                    return 0;
                }

            } else {
                printf("Next hop: (non-int)\n");
            }
            Py_DECREF(pNextNode);
        } else {
            PyErr_Clear();
            printf("Candidate object has no attribute next_node\n");
            Py_DECREF(candidates);
            Py_DECREF(pModule);
            Py_Finalize();
            return 0;
        }
    } else {
        printf("No candidate routes returned (list empty or not a list)\n");
        Py_DECREF(candidates);
        Py_DECREF(pModule);
        Py_Finalize();
        return 0;
    }
    
    Py_DECREF(candidates);
    Py_DECREF(pModule);
    Py_Finalize();
    return 1;
}

int ip6_addr_to_str(const ip6_addr_t *a, char *buf, size_t buflen) {
    if (!a || !buf) return -1;
    unsigned char tmp[16];
    for (int i = 0; i < 4; ++i) {
        uint32_t w = ntohl(a->addr[i]);
        tmp[i*4 + 0] = (w >> 24) & 0xFF;
        tmp[i*4 + 1] = (w >> 16) & 0xFF;
        tmp[i*4 + 2] = (w >> 8 ) & 0xFF;
        tmp[i*4 + 3] = (w >> 0 ) & 0xFF;
    }
    if (!inet_ntop(AF_INET6, tmp, buf, (socklen_t)buflen)) return -1;
    return 0;
}

long ipv6_to_nodeid(const char *ip6) {

    // Node 0 (id = 1)
    if (strcmp(ip6, "fd00:01::1") == 0) return 01;
    if (strcmp(ip6, "fd00:1::1") == 0) return 01;

    // Node 1 (id = 2)
    if (strcmp(ip6, "fd00:01::2") == 0) return 10;
    if (strcmp(ip6, "fd00:1::1") == 0) return 10;
    if (strcmp(ip6, "fd00:12::1") == 0) return 12;

    // Node 2 (id = 3)
    if (strcmp(ip6, "fd00:12::2") == 0) return 21;
    if (strcmp(ip6, "fd00:23::2") == 0) return 23;

    // Node 3 (id = 4)
    if (strcmp(ip6, "fd00:23::3") == 0) return 32;
    
    return -1;
}

int nodeid_to_ipv6(long node_id, ip6_addr_t *out) {

    const char *addr_txt = NULL;
    switch (node_id) {
        case 01: addr_txt = "fd00:01::1"; break;
        case 10: addr_txt = "fd00:01::2"; break;
        case 12: addr_txt = "fd00:12::1"; break;
        case 21: addr_txt = "fd00:12::2"; break;
        case 23: addr_txt = "fd00:23::2"; break;
        case 32: addr_txt = "fd00:23::3"; break;
        default: return -1;
    }

    unsigned char tmpbuf[16];
    if (inet_pton(AF_INET6, addr_txt, tmpbuf) != 1) {
        return -1;
    }

    for (int i = 0; i < 4; ++i) {
        uint32_t w = (tmpbuf[i*4 + 0] << 24) |
                     (tmpbuf[i*4 + 1] << 16) |
                     (tmpbuf[i*4 + 2] << 8 ) |
                     (tmpbuf[i*4 + 3] << 0 );
        out->addr[i] = ntohl(w);
    }
    return 0;
}

int dtn_routing_load_contacts(Routing_Function* routing, const char* filename) {
    if (!routing || !filename) return -1;

    FILE *f = fopen(filename, "r");
    if (!f) {
        fprintf(stderr, "DTN Routing: failed to open contact file '%s': %s\n", filename, strerror(errno));
        return -1;
    }

    char line[512];
    int loaded = 0;

    while (fgets(line, sizeof(line), f)) {
        // trim leading spaces
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;

        if (*p == '\0' || *p == '#') continue;

        char tok[8][64];
        int ntok = 0;
        char *s = p;
        while (ntok < 8) {
            // skip spaces
            while (*s && isspace((unsigned char)*s)) s++;
            if (!*s || *s == '\n' || *s == '\r') break;
            // read token
            int i = 0;
            while (*s && !isspace((unsigned char)*s) && i < 63) {
                tok[ntok][i++] = *s++;
            }
            tok[ntok][i] = '\0';
            ntok++;
        }

        if (ntok < 5) continue; 

        char *start_tok = NULL, *end_tok = NULL;
        for (int i = 0; i < ntok; ++i) {
            if (tok[i][0] == '+') {
                if (!start_tok) start_tok = tok[i];
                else if (!end_tok) end_tok = tok[i];
            }
        }

        char *from_tok = NULL, *to_tok = NULL;
        for (int i = 0; i < ntok; ++i) {
            bool all_digits = true;
            size_t L = strlen(tok[i]);
            if (L == 0 || L > 3) continue;
            for (size_t j = 0; j < L; ++j) if (!isdigit((unsigned char)tok[i][j])) { all_digits = false; break; }
            if (all_digits) {
                if (!from_tok) from_tok = tok[i];
                else if (!to_tok) to_tok = tok[i];
            }
        }

        if (!start_tok || !end_tok || !from_tok || !to_tok) {
            continue;
        }

        int start_sec = 0, end_sec = 0;
        if (sscanf(start_tok, "+%d", &start_sec) != 1) continue;
        if (sscanf(end_tok,   "+%d", &end_sec)   != 1) continue;

        u32_t start_ms = (u32_t)start_sec * 1000;
        u32_t end_ms   = (u32_t)end_sec   * 1000;

        long from_node = (long) atoi(from_tok);
        long to_node = (long) atoi(to_tok);

        if (from_node < 0 || to_node < 0) {
            fprintf(stderr, "DTN Routing: bad node token from='%s' to='%s' (skipping)\n", from_tok, to_tok);
            continue;
        }

        ip6_addr_t from_ip6, to_ip6;
        if (nodeid_to_ipv6(from_node, &from_ip6) != 0) {
            fprintf(stderr, "DTN Routing: nodeid_to_ipv6 failed for node %ld (from token '%s'), skipping\n", from_node, from_tok);
            continue;
        }
        if (nodeid_to_ipv6(to_node, &to_ip6) != 0) {
            fprintf(stderr, "DTN Routing: nodeid_to_ipv6 failed for node %ld (to token '%s'), skipping\n", to_node, to_tok);
            continue;
        }

        #if LWIP_IPV6_SCOPES
            ip6_addr_set_zone(&from_ip6, IP6_NO_ZONE);
            ip6_addr_set_zone(&to_ip6, IP6_NO_ZONE);
        #endif

        int added = dtn_routing_add_contact(routing, &to_ip6, &from_ip6, start_ms + routing->base_time, end_ms + routing->base_time, true);
        if (added) loaded++;
    }

    fclose(f);
    printf("DTN Routing: Loaded %d contacts from %s\n", loaded, filename);
    return loaded;
}