#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/ip6_addr.h"
#include "lwip/icmp6.h"
#include "lwip/timeouts.h"
#include "netif/tapif.h"   /* from lwip-contrib linux port */
#include <stdio.h>
#include <signal.h>
#include <unistd.h>

static struct netif tap_netif;
static volatile int running = 1;

static void sig_handler(int s) { (void)s; running = 0; }

int main(void)
{
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    lwip_init();

    /* Add TAP interface — tapif_init links it to /dev/tap0 */
    netif_add(&tap_netif,
              NULL, NULL, NULL,   /* no IPv4 */
              NULL,               /* state */
              tapif_init,
              netif_input);

    netif_set_default(&tap_netif);
    netif_set_up(&tap_netif);

    /* Assign fd00::1/64 to the LwIP interface */
    ip6_addr_t addr;
    ip6addr_aton("fd00::1", &addr);
    s8_t idx;
    netif_add_ip6_address(&tap_netif, &addr, &idx);
    netif_ip6_addr_set_state(&tap_netif, idx, IP6_ADDR_VALID);

    printf("[node1] LwIP up on fd00::1 — waiting for ping6...\n");

    /* LwIP handles ICMPv6 echo reply automatically once the
       interface is up — no manual handler needed for basic ping. */
    while (running) {
        /* Drive the TAP receive loop */
        tapif_poll(&tap_netif);
        sys_check_timeouts();
        usleep(1000);
    }

    printf("[node1] shutting down\n");
    return 0;
}