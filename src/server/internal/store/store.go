package store

import (
	"encoding/json"
	"os"
	"sync"
	"time"

	"ctforge/server/internal/proto"
)

type Agent struct {
	AgentID    string                  `json:"agent_id"`
	Name       string                  `json:"name"`
	Hostname   string                  `json:"hostname"`
	Remark     string                  `json:"remark,omitempty"` // 用户自定义备注名
	IP         string                  `json:"ip"`
	OS         string                  `json:"os"`
	Arch       string                  `json:"arch"`
	Version    string                  `json:"version"`
	Online     bool                    `json:"online"`
	LastSeen   time.Time               `json:"last_seen"`
	LastHB     *proto.HeartbeatPayload `json:"last_heartbeat,omitempty"`
	LastMetric *proto.MetricsPayload   `json:"last_metrics,omitempty"`
	Discovered bool                    `json:"discovered,omitempty"` // 是否通过局域网发现（未注册）

	// 批准状态：pending=待批准（未采集数据） approved=已批准 rejected=已拒绝
	Approval    string  `json:"approval"`
	OSVersion   string  `json:"os_version,omitempty"`
	Kernel      string  `json:"kernel,omitempty"`
	DiskTotalKB uint64  `json:"disk_total_kb,omitempty"`
	DiskUsedKB  uint64  `json:"disk_used_kb,omitempty"`
	DiskPercent float64 `json:"disk_percent,omitempty"`
	// 服务端绑定：一旦批准即绑定 server_id
	BoundServerID string `json:"bound_server_id,omitempty"`
	ApprovedAt    int64  `json:"approved_at,omitempty"`
	// 上下线记录
	OnlineAt     int64 `json:"online_at,omitempty"`
	OfflineAt    int64 `json:"offline_at,omitempty"`
	OfflineCount int   `json:"offline_count"`
	// 未解决告警
	Alerts []*Alert `json:"alerts,omitempty"`

	// 心跳图形状态：最近 N 次心跳的到达时间与间隔，用于前端画状态条
	HBHistory []int64 `json:"hb_history,omitempty"` // 每次心跳的时间戳（毫秒），最近 60 个
	HBSeq     int     `json:"-"`                    // 内部序号

	LatencyMs   float64   `json:"latency_ms"`                // 最近一次 ping 往返延迟
	LatencyHist []float64 `json:"latency_history,omitempty"` // 最近 60 次延迟
	LastPingAt  int64     `json:"last_ping_at,omitempty"`

	// 缓存最近一次系统管理结果（避免每次前端刷新都触发昂贵命令）
	Services   json.RawMessage `json:"services,omitempty"`
	PkgUpdates json.RawMessage `json:"pkg_updates,omitempty"`
	Security   json.RawMessage `json:"security,omitempty"`
	ServicesAt int64           `json:"services_at,omitempty"`
	PkgAt      int64           `json:"pkg_at,omitempty"`
	SecurityAt int64           `json:"security_at,omitempty"`

	// 网络指标（心跳时采集，用于列表展示与趋势）
	NetRxRate float64         `json:"net_rx_rate"` // 接收速率 B/s
	NetTxRate float64         `json:"net_tx_rate"` // 发送速率 B/s
	NetConns  int             `json:"net_conns"`   // TCP 连接总数
	NetRxHist []float64       `json:"net_rx_history,omitempty"`
	NetTxHist []float64       `json:"net_tx_history,omitempty"`
	NetAt     int64           `json:"net_at,omitempty"`
	NetIfaces json.RawMessage `json:"net_ifaces,omitempty"`
}

type MetricPoint struct {
	Timestamp int64   `json:"timestamp"`
	CPU       float64 `json:"cpu"`
	Mem       float64 `json:"mem"`
	Disk      float64 `json:"disk"`
	Load1     float64 `json:"load1"`
}

type LogEntry struct {
	Timestamp int64  `json:"timestamp"`
	Level     string `json:"level"`
	Message   string `json:"message"`
}

