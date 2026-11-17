# ctforge 服务端镜像（多阶段构建：前端 → 服务端 → agent → 运行镜像）
# 目标：docker compose up -d --build 后开箱即用，无需宿主机预编译

# ---------- 前端 ----------
FROM node:22-alpine AS frontend
WORKDIR /build
COPY src/frontend/package*.json ./
RUN npm ci --no-audit --no-fund || npm install --no-audit --no-fund
COPY src/frontend/ ./
RUN npm run build

# ---------- 服务端 ----------
FROM golang:1.26-alpine AS backend
WORKDIR /build
RUN apk add --no-cache git ca-certificates
COPY src/server/go.mod src/server/go.sum ./
RUN go mod download
COPY src/server/ ./
RUN CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /out/ctforge-server ./cmd/server

# ---------- x86_64 agent（在 alpine 里静态编译，产物零依赖） ----------
FROM alpine:3.20 AS agent-builder
RUN apk add --no-cache gcc musl-dev openssl-dev openssl-libs-static zlib-static make
WORKDIR /build
COPY src/clients/ ./clients/
# 直接用 gcc 编静态 agent（alpine 的 openssl-libs-static 提供 .a）
# 关键：-lssl -lcrypto 要放在 -static 后面，且加 -lz 因为 libcrypto.a 依赖它
RUN set -eux; \
    cd clients/src; \
    for f in *.c; do \
        gcc -O2 -Wall -std=c11 -D_GNU_SOURCE -I../include -I/usr/include \
            -c "$f" -o "${f%.c}.o"; \
    done; \
    cd ..; \
    gcc -static -pthread -lutil -o ctforge-agent \
        src/*.o -lssl -lcrypto -lz -ldl; \
    ls -la ctforge-agent; \
    ( ldd ctforge-agent || true ) 2>&1 | head -3

# ---------- 运行镜像 ----------
FROM alpine:3.20
RUN apk add --no-cache ca-certificates tzdata wget && \
    addgroup -S ctforge && adduser -S -G ctforge -h /var/lib/ctforge ctforge && \
    mkdir -p /var/lib/ctforge /app/static /etc/ctforge /app/bin && \
    chown -R ctforge:ctforge /var/lib/ctforge /etc/ctforge
WORKDIR /app

# 拷贝产物
COPY --from=backend  /out/ctforge-server            /usr/local/bin/ctforge-server
COPY --from=frontend /build/dist                    /app/static
COPY --from=agent-builder /build/clients/ctforge-agent /app/bin/ctforge-agent-amd64

# 软链，让 /download/agent 的默认查找路径（bin/ctforge-agent-amd64）能命中
RUN ln -sf /app/bin/ctforge-agent-amd64 /app/bin/ctforge-agent

ENV TZ=Asia/Shanghai \
    CTF_HTTP=:8080 \
    CTF_HTTPS=:9443 \
    CTF_SOCK=:9000 \
    CTF_STATIC=/app/static \
    CTF_ACCOUNTS=/var/lib/ctforge/accounts.json \
    CTF_DATA=/var/lib/ctforge \
    CTF_TLS_CERT=/etc/ctforge/server.crt \
    CTF_TLS_KEY=/etc/ctforge/server.key \
    CTF_AGENT_BIN_DIR=/app/bin

EXPOSE 8080/tcp 9443/tcp 9000/tcp 9001/udp 2222/tcp
VOLUME ["/var/lib/ctforge", "/etc/ctforge"]
USER ctforge

HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD wget -qO- http://127.0.0.1:8080/api/health >/dev/null 2>&1 || exit 1

ENTRYPOINT ["/usr/local/bin/ctforge-server"]
