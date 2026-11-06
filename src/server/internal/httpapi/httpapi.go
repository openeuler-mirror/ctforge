package httpapi

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"log"
	"net/http"
	"strconv"
	"strings"
	"time"

	"ctforge/server/internal/agentconn"
	"ctforge/server/internal/ai"
	"ctforge/server/internal/auth"
	"ctforge/server/internal/deploy"
	"ctforge/server/internal/history"
	"ctforge/server/internal/identity"
	"ctforge/server/internal/monitor"
	"ctforge/server/internal/perms"
	"ctforge/server/internal/portfwd"
	"ctforge/server/internal/proto"
	"ctforge/server/internal/rules"
	"ctforge/server/internal/store"
	"ctforge/server/internal/tasks"
)

type Server struct {
	st        *store.Store
	agent     *agentconn.Server
	auth      *auth.Manager
	mux       *http.ServeMux
	ident     *identity.Manager
	tasks     *tasks.Manager
	rules     *rules.Manager
	engine    *monitor.Engine
	deploy    *deploy.ScriptParams
	perms     *perms.Manager
	hist      *history.Manager
	pf        *portfwd.Manager
	aiCfg     *ai.Config
	ai        *ai.Analyzer
	aiCfgPath string
}

// Deps 服务端依赖注入
type Deps struct {
	Store     *store.Store
	Agent     *agentconn.Server
	Auth      *auth.Manager
	Ident     *identity.Manager
	Tasks     *tasks.Manager
	Rules     *rules.Manager
	Engine    *monitor.Engine
	Deploy    *deploy.ScriptParams
	Perms     *perms.Manager
	Hist      *history.Manager
	PF        *portfwd.Manager
	AI        *ai.Analyzer
	AICfg     *ai.Config
	AICfgPath string
}

func New(d Deps) *Server {
	s := &Server{
		st: d.Store, agent: d.Agent, auth: d.Auth,
		mux:   http.NewServeMux(),
		ident: d.Ident, tasks: d.Tasks, rules: d.Rules,
		engine: d.Engine, deploy: d.Deploy,
		perms: d.Perms, hist: d.Hist, pf: d.PF,
		aiCfg: d.AICfg, ai: d.AI, aiCfgPath: d.AICfgPath,
	}
	s.routes()
	return s
}

func (s *Server) Handler() http.Handler { return cors(s.mux) }

func cors(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Access-Control-Allow-Origin", "*")
		w.Header().Set("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS")
		w.Header().Set("Access-Control-Allow-Headers", "Content-Type, Authorization")
		if r.Method == http.MethodOptions {
			w.WriteHeader(http.StatusNoContent)
			return
		}
		next.ServeHTTP(w, r)
	})
}

func ok(w http.ResponseWriter, data any) {
	writeJSON(w, http.StatusOK, map[string]any{"code": 0, "data": data, "message": "success"})
}

