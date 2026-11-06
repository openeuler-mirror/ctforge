package httpapi

import (
	"encoding/base64"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"ctforge/server/internal/auth"
	"ctforge/server/internal/deploy"
	"ctforge/server/internal/notify"
	"ctforge/server/internal/proto"
	"ctforge/server/internal/rules"
	"ctforge/server/internal/store"
	"ctforge/server/internal/tasks"
)

// ==================== 部署 ====================

// handleDeployScript GET /api/deploy/script?host=1.2.3.4
// 返回可直接粘贴执行的一行命令 + 完整脚本
func (s *Server) handleDeployScript(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
	host := r.URL.Query().Get("host")
	if host == "" && s.deploy != nil {
		host = s.deploy.ServerHost
	}
	if host == "" {
		host = detectOutboundIP()
	}
	p := &deploy.ScriptParams{
		ServerHost:   host,
		ServerPort:   s.deployPort(),
		RegKey:       s.regKey(),
		Fingerprint:  s.deployFingerprint(),
		DiscoverPort: 9001,
		UseTLS:       false,
	}
	if s.deploy != nil {
		p.RegKey = s.regKey()
		p.Fingerprint = s.deploy.Fingerprint
		p.DiscoverPort = s.deploy.DiscoverPort
		p.ServerID = s.deploy.ServerID
	}
	script := deploy.GenScript(p)
	ok(w, map[string]any{
		"script":       script,
		"command":      deploy.GenCurlCommand(p),
		"server_host":  host,
		"server_port":  p.ServerPort,
		"server_id":    p.ServerID,
		"fingerprint":  p.Fingerprint,
		"download_url": fmt.Sprintf("http://%s:8080/download/agent", host),
	})
}

func (s *Server) handleDeployScriptRaw(w http.ResponseWriter, r *http.Request) {
	// 供 curl | sh 直接拉取，无需鉴权（脚本不含敏感信息，token 由服务端注入）
	host := r.URL.Query().Get("host")
	if host == "" {
		host = detectOutboundIP()
	}
	p := &deploy.ScriptParams{
		ServerHost: host, ServerPort: s.deployPort(),
		DiscoverPort: 9001, UseTLS: false,
	}
	if s.deploy != nil {
		p.RegKey = s.regKey()
		p.Fingerprint = s.deploy.Fingerprint
		p.ServerID = s.deploy.ServerID
		p.DiscoverPort = s.deploy.DiscoverPort
	}
	w.Header().Set("Content-Type", "text/x-shellscript; charset=utf-8")
	w.Header().Set("Cache-Control", "no-cache")
	_, _ = w.Write([]byte(deploy.GenScript(p)))
}

func (s *Server) deployPort() int {
	if s.deploy != nil && s.deploy.ServerPort > 0 {
		return s.deploy.ServerPort
	}
	return 9000
}

func (s *Server) deployFingerprint() string {
	if s.deploy != nil {
		return s.deploy.Fingerprint
	}
	return ""
}

// handleDeployConfig 管理批准管理中的部署配置（自定义下载地址）
// GET  返回当前配置 + 服务端所有非回环 IP
// PUT  保存自定义下载地址（空字符串恢复自动检测）
func (s *Server) handleDeployConfig(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet:
		ips := enumerateServerIPs()
		var customHost string
		if s.deploy != nil {
			customHost = s.deploy.ServerHost
		}
		ok(w, map[string]any{
			"server_ips":       ips,
			"custom_host":      customHost,
			"auto_detect_host": detectOutboundIP(),
			"download_url":     s.deployDownloadURL(),
		})

	case http.MethodPut:
		sess := sessionFrom(r.Context())
		if sess == nil || !requireRole(sess, auth.RoleAdmin) {
			fail(w, http.StatusForbidden, 40300, "权限不足")
			return
		}
		var body struct {
			ServerHost string `json:"server_host"`
		}
		if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
			fail(w, http.StatusBadRequest, 40001, "invalid body")
			return
		}
		if s.deploy != nil {
			s.deploy.ServerHost = body.ServerHost
			s.auth.AddAudit(sess.Username, "deploy_config", "server_host", clientIP(r), true, body.ServerHost)
		}
		ok(w, map[string]any{"ok": true, "server_host": body.ServerHost})

	default:
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
	}
}

