package httpapi

import (
	"encoding/json"
	"fmt"
	"net/http"
	"os"
	"sort"
	"strconv"
	"strings"
	"time"

	"ctforge/server/internal/ai"
	"ctforge/server/internal/auth"
	"ctforge/server/internal/history"
	"ctforge/server/internal/perms"
	"ctforge/server/internal/portfwd"
	"ctforge/server/internal/proto"
	"ctforge/server/internal/store"
)

// ==================== 权限元信息 ====================

// GET /api/perms/meta
func (s *Server) handlePermsMeta(w http.ResponseWriter, r *http.Request) {
	ok(w, map[string]any{
		"groups": perms.PermGroups,
		"perms":  perms.AllPerms(),
		"levels": map[string]int{
			"readonly": 1, "operator": 2, "advanced": 3, "admin": 4,
		},
	})
}

// ==================== 分组管理 ====================

// GET/POST /api/groups
func (s *Server) handleGroups(w http.ResponseWriter, r *http.Request) {
	sess := sessionFrom(r.Context())
	switch r.Method {
	case http.MethodGet:
		ok(w, s.perms.ListGroups())
	case http.MethodPost:
		if sess == nil || !requireRole(sess, auth.RoleAdmin) {
			fail(w, http.StatusForbidden, 40300, "权限不足")
			return
		}
		var g perms.Group
		if err := json.NewDecoder(r.Body).Decode(&g); err != nil || g.Name == "" {
			fail(w, http.StatusBadRequest, 40001, "invalid body: expect {name,agents}")
			return
		}
		if err := s.perms.UpsertGroup(&g); err != nil {
			fail(w, http.StatusInternalServerError, 50001, err.Error())
			return
		}
		s.auth.AddAudit(sess.Username, "group_create", g.ID, clientIP(r), true, g.Name)
		ok(w, g)
	default:
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
	}
}

// PUT/DELETE /api/groups/{id}
func (s *Server) handleGroupByID(w http.ResponseWriter, r *http.Request) {
	sess := sessionFrom(r.Context())
	if sess == nil || !requireRole(sess, auth.RoleAdmin) {
		fail(w, http.StatusForbidden, 40300, "权限不足")
		return
	}
	id := strings.Trim(strings.TrimPrefix(r.URL.Path, "/api/groups/"), "/")
	if id == "" {
		fail(w, http.StatusBadRequest, 40001, "missing group id")
		return
	}
	switch r.Method {
	case http.MethodPut:
		var g perms.Group
		if err := json.NewDecoder(r.Body).Decode(&g); err != nil {
			fail(w, http.StatusBadRequest, 40001, "invalid body")
			return
		}
		g.ID = id
		if err := s.perms.UpsertGroup(&g); err != nil {
			fail(w, http.StatusInternalServerError, 50001, err.Error())
			return
		}
		s.auth.AddAudit(sess.Username, "group_update", id, clientIP(r), true, g.Name)
		ok(w, g)
	case http.MethodDelete:
		if err := s.perms.DeleteGroup(id); err != nil {
			fail(w, http.StatusInternalServerError, 50001, err.Error())
			return
		}
		s.auth.AddAudit(sess.Username, "group_delete", id, clientIP(r), true, "")
		ok(w, map[string]any{"ok": true})
	default:
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
	}
}

// ==================== 账户授权 ====================

// GET /api/grants
func (s *Server) handleGrants(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
	ok(w, s.perms.ListGrants())
}

// GET/PUT/DELETE /api/grants/{username}
func (s *Server) handleGrantByUser(w http.ResponseWriter, r *http.Request) {
	sess := sessionFrom(r.Context())
	if sess == nil || !requireRole(sess, auth.RoleAdmin) {
		fail(w, http.StatusForbidden, 40300, "权限不足")
		return
	}
	username := strings.Trim(strings.TrimPrefix(r.URL.Path, "/api/grants/"), "/")
	if username == "" {
		fail(w, http.StatusBadRequest, 40001, "missing username")
		return
	}
	switch r.Method {
	case http.MethodGet:
		g := s.perms.GetGrant(username)
		if g == nil {
			// 未配置：返回空授权（admin 默认全权）
			if a := s.findAccount(username); a != nil && a.Role == auth.RoleAdmin {
				g = &perms.Grant{
					Username:    username,
					Permissions: []string{perms.PermBilling},
					Scope:       perms.MachineScope{All: true},
				}
			} else {
				g = &perms.Grant{Username: username, Permissions: []string{}}
			}
		}
		ok(w, g)
	case http.MethodPut:
		var g perms.Grant
		if err := json.NewDecoder(r.Body).Decode(&g); err != nil {
			fail(w, http.StatusBadRequest, 40001, "invalid body")
			return
		}
		g.Username = username
		g.UpdatedBy = sess.Username
		if err := s.perms.UpsertGrant(&g); err != nil {
			fail(w, http.StatusInternalServerError, 50001, err.Error())
			return
		}
		s.auth.AddAudit(sess.Username, "grant_update", username, clientIP(r), true,
			strconv.Itoa(len(g.Permissions))+" perms")
		ok(w, g)
	case http.MethodDelete:
		if err := s.perms.DeleteGrant(username); err != nil {
			fail(w, http.StatusInternalServerError, 50001, err.Error())
			return
		}
		s.auth.AddAudit(sess.Username, "grant_delete", username, clientIP(r), true, "")
		ok(w, map[string]any{"ok": true})
	default:
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
	}
}

