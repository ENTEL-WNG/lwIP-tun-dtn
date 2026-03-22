// main.c: Main entry point for the DTN-enabled IPv6 node implementation using LwIP and TUN interface
// Copyright (C) 2025 Michael Karpov
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>     
#include <fcntl.h>     
#include <unistd.h>     
#include <sys/ioctl.h> 
#include <linux/if.h>  
#include <linux/if_tun.h>
#include <sys/stat.h>
#include <errno.h>     
#include <sys/select.h> 
#include <sys/time.h>  
#include <stdbool.h>    

// LwIP headers
#include "lwip/init.h"      
#include "lwip/netif.h"     
#include "lwip/pbuf.h"     
#include "lwip/timeouts.h"  
#include "lwip/ip6.h"     
#include "lwip/ip6_addr.h"  
#include "lwip/ip.h"       
#include "lwip/sys.h"    
#include "lwip/err.h"    
#include "lwip/contrib/addons/ipv6_static_routing/ip6_route_table.h"
#include "dtn_module.h"
#include "dtn_controller.h" 
#include "dtn_routing.h"    
#include "dtn_icmpv6.h" 
#include "raw_socket.h"
#include "dtn_storage.h"

#define TUN_IFNAME "tun0"
#define PACKET_BUF_SIZE 2048
#define HOST_TUN_IPV6_ADDR "fd00::1"
#define HOST_LWIP_IPV6_ADDR "fd00::2"
#define HOST_enp0s9_IPV6_ADDR "fd00:01::2"
#define HOST_enp0s8_IPV6_ADDR "fd00:12::1"
#define CONTACT_CHECK_INTERVAL_MS 1000

DTN_Module* global_dtn_module = NULL;

int tun_alloc(char *dev_name, int max_len);
err_t tunif_output(struct netif *netif, struct pbuf *p);
err_t tunif_input(struct netif *netif);
err_t tunif_ip6_output(struct netif *netif, struct pbuf *p, const ip6_addr_t *ipaddr);
err_t tunif_init(struct netif *netif);

int tun_alloc(char *dev_name, int max_len) {
    struct ifreq ifr;
    int fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) { perror("Opening /dev/net/tun"); return fd; }
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    if (dev_name && *dev_name) { strncpy(ifr.ifr_name, dev_name, IFNAMSIZ); ifr.ifr_name[IFNAMSIZ - 1] = '\0'; }
    if (ioctl(fd, TUNSETIFF, (void *)&ifr) < 0) { perror("ioctl(TUNSETIFF)"); close(fd); return -1; }
    strncpy(dev_name, ifr.ifr_name, max_len); dev_name[max_len - 1] = '\0';
    printf("FILE DESCRIPTOR");
    return fd;
}

err_t tunif_output(struct netif *netif, struct pbuf *p) {
    if (!netif || !netif->state || !p) { return ERR_ARG; }
    int tun_fd = *(int *)netif->state; 
    char buffer[PACKET_BUF_SIZE];
    if (p->tot_len > sizeof(buffer)) { fprintf(stderr, "Packet too large for output buffer (%d vs %ld)\n", p->tot_len, sizeof(buffer)); return ERR_MEM; }

    if (pbuf_copy_partial(p, buffer, p->tot_len, 0) != p->tot_len) {
        fprintf(stderr, "pbuf_copy_partial failed to copy full packet\n");
        return ERR_BUF;
    }

    ssize_t written = write(tun_fd, buffer, p->tot_len);
    if (written < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) { return ERR_WOULDBLOCK; }
        perror("TUN write failed"); return ERR_IF;
    }
    if ((size_t)written != p->tot_len) { fprintf(stderr, "TUN write short: wrote %zd vs %d\n", written, p->tot_len); return ERR_IF; }
    return ERR_OK;
}