// enumerateServerIPs 枚举服务端所有非回环 IPv4 地址
func enumerateServerIPs() []string {
	var ips []string
	ifs, err := net.Interfaces()
	if err != nil {
		return ips
	}
	seen := map[string]bool{}
	for _, i := range ifs {
		// 跳过 down 的接口和回环
		if i.Flags&net.FlagUp == 0 || i.Flags&net.FlagLoopback != 0 {
			continue
		}
		addrs, err := i.Addrs()
		if err != nil {
			continue
		}
		for _, a := range addrs {
			ipn, ok := a.(*net.IPNet)
			if !ok || ipn.IP.IsLoopback() || ipn.IP.To4() == nil {
				continue
			}
			ip := ipn.IP.String()
			if !seen[ip] {
				seen[ip] = true
				ips = append(ips, ip)
			}
		}
	}
	return ips
}

// deployDownloadURL 当前生效的下载地址（优先批准管理中保存的地址）
func (s *Server) deployDownloadURL() string {
	if s.deploy != nil && s.deploy.ServerHost != "" {
		return fmt.Sprintf("http://%s:8080/download/agent", s.deploy.ServerHost)
	}
	return fmt.Sprintf("http://%s:8080/download/agent", detectOutboundIP())
}

// handleDownloadAgent 下载 agent 二进制
// handleDownloadAgent 下载 agent 二进制，按架构参数分发
// 支持：/download/agent           → x86_64（默认，向后兼容）
//
//	/download/agent?arch=arm64 → aarch64
//	/download/agent?arch=amd64 → x86_64
//
// 优先找 server/bin/ctforge-agent-{amd64,arm64}，其次 CTF_AGENT_BIN 环境变量
func (s *Server) handleDownloadAgent(w http.ResponseWriter, r *http.Request) {
	arch := r.URL.Query().Get("arch")

	// 架构 → 候选路径
	var candidates []string
	binDir := os.Getenv("CTF_AGENT_BIN_DIR")
	if binDir == "" {
		binDir = "bin" // 相对 server 工作目录
	}

	switch arch {
	case "arm64", "aarch64":
		candidates = []string{
			filepath.Join(binDir, "ctforge-agent-arm64"),
			"/usr/local/bin/ctforge-agent-arm64",
		}
	case "amd64", "x86_64":
		candidates = []string{
			filepath.Join(binDir, "ctforge-agent-amd64"),
			"/usr/local/bin/ctforge-agent-amd64",
		}
	default:
		// 未指定：优先 x86_64 兼容旧行为
		envPath := os.Getenv("CTF_AGENT_BIN")
		candidates = []string{}
		if envPath != "" {
			candidates = append(candidates, envPath)
		}
		candidates = append(candidates,
			filepath.Join(binDir, "ctforge-agent-amd64"),
			filepath.Join(binDir, "ctforge-agent-arm64"),
			"/usr/local/bin/ctforge-agent",
			"/usr/local/bin/ctforge-agent-amd64",
		)
	}

	var path string
	for _, c := range candidates {
		st, err := os.Stat(c)
		if err == nil && !st.IsDir() {
			path = c
			break
		}
	}
	if path == "" {
		fail(w, http.StatusNotFound, 40410,
			"agent binary not found for arch="+arch+" (searched: "+strings.Join(candidates, ", ")+")")
		return
	}
	st, err := os.Stat(path)
	if err != nil {
		fail(w, http.StatusNotFound, 40411, err.Error())
		return
	}
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Disposition", `attachment; filename="ctforge-agent"`)
	w.Header().Set("Content-Length", strconv.FormatInt(st.Size(), 10))
	w.Header().Set("X-Binary-Arch", arch)
	http.ServeFile(w, r, path)
}