func (s *Server) findAccount(username string) *auth.AccountView {
	for _, a := range s.auth.ListAccounts() {
		if a.Username == username {
			return a
		}
	}
	return nil
}

// myGrant 当前登录用户的授权（含可见主机）
// handleMySSHKeys 当前用户 SSH 公钥管理（GET 读取 / PUT 整体替换）
func (s *Server) handleMySSHKeys(w http.ResponseWriter, r *http.Request) {
	sess := sessionFrom(r.Context())
	if sess == nil {
		fail(w, http.StatusUnauthorized, 40100, "未登录")
		return
	}
	switch r.Method {
	case http.MethodGet:
		ok(w, map[string]any{"keys": s.auth.GetSSHPublicKeys(sess.Username)})
	case http.MethodPut:
		var req struct {
			Keys []string `json:"keys"`
		}
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			fail(w, http.StatusBadRequest, 40001, "invalid body: expect {keys: [...]}")
			return
		}
		// 校验公钥格式（ssh-rsa/ssh-ed25519/ecdsa-sha2 开头）
		for _, k := range req.Keys {
			fields := strings.Fields(k)
			if len(fields) < 2 || !strings.HasPrefix(fields[0], "ssh-") {
				fail(w, http.StatusBadRequest, 40002, "公钥格式错误（需 OpenSSH authorized_keys 格式）: "+k)
				return
			}
		}
		if err := s.auth.SetSSHPublicKeys(sess.Username, req.Keys, clientIP(r)); err != nil {
			fail(w, http.StatusInternalServerError, 50001, err.Error())
			return
		}
		ok(w, map[string]any{"ok": true, "count": len(req.Keys)})
	default:
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
	}
}

func (s *Server) handleMyGrant(w http.ResponseWriter, r *http.Request) {
	sess := s.requireAuth(w, r)
	if sess == nil {
		return
	}
	g := s.perms.GetGrant(sess.Username)
	if g == nil || len(g.Permissions) == 0 {
		// 未配置授权：admin 全权，其他角色按角色给默认只读/运维
		switch sess.Role {
		case auth.RoleAdmin:
			g = &perms.Grant{Username: sess.Username, Permissions: []string{perms.PermBilling}, Scope: perms.MachineScope{All: true}}
		case auth.RoleOperator:
			g = &perms.Grant{Username: sess.Username, Permissions: perms.PermGroups[2].Perms, Scope: perms.MachineScope{All: true}}
		default:
			g = &perms.Grant{Username: sess.Username, Permissions: perms.PermGroups[0].Perms, Scope: perms.MachineScope{All: true}}
		}
	}
	ok(w, map[string]any{
		"username":    sess.Username,
		"role":        sess.Role,
		"permissions": g.Permissions,
		"scope":       g.Scope,
		"expires_at":  g.ExpiresAt,
		"valid":       g.Valid(time.Now()),
	})
}

// ==================== 上下线历史 ====================

// GET /api/history?agent_id=&limit=
func (s *Server) handleHistory(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
	agentID := r.URL.Query().Get("agent_id")
	limit, _ := strconv.Atoi(r.URL.Query().Get("limit"))
	ok(w, s.hist.List(agentID, limit))
}

// GET /api/history/stats?agent_id=&hours=24
func (s *Server) handleHistoryStats(w http.ResponseWriter, r *http.Request) {
	hours, _ := strconv.Atoi(r.URL.Query().Get("hours"))
	if hours <= 0 || hours > 720 {
		hours = 24
	}
	since := time.Now().Add(-time.Duration(hours) * time.Hour).UnixMilli()
	ok(w, s.hist.Stats(r.URL.Query().Get("agent_id"), since))
}

// ==================== 端口转发 ====================