func fail(w http.ResponseWriter, status int, code int, msg string) {
	writeJSON(w, status, map[string]any{"code": code, "message": msg})
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

// requireAuth 校验 Bearer token，返回会话；失败已写响应。
func (s *Server) requireAuth(w http.ResponseWriter, r *http.Request) *auth.Session {
	if s.auth == nil {
		return &auth.Session{Username: "anonymous", Role: auth.RoleAdmin}
	}
	h := r.Header.Get("Authorization")
	token := strings.TrimPrefix(h, "Bearer ")
	if token == "" {
		fail(w, http.StatusUnauthorized, 40100, "missing token")
		return nil
	}
	sess, valid := s.auth.Verify(token)
	if !valid {
		fail(w, http.StatusUnauthorized, 40101, "invalid or expired token")
		return nil
	}
	return sess
}

// requireRole 要求最低角色。viewer < operator < admin。
func requireRole(sess *auth.Session, min auth.Role) bool {
	order := map[auth.Role]int{auth.RoleViewer: 0, auth.RoleOperator: 1, auth.RoleAdmin: 2}
	return order[sess.Role] >= order[min]
}

func (s *Server) routes() {
	// 认证
	s.mux.HandleFunc("/api/auth/login", s.handleLogin)
	s.mux.HandleFunc("/api/auth/logout", s.handleLogout)
	s.mux.HandleFunc("/api/auth/me", s.handleMe)
	s.mux.HandleFunc("/api/auth/password", s.handleChangePassword)

	// 账户管理（admin）
	s.mux.HandleFunc("/api/accounts", s.withAuth(auth.RoleAdmin, s.handleAccounts))
	s.mux.HandleFunc("/api/accounts/", s.withAuth(auth.RoleAdmin, s.handleAccountByName))
	s.mux.HandleFunc("/api/audit", s.withAuth(auth.RoleAdmin, s.handleAudit))

	// 监控数据
	s.mux.HandleFunc("/api/health", func(w http.ResponseWriter, r *http.Request) {
		ok(w, map[string]any{"status": "up", "time": time.Now().Unix()})
	})
	s.mux.HandleFunc("/api/agents", s.withAuth(auth.RoleViewer, s.handleAgents))
	s.mux.HandleFunc("/api/agents/", s.withAuth(auth.RoleViewer, s.handleAgentByID))

	// 部署
	s.mux.HandleFunc("/api/deploy/script", s.withAuth(auth.RoleAdmin, s.handleDeployScript))
	s.mux.HandleFunc("/api/deploy/script.sh", s.handleDeployScriptRaw)
	s.mux.HandleFunc("/api/deploy/config", s.withAuth(auth.RoleAdmin, s.handleDeployConfig))
	s.mux.HandleFunc("/download/agent", s.handleDownloadAgent)

	// 批准管理
	s.mux.HandleFunc("/api/approvals", s.withAuth(auth.RoleViewer, s.handleApprovals))
	s.mux.HandleFunc("/api/approvals/", s.withAuth(auth.RoleOperator, s.handleApprovalAction))

	// 定时任务
	s.mux.HandleFunc("/api/tasks", s.withAuth(auth.RoleViewer, s.handleTasks))
	s.mux.HandleFunc("/api/tasks/records", s.withAuth(auth.RoleViewer, s.handleTaskRecords))
	s.mux.HandleFunc("/api/tasks/", s.withAuth(auth.RoleViewer, s.handleTaskByID))

	// 告警
	s.mux.HandleFunc("/api/alerts", s.withAuth(auth.RoleViewer, s.handleAlerts))
	s.mux.HandleFunc("/api/alerts/", s.withAuth(auth.RoleOperator, s.handleAlertResolve))

	// 卸载审批
	s.mux.HandleFunc("/api/uninstall-requests", s.withAuth(auth.RoleViewer, s.handleUninstallRequests))
	s.mux.HandleFunc("/api/uninstall-requests/", s.withAuth(auth.RoleOperator, s.handleUninstallDecide))

	// 通知配置
	s.mux.HandleFunc("/api/notify/config", s.withAuth(auth.RoleAdmin, s.handleNotifyConfig))
	s.mux.HandleFunc("/api/notify/channels/", s.withAuth(auth.RoleAdmin, s.handleNotifyChannel))
	s.mux.HandleFunc("/api/notify/test", s.withAuth(auth.RoleAdmin, s.handleNotifyTest))

	// 权限元信息 / 分组 / 授权
	s.mux.HandleFunc("/api/perms/meta", s.withAuth(auth.RoleViewer, s.handlePermsMeta))
	s.mux.HandleFunc("/api/groups", s.withAuth(auth.RoleViewer, s.handleGroups))
	s.mux.HandleFunc("/api/groups/", s.withAuth(auth.RoleAdmin, s.handleGroupByID))
	s.mux.HandleFunc("/api/grants", s.withAuth(auth.RoleAdmin, s.handleGrants))
	s.mux.HandleFunc("/api/grants/", s.withAuth(auth.RoleAdmin, s.handleGrantByUser))
	s.mux.HandleFunc("/api/me/grant", s.handleMyGrant)
	s.mux.HandleFunc("/api/me/ssh-keys", s.withAuth(auth.RoleViewer, s.handleMySSHKeys))

	// 上下线历史
	s.mux.HandleFunc("/api/history", s.withAuth(auth.RoleViewer, s.handleHistory))
	s.mux.HandleFunc("/api/history/stats", s.withAuth(auth.RoleViewer, s.handleHistoryStats))

	// 端口转发
	s.mux.HandleFunc("/api/portforwards", s.withAuth(auth.RoleOperator, s.handlePortForwards))
	s.mux.HandleFunc("/api/portforwards/", s.withAuth(auth.RoleOperator, s.handlePortForwardByID))

	// AI
	s.mux.HandleFunc("/api/ai/config", s.withAuth(auth.RoleAdmin, s.handleAIConfig))

	// 统计
	s.mux.HandleFunc("/api/stats/overview", s.withAuth(auth.RoleViewer, s.handleStatsOverview))
	s.mux.HandleFunc("/api/stats/agents", s.withAuth(auth.RoleViewer, s.handleStatsAgents))

	// 拓扑
	s.mux.HandleFunc("/api/topology", s.withAuth(auth.RoleViewer, s.handleTopology))

	// 终端持久会话
	s.mux.HandleFunc("/api/terminals", s.withAuth(auth.RoleViewer, s.handleTerminals))
	s.mux.HandleFunc("/api/terminals/", s.withAuth(auth.RoleViewer, s.handleTerminalByID))

	// AI 可用性（所有登录用户可查）
	s.mux.HandleFunc("/api/ai/status", s.withAuth(auth.RoleViewer, func(w http.ResponseWriter, r *http.Request) {
		enabled := s.ai != nil && s.ai.Enabled()
		model := ""
		if s.aiCfg != nil {
			model = s.aiCfg.Model
		}
		ok(w, map[string]any{"enabled": enabled, "model": model})
	}))

	// 终端 WS（auth 在升级前校验）
	s.mux.HandleFunc("/ws/terminal", s.terminalWS)
}

func (s *Server) withAuth(minRole auth.Role, next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		sess := s.requireAuth(w, r)
		if sess == nil {
			return
		}
		if !requireRole(sess, minRole) {
			fail(w, http.StatusForbidden, 40300, "权限不足")
			return
		}
		next(w, r.WithContext(context.WithValue(r.Context(), ctxSession, sess)))
	}
}

func (s *Server) handleAgents(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
	ok(w, s.st.ListAgents())
}

