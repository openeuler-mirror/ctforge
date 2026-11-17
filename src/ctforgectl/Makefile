ARCH=$(shell uname -m)
BPF_SPIN_LOCK=$(shell grep '^struct bpf_spin_lock {' /usr/include/linux/bpf.h)
ifeq ($(BPF_SPIN_LOCK),)
EXTRA_FLAGS += -D__DEFINE_BPF_SPIN_LOCK__
endif

PRIVATE_KEY := ../key/CTFORGED/private.pem
PUBLIC_KEY   := ../key/CTFORGED/public.pem
PHONY        := default
CC           := clang
# ctforgectl.c 与 ctforged 共用 ctforge.h 及其公共模块
CFLAGS       := -g -O0 -Wno-deprecated-declarations -I. -I../ctforged $(EXTRA_FLAGS)
LDFLAGS      := -lelf -lz -lpthread -ldl -lssl -lcrypto -llzma -luv -ljson-c
SRCS         := ctforgectl.c config.c log.c tools.c
OBJS         := $(SRCS:.c=.o)

default: build

PHONY += build
build: ctforgectl

ctforgectl: $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)
	@echo "-> $@ build success"

# ctforgectl.c 在本目录；config.c/log.c/tools.c 来自 ../ctforged
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: ../ctforged/%.c
	$(CC) $(CFLAGS) -c $< -o $@

PHONY += install
install: ctforgectl
	install -d $(DESTDIR)/usr/sbin/
	install -m 755 ctforgectl $(DESTDIR)/usr/sbin/ctforgectl
	@ls -alh $(DESTDIR)/usr/sbin/ctforgectl
	@echo "-> Install ctforgectl success"

PHONY += uninstall
uninstall:
	[ -f $(DESTDIR)/usr/sbin/ctforgectl ] && rm -f $(DESTDIR)/usr/sbin/ctforgectl || :
	@echo "-> Uninstall ctforgectl success"

PHONY += clean
clean:
	rm -f $(OBJS) ctforgectl

rebuild:
	$(MAKE) clean && $(MAKE)

.PHONY: $(PHONY)