const (
	maxMetricPoints = 240
	maxLogEntries   = 500
)

// Store 分锁存储：
//
//	agentsMu  保护 agents（高频：心跳/指标/列表查询）
//	seriesMu  保护 series（高频：指标追加/图表查询）
//	logsMu    保护 logs（高频：日志写入/搜索）
//
// 三把锁独立，指标写入不再阻塞主机列表查询，反之亦然。
type Store struct {
	agentsMu sync.RWMutex
	agents   map[string]*Agent

	seriesMu sync.RWMutex
	series   map[string][]MetricPoint

	logsMu sync.RWMutex
	logs   map[string][]LogEntry

	// 低频操作（卸载请求）保留独立锁
	reqMu        sync.RWMutex
	uninstallReq map[string]*UninstallRequest

	// tombstone：被管理员删除的主机黑名单，拒绝重新注册/上线
	tombMu     sync.RWMutex
	tombstones map[string]bool
	tombFile   string // 黑名单持久化路径（重启后不丢失）
}

func New(dataDir ...string) *Store {
	st := &Store{
		agents:       make(map[string]*Agent),
		series:       make(map[string][]MetricPoint),
		logs:         make(map[string][]LogEntry),
		uninstallReq: make(map[string]*UninstallRequest),
		tombstones:   make(map[string]bool),
	}
	if len(dataDir) > 0 && dataDir[0] != "" {
		st.tombFile = dataDir[0] + "/tombstones.json"
		st.loadTombstones()
	}
	return st
}

// loadTombstones 从磁盘恢复黑名单（重启后不丢失）
func (s *Store) loadTombstones() {
	data, err := os.ReadFile(s.tombFile)
	if err != nil {
		return
	}
	var ids []string
	if json.Unmarshal(data, &ids) == nil {
		for _, id := range ids {
			s.tombstones[id] = true
		}
	}
}

// saveTombstones 持久化黑名单到磁盘
func (s *Store) saveTombstones() {
	if s.tombFile == "" {
		return
	}
	s.tombMu.RLock()
	ids := make([]string, 0, len(s.tombstones))
	for id := range s.tombstones {
		ids = append(ids, id)
	}
	s.tombMu.RUnlock()
	data, _ := json.Marshal(ids)
	_ = os.WriteFile(s.tombFile, data, 0o600)
}

func (s *Store) Register(a *Agent) {
	// 黑名单检查：被管理员删除的主机拒绝重新注册
	if s.IsTombstoned(a.AgentID) {
		return
	}
	s.agentsMu.Lock()
	defer s.agentsMu.Unlock()
	if old, ok := s.agents[a.AgentID]; ok {
		// 保留持久化的稳定字段（批准状态、绑定、系统信息、离线计数、告警）
		a.LastMetric = old.LastMetric
		a.LastHB = old.LastHB
		a.Approval = old.Approval
		a.BoundServerID = old.BoundServerID
		a.ApprovedAt = old.ApprovedAt
		a.OfflineCount = old.OfflineCount
		a.Alerts = old.Alerts
		if old.OSVersion != "" && a.OSVersion == "" {
			a.OSVersion = old.OSVersion
			a.Kernel = old.Kernel
			a.DiskTotalKB = old.DiskTotalKB
			a.DiskUsedKB = old.DiskUsedKB
			a.DiskPercent = old.DiskPercent
		}
	}
	a.Online = true
	a.OnlineAt = time.Now().UnixMilli()
	a.Discovered = false
	a.LastSeen = time.Now()
	s.agents[a.AgentID] = a
}

