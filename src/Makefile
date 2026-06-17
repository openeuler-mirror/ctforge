ARCH  = $(shell uname -m)
PHONY := default

default: help

PHONY += help
help:
	@echo "all            build all target"
	@echo "install        install all target"
	@echo "uninstall      remove all target"

PHONY += all
all:
	make -C ctforged
	make -C payload
	@echo "-> All ok! All success!"

PHONY += clean
clean:
	make -C ctforged clean
	@echo "-> clean ctforged success!"
	make -C payload clean
	@echo "-> clean payload success!"
	@echo "-> All ok! All success!"

PHONY += install
install:
	make -C ctforged install DESTDIR=${DESTDIR}
	make -C payload install DESTDIR=${DESTDIR}
	@echo "-> All ok! All success!"

PHONY += uninstall
uninstall:
	make -C ctforged uninstall DESTDIR=${DESTDIR}
	make -C payload uninstall DESTDIR=${DESTDIR}
	@echo "-> All ok! All success!"

.PHONY: $(PHONY)