// /api/agents/{id}[/...]
func (s *Server) handleAgentByID(w http.ResponseWriter, r *http.Request) {
	rest := r.URL.Path[len("/api/agents/"):]
	parts := splitPath(rest)
	if len(parts) == 0 || parts[0] == "" {
		fail(w, http.StatusNotFound, 40400, "not found")
		return
	}
	id := parts[0]

	if len(parts) == 1 {
		switch r.Method {
		case http.MethodGet:
			a, found := s.st.GetAgent(id)
			if !found {
				fail(w, http.StatusNotFound, 40401, "agent not found")
				return
			}
			ok(w, a)
		case http.MethodDelete:
			// 仅允许删除离线主机
			a, found := s.st.GetAgent(id)
			if !found {
				fail(w, http.StatusNotFound, 40401, "agent not found")
				return
			}
			if a.Online {
				fail(w, http.StatusBadRequest, 40001, "在线主机不能删除，请先远程卸载或等待离线")
				return
			}
			sess := sessionFrom(r.Context())
			if sess == nil || !requireRole(sess, auth.RoleOperator) {
				fail(w, http.StatusForbidden, 40300, "权限不足")
				return
			}
			s.st.RemoveAgent(id)
			s.auth.AddAudit(sess.Username, "agent_delete", id, clientIP(r), true, "")
			ok(w, map[string]any{"ok": true})
		default:
			fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		}
		return
	}

	switch parts[1] {
	case "metrics":
		s.handleMetrics(w, r, id)
	case "logs":
		s.handleLogs(w, r, id)
	case "logs-analyze":
		s.handleLogsAnalyze(w, r, id)
	case "config":
		s.handleConfig(w, r, id)
	case "files":
		s.handleFiles(w, r, id)
	case "files-edit":
		s.handleFileEdit(w, r, id)
	case "hardware":
		s.handleHardware(w, r, id)
	case "disk-io":
		s.handleDiskIO(w, r, id)
	case "ai-analyze":
		s.handleAIAnalyze(w, r, id)
	case "upgrade":
		s.handleRemoteUpgrade(w, r, id)
	case "remote-uninstall":
		s.handleRemoteUninstall(w, r, id)
	case "server-ip":
		s.handleChangeServerIP(w, r, id)
	case "remark":
		s.handleSetRemark(w, r, id)
	case "reconnect":
		s.handleReconnect(w, r, id)
	case "docker":
		sub := ""
		if idx := strings.Index(r.URL.Path, "/docker/"); idx >= 0 {
			sub = r.URL.Path[idx+len("/docker/"):]
		}
		switch sub {
		case "containers":
			s.handleDockerContainers(w, r, id)
		case "images":
			s.handleDockerImages(w, r, id)
		case "action":
			s.handleDockerAction(w, r, id)
		case "logs":
			s.handleDockerLogs(w, r, id)
		case "inspect":
			s.handleDockerInspect(w, r, id)
		case "create":
			s.handleDockerCreate(w, r, id)
		case "compose":
			s.handleDockerCompose(w, r, id)
		default:
			fail(w, http.StatusNotFound, 40400, "not found")
		}
	case "services":
		s.handleServices(w, r, id)
	case "packages":
		s.handlePackages(w, r, id)
	case "security":
		s.handleSecurity(w, r, id)
	case "sys-users":
		s.handleSysUsers(w, r, id)
	case "sys-log":
		s.handleSysLog(w, r, id)
	case "net":
		s.handleNet(w, r, id)
	case "exec":
		s.handleExec(w, r, id)
	case "ctforge":
		sub := ""
		if idx := strings.Index(r.URL.Path, "/ctforge/"); idx >= 0 {
			sub = r.URL.Path[idx+len("/ctforge/"):]
		}
		s.handleCtforge(w, r, id, sub)
	default:
		fail(w, http.StatusNotFound, 40400, "not found")
	}
}