// GET/POST /api/portforwards
func (s *Server) handlePortForwards(w http.ResponseWriter, r *http.Request) {
	sess := sessionFrom(r.Context())
	if sess == nil || !s.hasPerm(sess.Username, perms.PermPortForward, "") {
		fail(w, http.StatusForbidden, 40300, "无端口转发权限")
		return
	}
	switch r.Method {
	case http.MethodGet:
		ok(w, s.pf.List())
	case http.MethodPost:
		var f portfwd.Forward
		if err := json.NewDecoder(r.Body).Decode(&f); err != nil {
			fail(w, http.StatusBadRequest, 40001, "invalid body")
			return
		}
		if f.AgentID == "" || f.LocalPort <= 0 || f.TargetPort <= 0 {
			fail(w, http.StatusBadRequest, 40002, "需要 agent_id / local_port / target_port")
			return
		}
		if f.TargetPort < 1024 {
			fail(w, http.StatusBadRequest, 40003, "服务端监听端口需 >= 1024")
			return
		}
		f.CreatedBy = sess.Username
		if err := s.pf.Create(&f); err != nil {
			fail(w, http.StatusBadGateway, 50211, err.Error())
			return
		}
		s.auth.AddAudit(sess.Username, "portfwd_create", f.AgentID, clientIP(r), true,
			strconv.Itoa(f.TargetPort)+"->"+strconv.Itoa(f.LocalPort))
		ok(w, f)
	default:
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
	}
}