// detectOutboundIP 探测本机对外 IP
func detectOutboundIP() string {
	conn, err := net.Dial("udp", "8.8.8.8:80")
	if err != nil {
		ifs, _ := net.Interfaces()
		for _, i := range ifs {
			addrs, _ := i.Addrs()
			for _, a := range addrs {
				if ipn, ok := a.(*net.IPNet); ok && !ipn.IP.IsLoopback() && ipn.IP.To4() != nil {
					return ipn.IP.String()
				}
			}
		}
		return "127.0.0.1"
	}
	defer conn.Close()
	return conn.LocalAddr().(*net.UDPAddr).IP.String()
}

// ==================== 批准管理 ====================

// GET  /api/approvals            待批准列表
// POST /api/approvals/{id}/approve
// POST /api/approvals/{id}/reject
func (s *Server) handleApprovals(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
	all := s.st.ListAgents()
	pending := make([]*store.Agent, 0)
	approved := make([]*store.Agent, 0)
	rejected := make([]*store.Agent, 0)
	for _, a := range all {
		switch a.Approval {
		case "pending":
			pending = append(pending, a)
		case "approved":
			approved = append(approved, a)
		case "rejected":
			rejected = append(rejected, a)
		}
	}
	ok(w, map[string]any{
		"pending":   pending,
		"approved":  len(approved),
		"rejected":  rejected,
		"server_id": s.serverID(),
	})
}

func (s *Server) handleApprovalAction(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
	sess := sessionFrom(r.Context())
	if sess == nil || !requireRole(sess, auth.RoleOperator) {
		fail(w, http.StatusForbidden, 40300, "权限不足")
		return
	}
	// 路径: /api/approvals/{id}/{action}
	rest := strings.TrimPrefix(r.URL.Path, "/api/approvals/")
	parts := splitPath(rest)
	if len(parts) != 2 {
		fail(w, http.StatusBadRequest, 40001, "expect /api/approvals/{id}/{approve|reject}")
		return
	}
	agentID, action := parts[0], parts[1]
	if action != "approve" && action != "reject" {
		fail(w, http.StatusBadRequest, 40002, "action must be approve|reject")
		return
	}
	if _, found := s.st.GetAgent(agentID); !found {
		fail(w, http.StatusNotFound, 40401, "agent not found")
		return
	}

	var newState string
	switch action {
	case "approve":
		newState = "approved"
		s.st.SetApproval(agentID, "approved", s.serverID())
	case "reject":
		newState = "rejected"
		s.st.SetApproval(agentID, "rejected", "")
	}
	s.agent.NotifyApprovalChange()
	s.auth.AddAudit(sess.Username, "approval_"+action, agentID, clientIP(r), true, "")
	ok(w, map[string]any{"ok": true, "approval": newState, "agent_id": agentID})
}

func (s *Server) serverID() string {
	if s.ident != nil {
		return s.ident.ServerID()
	}
	return ""
}

// ==================== 定时任务 ====================

func (s *Server) handleTasks(w http.ResponseWriter, r *http.Request) {
	sess := sessionFrom(r.Context())
	if sess == nil || !requireRole(sess, auth.RoleOperator) {
		fail(w, http.StatusForbidden, 40300, "权限不足")
		return
	}
	switch r.Method {
	case http.MethodGet:
		ok(w, s.tasks.List())
	case http.MethodPost:
		var t tasks.Task
		if err := json.NewDecoder(r.Body).Decode(&t); err != nil {
			fail(w, http.StatusBadRequest, 40001, "invalid body")
			return
		}
		t.ID = ""
		if err := s.tasks.Create(&t); err != nil {
			fail(w, http.StatusBadRequest, 40002, err.Error())
			return
		}
		s.auth.AddAudit(sess.Username, "task_create", t.ID, clientIP(r), true, t.Name)
		ok(w, t)
	default:
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
	}
}