func (s *Server) handleMetrics(w http.ResponseWriter, r *http.Request, id string) {
	if r.Method != http.MethodGet {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
	ok(w, s.st.Series(id))
}

func (s *Server) handleLogs(w http.ResponseWriter, r *http.Request, id string) {
	if r.Method != http.MethodGet {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
	limit, _ := strconv.Atoi(r.URL.Query().Get("limit"))
	if limit <= 0 || limit > 500 {
		limit = 200
	}
	ok(w, s.st.Logs(id, limit))
}

// GET /api/agents/{id}/logs-analyze?keyword=x&level=error&limit=100
func (s *Server) handleLogsAnalyze(w http.ResponseWriter, r *http.Request, id string) {
	if r.Method != http.MethodGet {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
	keyword := r.URL.Query().Get("keyword")
	level := r.URL.Query().Get("level")
	limit, _ := strconv.Atoi(r.URL.Query().Get("limit"))
	if limit <= 0 || limit > 500 {
		limit = 200
	}
	entries := s.st.SearchLogs(id, keyword, level, limit)
	stats := s.st.LogStats(id)
	ok(w, map[string]any{
		"entries": entries,
		"stats":   stats,
		"filter":  map[string]string{"keyword": keyword, "level": level},
	})
}

func (s *Server) handleConfig(w http.ResponseWriter, r *http.Request, id string) {
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
		resp, err := ac.Request(proto.MsgConfigGet, "", nil, 10*time.Second)
		if err != nil {
			fail(w, http.StatusGatewayTimeout, 50401, err.Error())
			return
		}
		var cfg proto.ConfigGetResult
		if err := proto.DecodePayload(resp, &cfg); err != nil {
			fail(w, http.StatusInternalServerError, 50001, err.Error())
			return
		}
		ok(w, cfg.Config)
	case http.MethodPut, http.MethodPost:
		var kv proto.ConfigKV
		if err := json.NewDecoder(r.Body).Decode(&kv); err != nil || kv.Key == "" {
			fail(w, http.StatusBadRequest, 40001, "invalid body: expect {key,value}")
			return
		}
		resp, err := ac.Request(proto.MsgConfigSet, "", kv, 10*time.Second)
		if err != nil {
			fail(w, http.StatusGatewayTimeout, 50401, err.Error())
			return
		}
		var ack proto.AckPayload
		_ = proto.DecodePayload(resp, &ack)
		ok(w, ack)
	default:
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
	}
}

func (s *Server) handleFiles(w http.ResponseWriter, r *http.Request, id string) {
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
			path = "/"
		}
		download := r.URL.Query().Get("download") == "1"
		if download {
			// 流式下载：按 3MB 分块从 agent 拉取，直接写入 HTTP 响应，不经过 base64 整体解码
			s.streamFileDownload(w, r, ac, path)
			return
		}
		resp, err := ac.Request(proto.MsgFileList, "", proto.FileListPayload{Path: path}, 15*time.Second)
		if err != nil {
			fail(w, http.StatusGatewayTimeout, 50401, err.Error())
			return
		}
		var lr proto.FileListResult
		if err := proto.DecodePayload(resp, &lr); err != nil {
			fail(w, http.StatusInternalServerError, 50001, err.Error())
			return
		}
		ok(w, lr)
	case http.MethodPut:
		var p proto.FileWritePayload
		if err := json.NewDecoder(r.Body).Decode(&p); err != nil || p.Path == "" {
			fail(w, http.StatusBadRequest, 40001, "invalid body: expect {path,content_base64}")
			return
		}
		if _, err := base64.StdEncoding.DecodeString(p.ContentBase64); err != nil {
			fail(w, http.StatusBadRequest, 40002, "content_base64 is not valid base64")
			return
		}
		resp, err := ac.Request(proto.MsgFileWrite, "", p, 30*time.Second)
		if err != nil {
			fail(w, http.StatusGatewayTimeout, 50401, err.Error())
			return
		}
		var ack proto.AckPayload
		_ = proto.DecodePayload(resp, &ack)
		ok(w, ack)
	case http.MethodPost:
		var req struct {
			Action string `json:"action"`
			Path   string `json:"path"`
			From   string `json:"from,omitempty"`
			To     string `json:"to,omitempty"`
		}
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			fail(w, http.StatusBadRequest, 40001, "invalid body")
			return
		}
		var msgType string
		var payload any
		switch req.Action {
		case "mkdir":
			msgType, payload = proto.MsgFileMkdir, proto.FilePathPayload{Path: req.Path}
		case "delete":
			msgType, payload = proto.MsgFileDelete, proto.FilePathPayload{Path: req.Path}
		case "rename":
			msgType, payload = proto.MsgFileRename, proto.FileRenamePayload{From: req.From, To: req.To}
		default:
			fail(w, http.StatusBadRequest, 40003, "action must be mkdir|delete|rename")
			return
		}
		resp, err := ac.Request(msgType, "", payload, 15*time.Second)
		if err != nil {
			fail(w, http.StatusGatewayTimeout, 50401, err.Error())
			return
		}
		var ack proto.AckPayload
		_ = proto.DecodePayload(resp, &ack)
		ok(w, ack)
	default:
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
	}
}

// ---- 系统服务管理 ----

// GET  /api/agents/{id}/services?filter=sshd
// POST /api/agents/{id}/services {name,action}
func (s *Server) handleServices(w http.ResponseWriter, r *http.Request, id string) {
	ac, online := s.agent.Get(id)
	if !online {
		fail(w, http.StatusBadGateway, 50201, "agent offline")
		return
	}
	sess := sessionFrom(r.Context())
	switch r.Method {
	case http.MethodGet:
		filter := r.URL.Query().Get("filter")
		resp, err := ac.Request(proto.MsgServiceList, "", proto.ServiceListPayload{Filter: filter}, 20*time.Second)
		if err != nil {
			fail(w, http.StatusGatewayTimeout, 50401, err.Error())
			return
		}
		var lr proto.ServiceListResult
		if err := proto.DecodePayload(resp, &lr); err != nil {
			fail(w, http.StatusInternalServerError, 50001, err.Error())
			return
		}
		s.st.CacheResult(id, "services", resp.Payload)
		ok(w, lr.Services)
	case http.MethodPost:
		if sess == nil || !requireRole(sess, auth.RoleOperator) {
			fail(w, http.StatusForbidden, 40300, "权限不足")
			return
		}
		var p proto.ServiceActionPayload
		if err := json.NewDecoder(r.Body).Decode(&p); err != nil || p.Name == "" || p.Action == "" {
			fail(w, http.StatusBadRequest, 40001, "invalid body: expect {name,action}")
			return
		}
		resp, err := ac.Request(proto.MsgServiceAction, "", p, 35*time.Second)
		if err != nil {
			fail(w, http.StatusGatewayTimeout, 50401, err.Error())
			return
		}
		var ack proto.AckPayload
		_ = proto.DecodePayload(resp, &ack)
		s.auth.AddAudit(sess.Username, "service_"+p.Action, id+":"+p.Name, clientIP(r), true, "")
		ok(w, ack)
	default:
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
	}
}

