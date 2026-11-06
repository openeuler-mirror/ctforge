package main

import (
	"bufio"
	"context"
	"crypto/tls"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"strconv"
	"strings"
	"time"

	"ctforge/server/internal/agentconn"
	"ctforge/server/internal/ai"
	"ctforge/server/internal/auth"
	"ctforge/server/internal/deploy"
	"ctforge/server/internal/discover"
	"ctforge/server/internal/history"
	"ctforge/server/internal/httpapi"
	"ctforge/server/internal/identity"
	"ctforge/server/internal/monitor"
	"ctforge/server/internal/notify"
	"ctforge/server/internal/perms"
	"ctforge/server/internal/portfwd"
	"ctforge/server/internal/proto"
	"ctforge/server/internal/rules"
	"ctforge/server/internal/sshgateway"
	"ctforge/server/internal/store"
	"ctforge/server/internal/tasks"
	"ctforge/server/internal/tlscfg"
)

func main() {
	var (
		httpAddr  = flag.String("http", envOr("CTF_HTTP", ":8080"), "HTTP/WebSocket listen address")
		sockAddr  = flag.String("sock", envOr("CTF_SOCK", ":9000"), "agent socket listen address")
		token     = flag.String("token", envOr("CTF_TOKEN", ""), "shared token for agent register (empty = no auth)")
		staticDir = flag.String("static", envOr("CTF_STATIC", ""), "optional: dir of built frontend (dist) to serve")
		tlsCert   = flag.String("tls-cert", envOr("CTF_TLS_CERT", "server.crt"), "TLS cert file for agent channel (auto-generated if missing)")
		tlsKey    = flag.String("tls-key", envOr("CTF_TLS_KEY", "server.key"), "TLS key file for agent channel (auto-generated if missing)")
		tlsSAN    = flag.String("tls-san", envOr("CTF_TLS_SAN", ""), "extra SANs (comma separated host/IP) for auto-generated cert")
		noTLS     = flag.Bool("no-tls", envOr("CTF_NO_TLS", "") == "1", "disable TLS on agent channel (plaintext, dev only)")
		discPort  = flag.Int("discover", intEnvOr("CTF_DISCOVER", discover.DefaultPort), "UDP port for LAN agent discovery (0=disable)")
		sshPort   = flag.String("ssh", envOr("CTF_SSH", ":2222"), "SSH jump server listen address (empty=disable)")
	)
	stdAuthFile := flag.String("accounts", envOr("CTF_ACCOUNTS", "accounts.json"), "accounts file for web console users")
	dataDir := flag.String("data", envOr("CTF_DATA", "."), "data dir for server_id / tasks / rules")
	serverName := flag.String("name", envOr("CTF_NAME", ""), "server display name (default hostname)")
	agentBin := flag.String("agent-bin", envOr("CTF_AGENT_BIN", ""), "path to agent binary served at /download/agent")
	flag.Parse()

	st := store.New(*dataDir)
	agentSrv := agentconn.NewServer(st, *token)

	authMgr, err := auth.NewManager(*stdAuthFile)
	if err != nil {
		log.Fatalf("auth manager: %v", err)
	}
	log.Printf("[ctforge-server] accounts file: %s", *stdAuthFile)

	// 服务端身份
	ident, err := identity.NewManager(*dataDir+"/server_id", *serverName)
	if err != nil {
		log.Fatalf("identity: %v", err)
	}
	log.Printf("[ctforge-server] server_id: %s", ident.ServerID())

	// 定时任务
	tasksMgr, err := tasks.NewManager(*dataDir + "/tasks.json")
	if err != nil {
		log.Fatalf("tasks: %v", err)
	}
	tasksMgr.SetRunner(func(agentID, command string, timeoutSec int) (bool, int, string, string, int64, bool, error) {
		// "*" 表示全部已批准且在线的主机
		if agentID == "*" {
			agents := st.ListAgents()
			if len(agents) == 0 {
				return false, -1, "", "", 0, false, fmt.Errorf("no agents available")
			}
			var lastErr error
			ran := 0
			for _, a := range agents {
				if !a.Online || a.Approval != "approved" {
					continue
				}
				ac, ok := agentSrv.Get(a.AgentID)
				if !ok {
					continue
				}
				resp, err := ac.Request("task_run", "", map[string]any{
					"task_id": "adhoc", "command": command, "timeout_sec": timeoutSec,
				}, time.Duration(timeoutSec+5)*time.Second)
				if err != nil {
					lastErr = err
					continue
				}
				var r struct {
					OK       bool   `json:"ok"`
					ExitCode int    `json:"exit_code"`
					Stdout   string `json:"stdout"`
					Stderr   string `json:"stderr"`
					Duration int64  `json:"duration_ms"`
					TimedOut bool   `json:"timed_out"`
				}
				_ = json.Unmarshal(resp.Payload, &r)
				ran++
				if !r.OK {
					lastErr = fmt.Errorf("exit %d", r.ExitCode)
				}
			}
			if ran == 0 {
				if lastErr == nil {
					lastErr = fmt.Errorf("no online agents")
				}
				return false, -1, "", "", 0, false, lastErr
			}
			if lastErr != nil {
				return false, 1, "", "", 0, false, nil
			}
			return true, 0, fmt.Sprintf("executed on %d agent(s)", ran), "", 0, false, nil
		}
		ac, online := agentSrv.Get(agentID)
		if !online {
			return false, -1, "", "", 0, false, fmt.Errorf("agent offline: %s", agentID)
		}
		resp, err := ac.Request("task_run", "", map[string]any{
			"task_id": "adhoc", "command": command, "timeout_sec": timeoutSec,
		}, time.Duration(timeoutSec+5)*time.Second)
		if err != nil {
			return false, -1, "", "", 0, false, err
		}
		var r struct {
			OK       bool   `json:"ok"`
			ExitCode int    `json:"exit_code"`
			Stdout   string `json:"stdout"`
			Stderr   string `json:"stderr"`
			Duration int64  `json:"duration_ms"`
			TimedOut bool   `json:"timed_out"`
		}
		_ = json.Unmarshal(resp.Payload, &r)
		return r.OK, r.ExitCode, r.Stdout, r.Stderr, r.Duration, r.TimedOut, nil
	})
	tasksMgr.Start()

	// 批准状态持久化（重启后恢复）
	persister := store.NewPersister(*dataDir+"/approvals.json", st)
	if err := persister.Load(); err != nil {
		log.Printf("load approvals: %v", err)
	} else {
		log.Printf("[ctforge-server] approvals loaded from %s", *dataDir+"/approvals.json")
	}
	if err := persister.LoadUninstall(); err != nil {
		log.Printf("load uninstall requests: %v", err)
	}
	agentSrv.OnApprovalChange = func() {
		persister.SaveAsync()
		_ = persister.SaveUninstall()
	}

	// 告警规则 + 通知
	rulesMgr, err := rules.NewManager(*dataDir + "/rules.json")
	if err != nil {
		log.Fatalf("rules: %v", err)
	}
	sender := notify.NewSender()
	engine := monitor.NewEngine(st, rulesMgr, sender)
	engine.Start()

	// 权限体系（分组/授权/有效期）
	permsMgr, err := perms.NewManager(*dataDir + "/perms.json")
	if err != nil {
		log.Fatalf("perms: %v", err)
	}

	// 上下线历史
	histMgr, err := history.NewManager(*dataDir + "/history.json")
	if err != nil {
		log.Fatalf("history: %v", err)
	}

	// 端口转发管理
	pfMgr := portfwd.NewManager(agentSrv)

	// AI 分析配置
	aiCfg := &ai.Config{}
	aiCfgPath := *dataDir + "/ai.json"
	if data, err := os.ReadFile(aiCfgPath); err == nil {
		_ = json.Unmarshal(data, aiCfg)
	}
	aiAnalyzer := ai.NewAnalyzer(aiCfg)
	// AI 配置保存路径（httpapi 里用 os.WriteFile 直接落盘）
	_ = aiCfgPath

	// agentconn 回调：掉线告警 + 异常上报
	agentSrv.OnOffline = func(agentID string) {
		if ev := st.AddOfflineEvent(agentID); ev != nil {
			log.Printf("[ctforge-server] agent offline: %s (count=%d)", agentID, st.OfflineCountOf(agentID))
			histMgr.Record(&history.Event{
				AgentID: ev.AgentID, Hostname: ev.Hostname, Kind: "offline",
			})
		}
		pfMgr.CloseByAgent(agentID)
		httpapi.CleanupTermByAgent(agentID)
	}
	agentSrv.OnPFData = func(p *proto.PortForwardDataPayload) {
		pfMgr.OnData(p)
	}
	agentSrv.OnOnline = func(agentID, hostname, ip string, latency float64) {
		histMgr.Record(&history.Event{
			AgentID: agentID, Hostname: hostname, IP: ip, Kind: "online", LatencyMs: latency,
		})
		histMgr.CloseOfflineEvent(agentID)
	}
	agentSrv.OnAnomaly = func(agentID, kind, level, detail string) {
		a, _ := st.GetAgent(agentID)
		host := agentID
		if a != nil {
			host = a.Hostname
		}
		engine.ReportAnomaly(agentID, host, kind, level, detail)
	}

	// 端口 9000 协议嗅探：同端口同时支持 TLS 与明文（首字节 0x16 = TLS handshake）
	var agentListener net.Listener
	tlsFingerprint := ""
	var tlsCfg *tls.Config
	if !*noTLS {
		hosts := []string{}
		if *tlsSAN != "" {
			hosts = strings.Split(*tlsSAN, ",")
		}
		tlsCfg, tlsFingerprint, err = tlscfg.EnsureCert(*tlsCert, *tlsKey, hosts)
		if err != nil {
			log.Fatalf("tls setup: %v", err)
		}
		log.Printf("[ctforge-server] agent channel TLS cert: %s", *tlsCert)
		log.Printf("[ctforge-server] agent channel TLS fingerprint (sha256): %s", tlsFingerprint)
	}
	ln, lerr := net.Listen("tcp", *sockAddr)
	if lerr != nil {
		log.Fatalf("listen %s: %v", *sockAddr, lerr)
	}
	agentListener = &sniffListener{ln, tlsCfg}
	log.Printf("[ctforge-server] agent socket (TLS+PLAINTEXT sniff) listening on %s", *sockAddr)

	// 部署参数
	dep := &deploy.ScriptParams{
		ServerPort:   sockPort(*sockAddr),
		RegKey:       *token,
		DiscoverPort: *discPort,
		UseTLS:       !*noTLS,
		ServerID:     ident.ServerID(),
		Fingerprint:  tlsFingerprint, // 让安装脚本携带证书指纹，agent 才能 pinning
	}
	if *agentBin != "" {
		os.Setenv("CTF_AGENT_BIN", *agentBin)
	}

	api := httpapi.New(httpapi.Deps{
		Store: st, Agent: agentSrv, Auth: authMgr,
		Ident: ident, Tasks: tasksMgr, Rules: rulesMgr,
		Engine: engine, Deploy: dep,
		Perms: permsMgr, Hist: histMgr, PF: pfMgr,
		AI: aiAnalyzer, AICfg: aiCfg,
	})

	go func() {
		for {
			conn, err := agentListener.Accept()
			if err != nil {
				log.Printf("accept: %v", err)
				time.Sleep(time.Second)
				continue
			}
			go agentSrv.Handle(conn)
		}
	}()

	// 探测本机对外 IP（用于 scan 应答与握手返回）
	outboundIP := "127.0.0.1"
	if conn, err := net.Dial("udp", "8.8.8.8:80"); err == nil {
		outboundIP = conn.LocalAddr().(*net.UDPAddr).IP.String()
		conn.Close()
	}

	// agentconn 注入服务端身份（批准制必需）
	agentSrv.SetIdentity(ident.ServerID(), ident.ServerName(), outboundIP, sockPort(*sockAddr))

	// SSH 跳板机（JumpServer）：复用 web 账户 + agentconn 终端通道
	var sshGW *sshgateway.Server
	if *sshPort != "" {
		sshGW = sshgateway.New(st, authMgr, agentSrv)
		if err := sshGW.Start(*sshPort, *dataDir+"/ssh_host_rsa_key"); err != nil {
			log.Printf("[main] ssh gateway disabled: %v", err)
			sshGW = nil
		}
	}
	_ = sshGW

	// 局域网发现 + 扫描应答
	if *discPort > 0 {
		go func() {
			info := discover.Info{
				ServerID:   ident.ServerID(),
				ServerName: ident.ServerName(),
				Port:       sockPort(*sockAddr),
			}
			if err := discover.Listen(context.Background(), st, *discPort, info); err != nil {
				log.Printf("discover listen: %v", err)
			}
		}()
	}

	// 心跳延迟测量
	agentSrv.StartPingLoop(10 * time.Second)

	// http handler: /api + /ws + 可选静态站点
	root := http.NewServeMux()
	root.Handle("/api/", api.Handler())
	root.Handle("/ws/", api.Handler())
	root.Handle("/download/", api.Handler())
	if *staticDir != "" {
		fs := http.FileServer(http.Dir(*staticDir))
		root.Handle("/", spaHandler(*staticDir, fs))
		log.Printf("[ctforge-server] serving static from %s", *staticDir)
	} else {
		root.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
			w.WriteHeader(http.StatusOK)
			_, _ = w.Write([]byte("ctforge server is running. build frontend and pass -static to host UI."))
		})
	}

	httpSrv := &http.Server{
		Addr:              *httpAddr,
		Handler:           root,
		ReadHeaderTimeout: 10 * time.Second,
	}
	log.Printf("[ctforge-server] http listening on %s", *httpAddr)
	if err := httpSrv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		log.Fatal(err)
	}
}

