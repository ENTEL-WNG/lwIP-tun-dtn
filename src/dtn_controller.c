// dtn_controller.c: Implementation of the DTN Controller that processes incoming packets and
// manages store-and-forward operations Copyright (C) 2025 Michael Karpov & 2025 Cèlia Torras
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or any later
// version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "dtn_controller.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dtn_config.h"
#include "dtn_custody.h"
#include "dtn_icmpv6.h"
#include "dtn_logger.h"
#include "dtn_routing.h"
#include "dtn_storage.h"
#include "dtn_utils.h"
#include "lwip/err.h"
#include "lwip/icmp6.h"
#include "lwip/ip6.h"
#include "lwip/ip6_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/stats.h"
#include "lwip/sys.h"
#include "lwip/timeouts.h"  // sys_timeout
#include "raw_socket.h"

extern DTN_Module* global_dtn_module;

// Format the src/dest/custodian address strings used only by log lines. Kept
// separate so the hot path can build them lazily (skipping ip6addr_ntoa_r when
// the relevant log level is suppressed). custodian_addr is read only when
// has_custodian is true.
static void dtn_controller_format_addr_strings(const struct ip6_hdr* ip6hdr, bool has_custodian, const ip6_addr_t* custodian_addr,
                                               char* src_str, char* dest_str, char* custodian_str) {
    ip6_addr_t src_addr, dest_addr;
    ip6_addr_copy_from_packed(src_addr, ip6hdr->src);
    ip6_addr_copy_from_packed(dest_addr, ip6hdr->dest);
    ip6addr_ntoa_r(&src_addr, src_str, IP6ADDR_STRLEN_MAX);
    ip6addr_ntoa_r(&dest_addr, dest_str, IP6ADDR_STRLEN_MAX);
    if (has_custodian)
        ip6addr_ntoa_r(custodian_addr, custodian_str, IP6ADDR_STRLEN_MAX);
    else
        strncpy(custodian_str, "NONE", IP6ADDR_STRLEN_MAX);
}

// ---------------------------------------------------------------------------
// Per-node throughput stats
// ---------------------------------------------------------------------------

static uint32_t stat_fwd_immediate = 0;  // packets forwarded on-the-spot
static uint32_t stat_fwd_stored = 0;     // stored packets forwarded later
static uint32_t stat_stored = 0;         // packets placed in storage
static uint32_t stat_dropped = 0;        // packets discarded (no route / send error)

static void stats_timer_cb(void* arg) {
    (void)arg;
    int db_count = (global_dtn_module && global_dtn_module->storage) ? dtn_storage_count(global_dtn_module->storage) : -1;
    DTN_INFO("[STATS] fwd_now=%u fwd_stored=%u stored=%u cur_stored=%u dropped=%u db=%d", stat_fwd_immediate, stat_fwd_stored, stat_stored,
             stat_stored - stat_fwd_stored, stat_dropped, db_count);
#if MEM_STATS
    DTN_INFO("[MEM]   used=%" U32_F " max=%" U32_F " avail=%" U32_F " err=%" U16_F, (u32_t)lwip_stats.mem.used, (u32_t)lwip_stats.mem.max,
             (u32_t)lwip_stats.mem.avail, (u16_t)lwip_stats.mem.err);
#endif
    sys_timeout(DTN_STATS_INTERVAL_MS, stats_timer_cb, NULL);
}

void dtn_controller_stats_timer_start(void) { sys_timeout(DTN_STATS_INTERVAL_MS, stats_timer_cb, NULL); }

static dtn_icmpv6_process_result_t dtn_controller_process_icmpv6(struct pbuf* p) { return dtn_icmpv6_process(p); }

static dtn_socket_result_t dtn_controller_send(struct pbuf* p, DtnRoutingResult routing_result) {
    const DtnInterface* dtn_interface = dtn_raw_socket_get_interface_for_node(routing_result.next_hop_node_id);

    if (!dtn_interface) {
        return DTN_CONTROLLER_PROCESS_OUTGOING_ERR;
    }

    struct pbuf* send_p = p;
    if (dtn_interface->local_addr_valid) {
        // local_addr was parsed once at init (dtn_interface_cache_parsed); rebuild
        // the ip6_addr_t from the cached bytes instead of re-parsing the string.
        ip6_addr_t local_addr;
        memcpy(local_addr.addr, dtn_interface->local_addr_bytes, sizeof(local_addr.addr));
#if LWIP_IPV6_SCOPES
        ip6_addr_clear_zone(&local_addr);
#endif
        struct pbuf* p_custodian = dtn_update_or_add_custodian_option(p, &local_addr);
        if (p_custodian)
            send_p = p_custodian;
    }

    dtn_socket_result_t socket_result = dtn_raw_socket_send_via_interface(send_p, dtn_interface);

    if (send_p != p)
        pbuf_free(send_p);

    return socket_result;
}

