ROOT = ..

CC = gcc

PY_INCLUDES := $(shell python3-config --includes)
PY_LDFLAGS  := $(shell python3-config --ldflags --embed)

CFLAGS = -Wall \
	-DNO_SYS=1 \
	-I$(ROOT)/ \
	-I$(ROOT)/port/include \
	-I$(ROOT)/include \
	-I$(ROOT)/third_party/tomlc99 \
	-I$(ROOT)/third_party/sqlite \
	-I$(ROOT)/lwip/include \
	-I$(ROOT)/lwip/src/include \
	-I$(ROOT)/lwip/contrib/addons/ipv6_static_routing \
	$(PY_INCLUDES)

LDFLAGS = $(PY_LDFLAGS) -lpthread -lutil -lm

LWIP_SRC = \
	$(ROOT)/lwip/src/core/mem.c \
	$(ROOT)/lwip/src/core/memp.c \
	$(ROOT)/lwip/src/core/pbuf.c \
	$(ROOT)/lwip/src/core/timeouts.c \
	$(ROOT)/lwip/src/core/netif.c \
	$(ROOT)/lwip/src/core/init.c \
	$(ROOT)/lwip/src/core/stats.c \
	$(ROOT)/lwip/src/core/ip.c \
	$(ROOT)/lwip/src/core/def.c \
	$(ROOT)/lwip/src/core/ipv6/inet6.c \
	$(ROOT)/lwip/src/core/inet_chksum.c \
	$(ROOT)/lwip/src/core/ipv6/ip6.c \
	$(ROOT)/lwip/src/core/ipv6/icmp6.c \
	$(ROOT)/lwip/src/core/ipv6/ip6_addr.c \
	$(ROOT)/lwip/src/core/ipv6/nd6.c \
	$(ROOT)/lwip/src/core/ipv6/mld6.c \
	$(ROOT)/lwip/src/core/ipv6/ip6_frag.c \
	$(ROOT)/lwip/contrib/addons/ipv6_static_routing/ip6_route_table.c

APP_SRC = \
	$(ROOT)/src/dtn_config.c \
	$(ROOT)/src/dtn_module.c \
	$(ROOT)/src/dtn_controller.c \
	$(ROOT)/src/dtn_routing.c \
	$(ROOT)/src/dtn_icmpv6.c \
	$(ROOT)/src/raw_socket.c \
	$(ROOT)/src/dtn_storage.c \
	$(ROOT)/src/dtn_custody.c \
	$(ROOT)/src/dtn_utils.c \
	$(ROOT)/src/dtn_logger.c

THIRD_PARTY_SRC = \
    $(ROOT)/third_party/tomlc99/toml.c \
    $(ROOT)/third_party/sqlite/sqlite3.c

SOURCES = $(APP_SRC) $(THIRD_PARTY_SRC) $(ROOT)/port/sys_arch.c $(LWIP_SRC) test.c
OBJECTS = $(SOURCES:.c=.o)
TARGET  = lwip_tun_test

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJECTS) $(TARGET)
