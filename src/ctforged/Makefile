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
                server.c collector.c ttl.c
OBJS         := $(SRCS:.c=.o)
CTFORGED_OBJS := ctforged.o config.o pool.o log.o tools.o payload.o \
                 netlink.o signature.o server.o collector.o ttl.o
PHONY        := default

default: build


PHONY += build
build: ctforge-signer ctforge-lsiggen ctforged ctforgectl


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


ctforge-signer: ctforge-signer.o config.o log.o tools.o signature.o
	$(CC) -o $@ $^ $(LDFLAGS)
	@echo "-> $@ build success"


ctforge-lsiggen: lsiggen.o config.o log.o tools.o signature.o
	$(CC) -o $@ $^ $(LDFLAGS)
	@echo "-> $@ build success"



ctforged: $(CTFORGED_OBJS) $(LIBBPFA)
	$(CC) -o $@ $(CTFORGED_OBJS) $(LDFLAGS) \
		-Wl,--whole-archive  $(LIBBPFA) -Wl,--no-whole-archive \
		-lelf -lz -lpthread -ldl -lssl -lcrypto -llzma -luv -ljson-c \
		-Wl,--export-dynamic
	@echo "-> $@ build success"


%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@


PHONY += install
install:
	install -d $(DESTDIR)/etc/ctforge
	install -m 644 ctforge.cfg $(DESTDIR)/etc/ctforge/ctforge.cfg
	@ls -alh $(DESTDIR)/etc/ctforge/ctforge.cfg
	install -m 600 $(PRIVATE_KEY) $(DESTDIR)/etc/ctforge/private.pem
	install -m 600 $(PUBLIC_KEY) $(DESTDIR)/etc/ctforge/public.pem
	install -d $(DESTDIR)/usr/sbin/
	install -m 755 ctforged $(DESTDIR)/usr/sbin/ctforged
	@ls -alh $(DESTDIR)/usr/sbin/ctforged
	install -m 755 ctforgectl $(DESTDIR)/usr/sbin/ctforgectl
	@ls -alh $(DESTDIR)/usr/sbin/ctforgectl
	install -m 755 ctforge-signer $(DESTDIR)/usr/sbin/ctforge-signer
	@ls -alh $(DESTDIR)/usr/sbin/ctforge-signer
	install -m 755 ctforge-lsiggen $(DESTDIR)/usr/sbin/ctforge-lsiggen
	@ls -alh $(DESTDIR)/usr/sbin/ctforge-lsiggen
	install -d $(DESTDIR)/etc/systemd/system
	install -m 644 ctforged.service $(DESTDIR)/etc/systemd/system/ctforged.service
	@ls -alh $(DESTDIR)/etc/systemd/system/ctforged.service

	@echo "-> Install CTForge success"


PHONY += uninstall
uninstall:
	[ -f $(DESTDIR)/etc/ctforge/ctforge.cfg ]             && rm -f $(DESTDIR)/etc/ctforge/ctforge.cfg             || :
	[ -f $(DESTDIR)/usr/sbin/ctforged ]                   && rm -f $(DESTDIR)/usr/sbin/ctforged                   || :
	[ -f $(DESTDIR)/usr/sbin/ctforgectl ]                 && rm -f $(DESTDIR)/usr/sbin/ctforgectl                 || :
	[ -f $(DESTDIR)/usr/sbin/ctforge-signer ]             && rm -f $(DESTDIR)/usr/sbin/ctforge-signer             || :
	[ -f $(DESTDIR)/usr/sbin/ctforge-lsiggen ]            && rm -f $(DESTDIR)/usr/sbin/ctforge-lsiggen            || :
	[ -f $(DESTDIR)/etc/systemd/system/ctforged.service ] && rm -f $(DESTDIR)/etc/systemd/system/ctforged.service || :
	[ -f $(DESTDIR)/etc/ctforge/security.key ]            && rm -f $(DESTDIR)/etc/ctforge/security.key            || :
	[ -f $(DESTDIR)/etc/ctforge/public.pem ]       && rm -f $(DESTDIR)/etc/ctforge/public.pem       || :
	systemctl disable --now ctforged.service  || :
	systemctl daemon-reload                   || :
	@echo "-> Uninstall CTForge success"


PHONY += clean
clean:
	rm -f $(OBJS) ctforgectl ctforge-signer ctforge-lsiggen ctforged


.PHONY: $(PHONY)

run:
	make clean && rm -rf /usr/share/ctforge/ctforge.socket  && make && make install && ./ctforged

rebuild:
	make clean && make



