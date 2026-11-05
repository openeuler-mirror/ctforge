CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Wformat=2 -std=c11 -D_GNU_SOURCE

# 静态 OpenSSL（构建时用 ./build-static.sh 一键编译）
OPENSSL_STATIC ?= /tmp/openssl-3.0.15
OSSL_INC   := $(OPENSSL_STATIC)/include
OSSL_LIBS  := $(OPENSSL_STATIC)/libssl.a $(OPENSSL_STATIC)/libcrypto.a

# 完全静态链接（零依赖单文件）；失败可降级为半静态（仅静态 OpenSSL）
STATIC_ALL ?= 1
ifeq ($(STATIC_ALL),1)
LDFLAGS ?= -static -pthread -lutil $(OSSL_LIBS) -ldl
else
LDFLAGS ?= -pthread -lutil -Wl,-Bstatic $(OSSL_LIBS) -Wl,-Bdynamic -ldl
endif

CFLAGS += -I$(OSSL_INC)
PREFIX  ?= /usr/local

SRC := src/agent.c src/net.c src/util.c src/metrics.c src/config.c src/terminal.c src/files.c src/sysadmin.c src/sysops.c src/binding.c src/anomaly.c src/hardware.c src/portforward.c src/tls.c src/discover.c src/uninstall.c
OBJ := $(SRC:.c=.o)

all: ctforge-agent

ctforge-agent: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -Iinclude -c -o $@ $<

install: ctforge-agent
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m755 ctforge-agent $(DESTDIR)$(PREFIX)/bin/
	install -d $(DESTDIR)/etc
	install -m644 ctforge-agent.conf.example $(DESTDIR)/etc/ctforge-agent.conf || true

clean:
	rm -f $(OBJ) ctforge-agent

.PHONY: all install clean