// Discover 记录局域网发现的 agent（未注册状态）
func (s *Store) Discover(a *Agent) {
	// 黑名单检查：被管理员删除的主机不再重新发现
	if s.IsTombstoned(a.AgentID) {
		return
	}
	s.agentsMu.Lock()
	defer s.agentsMu.Unlock()

	// 已存在：只更新发现相关的元信息，绝不覆盖批准状态/绑定/系统信息
	if old, ok := s.agents[a.AgentID]; ok {
		if old.Online {
			return // 已在线，无需更新
		}
		if a.IP != "" {
			old.IP = a.IP
		}
		if a.OS != "" {
			old.OS = a.OS
		}
		if a.OSVersion != "" && old.OSVersion == "" {
			old.OSVersion = a.OSVersion
		}
		if a.Kernel != "" && old.Kernel == "" {
			old.Kernel = a.Kernel
		}
		if a.Arch != "" {
			old.Arch = a.Arch
		}
		if a.Version != "" {
			old.Version = a.Version
		}
		if a.DiskPercent > 0 {
			old.DiskPercent = a.DiskPercent
		}
		// 关键：无论 Approval 是否为空，只要再次被发现且未批准，就标记为待接入
		if old.Approval != "approved" {
			old.Discovered = true
			if old.Approval == "" {
				old.Approval = "pending"
			}
		}
		old.LastSeen = time.Now()
		return
	}

	a.Online = false
	a.Discovered = true
	a.LastSeen = time.Now()
	// 新发现的主机默认进入待批准状态，才能在「批准管理」中被看到
	if a.Approval == "" {
		a.Approval = "pending"
	}
	s.agents[a.AgentID] = a
}

// PruneDiscovered 清理超过 timeout 未再次广播的发现记录
func (s *Store) PruneDiscovered(timeout time.Duration) {
	s.agentsMu.Lock()
	defer s.agentsMu.Unlock()
	cutoff := time.Now().Add(-timeout)
	for id, a := range s.agents {
		if a.Discovered && a.LastSeen.Before(cutoff) {
			delete(s.agents, id)
		}
	}
}

func (s *Store) SetOnline(agentID string, online bool) {
	s.agentsMu.Lock()
	defer s.agentsMu.Unlock()
	if a, ok := s.agents[agentID]; ok {
		a.Online = online
		a.LastSeen = time.Now()
	}
}

func (s *Store) UpdateHeartbeat(agentID string, hb *proto.HeartbeatPayload) {
	s.agentsMu.Lock()
	defer s.agentsMu.Unlock()
	a, ok := s.agents[agentID]
	if !ok {
		return
	}
	now := time.Now()
	a.LastHB = hb
	a.LastSeen = now
	a.Online = true
	// 记录心跳时间点，保留最近 60 个（约 10 分钟 @10s 间隔）
	a.HBHistory = append(a.HBHistory, now.UnixMilli())
	const maxHB = 60
	if len(a.HBHistory) > maxHB {
		a.HBHistory = a.HBHistory[len(a.HBHistory)-maxHB:]
	}
}

func (s *Store) UpdateMetrics(agentID string, m *proto.MetricsPayload) {
	s.seriesMu.Lock()
	defer s.seriesMu.Unlock()
	if a, ok := s.agents[agentID]; ok {
		a.LastMetric = m
		a.LastSeen = time.Now()
		a.Online = true
	}
	p := MetricPoint{Timestamp: m.Timestamp, CPU: m.CPUPercent, Mem: m.MemPercent, Disk: m.DiskPercent, Load1: m.Load1}
	arr := append(s.series[agentID], p)
	if len(arr) > maxMetricPoints {
		arr = arr[len(arr)-maxMetricPoints:]
	}
	s.series[agentID] = arr
}

func (s *Store) AddLog(agentID string, e LogEntry) {
	s.logsMu.Lock()
	defer s.logsMu.Unlock()
	arr := append(s.logs[agentID], e)
	if len(arr) > maxLogEntries {
		arr = arr[len(arr)-maxLogEntries:]
	}
	s.logs[agentID] = arr
}

func (s *Store) ListAgents() []*Agent {
	s.agentsMu.RLock()
	defer s.agentsMu.RUnlock()
	out := make([]*Agent, 0, len(s.agents))
	for _, a := range s.agents {
		cp := *a
		out = append(out, &cp)
	}
	return out
}