// ---- 软件包管理 ----

// GET  /api/agents/{id}/packages/updates
// POST /api/agents/{id}/packages/upgrade {packages:[...]}
// GET  /api/agents/{id}/packages/history
func (s *Server) handlePackages(w http.ResponseWriter, r *http.Request, id string) {
	ac, online := s.agent.Get(id)
	if !online {
		fail(w, http.StatusBadGateway, 50201, "agent offline")
		return
	}
	sess := sessionFrom(r.Context())
	sub := ""
	if idx := strings.Index(r.URL.Path, "/packages/"); idx >= 0 {
		sub = r.URL.Path[idx+len("/packages/"):]
	}
	switch {
	case sub == "updates" && r.Method == http.MethodGet:
		resp, err := ac.Request(proto.MsgPkgUpdates, "", nil, 65*time.Second)
		if err != nil {
			fail(w, http.StatusGatewayTimeout, 50401, err.Error())
			return
		}
		var res proto.PkgUpdatesResult
		if err := proto.DecodePayload(resp, &res); err != nil {
			fail(w, http.StatusInternalServerError, 50001, err.Error())
			return
		}
		s.st.CacheResult(id, "pkg", resp.Payload)
		ok(w, res)
	case sub == "upgrade" && r.Method == http.MethodPost:
		if sess == nil || !requireRole(sess, auth.RoleOperator) {
			fail(w, http.StatusForbidden, 40300, "权限不足")
			return
		}
		var p proto.PkgUpgradePayload
		if err := json.NewDecoder(r.Body).Decode(&p); err != nil {
			fail(w, http.StatusBadRequest, 40001, "invalid body")
			return
		}
		resp, err := ac.Request(proto.MsgPkgUpgrade, "", p, 610*time.Second)
		if err != nil {
			fail(w, http.StatusGatewayTimeout, 50401, err.Error())
			return
		}
		var res proto.PkgUpgradeResult
		_ = proto.DecodePayload(resp, &res)
		s.auth.AddAudit(sess.Username, "pkg_upgrade", id, clientIP(r), res.OK, strings.Join(p.Packages, ","))
		ok(w, res)
	case sub == "history" && r.Method == http.MethodGet:
		resp, err := ac.Request(proto.MsgPkgHistory, "", nil, 30*time.Second)
		if err != nil {
			fail(w, http.StatusGatewayTimeout, 50401, err.Error())
			return
		}
		var res proto.PkgHistoryResult
		if err := proto.DecodePayload(resp, &res); err != nil {
			fail(w, http.StatusInternalServerError, 50001, err.Error())
			return
		}
		ok(w, res.Entries)
	default:
		fail(w, http.StatusNotFound, 40400, "not found")
	}
}

// ---- 安全审计 ----

// GET /api/agents/{id}/security
func (s *Server) handleSecurity(w http.ResponseWriter, r *http.Request, id string) {
	if r.Method != http.MethodGet {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
	ac, online := s.agent.Get(id)
	if !online {
		fail(w, http.StatusBadGateway, 50201, "agent offline")
		return
	}
	resp, err := ac.Request(proto.MsgSecurityAudit, "", nil, 30*time.Second)
	if err != nil {
		fail(w, http.StatusGatewayTimeout, 50401, err.Error())
		return
	}
	var res proto.SecurityAuditResult
	if err := proto.DecodePayload(resp, &res); err != nil {
		fail(w, http.StatusInternalServerError, 50001, err.Error())
		return
	}
	s.st.CacheResult(id, "security", resp.Payload)
	ok(w, res)
}

// ---- 认证 ----

func (s *Server) handleLogin(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
	var req struct {
		Username string `json:"username"`
		Password string `json:"password"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.Username == "" || req.Password == "" {
		fail(w, http.StatusBadRequest, 40001, "invalid body")
		return
	}
	sess, err := s.auth.Login(req.Username, req.Password, clientIP(r))
	if err != nil {
		fail(w, http.StatusUnauthorized, 40102, err.Error())
		return
	}
	ok(w, map[string]any{
		"token":      sess.Token,
		"username":   sess.Username,
		"role":       sess.Role,
		"expires_at": sess.ExpiresAt.Unix(),
	})
}

func (s *Server) handleLogout(w http.ResponseWriter, r *http.Request) {
	h := r.Header.Get("Authorization")
	token := strings.TrimPrefix(h, "Bearer ")
	if token != "" {
		s.auth.Logout(token)
	}
	ok(w, map[string]any{"ok": true})
}

func (s *Server) handleMe(w http.ResponseWriter, r *http.Request) {
	sess := s.requireAuth(w, r)
	if sess == nil {
		return
	}
	ok(w, map[string]any{
		"username": sess.Username,
		"role":     sess.Role,
	})
}

func (s *Server) handleChangePassword(w http.ResponseWriter, r *http.Request) {
	sess := s.requireAuth(w, r)
	if sess == nil {
		return
	}
	if r.Method != http.MethodPost {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
	var req struct {
		OldPassword string `json:"old_password"`
		NewPassword string `json:"new_password"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		fail(w, http.StatusBadRequest, 40001, "invalid body")
		return
	}
	if err := s.auth.ChangePassword(sess.Username, req.OldPassword, req.NewPassword); err != nil {
		fail(w, http.StatusBadRequest, 40002, err.Error())
		return
	}
	ok(w, map[string]any{"ok": true})
}

// ---- 账户管理 ----

func (s *Server) handleAccounts(w http.ResponseWriter, r *http.Request) {
	sess := sessionFrom(r.Context())
	switch r.Method {
	case http.MethodGet:
		ok(w, s.auth.ListAccounts())
	case http.MethodPost:
		var req struct {
			Username    string    `json:"username"`
			Password    string    `json:"password"`
			Role        auth.Role `json:"role"`
			DisplayName string    `json:"display_name,omitempty"`
		}
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			fail(w, http.StatusBadRequest, 40001, "invalid body")
			return
		}
		if req.Role == "" {
			req.Role = auth.RoleViewer
		}
		if err := s.auth.CreateAccount(req.Username, req.Password, req.Role, req.DisplayName, clientIP(r)); err != nil {
			fail(w, http.StatusBadRequest, 40002, err.Error())
			return
		}
		_ = sess
		ok(w, map[string]any{"ok": true})
	default:
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
	}
}

