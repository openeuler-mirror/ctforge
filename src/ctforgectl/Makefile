ARCH=$(shell uname -m)
BPF_SPIN_LOCK=$(shell grep '^struct bpf_spin_lock {' /usr/include/linux/bpf.h)
ifeq ($(BPF_SPIN_LOCK),)
EXTRA_FLAGS += -D__DEFINE_BPF_SPIN_LOCK__
endif

LIBBPFA      := ../libbpf/libbpf.a
PRIVATE_KEY := ../key/CTFORGED/private.pem
PUBLIC_KEY   := ../key/CTFORGED/public.pem
BPFTOOL      := /tmp/bpftool
BPFTOOLTGZ   := ../tools/$(ARCH)/bpftool.tar.gz
PHONY        := default
CC           := clang
CFLAGS       := -g -O0 -Wno-deprecated-declarations -I. $(EXTRA_FLAGS)
LDFLAGS      := -lelf -lz -lpthread -ldl -lssl -lcrypto -llzma -luv -ljson-c
SRCS         := ctforgectl.c ctforge-signer.c ctforged.c config.c \
                pool.c log.c tools.c payload.c netlink.c signature.c \
                server.c error.c
OBJS         := $(SRCS:.c=.o)
PHONY        := default

default: build


PHONY += build
build: ctforge-signer ctforged ctforgectl


$(LIBBPFA):
	@make -C ../libbpf libbpf.a
	@echo "-> libbpf build success"


$(BPFTOOL): $(BPFTOOLTGZ)
	@tar -xf $< -C /tmp/
	@chmod +x $@
	@echo "-> find bpftool"


ctforgectl: ctforgectl.o config.o log.o tools.o
	$(CC) -o $@ $^ $(LDFLAGS)
	@echo "-> $@ build success"


%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@


PHONY += install
install:
	install -m 755 ctforgectl $(DESTDIR)/usr/sbin/ctforgectl
	@ls -alh $(DESTDIR)/usr/sbin/ctforgectl
	@echo "-> Install CTForge success"


PHONY += uninstall
uninstall:
	[ -f $(DESTDIR)/usr/sbin/ctforgectl ]                 && rm -f $(DESTDIR)/usr/sbin/ctforgectl                 || :
	@echo "-> Uninstall CTForge success"


PHONY += clean
clean:
	rm -f $(OBJS) ctforgectl


.PHONY: $(PHONY)

run:
	make clean && rm -rf /usr/share/ctforge/ctforge.socket  && make && make install && ./ctforged

rebuild:
	make clean && make



