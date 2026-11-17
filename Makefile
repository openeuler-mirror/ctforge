# ============================================================
# CTForge 顶层 Makefile
# 一键构建所有组件：ctforged, ctforgectl, payload, agent, server, frontend
# ============================================================

ARCH  := $(shell uname -m)
PHONY := default

default: help

PHONY += help
help:
	@echo "CTForge Build System"
	@echo ""
	@echo "  make all          build all components"
	@echo "  make ctforged     build eBPF daemon (requires clang, libbpf)"
	@echo "  make ctforgectl   build management CLI"
	@echo "  make payload      build eBPF payload modules (目前 payload/ 为空，保留占位)"
	@echo "  make agent        build C monitoring agent (requires gcc, openssl)"
	@echo "  make server       build Go server (requires Go 1.21+)"
	@echo "  make frontend     build Vue3 frontend (requires Node.js 18+)"
	@echo "  make install      install all binaries and configs"
	@echo "  make clean        clean all build artifacts"
	@echo "  make test         run all tests"
	@echo ""

# ============================================================
# eBPF 组件 (C + clang + libbpf)
# ============================================================

PHONY += ctforged
ctforged:
	@$(MAKE) -C src/ctforged

PHONY += ctforgectl
ctforgectl:
	@$(MAKE) -C src/ctforgectl

PHONY += payload
payload:
	@if [ -z "$$(ls -d src/payload/*/ 2>/dev/null)" ]; then \
		echo "⚠️  src/payload/ 下没有模块目录，跳过（本仓库未附带 payload 源码）"; \
	else \
		$(MAKE) -C src/payload; \
	fi

# ============================================================
# C Agent (gcc + openssl)
# ============================================================

PHONY += agent
agent:
	@$(MAKE) -C src/clients

# ============================================================
# Go Server
# ============================================================

PHONY += server
server:
	@cd src/server && CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o bin/ctforge-server ./cmd/server/

# ============================================================
# Vue3 Frontend (Node.js)
# ============================================================

PHONY += frontend
frontend:
	@cd src/frontend && npm install && npm run build

# ============================================================
# 聚合目标
# ============================================================

PHONY += all
all: ctforged agent server frontend
	@mkdir -p src/server/bin
	@cp -f src/ctforged/ctforged src/server/bin/ctforged-amd64
	@cp -f src/ctforged/ctforged src/server/bin/ctforged 2>/dev/null || true
	@cp -f src/clients/ctforge-agent src/server/bin/ctforge-agent-amd64 2>/dev/null || true
	@cp -f src/clients/ctforge-agent src/server/bin/ctforge-agent 2>/dev/null || true
	@echo ""
	@echo "✅ All CTForge components built successfully!"
	@echo "   - ctforged:    src/ctforged/ctforged"
	@echo "   - ctforgectl:  src/ctforgectl/ctforgectl"
	@echo "   - agent:       src/clients/ctforge-agent"
	@echo "   - server:      src/server/bin/ctforge-server"
	@echo "   - frontend:    src/frontend/dist/"
	@echo ""
	@echo "📦 Download endpoints ready (server/bin/):"
	@echo "   - /download/agent    ← ctforge-agent-amd64"
	@echo "   - /download/ctforged ← ctforged-amd64"

PHONY += install
install: ctforged ctforgectl agent server
	@$(MAKE) -C src/ctforged install DESTDIR=$(DESTDIR)
	@$(MAKE) -C src/ctforgectl install DESTDIR=$(DESTDIR)
	@$(MAKE) -C src/clients install DESTDIR=$(DESTDIR)
	@install -d $(DESTDIR)/usr/local/bin
	@install -m 755 src/server/bin/ctforge-server $(DESTDIR)/usr/local/bin/ctforge-server
	@echo "✅ Install complete!"

PHONY += clean
clean:
	@$(MAKE) -C src/ctforged clean
	@$(MAKE) -C src/ctforgectl clean
	@$(MAKE) -C src/clients clean
	@rm -rf src/server/bin/ctforge-server src/server/bin/ctforge-agent* src/server/bin/ctforged*
	@rm -rf src/frontend/dist
	@echo "✅ Clean complete!"

PHONY += test
test:
	@echo "Running Go server tests..."
	@cd src/server && go test -v ./...
	@echo ""
	@echo "Checking C agent syntax..."
	@for f in src/clients/src/*.c; do \
		echo "  $$f"; \
		gcc -fsyntax-only -I src/clients/include -D_GNU_SOURCE $$f || exit 1; \
	done
	@echo ""
	@echo "✅ All tests passed!"

.PHONY: $(PHONY)