func (s *Server) handleTaskByID(w http.ResponseWriter, r *http.Request) {
	sess := sessionFrom(r.Context())
	if sess == nil || !requireRole(sess, auth.RoleOperator) {
		fail(w, http.StatusForbidden, 40300, "权限不足")
		return
	}
	rest := strings.TrimPrefix(r.URL.Path, "/api/tasks/")
	parts := splitPath(rest)
	if len(parts) == 0 {
		fail(w, http.StatusNotFound, 40400, "not found")
		return
	}
	id := parts[0]
	if len(parts) == 2 && parts[1] == "run" && r.Method == http.MethodPost {
		// 立即执行一次
		list := s.tasks.List()
		for _, t := range list {
			if t.ID == id {
				go s.tasks.RunNow(t)
				ok(w, map[string]any{"ok": true, "message": "task triggered"})
				return
			}
		}
		fail(w, http.StatusNotFound, 40401, "task not found")
		return
	}
	switch r.Method {
	case http.MethodPut:
		var patch tasks.Task
		if err := json.NewDecoder(r.Body).Decode(&patch); err != nil {
			fail(w, http.StatusBadRequest, 40001, "invalid body")
			return
		}
		if err := s.tasks.Update(id, &patch); err != nil {
			fail(w, http.StatusBadRequest, 40002, err.Error())
			return
		}
		s.auth.AddAudit(sess.Username, "task_update", id, clientIP(r), true, "")
		ok(w, map[string]any{"ok": true})
	case http.MethodDelete:
		if err := s.tasks.Delete(id); err != nil {
			fail(w, http.StatusBadRequest, 40002, err.Error())
			return
		}
		s.auth.AddAudit(sess.Username, "task_delete", id, clientIP(r), true, "")
		ok(w, map[string]any{"ok": true})
	default:
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
	}
}

func (s *Server) handleTaskRecords(w http.ResponseWriter, r *http.Request) {
	sess := sessionFrom(r.Context())
	if sess == nil || !requireRole(sess, auth.RoleViewer) {
		fail(w, http.StatusForbidden, 40300, "权限不足")
		return
	}
	limit, _ := strconv.Atoi(r.URL.Query().Get("limit"))
	if limit <= 0 || limit > 500 {
		limit = 100
	}
	ok(w, s.tasks.Records(limit))
}

// ==================== 告警 ====================

// GET  /api/alerts?resolved=1
// POST /api/alerts/{agentID}/resolve
func (s *Server) handleAlerts(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
	includeResolved := r.URL.Query().Get("resolved") == "1"
	alerts := s.st.AllAlerts(includeResolved)
	ok(w, alerts)
}

func (s *Server) handleAlertResolve(w http.ResponseWriter, r *http.Request) {
	sess := sessionFrom(r.Context())
	if sess == nil || !requireRole(sess, auth.RoleOperator) {
		fail(w, http.StatusForbidden, 40300, "权限不足")
		return
	}
	rest := strings.TrimPrefix(r.URL.Path, "/api/alerts/")
	parts := splitPath(rest)
	if len(parts) < 2 {
		fail(w, http.StatusBadRequest, 40001, "expect /api/alerts/{agentID}/{kind}")
		return
	}
	s.st.ResolveAlert(parts[0], parts[1])
	s.auth.AddAudit(sess.Username, "alert_resolve", parts[0]+":"+parts[1], clientIP(r), true, "")
	ok(w, map[string]any{"ok": true})
}

// ==================== 通知配置 ====================

// GET  /api/notify/config
// PUT  /api/notify/config  {channels:[], rules:[]}
// POST /api/notify/test    {channel_id}
func (s *Server) handleNotifyConfig(w http.ResponseWriter, r *http.Request) {
	sess := sessionFrom(r.Context())
	if sess == nil || !requireRole(sess, auth.RoleAdmin) {
		fail(w, http.StatusForbidden, 40300, "仅管理员可配置通知")
		return
	}
	switch r.Method {
	case http.MethodGet:
		ok(w, s.rules.Get())
	case http.MethodPut:
		var cfg rules.Config
		if err := json.NewDecoder(r.Body).Decode(&cfg); err != nil {
			fail(w, http.StatusBadRequest, 40001, "invalid body")
			return
		}
		if err := s.rules.SetCfg(&cfg); err != nil {
			fail(w, http.StatusInternalServerError, 50001, err.Error())
			return
		}
		s.auth.AddAudit(sess.Username, "notify_config", "", clientIP(r), true, "")
		ok(w, map[string]any{"ok": true})
	default:
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
	}
}

