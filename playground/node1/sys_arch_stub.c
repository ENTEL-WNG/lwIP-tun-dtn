/* sys_arch_stub.c
   Minimal NO_SYS=1 sys_arch implementation.
   LwIP only needs sys_now() when running without an OS. */
#include "lwip/sys.h"
#include <time.h>

u32_t sys_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}