func envOr(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}

// sockPort 从 ":9000" 形式的监听地址取端口号
func sockPort(addr string) int {
	if i := strings.LastIndex(addr, ":"); i >= 0 {
		if p, err := strconv.Atoi(addr[i+1:]); err == nil {
			return p
		}
	}
	return 9000
}

func intEnvOr(k string, def int) int {
	if v := os.Getenv(k); v != "" {
		var n int
		if _, err := fmt.Sscanf(v, "%d", &n); err == nil {
			return n
		}
	}
	return def
}

// spaHandler: 找不到的路径回落到 index.html，适配前端 history 路由。
// 缓存策略：assets 带内容 hash 用长缓存，index.html 用 no-cache 保证发版即生效。
func spaHandler(dir string, fs http.Handler) http.Handler {
	index := dir + "/index.html"
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		p := dir + r.URL.Path
		if st, err := os.Stat(p); err == nil && !st.IsDir() {
			if strings.HasPrefix(r.URL.Path, "/assets/") {
				w.Header().Set("Cache-Control", "public, max-age=31536000, immutable")
			} else {
				w.Header().Set("Cache-Control", "no-cache")
			}
			fs.ServeHTTP(w, r)
			return
		}
		w.Header().Set("Cache-Control", "no-cache")
		http.ServeFile(w, r, index)
	})
}

// sniffListener 同端口协议嗅探：Peek 首字节，0x16（TLS handshake）走 TLS，否则明文
type sniffListener struct {
	net.Listener
	tlsCfg *tls.Config
}

func (l *sniffListener) Accept() (net.Conn, error) {
	conn, err := l.Listener.Accept()
	if err != nil {
		return nil, err
	}
	if l.tlsCfg == nil {
		return conn, nil // -noTLS：纯明文
	}
	br := bufio.NewReader(conn)
	b, perr := br.Peek(1)
	if perr != nil {
		conn.Close()
		return nil, perr
	}
	if b[0] == 0x16 { // TLS handshake record
		return tls.Server(&peekConn{conn, br}, l.tlsCfg), nil
	}
	return &peekConn{conn, br}, nil
}

type peekConn struct {
	net.Conn
	r *bufio.Reader
}

func (c *peekConn) Read(b []byte) (int, error) { return c.r.Read(b) }

func (c *peekConn) Close() error { return c.Conn.Close() }