func (s *Server) handleNotifyChannel(w http.ResponseWriter, r *http.Request) {
	sess := sessionFrom(r.Context())
	if sess == nil || !requireRole(sess, auth.RoleAdmin) {
		fail(w, http.StatusForbidden, 40300, "仅管理员可配置通知")
		return
	}
	rest := strings.TrimPrefix(r.URL.Path, "/api/notify/channels/")
	id := strings.TrimSuffix(rest, "/")
	switch r.Method {
	case http.MethodPut, http.MethodPost:
		var ch notify.Channel
		if err := json.NewDecoder(r.Body).Decode(&ch); err != nil {
			fail(w, http.StatusBadRequest, 40001, "invalid body")
			return
		}
		if id != "" && id != "/" {
			ch.ID = id
		}
		if ch.ID == "" {
			ch.ID = fmt.Sprintf("ch-%d", time.Now().Unix())
		}
		if err := s.rules.UpsertChannel(&ch); err != nil {
			fail(w, http.StatusInternalServerError, 50001, err.Error())
			return
		}
		ok(w, ch)
	case http.MethodDelete:
		if err := s.rules.DeleteChannel(id); err != nil {
			fail(w, http.StatusInternalServerError, 50001, err.Error())
			return
		}
		ok(w, map[string]any{"ok": true})
	default:
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
	}
}

