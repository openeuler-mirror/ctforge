SUBDIRS_WITH_SLASH := $(sort $(dir $(wildcard */.)))
MODULES := $(basename $(SUBDIRS_WITH_SLASH))

LIBBPF := ../libbpf/libbpf.a

.PHONY: default payload install uninstall clean $(MODULES)

default: payload

$(LIBBPF):
	@cd ../libbpf && $(MAKE)

payload: $(LIBBPF)
	+@$(MAKE) -j$(shell nproc) -Oline --no-print-directory $(MODULES)
	@echo "-> All done! All ok!"

$(MODULES):
	+@$(MAKE) -C $@

clean:
	@for mod in $(MODULES); do \
		$(MAKE) -C $$mod clean; \
	done
	@echo "-> All clean done!"

install:
	@for mod in $(MODULES); do \
		$(MAKE) -C $$mod install; \
	done
	@echo "-> All install done!"

uninstall:
	@for mod in $(MODULES); do \
		$(MAKE) -C $$mod uninstall; \
	done
	@echo "-> All uninstall done!"

