// raw_socket.c (versió corregida amb una sola interfície)
// (Assumeixo raw_socket.h declara prototips i externs necessaris)

#include "raw_socket.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <linux/if_packet.h>
#include <netinet/ip6.h>
#include <errno.h>
#include "lwip/pbuf.h"
#include "lwip/ip6_addr.h"

// Interface index / name
static int if_index_1 = 0;  // enp0s8 (o enp8s0 segons el teu sistema)
static char if_name_1_global[IFNAMSIZ] = {0};

// Socket handle
int raw_socket_enp0s8 = -1;

int raw_socket_init(const char* if_name_1) {
    struct ifreq ifr;
    //int tmp_sock = -1;

    strncpy(if_name_1_global, if_name_1, IFNAMSIZ-1);
    //if_name_1_global[IFNAMSIZ-1] = '\0';

    raw_socket_enp0s8 = socket(AF_INET6, SOCK_RAW, IPPROTO_RAW);
    if (raw_socket_enp0s8 < 0) {
        perror("Failed to create raw socket for interface");
        return -1;
    }

    /* Per obtenir ifindex fem l'ioctl sobre un socket AF_INET (SOCK_DGRAM) temporal:
       és la pràctica recomanada i evita problemes d'especificitat de socket. */
    /*tmp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (tmp_sock < 0) {
        perror("Failed to create temporary socket for ioctl");
        close(raw_socket_enp0s8);
        return -1;
    }*/

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, if_name_1_global, IFNAMSIZ-1);
    if (ioctl(raw_socket_enp0s8, SIOCGIFINDEX, &ifr) < 0) {
        perror("Failed to get interface index for interface");
        //close(tmp_sock);
        close(raw_socket_enp0s8);
        return -1;
    }
    if_index_1 = ifr.ifr_ifindex;
    //close(tmp_sock);

    int on = 1;
    if (setsockopt(raw_socket_enp0s8, IPPROTO_IPV6, IPV6_HDRINCL, &on, sizeof(on)) < 0) {
        perror("Failed to set IPV6_HDRINCL option on socket");
        close(raw_socket_enp0s8);
        return -1;
    }

    printf("Raw socket initialized:\n");
    printf("  %s: socket %d, index %d\n", if_name_1_global, raw_socket_enp0s8, if_index_1);

    return 0;
}

/* Funció d'enviament: converteix ip6_addr_t (lwIP) a struct in6_addr i envia */
int raw_socket_send_ipv6(struct pbuf *p, const ip6_addr_t *dest_addr) {
    struct sockaddr_in6 sin6;
    int socket_to_use;
    int if_index_to_use;
    char *if_name_to_use;
    int sent_bytes;
    char buf[2048];

    if (p->tot_len > (int)sizeof(buf)) {
        fprintf(stderr, "Packet too large for raw socket buffer\n");
        return -1;
    }

    if (pbuf_copy_partial(p, buf, p->tot_len, 0) != p->tot_len) {
        fprintf(stderr, "Failed to copy pbuf data\n");
        return -1;
    }

    // If destination is in fd00:23::/64, use enp0s8
    int use_second_interface = 0;
    if (dest_addr->addr[0] == PP_HTONL(0xfd000023) &&
        dest_addr->addr[1] == 0 &&
        dest_addr->addr[2] == 0) {
        use_second_interface = 1;
    }
    
    if (use_second_interface) {
    } else {
        socket_to_use = raw_socket_enp0s8;
        if_index_to_use = if_index_1;
        if_name_to_use = if_name_1_global;
    }

    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;
    sin6.sin6_port = 0;
    sin6.sin6_flowinfo = 0;
    sin6.sin6_scope_id = if_index_to_use;

    memcpy(&sin6.sin6_addr, dest_addr, sizeof(struct in6_addr));

    if (setsockopt(socket_to_use, SOL_SOCKET, SO_BINDTODEVICE, 
                  if_name_to_use, strlen(if_name_to_use)) < 0) {
        perror("Failed to bind socket to interface");
        return -1;
    }

    sent_bytes = sendto(socket_to_use, buf, p->tot_len, 0,
                        (struct sockaddr *)&sin6, sizeof(sin6));

    if (sent_bytes < 0) {
        perror("Failed to send packet via raw socket");
        return -1;
    } else if ((size_t)sent_bytes != p->tot_len) {
        fprintf(stderr, "Sent only %d bytes out of %d\n", sent_bytes, p->tot_len);
        return -1;
    }
    
    char addr_str[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &sin6.sin6_addr, addr_str, sizeof(addr_str));

    return 0;
}

void raw_socket_cleanup(void) {
    if (raw_socket_enp0s8 >= 0) {
        close(raw_socket_enp0s8);
        raw_socket_enp0s8 = -1;
    }
    printf("Raw socket closed\n");
}