dtn_controller_process_incoming_result_t dtn_controller_process_incoming(struct pbuf* p, struct netif* inp_netif) {
    if (!p || !global_dtn_module || !global_dtn_module->routing || !global_dtn_module->storage || !inp_netif) {
        DTN_ERROR("DTN Controller: Invalid arguments or uninitialized components for incoming.");
        return DTN_CONTROLLER_PROCESS_INCOMING_ERR;
    }

    if (p->len < IP6_HLEN) {
        DTN_WARN("DTN Controller: Packet too small for IPv6 header.");
        return DTN_CONTROLLER_PROCESS_INCOMING_ERR;
    }

    const struct ip6_hdr* ip6hdr = (const struct ip6_hdr*)p->payload;
    if (IP6H_V(ip6hdr) != 6) {
        DTN_WARN("DTN Controller: Packet is not IPv6 (version %d).", IP6H_V(ip6hdr));
        return DTN_CONTROLLER_PROCESS_INCOMING_ERR;
    }

    char src_str[IP6ADDR_STRLEN_MAX], dest_str[IP6ADDR_STRLEN_MAX], custodian_str[IP6ADDR_STRLEN_MAX];
    ip6_addr_t dest_addr, custodian_addr;
    ip6_addr_copy_from_packed(dest_addr, ip6hdr->dest);  // needed below for the local-address check
    bool has_custodian = dtn_extract_custodian_option(p, &custodian_addr);

    // Address strings are log-only; build them now only if INFO is enabled.
    // Rare failure branches below rebuild them just-in-time so WARN/ERROR logs
    // keep their addresses even when INFO is suppressed.
    bool addr_strings_built = dtn_log_enabled(DTN_LOG_LEVEL_INFO);
    if (addr_strings_built)
        dtn_controller_format_addr_strings(ip6hdr, has_custodian, &custodian_addr, src_str, dest_str, custodian_str);
    else
        src_str[0] = dest_str[0] = custodian_str[0] = '\0';

    DTN_INFO("Packet|| src: %s -> dest: %s | custodian: %s || TRY process incoming.", src_str, dest_str, custodian_str);

    if (PRINT_PAYLOAD) {
        const char* payload = dtn_utils_print_packet_payload(p);
        DTN_INFO("Packet|| src: %s -> dest: %s | custodian: %s | payload: %s ||", src_str, dest_str, custodian_str, payload);
    }

    // Check if this is ICMPv6 and process it
    if (IP6H_NEXTH(ip6hdr) == IP6_NEXTH_ICMP6) {
        DTN_INFO("Packet|| src: %s -> dest: %s | custodian: %s || TRY to process as ICMPv6.", src_str, dest_str, custodian_str);
        dtn_icmpv6_process_result_t result = dtn_controller_process_icmpv6(p);
        if (result == DTN_ICMPV6_PROCESS_OK) {
            DTN_INFO("Packet|| src: %s -> dest: %s | custodian: %s || SUCCESSFUL processed as ICMPv6.", src_str, dest_str, custodian_str);
            return DTN_CONTROLLER_PROCESS_INCOMING_ICMPV6;
        }
    }

    if (has_custodian) {
        dtn_icmpv6_send_pck_received(p, ICMP6_CODE_DTN_NO_INFO);
    }

    if (dtn_utils_is_local_addr(&dest_addr)) {
        DTN_INFO("Packet|| src: %s -> dest: %s | custodian: %s || TRY to process as local.", src_str, dest_str, custodian_str);
        dtn_icmpv6_send_pck_delivered(p, ICMP6_CODE_DTN_NO_INFO);

        // ip6_input takes ownership of p and frees it internally.
        err_t err = ip6_input(p, inp_netif);

        if (err == ERR_OK) {
            DTN_INFO("Packet|| src: %s -> dest: %s | custodian: %s || SUCCESSFUL to process as local.", src_str, dest_str, custodian_str);
            return DTN_CONTROLLER_PROCESS_INCOMING_LOCAL;
        }
        if (!addr_strings_built)
            dtn_controller_format_addr_strings(ip6hdr, has_custodian, &custodian_addr, src_str, dest_str, custodian_str);
        DTN_ERROR("Packet|| src: %s -> dest: %s | custodian: %s || FAILED to process as local, error: %d.", src_str, dest_str,
                  custodian_str, err);
        return DTN_CONTROLLER_PROCESS_INCOMING_ERR;
    }

    DtnRoutingResult routing_result;
    dtn_controller_process_outgoing_result_t result = dtn_controller_process_outgoing(p, &routing_result);

    switch (result) {
        case DTN_CONTROLLER_PROCESS_OUTGOING_OK: {
            DTN_INFO("Packet|| src: %s -> dest: %s | custodian: %s || TRY to forward.", src_str, dest_str, custodian_str);
            dtn_socket_result_t socket_result = dtn_controller_send(p, routing_result);
            if (socket_result == DTN_SOCKET_OK) {
                stat_fwd_immediate++;
                DTN_INFO("Packet|| src: %s -> dest: %s | custodian: %s || SUCCESSFUL forwarded.", src_str, dest_str, custodian_str);
                if (!has_custodian) {
                    DTN_INFO("Packet|| src: %s -> dest: %s | custodian: %s || no custodian so no ICMPV6_PCK_FORWARDED.", src_str, dest_str,
                             custodian_str);
                    return DTN_CONTROLLER_PROCESS_INCOMING_FORWARDED;
                }
                dtn_icmpv6_send_message_result_t icmp_result = dtn_icmpv6_send_pck_forwarded(p, ICMP6_CODE_DTN_NO_INFO);
                if (icmp_result == DTN_ICMPV6_SEND_MESSAGE_OK) {
                    DTN_INFO(
                        "Packet|| src: %s -> dest: %s | custodian: %s || SUCCESSFUL send "
                        "ICMPV6_PCK_FORWARDED",
                        src_str, dest_str, custodian_str);
                }
                return DTN_CONTROLLER_PROCESS_INCOMING_FORWARDED;
            }
            stat_dropped++;
            if (!addr_strings_built)
                dtn_controller_format_addr_strings(ip6hdr, has_custodian, &custodian_addr, src_str, dest_str, custodian_str);
            DTN_ERROR("Packet|| src: %s -> dest: %s | custodian: %s || FAILED to forward", src_str, dest_str, custodian_str);
            return DTN_CONTROLLER_PROCESS_INCOMING_ERR;
        }
        case DTN_CONTROLLER_PROCESS_OUTGOING_STORE: {
            dtn_storage_store_packet_result_t storage_result = dtn_storage_store_packet(global_dtn_module->storage, p, &routing_result);
            if (storage_result == DTN_STORAGE_STORE_OK) {
                stat_stored++;
                DTN_INFO("Packet|| src: %s -> dest: %s | custodian: %s || SUCCESSFUL stored.", src_str, dest_str, custodian_str);
            } else {
                stat_dropped++;
                DTN_INFO("Packet|| src: %s -> dest: %s | custodian: %s || FAILED storing %d.", src_str, dest_str, custodian_str,
                         storage_result);
            }
            return DTN_CONTROLLER_PROCESS_INCOMING_STORED;
        }
        case DTN_CONTROLLER_PROCESS_OUTGOING_ERR: {
            DTN_INFO("Packet|| src: %s -> dest: %s | custodian: %s || TRY to send without CGR", src_str, dest_str, custodian_str);
            dtn_socket_result_t socket_result = dtn_raw_socket_send(p);
            if (socket_result == DTN_SOCKET_OK) {
                stat_fwd_immediate++;
                DTN_INFO("Packet|| src: %s -> dest: %s | custodian: %s || SUCCESSFUL send without CGR", src_str, dest_str, custodian_str);
                return DTN_CONTROLLER_PROCESS_INCOMING_FORWARDED;
            }
            stat_dropped++;
            if (!addr_strings_built)
                dtn_controller_format_addr_strings(ip6hdr, has_custodian, &custodian_addr, src_str, dest_str, custodian_str);
            DTN_WARN("Packet|| src: %s -> dest: %s | custodian: %s || FAILED to send without CGR", src_str, dest_str, custodian_str);
            return DTN_CONTROLLER_PROCESS_INCOMING_ERR;
        }
        default:
            break;
    }

    return DTN_CONTROLLER_PROCESS_INCOMING_ERR;
}