func (s *Server) handleAccountByName(w http.ResponseWriter, r *http.Request) {
	name := r.URL.Path[len("/api/accounts/"):]
	if name == "" {
		fail(w, http.StatusNotFound, 40400, "not found")
		return
	}
	switch r.Method {
	case http.MethodPut:
		var req struct {
			Role     *auth.Role `json:"role,omitempty"`
			Disabled *bool      `json:"disabled,omitempty"`
			Password string     `json:"password,omitempty"`
		}
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			fail(w, http.StatusBadRequest, 40001, "invalid body")
			return
		}
		if err := s.auth.UpdateAccount(name, req.Role, req.Disabled, req.Password, clientIP(r)); err != nil {
			fail(w, http.StatusBadRequest, 40002, err.Error())
			return
		}
		ok(w, map[string]any{"ok": true})
	case http.MethodDelete:
		if err := s.auth.DeleteAccount(name, clientIP(r)); err != nil {
			fail(w, http.StatusBadRequest, 40002, err.Error())
			return
		}
		ok(w, map[string]any{"ok": true})
	default:
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
	}
}

func (s *Server) handleAudit(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
	limit, _ := strconv.Atoi(r.URL.Query().Get("limit"))
	if limit <= 0 || limit > 500 {
		limit = 200
	}
	ok(w, s.auth.Audit(limit))
}

// ---- helpers ----

type ctxKey string

const ctxSession ctxKey = "session"

func sessionFrom(ctx context.Context) *auth.Session {
	if ctx == nil {
		return nil
	}
	if v := ctx.Value(ctxSession); v != nil {
		if s, ok := v.(*auth.Session); ok {
			return s
		}
	}
	return nil
}

func clientIP(r *http.Request) string {
	if xff := r.Header.Get("X-Forwarded-For"); xff != "" {
		if i := strings.Index(xff, ","); i > 0 {
			return strings.TrimSpace(xff[:i])
		}
		return xff
	}
	return strings.Split(r.RemoteAddr, ":")[0]
}

// ---- 系统账户管理 ----

