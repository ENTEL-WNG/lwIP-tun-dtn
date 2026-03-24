CC = gcc

PY_INCLUDES := $(shell python3-config --includes)
PY_LDFLAGS  := $(shell python3-config --ldflags)

PY_LIBS := -lpython3.12 -lpthread -lutil

CFLAGS = -Wall \
	-DNO_SYS=1 \
	-I./ \
	-I./port/include \
	-I./include \
	-Ilwip/include \
	-Ilwip/src/include \
	-Ilwip/contrib/addons/ipv6_static_routing \
	$(PY_INCLUDES)

LDFLAGS = $(PY_LDFLAGS) $(PY_LIBS)

LWIP_SRC = \
	lwip/src/core/mem.c \
	lwip/src/core/memp.c \
	lwip/src/core/pbuf.c \
	lwip/src/core/timeouts.c \
	lwip/src/core/netif.c \
	lwip/src/core/init.c \
	lwip/src/core/stats.c \
	lwip/src/core/ip.c \
	lwip/src/core/def.c \
	lwip/src/core/ipv6/inet6.c \
	lwip/src/core/inet_chksum.c \
	lwip/src/core/ipv6/ip6.c \
	lwip/src/core/ipv6/icmp6.c \
	lwip/src/core/ipv6/ip6_addr.c \
	lwip/src/core/ipv6/nd6.c \
	lwip/src/core/ipv6/mld6.c \
	lwip/src/core/ipv6/ip6_frag.c \
	lwip/contrib/addons/ipv6_static_routing/ip6_route_table.c

APP_SRC = \
    src/main.c \
    src/dtn_module.c \
    src/dtn_controller.c \
    src/dtn_routing.c \
	src/dtn_icmpv6.c \
	src/raw_socket.c \
    src/dtn_storage.c \
	src/dtn_custody.c

SOURCES = $(APP_SRC) port/sys_arch.c $(LWIP_SRC)
OBJECTS = $(SOURCES:.c=.o)
TARGET = lwip_tun

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJECTS) $(TARGET)