dtn_controller_process_outgoing_result_t dtn_controller_process_outgoing(struct pbuf* p, DtnRoutingResult* out_routing_result) {
    Routing_Function* routing = global_dtn_module->routing;
    struct ip6_hdr* ip6hdr = (struct ip6_hdr*)p->payload;

    // These strings are log-only; build lazily (the NO_ROUTE error path below
    // rebuilds them if INFO was suppressed). has_custodian is needed regardless.
    char src_str[IP6ADDR_STRLEN_MAX], dest_str[IP6ADDR_STRLEN_MAX], custodian_str[IP6ADDR_STRLEN_MAX];
    ip6_addr_t custodian_addr;
    bool has_custodian = dtn_extract_custodian_option(p, &custodian_addr);
    bool addr_strings_built = dtn_log_enabled(DTN_LOG_LEVEL_INFO);
    if (addr_strings_built)
        dtn_controller_format_addr_strings(ip6hdr, has_custodian, &custodian_addr, src_str, dest_str, custodian_str);
    else
        src_str[0] = dest_str[0] = custodian_str[0] = '\0';

    DTN_INFO("Packet|| src: %s -> dest: %s | custodian: %s || TRY to process outgoing.", src_str, dest_str, custodian_str);

    DtnRoutingResult routing_result;
    dtn_routing_result_t routing_status =
        dtn_routing_get_next_hop_node_id(routing->base_time_in_ms, sys_now(), (struct ip6_hdr*)ip6hdr, &routing_result);
    if (out_routing_result)
        *out_routing_result = routing_result;

    // No route found
    if (routing_status != DTN_ROUTING_OK) {
        if (routing_status == DTN_ROUTING_NO_ROUTE) {
            if (!addr_strings_built)
                dtn_controller_format_addr_strings(ip6hdr, has_custodian, &custodian_addr, src_str, dest_str, custodian_str);
            DTN_ERROR("Packet|| src: %s -> dest: %s | custodian: %s || not route found.", src_str, dest_str, custodian_str);
        }
        return DTN_CONTROLLER_PROCESS_OUTGOING_ERR;
    }

    bool is_next_hop_active;
    int active_result = dtn_routing_is_next_hop_active(sys_now(), routing_result, &is_next_hop_active);

    // Store
    if (active_result == DTN_ROUTING_OK && !is_next_hop_active) {
        return DTN_CONTROLLER_PROCESS_OUTGOING_STORE;
    }

    return DTN_CONTROLLER_PROCESS_OUTGOING_OK;
}