func (s *Store) GetAgent(id string) (*Agent, bool) {
	s.agentsMu.RLock()
	defer s.agentsMu.RUnlock()
	a, ok := s.agents[id]
	if !ok {
		return nil, false
	}
	cp := *a
	return &cp, true
}

func (s *Store) Series(agentID string) []MetricPoint {
	s.seriesMu.RLock()
	defer s.seriesMu.RUnlock()
	arr := s.series[agentID]
	out := make([]MetricPoint, len(arr))
	copy(out, arr)
	return out
}

func (s *Store) Logs(agentID string, limit int) []LogEntry {
	s.logsMu.RLock()
	defer s.logsMu.RUnlock()
	arr := s.logs[agentID]
	if limit > 0 && len(arr) > limit {
		arr = arr[len(arr)-limit:]
	}
	out := make([]LogEntry, len(arr))
	copy(out, arr)
	return out
}

// RecordLatency 记录一次 ping 往返延迟。
func (s *Store) RecordLatency(agentID string, ms float64) {
	s.agentsMu.Lock()
	defer s.agentsMu.Unlock()
	a, ok := s.agents[agentID]
	if !ok {
		return
	}
	a.LatencyMs = ms
	a.LastPingAt = time.Now().UnixMilli()
	a.LatencyHist = append(a.LatencyHist, ms)
	if len(a.LatencyHist) > 60 {
		a.LatencyHist = a.LatencyHist[len(a.LatencyHist)-60:]
	}
}

// CacheResult 缓存系统管理查询结果。
func (s *Store) CacheResult(agentID, kind string, data json.RawMessage) {
	s.agentsMu.Lock()
	defer s.agentsMu.Unlock()
	a, ok := s.agents[agentID]
	if !ok {
		return
	}
	now := time.Now().UnixMilli()
	switch kind {
	case "services":
		a.Services, a.ServicesAt = data, now
	case "pkg":
		a.PkgUpdates, a.PkgAt = data, now
	case "security":
		a.Security, a.SecurityAt = data, now
	}
}

// LogStats 汇总日志级别分布。
func (s *Store) LogStats(agentID string) map[string]int {
	s.logsMu.RLock()
	defer s.logsMu.RUnlock()
	out := map[string]int{"debug": 0, "info": 0, "warn": 0, "error": 0, "total": 0}
	for _, e := range s.logs[agentID] {
		out["total"]++
		if _, ok := out[e.Level]; ok {
			out[e.Level]++
		}
	}
	return out
}

// SearchLogs 按关键字与级别过滤。
func (s *Store) SearchLogs(agentID, keyword, level string, limit int) []LogEntry {
	s.logsMu.RLock()
	defer s.logsMu.RUnlock()
	arr := s.logs[agentID]
	out := make([]LogEntry, 0, limit)
	for i := len(arr) - 1; i >= 0 && len(out) < limit; i-- {
		e := arr[i]
		if level != "" && e.Level != level {
			continue
		}
		if keyword != "" && !containsFold(e.Message, keyword) {
			continue
		}
		out = append(out, e)
	}
	return out
}

func containsFold(s, sub string) bool {
	if len(sub) > len(s) {
		return false
	}
	ls, lsub := []rune(s), []rune(sub)
	for i := 0; i+len(lsub) <= len(ls); i++ {
		match := true
		for j := range lsub {
			if unicodeLower(ls[i+j]) != unicodeLower(lsub[j]) {
				match = false
				break
			}
		}
		if match {
			return true
		}
	}
	return false
}

func unicodeLower(r rune) rune {
	if r >= 'A' && r <= 'Z' {
		return r + 32
	}
	return r
}

