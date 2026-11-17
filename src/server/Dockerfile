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

# ---------- ctforged + ctforgectl（eBPF 守护进程，容器内静态编译） ----------
# 静态链接：下载到 glibc 目标主机（Fedora/Ubuntu/OpenWrt）可直接运行，不依赖 musl
FROM alpine:3.20 AS ctforged-builder
RUN apk add --no-cache clang make musl-dev openssl linux-headers \
        libbpf-dev elfutils-dev zlib-static zstd-static \
        libuv-dev libuv-static json-c-dev \
        openssl-dev openssl-libs-static xz-static
WORKDIR /build
# 拷入关键源文件（key 头文件缺失时由 user_sm2_test 现场生成）
COPY src/ctforged/ ./ctforged/
COPY src/ctforgectl/ ./ctforgectl/
COPY src/key/ ./key/
# 在容器里现场生成 ECDSA key + sm2_signature.h，再静态编译 ctforged / ctforgectl
RUN set -eux; \
    cd key; \
    mkdir -p CTFORGED; \
    # 1) ctforged 运行时用的密钥对\
    openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 \
        -out CTFORGED/private.pem; \
    openssl pkey -in CTFORGED/private.pem -pubout -out CTFORGED/public.pem; \
    # 2) user_sm2_test 硬编码读取当前目录的这两个文件名（被 gitignore 排除，
    #    新克隆下不存在），必须在容器内现场生成\
    cp CTFORGED/private.pem sm2_private_key.pem; \
    cp CTFORGED/public.pem  sm2_public_key.pem; \
    gcc -O2 -std=c99 -o user_sm2_test user_sm2_test.c -lssl -lcrypto; \
    ./user_sm2_test; \
    cp sm2_signature.h CTFORGED/; \
    \
    cd ../ctforged; \
    clang -g -O0 -Wno-deprecated-declarations -I. -I/usr/include \
        -c ctforged.c config.c pool.c log.c tools.c payload.c netlink.c signature.c server.c error.c; \
    clang -static -o ctforged \
        ctforged.o config.o pool.o log.o tools.o payload.o netlink.o signature.o server.o error.o \
        -lbpf -lelf -lzstd -lz -lpthread -ldl -lssl -lcrypto -llzma -luv -ljson-c \
        -Wl,--export-dynamic; \
    \
    cd ../ctforgectl; \
    clang -g -O0 -Wno-deprecated-declarations -I. -I../ctforged -I/usr/include \
        -c ctforgectl.c ../ctforged/config.c ../ctforged/log.c ../ctforged/tools.c; \
    clang -static -o ctforgectl \
        ctforgectl.o config.o log.o tools.o \
        -lbpf -lelf -lzstd -lz -lpthread -ldl -lssl -lcrypto -llzma -luv -ljson-c; \
    \
    echo "== 静态性验证 =="; \
    ( ldd ../ctforged/ctforged || true ) 2>&1 | head -3; \
    ( ldd ctforgectl || true ) 2>&1 | head -3

# ---------- 运行镜像 ----------
FROM alpine:3.20
RUN apk add --no-cache ca-certificates tzdata wget && \
    addgroup -S ctforge && adduser -S -G ctforge -h /var/lib/ctforge ctforge && \
    mkdir -p /var/lib/ctforge /app/static /etc/ctforge /app/bin && \
    chown -R ctforge:ctforge /var/lib/ctforge /etc/ctforge
WORKDIR /app

# 拷贝产物
COPY --from=backend       /out/ctforge-server                 /usr/local/bin/ctforge-server
COPY --from=frontend      /build/dist                         /app/static
COPY --from=agent-builder /build/clients/ctforge-agent        /app/bin/ctforge-agent-amd64
COPY --from=ctforged-builder /build/ctforged/ctforged         /app/bin/ctforged-amd64
COPY --from=ctforged-builder /build/ctforgectl/ctforgectl     /app/bin/ctforgectl

# 软链，让 /download/agent 与 /download/ctforged 的默认查找路径命中
RUN ln -sf /app/bin/ctforge-agent-amd64 /app/bin/ctforge-agent \
 && ln -sf /app/bin/ctforged-amd64       /app/bin/ctforged

ENV TZ=Asia/Shanghai \
    CTF_HTTP=:8080 \
    CTF_HTTPS=:9443 \
    CTF_SOCK=:9000 \
    CTF_STATIC=/app/static \
    CTF_ACCOUNTS=/var/lib/ctforge/accounts.json \
    CTF_DATA=/var/lib/ctforge \
    CTF_TLS_CERT=/etc/ctforge/server.crt \
    CTF_TLS_KEY=/etc/ctforge/server.key \
    CTF_AGENT_BIN_DIR=/app/bin \
    CTF_CTFD_BIN_DIR=/app/bin

EXPOSE 8080/tcp 9443/tcp 9000/tcp 9001/udp 2222/tcp
VOLUME ["/var/lib/ctforge", "/etc/ctforge"]
USER ctforge

HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD wget -qO- http://127.0.0.1:8080/api/health >/dev/null 2>&1 || exit 1

ENTRYPOINT ["/usr/local/bin/ctforge-server"]