// DELETE /api/portforwards/{id}
func (s *Server) handlePortForwardByID(w http.ResponseWriter, r *http.Request) {
	sess := sessionFrom(r.Context())
	if sess == nil || !s.hasPerm(sess.Username, perms.PermPortForward, "") {
		fail(w, http.StatusForbidden, 40300, "无端口转发权限")
		return
	}
	if r.Method != http.MethodDelete {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
	id := strings.Trim(strings.TrimPrefix(r.URL.Path, "/api/portforwards/"), "/")
	if err := s.pf.Close(id); err != nil {
		fail(w, http.StatusBadRequest, 40004, err.Error())
		return
	}
	s.auth.AddAudit(sess.Username, "portfwd_close", id, clientIP(r), true, "")
	ok(w, map[string]any{"ok": true})
}

// ==================== AI 分析 ====================

// GET/PUT /api/ai/config
func (s *Server) handleAIConfig(w http.ResponseWriter, r *http.Request) {
	sess := sessionFrom(r.Context())
	if sess == nil || !requireRole(sess, auth.RoleAdmin) {
		fail(w, http.StatusForbidden, 40300, "仅管理员可配置 AI")
		return
	}
	switch r.Method {
	case http.MethodGet:
		if s.aiCfg == nil {
			ok(w, &ai.Config{})
			return
		}
		cp := *s.aiCfg
		if cp.APIKey != "" {
			cp.APIKey = "configured"
		}
		ok(w, cp)
	case http.MethodPut:
		var cfg ai.Config
		if err := json.NewDecoder(r.Body).Decode(&cfg); err != nil {
			fail(w, http.StatusBadRequest, 40001, "invalid body")
			return
		}
		// 若前端回传 "configured" 表示未修改，保留原 key
		if cfg.APIKey == "configured" && s.aiCfg != nil {
			cfg.APIKey = s.aiCfg.APIKey
		}
		if s.aiCfg != nil {
			*s.aiCfg = cfg
			// 落盘
			if data, err := json.MarshalIndent(&cfg, "", "  "); err == nil {
				_ = os.WriteFile(s.aiCfgPath, data, 0600)
			}
		}
		s.auth.AddAudit(sess.Username, "ai_config", "", clientIP(r), true, "")
		ok(w, map[string]any{"ok": true})
	default:
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
	}
}

// POST /api/agents/{id}/ai-analyze
func (s *Server) handleAIAnalyze(w http.ResponseWriter, r *http.Request, id string) {
	sess := sessionFrom(r.Context())
	if sess == nil || !s.hasPerm(sess.Username, perms.PermAI, id) {
		fail(w, http.StatusForbidden, 40300, "无 AI 分析权限")
		return
	}
	if r.Method != http.MethodPost {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
	if s.ai == nil || !s.ai.Enabled() {
		fail(w, http.StatusBadRequest, 40010, "AI 分析未启用，请先在系统设置中配置 API Key")
		return
	}
	a, found := s.st.GetAgent(id)
	if !found {
		fail(w, http.StatusNotFound, 40401, "agent not found")
		return
	}
	ac, online := s.agent.Get(id)
	if !online {
		fail(w, http.StatusBadGateway, 50201, "agent offline")
		return
	}

	// 采集硬件信息
	hw := map[string]any{}
	if resp, err := ac.Request(proto.MsgHardware, "", nil, 25*time.Second); err == nil {
		_ = json.Unmarshal(resp.Payload, &hw)
	}

	metrics := map[string]any{}
	if a.LastMetric != nil {
		metrics["cpu_percent"] = a.LastMetric.CPUPercent
		metrics["mem_percent"] = a.LastMetric.MemPercent
		metrics["mem_used_mb"] = a.LastMetric.MemUsedKB / 1024
		metrics["mem_total_mb"] = a.LastMetric.MemTotalKB / 1024
		metrics["disk_percent"] = a.LastMetric.DiskPercent
		metrics["disk_used_gb"] = float64(a.LastMetric.DiskUsedKB) / 1024 / 1024
		metrics["disk_total_gb"] = float64(a.LastMetric.DiskTotalKB) / 1024 / 1024
		metrics["load1"] = a.LastMetric.Load1
		metrics["load5"] = a.LastMetric.Load5
		metrics["load15"] = a.LastMetric.Load15
		metrics["procs"] = a.LastMetric.Procs
		metrics["uptime_sec"] = a.LastMetric.UptimeSec
	}
	metrics["net_rx_rate_bps"] = a.NetRxRate
	metrics["net_tx_rate_bps"] = a.NetTxRate
	metrics["tcp_conns"] = a.NetConns
	metrics["latency_ms"] = a.LatencyMs
	metrics["offline_count"] = a.OfflineCount

	prompt := ai.BuildPrompt(a.Hostname, a.OSVersion, a.Kernel, metrics, hw)
	res, err := s.ai.Analyze(prompt)
	if err != nil {
		fail(w, http.StatusBadGateway, 50212, err.Error())
		return
	}
	s.auth.AddAudit(sess.Username, "ai_analyze", id, clientIP(r), true, "")
	ok(w, res)
}

// ==================== 硬件 / 磁盘 IO ====================

// GET /api/agents/{id}/hardware
func (s *Server) handleHardware(w http.ResponseWriter, r *http.Request, id string) {
	if r.Method != http.MethodGet {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
	ac, online := s.agent.Get(id)
	if !online {
		fail(w, http.StatusBadGateway, 50201, "agent offline")
		return
	}
	resp, err := ac.Request(proto.MsgHardware, "", nil, 25*time.Second)
	if err != nil {
		fail(w, http.StatusGatewayTimeout, 50401, err.Error())
		return
	}
	var hw proto.HardwareResult
	if err := proto.DecodePayload(resp, &hw); err != nil {
		fail(w, http.StatusInternalServerError, 50001, err.Error())
		return
	}
	ok(w, hw)
}

// GET /api/agents/{id}/disk-io
func (s *Server) handleDiskIO(w http.ResponseWriter, r *http.Request, id string) {
	if r.Method != http.MethodGet {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
	ac, online := s.agent.Get(id)
	if !online {
		fail(w, http.StatusBadGateway, 50201, "agent offline")
		return
	}
	resp, err := ac.Request(proto.MsgDiskIO, "", nil, 15*time.Second)
	if err != nil {
		fail(w, http.StatusGatewayTimeout, 50401, err.Error())
		return
	}
	var io proto.DiskIOPayload
	if err := proto.DecodePayload(resp, &io); err != nil {
		fail(w, http.StatusInternalServerError, 50001, err.Error())
		return
	}
	ok(w, io)
}

// ==================== 统计 ====================

// GET /api/stats/overview
func (s *Server) handleStatsOverview(w http.ResponseWriter, r *http.Request) {
	agents := s.st.ListAgents()
	var online, offline, pending int
	var cpuSum, memSum, diskSum, latSum float64
	var cpuN, memN, diskN, latN int
	var offlineTotal int
	for _, a := range agents {
		switch a.Approval {
		case "pending":
			pending++
		case "approved":
			if a.Online {
				online++
			} else {
				offline++
			}
		}
		offlineTotal += a.OfflineCount
		if a.LastMetric != nil {
			cpuSum += a.LastMetric.CPUPercent
			cpuN++
			memSum += a.LastMetric.MemPercent
			memN++
		}
		if diskSum >= 0 && a.LastMetric != nil {
			diskSum += a.LastMetric.DiskPercent
			diskN++
		}
		if a.LatencyMs > 0 {
			latSum += a.LatencyMs
			latN++
		}
	}
	avg := func(sum float64, n int) float64 {
		if n == 0 {
			return 0
		}
		return float64(int(sum/float64(n)*100)) / 100
	}
	total := online + offline
	avail := 0.0
	if total > 0 {
		avail = float64(online) / float64(total) * 100
	}
	ok(w, map[string]any{
		"total":         total,
		"online":        online,
		"offline":       offline,
		"pending":       pending,
		"availability":  float64(int(avail*100)) / 100,
		"avg_cpu":       avg(cpuSum, cpuN),
		"avg_mem":       avg(memSum, memN),
		"avg_disk":      avg(diskSum, diskN),
		"avg_latency":   avg(latSum, latN),
		"offline_total": offlineTotal,
		"alerts_open":   len(s.st.AllAlerts(false)),
	})
}

// GET /api/stats/agents — 每台主机的 24h 统计
func (s *Server) handleStatsAgents(w http.ResponseWriter, r *http.Request) {
	hours, _ := strconv.Atoi(r.URL.Query().Get("hours"))
	if hours <= 0 || hours > 720 {
		hours = 24
	}
	since := time.Now().Add(-time.Duration(hours) * time.Hour).UnixMilli()
	out := make([]map[string]any, 0)
	for _, a := range s.st.ListAgents() {
		if a.Approval != "approved" {
			continue
		}
		st := s.hist.Stats(a.AgentID, since)
		m := map[string]any{
			"agent_id":      a.AgentID,
			"hostname":      a.Hostname,
			"online":        a.Online,
			"offline_count": a.OfflineCount,
			"latency_ms":    a.LatencyMs,
		}
		for k, v := range st {
			m[k] = v
		}
		if a.LastMetric != nil {
			m["cpu"] = a.LastMetric.CPUPercent
			m["mem"] = a.LastMetric.MemPercent
			m["disk"] = a.LastMetric.DiskPercent
		}
		out = append(out, m)
	}
	ok(w, out)
}

// ==================== 拓扑 ====================

// GET /api/topology
func (s *Server) handleTopology(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
	serverID := ""
	serverName := ""
	if s.ident != nil {
		serverID = s.ident.ServerID()
		serverName = s.ident.ServerName()
	}
	if serverName == "" {
		serverName = "CTForge Server"
	}
	nodes := make([]map[string]any, 0)
	edges := make([]map[string]any, 0)
	var segs = map[string]int{}

	// 按 /16 网段聚合
	for _, a := range s.st.ListAgents() {
		if a.Approval != "approved" {
			continue
		}
		nodes = append(nodes, map[string]any{
			"id":      a.AgentID,
			"label":   a.Hostname,
			"type":    "agent",
			"ip":      a.IP,
			"online":  a.Online,
			"os":      a.OSVersion,
			"kernel":  a.Kernel,
			"latency": a.LatencyMs,
			"cpu":     metricVal(a, "cpu"),
			"mem":     metricVal(a, "mem"),
			"disk":    metricVal(a, "disk"),
			"groups":  s.perms.GroupsOfAgent(a.AgentID),
			"alerts":  openAlertCount(a),
		})
		edges = append(edges, map[string]any{
			"from":    serverID,
			"to":      a.AgentID,
			"latency": a.LatencyMs,
			"online":  a.Online,
		})
		if seg := subnet16(a.IP); seg != "" {
			segs[seg]++
		}
	}
	segList := make([]map[string]any, 0, len(segs))
	for k, v := range segs {
		segList = append(segList, map[string]any{"subnet": k, "count": v})
	}
	sort.Slice(segList, func(i, j int) bool {
		return segList[i]["subnet"].(string) < segList[j]["subnet"].(string)
	})
	ok(w, map[string]any{
		"server":  map[string]any{"id": serverID, "label": serverName, "type": "server"},
		"nodes":   nodes,
		"edges":   edges,
		"subnets": segList,
	})
}

func metricVal(a *store.Agent, k string) float64 {
	if a.LastMetric == nil {
		return 0
	}
	switch k {
	case "cpu":
		return a.LastMetric.CPUPercent
	case "mem":
		return a.LastMetric.MemPercent
	case "disk":
		return a.LastMetric.DiskPercent
	}
	return 0
}

func openAlertCount(a *store.Agent) int {
	n := 0
	for _, al := range a.Alerts {
		if !al.Resolved {
			n++
		}
	}
	return n
}

func subnet16(ip string) string {
	parts := strings.Split(ip, ".")
	if len(parts) != 4 {
		return ""
	}
	return parts[0] + "." + parts[1] + ".0.0/16"
}

// ==================== 远程升级 / 卸载 ====================

// POST /api/agents/{id}/upgrade {url?, sha256?, version?}
func (s *Server) handleRemoteUpgrade(w http.ResponseWriter, r *http.Request, id string) {
	sess := sessionFrom(r.Context())
	if sess == nil || !s.hasPerm(sess.Username, perms.PermUpgrade, id) {
		fail(w, http.StatusForbidden, 40300, "无远程升级权限")
		return
	}
	if r.Method != http.MethodPost {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
	ac, online := s.agent.Get(id)
	if !online {
		fail(w, http.StatusBadGateway, 50201, "agent offline")
		return
	}
	var req struct {
		URL     string `json:"url"`
		SHA256  string `json:"sha256"`
		Version string `json:"version"`
	}
	_ = json.NewDecoder(r.Body).Decode(&req)
	if req.URL == "" {
		// 默认从本服务端下载
		host := ""
		if s.deploy != nil && s.deploy.ServerHost != "" {
			host = s.deploy.ServerHost
		} else {
			host = detectOutboundIP()
		}
		req.URL = "http://" + host + ":8080/download/agent"
	}
	resp, err := ac.Request(proto.MsgUpgrade, "", proto.UpgradePayload{
		URL: req.URL, SHA256: req.SHA256, Version: req.Version,
	}, 320*time.Second)
	if err != nil {
		fail(w, http.StatusGatewayTimeout, 50401, err.Error())
		return
	}
	var res proto.UpgradeResult
	_ = proto.DecodePayload(resp, &res)
	s.auth.AddAudit(sess.Username, "agent_upgrade", id, clientIP(r), res.OK, req.Version)
	ok(w, res)
}

// POST /api/agents/{id}/remote-uninstall
func (s *Server) handleRemoteUninstall(w http.ResponseWriter, r *http.Request, id string) {
	sess := sessionFrom(r.Context())
	if sess == nil || !s.hasPerm(sess.Username, perms.PermUninstall, id) {
		fail(w, http.StatusForbidden, 40300, "无远程卸载权限")
		return
	}
	if r.Method != http.MethodPost {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
	ac, online := s.agent.Get(id)
	if !online {
		fail(w, http.StatusBadGateway, 50201, "agent offline")
		return
	}
	if err := ac.SendNoWait(proto.MsgUninstall, "", map[string]any{"reason": "remote uninstall by admin"}); err != nil {
		fail(w, http.StatusBadGateway, 50213, err.Error())
		return
	}
	// 服务端主动卸载：标记批准、从主机列表彻底移除（含指标/日志/告警）
	s.st.DecideUninstall(id, true, sess.Username)
	go func(agentID string) {
		// 给客户端 2s 执行自删后再清理服务端记录
		time.Sleep(2500 * time.Millisecond)
		s.st.RemoveAgent(agentID)
	}(id)
	s.auth.AddAudit(sess.Username, "agent_remote_uninstall", id, clientIP(r), true, "")
	ok(w, map[string]any{"ok": true, "message": "uninstall command sent, agent will be removed"})
}

// hasPerm 综合权限判断（无授权配置时按角色兜底）
func (s *Server) hasPerm(username, perm, agentID string) bool {
	if s.perms == nil {
		return true
	}
	g := s.perms.GetGrant(username)
	if g == nil || len(g.Permissions) == 0 {
		// 未配置：按角色兜底
		for _, a := range s.auth.ListAccounts() {
			if a.Username != username {
				continue
			}
			switch a.Role {
			case auth.RoleAdmin:
				return true
			case auth.RoleOperator:
				return perm != perms.PermAccounts && perm != perms.PermNotify && perm != perms.PermGroup
			default:
				return perms.PermLevel(perm) <= 1
			}
		}
		return false
	}
	return s.perms.Check(username, perm, agentID)
}

var _ = history.Event{}

// ==================== Docker 管理 ====================

// GET /api/agents/{id}/docker/containers
func (s *Server) handleDockerContainers(w http.ResponseWriter, r *http.Request, id string) {
	if r.Method != http.MethodGet {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
	ac, online := s.agent.Get(id)
	if !online {
		fail(w, http.StatusBadGateway, 50201, "agent offline")
		return
	}
	resp, err := ac.Request(proto.MsgDockerList, "", nil, 30*time.Second)
	if err != nil {
		fail(w, http.StatusGatewayTimeout, 50401, err.Error())
		return
	}
	var res proto.DockerListResult
	if err := proto.DecodePayload(resp, &res); err != nil {
		fail(w, http.StatusInternalServerError, 50001, err.Error())
		return
	}
	ok(w, res)
}

// GET /api/agents/{id}/docker/images
func (s *Server) handleDockerImages(w http.ResponseWriter, r *http.Request, id string) {
	if r.Method != http.MethodGet {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
	ac, online := s.agent.Get(id)
	if !online {
		fail(w, http.StatusBadGateway, 50201, "agent offline")
		return
	}
	resp, err := ac.Request(proto.MsgDockerImages, "", nil, 25*time.Second)
	if err != nil {
		fail(w, http.StatusGatewayTimeout, 50401, err.Error())
		return
	}
	var res proto.DockerImagesResult
	if err := proto.DecodePayload(resp, &res); err != nil {
		fail(w, http.StatusInternalServerError, 50001, err.Error())
		return
	}
	ok(w, res.Images)
}

// POST /api/agents/{id}/docker/action {container_id, action}
func (s *Server) handleDockerAction(w http.ResponseWriter, r *http.Request, id string) {
	sess := sessionFrom(r.Context())
	if sess == nil || !requireRole(sess, auth.RoleOperator) {
		fail(w, http.StatusForbidden, 40300, "权限不足")
		return
	}
	if r.Method != http.MethodPost {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
	ac, online := s.agent.Get(id)
	if !online {
		fail(w, http.StatusBadGateway, 50201, "agent offline")
		return
	}
	var p proto.DockerActionPayload
	if err := json.NewDecoder(r.Body).Decode(&p); err != nil || p.ContainerID == "" || p.Action == "" {
		fail(w, http.StatusBadRequest, 40001, "invalid body")
		return
	}
	resp, err := ac.Request(proto.MsgDockerAction, "", p, 70*time.Second)
	if err != nil {
		fail(w, http.StatusGatewayTimeout, 50401, err.Error())
		return
	}
	var ack proto.AckPayload
	_ = proto.DecodePayload(resp, &ack)
	s.auth.AddAudit(sess.Username, "docker_"+p.Action, id+":"+p.ContainerID, clientIP(r), ack.OK, "")
	ok(w, ack)
}

// GET /api/agents/{id}/docker/logs?container_id=x&lines=200
func (s *Server) handleDockerLogs(w http.ResponseWriter, r *http.Request, id string) {
	if r.Method != http.MethodGet {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
	ac, online := s.agent.Get(id)
	if !online {
		fail(w, http.StatusBadGateway, 50201, "agent offline")
		return
	}
	lines, _ := strconv.Atoi(r.URL.Query().Get("lines"))
	if lines <= 0 || lines > 2000 {
		lines = 200
	}
	resp, err := ac.Request(proto.MsgDockerLogs, "", proto.DockerLogsPayload{
		ContainerID: r.URL.Query().Get("container_id"),
		Lines:       lines,
	}, 35*time.Second)
	if err != nil {
		fail(w, http.StatusGatewayTimeout, 50401, err.Error())
		return
	}
	var res proto.DockerLogsResult
	if err := proto.DecodePayload(resp, &res); err != nil {
		fail(w, http.StatusInternalServerError, 50001, err.Error())
		return
	}
	ok(w, res)
}

// ==================== Docker 详细能力 ====================

// GET /api/agents/{id}/docker/inspect?container_id=x
func (s *Server) handleDockerInspect(w http.ResponseWriter, r *http.Request, id string) {
	if r.Method != http.MethodGet {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
	ac, online := s.agent.Get(id)
	if !online {
		fail(w, http.StatusBadGateway, 50201, "agent offline")
		return
	}
	cid := r.URL.Query().Get("container_id")
	if cid == "" {
		fail(w, http.StatusBadRequest, 40001, "missing container_id")
		return
	}
	resp, err := ac.Request(proto.MsgDockerInspect, "", map[string]string{"container_id": cid}, 30*time.Second)
	if err != nil {
		fail(w, http.StatusGatewayTimeout, 50401, err.Error())
		return
	}
	// agent 返回 {"data":{...原始 inspect JSON...}}，直接透传 data 部分
	var raw struct {
		Data  json.RawMessage `json:"data"`
		Error string          `json:"error"`
	}
	if err := json.Unmarshal(resp.Payload, &raw); err != nil {
		fail(w, http.StatusInternalServerError, 50001, err.Error())
		return
	}
	if raw.Error != "" {
		fail(w, http.StatusBadGateway, 50202, raw.Error)
		return
	}
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.Write([]byte(fmt.Sprintf(`{"code":0,"data":%s,"message":"success"}`, raw.Data)))
}

// POST /api/agents/{id}/docker/create
func (s *Server) handleDockerCreate(w http.ResponseWriter, r *http.Request, id string) {
	sess := sessionFrom(r.Context())
	if sess == nil || !requireRole(sess, auth.RoleOperator) {
		fail(w, http.StatusForbidden, 40300, "权限不足")
		return
	}
	if r.Method != http.MethodPost {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
	ac, online := s.agent.Get(id)
	if !online {
		fail(w, http.StatusBadGateway, 50201, "agent offline")
		return
	}
	var p proto.DockerCreatePayload
	if err := json.NewDecoder(r.Body).Decode(&p); err != nil {
		fail(w, http.StatusBadRequest, 40001, "invalid body")
		return
	}
	if p.Image == "" || p.Name == "" {
		fail(w, http.StatusBadRequest, 40002, "需要 image 和 name")
		return
	}
	resp, err := ac.Request(proto.MsgDockerCreate, "", p, 120*time.Second)
	if err != nil {
		fail(w, http.StatusGatewayTimeout, 50401, err.Error())
		return
	}
	var res proto.DockerExecResult
	_ = proto.DecodePayload(resp, &res)
	s.auth.AddAudit(sess.Username, "docker_create", id+":"+p.Name, clientIP(r), res.OK, p.Image)
	ok(w, res)
}

// POST /api/agents/{id}/docker/compose
func (s *Server) handleDockerCompose(w http.ResponseWriter, r *http.Request, id string) {
	sess := sessionFrom(r.Context())
	if sess == nil || !requireRole(sess, auth.RoleOperator) {
		fail(w, http.StatusForbidden, 40300, "权限不足")
		return
	}
	if r.Method != http.MethodPost {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
	ac, online := s.agent.Get(id)
	if !online {
		fail(w, http.StatusBadGateway, 50201, "agent offline")
		return
	}
	var p proto.DockerComposePayload
	if err := json.NewDecoder(r.Body).Decode(&p); err != nil {
		fail(w, http.StatusBadRequest, 40001, "invalid body")
		return
	}
	if p.Action == "" {
		fail(w, http.StatusBadRequest, 40002, "需要 action")
		return
	}
	resp, err := ac.Request(proto.MsgDockerCompose, "", p, 180*time.Second)
	if err != nil {
		fail(w, http.StatusGatewayTimeout, 50401, err.Error())
		return
	}
	var res proto.DockerExecResult
	_ = proto.DecodePayload(resp, &res)
	s.auth.AddAudit(sess.Username, "docker_compose_"+p.Action, id, clientIP(r), res.OK, p.WorkDir)
	ok(w, res)
}

// ==================== 修改客户端 IP（服务端地址变更） ====================

// POST /api/agents/{id}/server-ip {server_host, server_port}
// 让客户端把配置里的 server_host 改为新地址，并自动重连
// handleSetRemark 设置/清除主机备注名（POST /api/agents/{id}/remark，body: {remark})
func (s *Server) handleSetRemark(w http.ResponseWriter, r *http.Request, id string) {
	sess := sessionFrom(r.Context())
	if sess == nil || !requireRole(sess, auth.RoleOperator) {
		fail(w, http.StatusForbidden, 40300, "权限不足")
		return
	}
	if r.Method != http.MethodPost {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
	var req struct {
		Remark string `json:"remark"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		fail(w, http.StatusBadRequest, 40001, "invalid body: expect {remark}")
		return
	}
	if !s.st.SetAgentRemark(id, req.Remark) {
		fail(w, http.StatusNotFound, 40401, "agent not found")
		return
	}
	s.auth.AddAudit(sess.Username, "set_remark", id, clientIP(r), true, req.Remark)
	ok(w, map[string]any{"ok": true, "remark": req.Remark})
}

func (s *Server) handleChangeServerIP(w http.ResponseWriter, r *http.Request, id string) {
	sess := sessionFrom(r.Context())
	if sess == nil || !requireRole(sess, auth.RoleOperator) {
		fail(w, http.StatusForbidden, 40300, "权限不足")
		return
	}
	if r.Method != http.MethodPost {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
	var req struct {
		ServerHost string `json:"server_host"`
		ServerPort int    `json:"server_port"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.ServerHost == "" {
		fail(w, http.StatusBadRequest, 40001, "invalid body: expect {server_host,server_port}")
		return
	}
	ac, online := s.agent.Get(id)
	if !online {
		fail(w, http.StatusBadGateway, 50201, "agent offline")
		return
	}

	// 依次下发：server_host → server_port，最后触发重连
	type kv = proto.ConfigKV
	if _, err := ac.Request(proto.MsgConfigSet, "", kv{Key: "server_host", Value: req.ServerHost}, 10*time.Second); err != nil {
		fail(w, http.StatusGatewayTimeout, 50401, "set server_host: "+err.Error())
		return
	}
	if req.ServerPort > 0 {
		portStr := strconv.Itoa(req.ServerPort)
		if _, err := ac.Request(proto.MsgConfigSet, "", kv{Key: "server_port", Value: portStr}, 10*time.Second); err != nil {
			fail(w, http.StatusGatewayTimeout, 50401, "set server_port: "+err.Error())
			return
		}
	}
	s.auth.AddAudit(sess.Username, "change_server_ip", id, clientIP(r), true,
		req.ServerHost+":"+strconv.Itoa(req.ServerPort))
	ok(w, map[string]any{
		"ok":      true,
		"message": "客户端配置已更新，将在下次重连时生效",
	})
}

// POST /api/agents/{id}/reconnect — 主动让客户端重连（应用新配置）
func (s *Server) handleReconnect(w http.ResponseWriter, r *http.Request, id string) {
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
	// 发送 offline 消息让客户端主动断开重连
	_ = ac.SendNoWait(proto.MsgOffline, "", map[string]any{"reason": "reconnect requested"})
	s.auth.AddAudit(sess.Username, "agent_reconnect", id, clientIP(r), true, "")
	ok(w, map[string]any{"ok": true, "message": "reconnect signal sent"})
}