// GET  /api/agents/{id}/sys-users
// POST /api/agents/{id}/sys-users/passwd {username,new_password}
// POST /api/agents/{id}/sys-users/lock   {username,lock}
func (s *Server) handleSysUsers(w http.ResponseWriter, r *http.Request, id string) {
	ac, online := s.agent.Get(id)
	if !online {
		fail(w, http.StatusBadGateway, 50201, "agent offline")
		return
	}
	sess := sessionFrom(r.Context())

	if r.Method == http.MethodGet {
		resp, err := ac.Request(proto.MsgSysUserList, "", nil, 20*time.Second)
		if err != nil {
			fail(w, http.StatusGatewayTimeout, 50401, err.Error())
			return
		}
		var res proto.SysUserListResult
		if err := proto.DecodePayload(resp, &res); err != nil {
			fail(w, http.StatusInternalServerError, 50001, err.Error())
			return
		}
		ok(w, res.Users)
		return
	}

	if r.Method != http.MethodPost {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
	if sess == nil || !requireRole(sess, auth.RoleAdmin) {
		fail(w, http.StatusForbidden, 40300, "仅管理员可修改系统账户")
		return
	}

	sub := ""
	if idx := strings.Index(r.URL.Path, "/sys-users/"); idx >= 0 {
		sub = r.URL.Path[idx+len("/sys-users/"):]
	}

	switch sub {
	case "passwd":
		var p proto.SysUserPasswdPayload
		if err := json.NewDecoder(r.Body).Decode(&p); err != nil || p.Username == "" || p.NewPassword == "" {
			fail(w, http.StatusBadRequest, 40001, "invalid body: expect {username,new_password}")
			return
		}
		if len(p.NewPassword) < 6 {
			fail(w, http.StatusBadRequest, 40002, "密码至少 6 个字符")
			return
		}
		resp, err := ac.Request(proto.MsgSysUserPasswd, "", p, 20*time.Second)
		if err != nil {
			fail(w, http.StatusGatewayTimeout, 50401, err.Error())
			return
		}
		var ack proto.AckPayload
		_ = proto.DecodePayload(resp, &ack)
		s.auth.AddAudit(sess.Username, "sys_user_passwd", id+":"+p.Username, clientIP(r), ack.OK, "")
		ok(w, ack)
	case "lock":
		var p proto.SysUserLockPayload
		if err := json.NewDecoder(r.Body).Decode(&p); err != nil || p.Username == "" {
			fail(w, http.StatusBadRequest, 40001, "invalid body: expect {username,lock}")
			return
		}
		resp, err := ac.Request(proto.MsgSysUserLock, "", p, 20*time.Second)
		if err != nil {
			fail(w, http.StatusGatewayTimeout, 50401, err.Error())
			return
		}
		var ack proto.AckPayload
		_ = proto.DecodePayload(resp, &ack)
		s.auth.AddAudit(sess.Username, map[bool]string{true: "sys_user_lock", false: "sys_user_unlock"}[p.Lock], id+":"+p.Username, clientIP(r), ack.OK, "")
		ok(w, ack)
	default:
		fail(w, http.StatusNotFound, 40400, "not found")
	}
}

// ---- 系统日志 ----

// GET /api/agents/{id}/sys-log?source=journald&priority=err&unit=sshd&keyword=failed&lines=100
func (s *Server) handleSysLog(w http.ResponseWriter, r *http.Request, id string) {
	if r.Method != http.MethodGet {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
	ac, online := s.agent.Get(id)
	if !online {
		fail(w, http.StatusBadGateway, 50201, "agent offline")
		return
	}
	q := r.URL.Query()
	lines, _ := strconv.Atoi(q.Get("lines"))
	if lines <= 0 || lines > 500 {
		lines = 100
	}
	payload := proto.SysLogPayload{
		Source:   q.Get("source"),
		Priority: q.Get("priority"),
		Unit:     q.Get("unit"),
		Keyword:  q.Get("keyword"),
		Lines:    lines,
	}
	resp, err := ac.Request(proto.MsgSysLog, "", payload, 35*time.Second)
	if err != nil {
		fail(w, http.StatusGatewayTimeout, 50401, err.Error())
		return
	}
	var res proto.SysLogResult
	if err := proto.DecodePayload(resp, &res); err != nil {
		fail(w, http.StatusInternalServerError, 50001, err.Error())
		return
	}
	ok(w, res)
}

// ---- 网络监控 ----

// GET /api/agents/{id}/net/traffic
// GET /api/agents/{id}/net/conn
func (s *Server) handleNet(w http.ResponseWriter, r *http.Request, id string) {
	if r.Method != http.MethodGet {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
	ac, online := s.agent.Get(id)
	if !online {
		fail(w, http.StatusBadGateway, 50201, "agent offline")
		return
	}
	sub := ""
	if idx := strings.Index(r.URL.Path, "/net/"); idx >= 0 {
		sub = r.URL.Path[idx+len("/net/"):]
	}
	switch sub {
	case "traffic":
		resp, err := ac.Request(proto.MsgNetTraffic, "", nil, 15*time.Second)
		if err != nil {
			fail(w, http.StatusGatewayTimeout, 50401, err.Error())
			return
		}
		var res proto.NetTrafficResult
		if err := proto.DecodePayload(resp, &res); err != nil {
			fail(w, http.StatusInternalServerError, 50001, err.Error())
			return
		}
		// 计算速率并缓存（与前一次差值 / 时间差）
		a, _ := s.st.GetAgent(id)
		var rxTotal, txTotal uint64
		for _, ifc := range res.Interfaces {
			rxTotal += ifc.RxBytes
			txTotal += ifc.TxBytes
		}
		nowMs := time.Now().UnixMilli()
		var rxRate, txRate float64
		if a != nil && a.NetAt > 0 && a.NetIfaces != nil {
			var prev proto.NetTrafficResult
			if err := json.Unmarshal(a.NetIfaces, &prev); err == nil {
				var prevRx, prevTx uint64
				for _, ifc := range prev.Interfaces {
					prevRx += ifc.RxBytes
					prevTx += ifc.TxBytes
				}
				dt := float64(nowMs-a.NetAt) / 1000.0
				if dt > 0.5 {
					rxRate = float64(int64(rxTotal)-int64(prevRx)) / dt
					txRate = float64(int64(txTotal)-int64(prevTx)) / dt
					if rxRate < 0 {
						rxRate = 0
					}
					if txRate < 0 {
						txRate = 0
					}
				}
			}
		}
		s.st.RecordNet(id, rxRate, txRate, 0, resp.Payload)
		ok(w, map[string]any{
			"timestamp":  res.Timestamp,
			"interfaces": res.Interfaces,
			"rx_rate":    rxRate,
			"tx_rate":    txRate,
		})
	case "conn":
		resp, err := ac.Request(proto.MsgNetConn, "", nil, 15*time.Second)
		if err != nil {
			fail(w, http.StatusGatewayTimeout, 50401, err.Error())
			return
		}
		var res proto.NetConnResult
		if err := proto.DecodePayload(resp, &res); err != nil {
			fail(w, http.StatusInternalServerError, 50001, err.Error())
			return
		}
		// 同步连接数到 store（用于列表展示）
		a, _ := s.st.GetAgent(id)
		if a != nil {
			s.st.RecordNet(id, a.NetRxRate, a.NetTxRate, res.TotalTCP, a.NetIfaces)
		}
		ok(w, res)
	default:
		fail(w, http.StatusNotFound, 40400, "not found")
	}
}

// ---- 远程命令执行 ----

// POST /api/agents/{id}/exec {command, timeout_sec, shell}
func (s *Server) handleExec(w http.ResponseWriter, r *http.Request, id string) {
	if r.Method != http.MethodPost {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
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
	var p proto.CmdExecPayload
	if err := json.NewDecoder(r.Body).Decode(&p); err != nil || p.Command == "" {
		fail(w, http.StatusBadRequest, 40001, "invalid body: expect {command}")
		return
	}
	if p.TimeoutSec <= 0 || p.TimeoutSec > 300 {
		p.TimeoutSec = 60
	}
	// 超时时间比 agent 侧多 5s 缓冲
	resp, err := ac.Request(proto.MsgCmdExec, "", p, time.Duration(p.TimeoutSec+5)*time.Second)
	if err != nil {
		fail(w, http.StatusGatewayTimeout, 50401, err.Error())
		return
	}
	var res proto.CmdExecResult
	if err := proto.DecodePayload(resp, &res); err != nil {
		fail(w, http.StatusInternalServerError, 50001, err.Error())
		return
	}
	// 审计：命令截断到 200 字符
	cmdLog := p.Command
	if len(cmdLog) > 200 {
		cmdLog = cmdLog[:200] + "..."
	}
	s.auth.AddAudit(sess.Username, "cmd_exec", id, clientIP(r), res.OK, cmdLog)
	ok(w, res)
}

func splitPath(s string) []string {
	var out []string
	start := 0
	for i := 0; i <= len(s); i++ {
		if i == len(s) || s[i] == '/' {
			if i > start {
				out = append(out, s[start:i])
			}
			start = i + 1
		}
	}
	return out
}

// streamFileDownload 分块拉取并流式写出，支持任意大小文件。
// 关键点：不把整个文件读进内存，边收边写。
func (s *Server) streamFileDownload(w http.ResponseWriter, r *http.Request, ac interface {
	Request(msgType, sessionID string, payload any, timeout time.Duration) (*proto.Message, error)
}, path string) {
	const chunkSize = 3 * 1024 * 1024

	// 先取第一块，拿到文件总大小
	resp, err := ac.Request(proto.MsgFileRead, "", proto.FileReadPayload{
		Path: path, Offset: 0, Limit: chunkSize,
	}, 60*time.Second)
	if err != nil {
		fail(w, http.StatusGatewayTimeout, 50401, err.Error())
		return
	}
	var fr struct {
		Path          string `json:"path"`
		Size          int64  `json:"size"`
		Offset        int64  `json:"offset"`
		NextOffset    int64  `json:"next_offset"`
		Done          bool   `json:"done"`
		ContentBase64 string `json:"content_base64"`
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

	name := path
	if i := strings.LastIndex(name, "/"); i >= 0 && i+1 < len(name) {
		name = name[i+1:]
	}
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Disposition", `attachment; filename="`+name+`"`)
	if fr.Size > 0 {
		w.Header().Set("Content-Length", strconv.FormatInt(fr.Size, 10))
	}
	w.Header().Set("X-File-Size", strconv.FormatInt(fr.Size, 10))
	w.Header().Set("Cache-Control", "no-cache")

	flusher, _ := w.(http.Flusher)

	writeChunk := func(b64 string) bool {
		raw, derr := base64.StdEncoding.DecodeString(b64)
		if derr != nil {
			return false
		}
		if _, werr := w.Write(raw); werr != nil {
			return false
		}
		if flusher != nil {
			flusher.Flush()
		}
		return true
	}

	if !writeChunk(fr.ContentBase64) {
		return
	}
	offset := fr.NextOffset
	total := fr.Size
	if fr.Done {
		log.Printf("[download] %s complete (%d bytes)", path, total)
		return
	}

	// 循环拉剩余块
	for offset < total {
		resp, err := ac.Request(proto.MsgFileRead, "", proto.FileReadPayload{
			Path: path, Offset: offset, Limit: chunkSize,
		}, 120*time.Second)
		if err != nil {
			log.Printf("[download] %s chunk@%d failed: %v", path, offset, err)
			return
		}
		var c struct {
			Size          int64  `json:"size"`
			NextOffset    int64  `json:"next_offset"`
			Done          bool   `json:"done"`
			ContentBase64 string `json:"content_base64"`
			Error         string `json:"error"`
		}
		if err := json.Unmarshal(resp.Payload, &c); err != nil {
			log.Printf("[download] %s decode failed: %v", path, err)
			return
		}
		if c.Error != "" {
			log.Printf("[download] %s chunk error: %s", path, c.Error)
			return
		}
		if !writeChunk(c.ContentBase64) {
			return
		}
		if c.NextOffset <= offset {
			break
		}
		offset = c.NextOffset
		if c.Done {
			break
		}
	}
	log.Printf("[download] %s complete (%d bytes)", path, offset)
}