err_t tunif_input(struct netif *netif) {
     if (!netif || !netif->state) { return ERR_ARG; }
     if (!global_dtn_module || !global_dtn_module->controller) {
         fprintf(stderr, "tunif_input: DTN Module or Controller not initialized!\n");
         char discard_buf[100];
         read(*(int *)netif->state, discard_buf, sizeof(discard_buf));
         return ERR_IF;
     }

     int tun_fd = *(int *)netif->state;
     char buf[PACKET_BUF_SIZE]; 
     ssize_t len = read(tun_fd, buf, sizeof(buf));

     if (len < 0) { if (errno == EAGAIN || errno == EWOULDBLOCK) { return ERR_OK; } perror("TUN read error"); return ERR_IF; }
     if (len == 0) { printf("TUN read 0 bytes, tunnel closed by peer?\n"); return ERR_CONN; }

     struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
     if (!p) { fprintf(stderr, "Failed to allocate pbuf for incoming packet of size %zd\n", len); return ERR_MEM; }

     err_t copy_err = pbuf_take(p, buf, len);
     if (copy_err != ERR_OK) { fprintf(stderr, "Failed to copy buffer to pbuf (%d)\n", copy_err); pbuf_free(p); return copy_err; }

     dtn_controller_process_incoming(global_dtn_module->controller, p, netif);
     return ERR_OK;
}

err_t tunif_ip6_output(struct netif *netif, struct pbuf *p, const ip6_addr_t *ipaddr) {
    LWIP_UNUSED_ARG(ipaddr);
    return netif->linkoutput(netif, p);
}

err_t tunif_init(struct netif *netif) {
    if (!netif) { return ERR_ARG; }
    netif->name[0] = 't'; netif->name[1] = 'n';
    netif->output_ip6 = tunif_ip6_output;
    netif->linkoutput = tunif_output;
    netif->input = ip6_input;
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_UP | NETIF_FLAG_LINK_UP;
    return ERR_OK;
}

