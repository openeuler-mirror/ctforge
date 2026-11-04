# ctforge 服务端镜像（多阶段构建：前端 → 服务端 → 运行镜像）
FROM node:20-alpine AS frontend
WORKDIR /build
COPY frontend/package*.json ./
RUN npm ci --no-audit --no-fund || npm install --no-audit --no-fund
COPY frontend/ ./
RUN npm run build

FROM golang:1.22-alpine AS backend
WORKDIR /build
RUN apk add --no-cache git ca-certificates
COPY server/go.mod server/go.sum ./
RUN go mod download
COPY server/ ./
RUN CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /out/ctforge-server ./cmd/server

FROM alpine:3.20
RUN apk add --no-cache ca-certificates tzdata && \
    addgroup -S ctforge && adduser -S -G ctforge -h /var/lib/ctforge ctforge && \
    mkdir -p /var/lib/ctforge /app/static /etc/ctforge && \
    chown -R ctforge:ctforge /var/lib/ctforge /etc/ctforge
WORKDIR /app
COPY --from=backend /out/ctforge-server /usr/local/bin/ctforge-server
COPY --from=frontend /build/dist /app/static

ENV TZ=Asia/Shanghai \
    CTC_HTTP=:8080 \
    CTC_SOCK=:9000 \
    CTC_STATIC=/app/static \
    CTC_ACCOUNTS=/var/lib/ctforge/accounts.json \
    CTC_TLS_CERT=/etc/ctforge/server.crt \
    CTC_TLS_KEY=/etc/ctforge/server.key

EXPOSE 8080/tcp 9000/tcp 9001/udp
VOLUME ["/var/lib/ctforge", "/etc/ctforge"]
USER ctforge

HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD wget -qO- http://127.0.0.1:8080/api/health >/dev/null 2>&1 || exit 1

ENTRYPOINT ["/usr/local/bin/ctforge-server"]