// RecordNet 记录一次网络指标采样（速率由调用方计算）。
func (s *Store) RecordNet(agentID string, rxRate, txRate float64, conns int, ifaces json.RawMessage) {
	s.agentsMu.Lock()
	defer s.agentsMu.Unlock()
	a, ok := s.agents[agentID]
	if !ok {
		return
	}
	a.NetRxRate = rxRate
	a.NetTxRate = txRate
	a.NetConns = conns
	a.NetIfaces = ifaces
	a.NetAt = time.Now().UnixMilli()
	a.NetRxHist = append(a.NetRxHist, rxRate)
	a.NetTxHist = append(a.NetTxHist, txRate)
	if len(a.NetRxHist) > 120 {
		a.NetRxHist = a.NetRxHist[len(a.NetRxHist)-120:]
		a.NetTxHist = a.NetTxHist[len(a.NetTxHist)-120:]
	}
}

// Alert 客户端告警
type Alert struct {
	ID         string `json:"id"`
	AgentID    string `json:"agent_id"`
	Kind       string `json:"kind"`
	Level      string `json:"level"`
	Title      string `json:"title"`
	Detail     string `json:"detail"`
	CreatedAt  int64  `json:"created_at"`
	Resolved   bool   `json:"resolved"`
	ResolvedAt int64  `json:"resolved_at,omitempty"`
}

// OfflineEvent 离线事件记录
type OfflineEvent struct {
	AgentID   string `json:"agent_id"`
	Hostname  string `json:"hostname"`
	At        int64  `json:"at"`
	LastSeen  int64  `json:"last_seen"`
	DurationS int64  `json:"duration_s,omitempty"`
}

func (s *Store) SetApproval(agentID, state, serverID string) {
	s.agentsMu.Lock()
	defer s.agentsMu.Unlock()
	a, ok := s.agents[agentID]
	if !ok {
		return
	}
	a.Approval = state
	if state == "approved" {
		a.BoundServerID = serverID
		a.ApprovedAt = time.Now().UnixMilli()
	}
}

func (s *Store) ApprovalOf(agentID string) string {
	s.agentsMu.RLock()
	defer s.agentsMu.RUnlock()
	if a, ok := s.agents[agentID]; ok {
		return a.Approval
	}
	return ""
}

func (s *Store) AddOfflineEvent(agentID string) *OfflineEvent {
	s.agentsMu.Lock()
	defer s.agentsMu.Unlock()
	a, ok := s.agents[agentID]
	if !ok {
		return nil
	}
	a.OfflineAt = time.Now().UnixMilli()
	a.OfflineCount++
	a.Online = false
	return &OfflineEvent{
		AgentID:  agentID,
		Hostname: a.Hostname,
		At:       a.OfflineAt,
		LastSeen: a.LastSeen.UnixMilli(),
	}
}

func (s *Store) AddAlert(al *Alert) bool {
	s.agentsMu.Lock()
	defer s.agentsMu.Unlock()
	a, ok := s.agents[al.AgentID]
	if !ok {
		return false
	}
	for _, e := range a.Alerts {
		if e.Kind == al.Kind && !e.Resolved {
			e.Detail = al.Detail
			e.Level = al.Level
			e.CreatedAt = al.CreatedAt
			return false
		}
	}
	a.Alerts = append(a.Alerts, al)
	if len(a.Alerts) > 50 {
		a.Alerts = a.Alerts[len(a.Alerts)-50:]
	}
	return true
}

func (s *Store) ResolveAlert(agentID, kind string) {
	s.agentsMu.Lock()
	defer s.agentsMu.Unlock()
	a, ok := s.agents[agentID]
	if !ok {
		return
	}
	now := time.Now().UnixMilli()
	for _, al := range a.Alerts {
		if al.Kind == kind && !al.Resolved {
			al.Resolved = true
			al.ResolvedAt = now
		}
	}
}

func (s *Store) AllAlerts(includeResolved bool) []*Alert {
	s.agentsMu.RLock()
	defer s.agentsMu.RUnlock()
	var out []*Alert
	for _, a := range s.agents {
		for _, al := range a.Alerts {
			if !includeResolved && al.Resolved {
				continue
			}
			cp := *al
			out = append(out, &cp)
		}
	}
	return out
}