int dtn_controller_process_stored(void) {
    if (!global_dtn_module || !global_dtn_module->storage || !global_dtn_module->routing) {
        return 0;
    }

    Storage_Function* storage = global_dtn_module->storage;

    double now_sec = sys_now() / 1000.0;
    if (!dtn_storage_any_ready_entries(storage, now_sec)) {
        return 0;
    }

    static double pacing_tokens = 0.0;
    static u32_t pacing_last_ms = 0;
    const double tokens_per_ms = (double)DTN_FORWARD_RATE_PKTS_PER_SEC / 1000.0;
    const double token_cap = tokens_per_ms * (double)DTN_FORWARD_PACING_TICK_MS * 2.0;

    u32_t now_ms = sys_now();
    if (pacing_last_ms != 0) {
        pacing_tokens += (double)(now_ms - pacing_last_ms) * tokens_per_ms;
    }
    pacing_last_ms = now_ms;
    if (pacing_tokens > token_cap) {
        pacing_tokens = token_cap;
    }

    int budget = (int)pacing_tokens;
    if (budget < 1) {
        return 1;
    }
    if (budget > MAX_STORED_PACKETS_FORWARD_PER_TICK) {
        budget = MAX_STORED_PACKETS_FORWARD_PER_TICK;
    }

    int number_of_stored_packages = dtn_storage_count(storage);

    Stored_Packet_Entry entries[MAX_STORED_PACKETS_FORWARD_PER_TICK];
    int n = dtn_storage_get_ready_entries(storage, now_sec, entries, budget);

    DTN_INFO("%d packekts stored / %d packets ready for forwarding at %f (budget %d)", number_of_stored_packages, n, now_sec, budget);

    int forwarded = 0;  // packets actually sent this tick; charged against pacing_tokens below
    for (int i = 0; i < n; i++) {
        Stored_Packet_Entry* entry = &entries[i];
        struct pbuf* p = entry->p;

        struct ip6_hdr* ip6hdr = (struct ip6_hdr*)p->payload;

        char src_str[IP6ADDR_STRLEN_MAX], dest_str[IP6ADDR_STRLEN_MAX], custodian_str[IP6ADDR_STRLEN_MAX];
        ip6_addr_t src_addr, dest_addr, custodian_addr;
        ip6_addr_copy_from_packed(src_addr, ip6hdr->src);
        ip6_addr_copy_from_packed(dest_addr, ip6hdr->dest);
        ip6addr_ntoa_r(&src_addr, src_str, sizeof(src_str));
        ip6addr_ntoa_r(&dest_addr, dest_str, sizeof(dest_str));

        bool has_custodian = dtn_extract_custodian_option(p, &custodian_addr);
        if (has_custodian) {
            ip6addr_ntoa_r(&custodian_addr, custodian_str, sizeof(custodian_str));
        } else {
            strncpy(custodian_str, "NONE", sizeof(custodian_str));
        }

        DTN_INFO("Packet|| src: %s -> dest: %s | custodian: %s || TRY to process STORED.", src_str, dest_str, custodian_str);

        bool is_next_hop_active;
        dtn_routing_is_next_hop_active(sys_now(), entry->routing_result, &is_next_hop_active);
        if (is_next_hop_active) {
            DTN_INFO("Packet|| src: %s -> dest: %s | custodian: %s || TRY to forward STORED.", src_str, dest_str, custodian_str);
            dtn_socket_result_t socket_result = dtn_controller_send(p, entry->routing_result);
            if (socket_result == DTN_SOCKET_OK) {
                stat_fwd_stored++;
                forwarded++;
                DTN_INFO("Packet|| src: %s -> dest: %s | custodian: %s || SUCCESSFUL forwarded STORED.", src_str, dest_str, custodian_str);
                if (IS_DTN_ICMPV6_SEND_MESSAGE_DISABLED) {
                    dtn_storage_delete_by_id(storage, (u32_t)entry->db_id);
                } else {
                    dtn_storage_increment_forward_attempts(storage, (u32_t)entry->db_id);
                }
                // Continue draining the rest of the ready batch (no break): every
                // entry's pbuf is freed at the end of the loop body below.
                if (has_custodian) {
                    dtn_icmpv6_send_message_result_t icmp_result = dtn_icmpv6_send_pck_forwarded(p, ICMP6_CODE_DTN_NO_INFO);
                    if (icmp_result == DTN_ICMPV6_SEND_MESSAGE_OK) {
                        DTN_INFO("Packet|| src: %s -> dest: %s | custodian: %s || SUCCESSFUL send ICMPV6_PCK_FORWARDED for STORED", src_str,
                                 dest_str, custodian_str);
                    }
                } else {
                    DTN_INFO("Packet|| src: %s -> dest: %s | custodian: %s || no custodian so no ICMPV6_PCK_FORWARDED for STORED.", src_str,
                             dest_str, custodian_str);
                }
            } else {
                DTN_ERROR("Packet|| src: %s -> dest: %s | custodian: %s || FAILED to forward STORED", src_str, dest_str, custodian_str);
            }
        } else {
            DTN_INFO("Packet|| src: %s -> dest: %s | custodian: %s || STORED NOT ACTIVE", src_str, dest_str, custodian_str);
            DtnRoutingResult routing_result;
            dtn_controller_process_outgoing_result_t result = dtn_controller_process_outgoing(p, &routing_result);
            switch (result) {
                case DTN_CONTROLLER_PROCESS_OUTGOING_OK:
                    DTN_WARN("Packet|| src: %s -> dest: %s | custodian: %s || UNSUPPORTED STATE", src_str, dest_str, custodian_str);
                    dtn_storage_delete_by_id(storage, (u32_t)entry->db_id);
                    break;
                case DTN_CONTROLLER_PROCESS_OUTGOING_STORE:
                    DTN_INFO("Packet|| src: %s -> dest: %s | custodian: %s || TRY to UPDATE STORED", src_str, dest_str, custodian_str);
                    dtn_storage_update_routing_result(storage, entry->db_id, &routing_result);
                    break;
                case DTN_CONTROLLER_PROCESS_OUTGOING_ERR:
                    DTN_ERROR("Packet|| src: %s -> dest: %s | custodian: %s || FAILED to UPDATE STORED", src_str, dest_str, custodian_str);
                    dtn_storage_delete_by_id(storage, (u32_t)entry->db_id);
                    break;
                default:
                    break;
            }
        }

        if (entry->p != NULL) {
            pbuf_free(entry->p);
            entry->p = NULL;
        }
    }

    // Charge the pacing budget only for packets actually put on the wire.
    pacing_tokens -= forwarded;

    // Leak insurance: every entry should already be freed above, but guarantee it
    // so any future early exit from the loop cannot leak the fetched pbufs.
    for (int i = 0; i < n; i++) {
        if (entries[i].p != NULL) {
            pbuf_free(entries[i].p);
            entries[i].p = NULL;
        }
    }

    dtn_storage_purge_exhausted(global_dtn_module->storage);

    // Tell the main loop to keep polling at the pacing tick while ready packets
    // remain (e.g. we were budget-limited, or more became ready meanwhile).
    return dtn_storage_any_ready_entries(storage, now_sec) ? 1 : 0;
}