int main() {
    lwip_init();

    struct stat st = {0};
    if (stat(STORAGE_DIR, &st) == -1) {
        printf("Creating DTN storage directory: %s\n", STORAGE_DIR);
        if (mkdir(STORAGE_DIR, 0755) != 0) {
            fprintf(stderr, "Failed to create DTN storage directory: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }
    }
    
    global_dtn_module = dtn_module_init(); 
    if (!global_dtn_module) {
         fprintf(stderr, "Failed to initialize DTN Module\n");
         exit(EXIT_FAILURE);
    }

    struct netif tun_netif; 
    memset(&tun_netif, 0, sizeof(tun_netif));

    char tun_name[IFNAMSIZ]; 
    strncpy(tun_name, TUN_IFNAME, IFNAMSIZ -1); 
    tun_name[IFNAMSIZ - 1] = '\0';

    int tun_fd = tun_alloc(tun_name, sizeof(tun_name)); 
    if (tun_fd < 0) {
        fprintf(stderr, "TUN device allocation failed\n");
        dtn_module_cleanup(global_dtn_module);
        exit(EXIT_FAILURE);
    }
    printf("TUN device '%s' created successfully (fd: %d).\n", tun_name, tun_fd);

    if (raw_socket_init("enp0s8", "enp0s9") < 0) {
        fprintf(stderr, "Failed to initialize raw sockets\n");
        netif_remove(&tun_netif);
        close(tun_fd);
        dtn_module_cleanup(global_dtn_module);
        exit(EXIT_FAILURE);
    }

    int flags = fcntl(tun_fd, F_GETFL, 0);
    if (flags == -1) { perror("fcntl F_GETFL"); close(tun_fd); dtn_module_cleanup(global_dtn_module); exit(EXIT_FAILURE); }
    if (fcntl(tun_fd, F_SETFL, flags | O_NONBLOCK) == -1) { perror("fcntl F_SETFL O_NONBLOCK"); close(tun_fd); dtn_module_cleanup(global_dtn_module); exit(EXIT_FAILURE); }

    if (!netif_add(&tun_netif, (void*)&tun_fd, tunif_init, ip6_input)) { 
        fprintf(stderr, "Failed to add netif to lwIP\n");
        close(tun_fd);
        dtn_module_cleanup(global_dtn_module);
        exit(EXIT_FAILURE);
    }

    netif_set_default(&tun_netif);
    netif_set_up(&tun_netif); 
    printf("Interface '%s' (LwIP: %c%c) set UP and default.\n", tun_name, tun_netif.name[0], tun_netif.name[1]);

    netif_create_ip6_linklocal_address(&tun_netif, 1);
    printf("Link-local address creation requested for %c%c.\n", tun_netif.name[0], tun_netif.name[1]);

    printf("Current IPv6 addresses on %c%c after link-local creation attempt:\n", tun_netif.name[0], tun_netif.name[1]);
    for (int i = 0; i < LWIP_IPV6_NUM_ADDRESSES; ++i) {
        if (netif_ip6_addr_state(&tun_netif, i) != IP6_ADDR_INVALID) {
            char s[IP6ADDR_STRLEN_MAX]; 
            ip6addr_ntoa_r(netif_ip6_addr(&tun_netif, i), s, sizeof(s));
            printf("  Index %d: %s (State: %u %s)\n", i, s,
                netif_ip6_addr_state(&tun_netif, i),
                ip6_addr_ispreferred(netif_ip6_addr_state(&tun_netif, i)) ? "[Preferred]" :
                (ip6_addr_isvalid(netif_ip6_addr_state(&tun_netif, i)) ? "[Valid]" :
                (ip6_addr_istentative(netif_ip6_addr_state(&tun_netif, i))  ? "[Tentative]" : "[Other]")));

            if (ip6_addr_islinklocal(netif_ip6_addr(&tun_netif, i)) &&
                !ip6_addr_ispreferred(netif_ip6_addr_state(&tun_netif, i))) {
                netif_ip6_addr_set_state(&tun_netif, i, IP6_ADDR_PREFERRED);
                printf("    -> Set Link-Local (Index %d) to PREFERRED.\n", i);
            }
        }
    }

    ip6_addr_t ip6addr_lwip_stack;
    if (!ip6addr_aton(HOST_LWIP_IPV6_ADDR, &ip6addr_lwip_stack)) {
        fprintf(stderr, "FATAL: Failed to parse LwIP stack IPv6 address %s\n", HOST_LWIP_IPV6_ADDR);
        netif_remove(&tun_netif); 
        close(tun_fd);
        dtn_module_cleanup(global_dtn_module);
        exit(EXIT_FAILURE);
    }

    s8_t assigned_idx_global = -1; 
    err_t add_global_err = netif_add_ip6_address(&tun_netif, &ip6addr_lwip_stack, &assigned_idx_global);

    if (add_global_err == ERR_OK) {
        printf("LwIP stack address %s added successfully at index %d.\n", HOST_LWIP_IPV6_ADDR, assigned_idx_global);
        if (assigned_idx_global >= 0 && netif_ip6_addr_state(&tun_netif, assigned_idx_global) != IP6_ADDR_INVALID) {
            netif_ip6_addr_set_state(&tun_netif, assigned_idx_global, IP6_ADDR_PREFERRED);
            printf("  State for address %s (Index %d) set to PREFERRED.\n", HOST_LWIP_IPV6_ADDR, assigned_idx_global);
        } else {
            fprintf(stderr, "  WARNING: Address %s (Index %d) reported as added but state is invalid or index is negative.\n", HOST_LWIP_IPV6_ADDR, assigned_idx_global);
        }
    } else {
        fprintf(stderr, "WARNING: netif_add_ip6_address for %s failed with error code %d.\n", HOST_LWIP_IPV6_ADDR, (int)add_global_err);
        s8_t found_idx_after_fail = netif_get_ip6_addr_match(&tun_netif, &ip6addr_lwip_stack);
        if (found_idx_after_fail >= 0) {
            printf("  However, %s was found at Index %d after the reported failure.\n", HOST_LWIP_IPV6_ADDR, found_idx_after_fail);
            if (netif_ip6_addr_state(&tun_netif, found_idx_after_fail) != IP6_ADDR_INVALID &&
                !ip6_addr_ispreferred(netif_ip6_addr_state(&tun_netif, found_idx_after_fail))) {
                netif_ip6_addr_set_state(&tun_netif, found_idx_after_fail, IP6_ADDR_PREFERRED);
                printf("  State for %s (Index %d) set to PREFERRED.\n", HOST_LWIP_IPV6_ADDR, found_idx_after_fail);
            } else if (ip6_addr_ispreferred(netif_ip6_addr_state(&tun_netif, found_idx_after_fail))) {
                printf("  Address %s (Index %d) was already preferred.\n", HOST_LWIP_IPV6_ADDR, found_idx_after_fail);
            }
        } else {
            fprintf(stderr, "  And %s was NOT found by netif_get_ip6_addr_match after the reported failure.\n", HOST_LWIP_IPV6_ADDR);
        }
    }

    ip6_addr_t ip6addr_lwip_stack_enp0s9;
    if (!ip6addr_aton(HOST_enp0s9_IPV6_ADDR, &ip6addr_lwip_stack_enp0s9)) {
        fprintf(stderr, "FATAL: Failed to parse Node 1 IPv6 address %s\n", HOST_enp0s9_IPV6_ADDR);
    }

    s8_t assigned_idx_node_enp0s9 = -1; 
    err_t add_node_enp0s9_err = netif_add_ip6_address(&tun_netif, &ip6addr_lwip_stack_enp0s9, &assigned_idx_node_enp0s9);

    if (add_node_enp0s9_err == ERR_OK) {
        printf("LwIP stack address %s added successfully at index %d.\n", HOST_enp0s9_IPV6_ADDR, assigned_idx_node_enp0s9);
        if (assigned_idx_node_enp0s9 >= 0 && netif_ip6_addr_state(&tun_netif, assigned_idx_node_enp0s9) != IP6_ADDR_INVALID) {
            netif_ip6_addr_set_state(&tun_netif, assigned_idx_node_enp0s9, IP6_ADDR_PREFERRED);
            printf("  State for address %s (Index %d) set to PREFERRED.\n", HOST_enp0s9_IPV6_ADDR, assigned_idx_node_enp0s9);
        } else {
            fprintf(stderr, "  WARNING: Address %s (Index %d) reported as added but state is invalid.\n", HOST_enp0s9_IPV6_ADDR, assigned_idx_node_enp0s9);
        }
    } else {
        fprintf(stderr, "WARNING: netif_add_ip6_address for %s failed with error code %d.\n", HOST_enp0s9_IPV6_ADDR, (int)add_node_enp0s9_err);
        s8_t found_idx_enp0s9_after_fail = netif_get_ip6_addr_match(&tun_netif, &ip6addr_lwip_stack_enp0s9);
        if (found_idx_enp0s9_after_fail >= 0) {
            printf("  However, %s was found at Index %d after the reported failure.\n", HOST_enp0s9_IPV6_ADDR, found_idx_enp0s9_after_fail);
            if (netif_ip6_addr_state(&tun_netif, found_idx_enp0s9_after_fail) != IP6_ADDR_INVALID &&
                !ip6_addr_ispreferred(netif_ip6_addr_state(&tun_netif, found_idx_enp0s9_after_fail))) {
                netif_ip6_addr_set_state(&tun_netif, found_idx_enp0s9_after_fail, IP6_ADDR_PREFERRED);
                printf("  State for %s (Index %d) set to PREFERRED.\n", HOST_enp0s9_IPV6_ADDR, found_idx_enp0s9_after_fail);
            }
        } else {
            fprintf(stderr, "  And %s was NOT found by netif_get_ip6_addr_match after the reported failure.\n", HOST_enp0s9_IPV6_ADDR);
        }
    }

    ip6_addr_t ip6addr_lwip_stack_enp0s8;
    if (!ip6addr_aton(HOST_enp0s8_IPV6_ADDR, &ip6addr_lwip_stack_enp0s8)) {
        fprintf(stderr, "FATAL: Failed to parse Node IPv6 address %s (enp0s8)\n", HOST_enp0s8_IPV6_ADDR);
    }

    s8_t assigned_idx_node_enp0s8 = -1; 
    err_t add_node_enp0s8_err = netif_add_ip6_address(&tun_netif, &ip6addr_lwip_stack_enp0s8, &assigned_idx_node_enp0s8);

    if (add_node_enp0s8_err == ERR_OK) {
        printf("LwIP stack address %s added successfully at index %d.\n", HOST_enp0s8_IPV6_ADDR, assigned_idx_node_enp0s8);
        if (assigned_idx_node_enp0s8 >= 0 && netif_ip6_addr_state(&tun_netif, assigned_idx_node_enp0s8) != IP6_ADDR_INVALID) {
            netif_ip6_addr_set_state(&tun_netif, assigned_idx_node_enp0s8, IP6_ADDR_PREFERRED);
            printf("  State for address %s (Index %d) set to PREFERRED.\n", HOST_enp0s8_IPV6_ADDR, assigned_idx_node_enp0s8);
        } else {
            fprintf(stderr, "  WARNING: Address %s (Index %d) reported as added but state is invalid.\n", HOST_enp0s8_IPV6_ADDR, assigned_idx_node_enp0s8);
        }
    } else {
        fprintf(stderr, "WARNING: netif_add_ip6_address for %s failed with error code %d.\n", HOST_enp0s8_IPV6_ADDR, (int)add_node_enp0s8_err);
        s8_t found_idx_enp0s8_after_fail = netif_get_ip6_addr_match(&tun_netif, &ip6addr_lwip_stack_enp0s8);
        if (found_idx_enp0s8_after_fail >= 0) {
            printf("  However, %s was found at Index %d after the reported failure.\n", HOST_enp0s8_IPV6_ADDR, found_idx_enp0s8_after_fail);
            if (netif_ip6_addr_state(&tun_netif, found_idx_enp0s8_after_fail) != IP6_ADDR_INVALID &&
                !ip6_addr_ispreferred(netif_ip6_addr_state(&tun_netif, found_idx_enp0s8_after_fail))) {
                netif_ip6_addr_set_state(&tun_netif, found_idx_enp0s8_after_fail, IP6_ADDR_PREFERRED);
                printf("  State for %s (Index %d) set to PREFERRED.\n", HOST_enp0s8_IPV6_ADDR, found_idx_enp0s8_after_fail);
            }
        } else {
            fprintf(stderr, "  And %s was NOT found by netif_get_ip6_addr_match after the reported failure.\n", HOST_enp0s8_IPV6_ADDR);
        }
    }

    struct ip6_prefix default_prefix;
    ip6_addr_t gw_addr;
    s8_t route_idx;

    ip6_addr_set_any(&default_prefix.addr);
    default_prefix.prefix_len = 0;      

    if (ip6addr_aton(HOST_TUN_IPV6_ADDR, &gw_addr)) { 
        if (ip6_add_route_entry(&default_prefix, &tun_netif, &gw_addr, &route_idx) == ERR_OK) {
            printf("LwIP: Static default IPv6 route added via %s (index %d).\n", HOST_TUN_IPV6_ADDR, route_idx);
        } else {
            fprintf(stderr, "LwIP: Failed to add static default IPv6 route.\n");
        }
    } else {
        fprintf(stderr, "LwIP: Failed to parse gateway address %s for static route.\n", HOST_TUN_IPV6_ADDR);
    }

    printf("Waiting for addresses to settle...\n");
    sleep(2);

    printf("LwIP stack started. Interface %s (LwIP: %c%c) is up and configured.\n",
           tun_name, tun_netif.name[0], tun_netif.name[1]);

    printf("Entering main loop...\n");

    while (1) {
        fd_set readfds; 
        FD_ZERO(&readfds);
        FD_SET(tun_fd, &readfds);

        struct timeval tv; 
        u32_t lwip_timeout_ms = sys_timeouts_sleeptime();

        u32_t app_timeout_ms = CONTACT_CHECK_INTERVAL_MS;
        if (lwip_timeout_ms != SYS_TIMEOUTS_SLEEPTIME_INFINITE && lwip_timeout_ms < app_timeout_ms) {
            app_timeout_ms = lwip_timeout_ms;
        }

        tv.tv_sec = app_timeout_ms / 1000;
        tv.tv_usec = (app_timeout_ms % 1000) * 1000;

        int ret = select(tun_fd + 1, &readfds, NULL, NULL, &tv);

        if (ret < 0) { if (errno == EINTR) { continue; } perror("select error"); break; }

        sys_check_timeouts();

        if (FD_ISSET(tun_fd, &readfds)) {
            if (tunif_input(&tun_netif) == ERR_CONN) { fprintf(stderr, "TUN connection closed. Exiting.\n"); break; }
        }

        bool cont = false;
        if (global_dtn_module && global_dtn_module->routing) {
            cont = dtn_routing_update_contacts(global_dtn_module->routing);
        }
        
        if (global_dtn_module && global_dtn_module->controller && cont) {
             dtn_controller_attempt_forward_stored(global_dtn_module->controller, &tun_netif);
        }
    }

    if (global_dtn_module && global_dtn_module->routing) {
    }

    printf("Shutting down...\n");
    netif_set_down(&tun_netif);
    netif_remove(&tun_netif);
    close(tun_fd); 
    dtn_module_cleanup(global_dtn_module); 
    raw_socket_cleanup();

    printf("Shutdown complete.\n");
    return 0;
}