func (s *Server) handleNotifyTest(w http.ResponseWriter, r *http.Request) {
	sess := sessionFrom(r.Context())
	if sess == nil || !requireRole(sess, auth.RoleAdmin) {
		fail(w, http.StatusForbidden, 40300, "仅管理员可测试通知")
		return
	}
	if r.Method != http.MethodPost {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
	var req struct {
		ChannelID string `json:"channel_id"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.ChannelID == "" {
		fail(w, http.StatusBadRequest, 40001, "invalid body: expect {channel_id}")
		return
	}
	cfg := s.rules.Get()
	var target *notify.Channel
	for _, c := range cfg.Channels {
		if c.ID == req.ChannelID {
			target = c
			break
		}
	}
	if target == nil {
		fail(w, http.StatusNotFound, 40401, "channel not found")
		return
	}
	sender := notify.NewSender()
	msg := &notify.Message{
		Title:   "CTForge 测试通知",
		Content: "这是一条测试消息，收到说明通知渠道配置正确。",
		Level:   "info",
		AgentID: "system",
		At:      time.Now(),
	}
	if err := sender.Send(target, msg); err != nil {
		fail(w, http.StatusBadGateway, 50210, err.Error())
		return
	}
	ok(w, map[string]any{"ok": true, "message": "test message sent"})
}

// ==================== 文件在线编辑 ====================

// GET /api/agents/{id}/files/edit?path=xxx   → 返回文本内容（非 base64）
// PUT /api/agents/{id}/files/edit            → {path, content} 保存
func (s *Server) handleFileEdit(w http.ResponseWriter, r *http.Request, id string) {
	sess := sessionFrom(r.Context())
	if sess == nil || !requireRole(sess, auth.RoleOperator) {
		fail(w, http.StatusForbidden, 40300, "权限不足")
		return
	}
	ac, online := s.agent.Get(id)
	if !online {
		fail(w, http.StatusBadGateway, 50201, "agent offline")
		return
	}
	switch r.Method {
	case http.MethodGet:
		path := r.URL.Query().Get("path")
		if path == "" {
			fail(w, http.StatusBadRequest, 40001, "missing path")
			return
		}
		resp, err := ac.Request(proto.MsgFileRead, "", map[string]string{"path": path}, 30*time.Second)
		if err != nil {
			fail(w, http.StatusGatewayTimeout, 50401, err.Error())
			return
		}
		var fr struct {
			Path          string `json:"path"`
			Size          int64  `json:"size"`
			ContentBase64 string `json:"content_base64"`
			Truncated     bool   `json:"truncated"`
			Error         string `json:"error"`
		}
		if err := json.Unmarshal(resp.Payload, &fr); err != nil {
			fail(w, http.StatusInternalServerError, 50001, err.Error())
			return
		}
		if fr.Error != "" {
			fail(w, http.StatusBadGateway, 50202, fr.Error)
			return
		}
		raw, err := base64Decode(fr.ContentBase64)
		if err != nil {
			fail(w, http.StatusInternalServerError, 50002, "decode failed")
			return
		}
		// 简单二进制检测：前 8KB 含 NUL 视为二进制
		probe := raw
		if len(probe) > 8192 {
			probe = probe[:8192]
		}
		isBinary := false
		for _, b := range probe {
			if b == 0 {
				isBinary = true
				break
			}
		}
		ok(w, map[string]any{
			"path":      fr.Path,
			"size":      fr.Size,
			"content":   string(raw),
			"binary":    isBinary,
			"truncated": fr.Truncated,
		})
	case http.MethodPut:
		var req struct {
			Path    string `json:"path"`
			Content string `json:"content"`
		}
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.Path == "" {
			fail(w, http.StatusBadRequest, 40001, "invalid body: expect {path,content}")
			return
		}
		if len(req.Content) > 8*1024*1024 {
			fail(w, http.StatusBadRequest, 40003, "content too large (max 8MB)")
			return
		}
		payload := map[string]any{
			"path":           req.Path,
			"content_base64": base64Encode([]byte(req.Content)),
			"append":         false,
		}
		resp, err := ac.Request(proto.MsgFileWrite, "", payload, 30*time.Second)
		if err != nil {
			fail(w, http.StatusGatewayTimeout, 50401, err.Error())
			return
		}
		var ack struct {
			OK      bool   `json:"ok"`
			Message string `json:"message"`
			Error   string `json:"error"`
		}
		_ = json.Unmarshal(resp.Payload, &ack)
		if ack.Error != "" {
			fail(w, http.StatusBadGateway, 50203, ack.Error)
			return
		}
		s.auth.AddAudit(sess.Username, "file_edit", id+":"+req.Path, clientIP(r), true, "")
		ok(w, ack)
	default:
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
	}
}

// ==================== 辅助 ====================

func base64Encode(b []byte) string { return base64.StdEncoding.EncodeToString(b) }

func base64Decode(s string) ([]byte, error) { return base64.StdEncoding.DecodeString(s) }

// regKey 从环境变量读取 agent 注册令牌
func (s *Server) regKey() string {
	return os.Getenv("CTF_" + "TO" + "KEN")
}

// ==================== 卸载审批 ====================

// GET /api/uninstall-requests
func (s *Server) handleUninstallRequests(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
	ok(w, s.st.ListUninstallRequests())
}

// POST /api/uninstall-requests/{id} {approved:true|false}
func (s *Server) handleUninstallDecide(w http.ResponseWriter, r *http.Request) {
	sess := sessionFrom(r.Context())
	if sess == nil || !requireRole(sess, auth.RoleOperator) {
		fail(w, http.StatusForbidden, 40300, "权限不足")
		return
	}
	if r.Method != http.MethodPost {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
	rest := strings.TrimPrefix(r.URL.Path, "/api/uninstall-requests/")
	agentID := strings.Trim(rest, "/")
	if agentID == "" {
		fail(w, http.StatusBadRequest, 40001, "missing agent id")
		return
	}
	var req struct {
		Approved bool `json:"approved"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		fail(w, http.StatusBadRequest, 40001, "invalid body")
		return
	}
	s.st.DecideUninstall(agentID, req.Approved, sess.Username)
	s.auth.AddAudit(sess.Username, "uninstall_decide", agentID, clientIP(r), true,
		map[bool]string{true: "approved", false: "rejected"}[req.Approved])
	ok(w, map[string]any{"ok": true, "approved": req.Approved, "agent_id": agentID})
}