func (s *Store) ResolveAllAlerts(agentID string) {
	s.agentsMu.Lock()
	defer s.agentsMu.Unlock()
	a, ok := s.agents[agentID]
	if !ok {
		return
	}
	now := time.Now().UnixMilli()
	for _, al := range a.Alerts {
		if !al.Resolved {
			al.Resolved = true
			al.ResolvedAt = now
		}
	}
}

// OfflineCountOf 查询某 agent 的离线次数
func (s *Store) OfflineCountOf(agentID string) int {
	s.agentsMu.RLock()
	defer s.agentsMu.RUnlock()
	if a, ok := s.agents[agentID]; ok {
		return a.OfflineCount
	}
	return 0
}

// SetSystemInfo 握手时写入系统信息
func (s *Store) SetSystemInfo(agentID, osVersion, kernel string, diskTotal, diskUsed uint64, diskPct float64) {
	s.agentsMu.Lock()
	defer s.agentsMu.Unlock()
	a, ok := s.agents[agentID]
	if !ok {
		return
	}
	a.OSVersion = osVersion
	a.Kernel = kernel
	a.DiskTotalKB = diskTotal
	a.DiskUsedKB = diskUsed
	a.DiskPercent = diskPct
}

// UninstallRequest 卸载审批请求
type UninstallRequest struct {
	AgentID   string `json:"agent_id"`
	Hostname  string `json:"hostname"`
	Reason    string `json:"reason"`
	At        int64  `json:"at"`
	Approved  bool   `json:"approved"`
	DecidedAt int64  `json:"decided_at,omitempty"`
	DecidedBy string `json:"decided_by,omitempty"`
}

// AddUninstallRequest 记录/覆盖一条卸载请求
func (s *Store) AddUninstallRequest(r *UninstallRequest) {
	s.reqMu.Lock()
	defer s.reqMu.Unlock()
	s.uninstallReq[r.AgentID] = r
}

// DecideUninstall 审批卸载
func (s *Store) DecideUninstall(agentID string, approved bool, by string) {
	s.reqMu.Lock()
	defer s.reqMu.Unlock()
	if r, ok := s.uninstallReq[agentID]; ok {
		r.Approved = approved
		r.DecidedAt = time.Now().UnixMilli()
		r.DecidedBy = by
	}
}

// GetUninstallRequest 查询
func (s *Store) GetUninstallRequest(agentID string) (*UninstallRequest, bool) {
	s.reqMu.RLock()
	defer s.reqMu.RUnlock()
	r, ok := s.uninstallReq[agentID]
	if !ok {
		return nil, false
	}
	cp := *r
	return &cp, true
}

// ListUninstallRequests 列表
func (s *Store) ListUninstallRequests() []*UninstallRequest {
	s.reqMu.RLock()
	defer s.reqMu.RUnlock()
	out := make([]*UninstallRequest, 0, len(s.uninstallReq))
	for _, r := range s.uninstallReq {
		cp := *r
		out = append(out, &cp)
	}
	return out
}

// RemoveAgent 移除主机（卸载批准后清理 / 管理员删除离线主机）
func (s *Store) RemoveAgent(agentID string) {
	s.agentsMu.Lock()
	delete(s.agents, agentID)
	delete(s.series, agentID)
	delete(s.logs, agentID)
	s.agentsMu.Unlock()
	// 加入黑名单：该主机重新连接/被发现时拒绝注册，防止删除后重出现
	s.tombMu.Lock()
	s.tombstones[agentID] = true
	s.tombMu.Unlock()
	s.saveTombstones()
}

// SetAgentRemark 设置/清除主机备注名，返回是否存在该 agent
func (s *Store) SetAgentRemark(agentID, remark string) bool {
	s.agentsMu.Lock()
	defer s.agentsMu.Unlock()
	a, ok := s.agents[agentID]
	if !ok {
		return false
	}
	a.Remark = remark
	return true
}

// IsTombstoned 检查主机是否已被管理员删除（黑名单）
func (s *Store) IsTombstoned(agentID string) bool {
	s.tombMu.RLock()
	defer s.tombMu.RUnlock()
	return s.tombstones[agentID]
}
