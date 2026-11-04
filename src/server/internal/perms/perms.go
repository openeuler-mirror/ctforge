// Package perms 实现细粒度权限：机器范围、功能权限点、分组、有效期。
//
// 设计：
//   - 权限点（permission）用字符串常量表示，按等级分为若干组
//   - 账户授权（Grant）可以限定机器范围（全部 / 分组 / 指定主机）
//   - 授权可设置有效期，过期自动失效
package perms

import (
	"encoding/json"
	"os"
	"path/filepath"
	"sync"
	"time"
)

// ---- 功能权限点 ----

const (
	PermAgentView     = "agent.view"      // 查看主机
	PermAgentMetrics  = "agent.metrics"   // 查看指标
	PermAgentLogs     = "agent.logs"      // 查看日志
	PermTerminal      = "agent.terminal"  // 远程终端
	PermFileView      = "agent.file.view" // 查看文件
	PermFileEdit      = "agent.file.edit" // 编辑/上传/删除文件
	PermCmdExec       = "agent.cmd.exec"  // 执行命令
	PermService       = "agent.service"   // 服务管理
	PermPkg           = "agent.pkg"       // 软件包管理
	PermSysUser       = "agent.sysuser"   // 系统账户管理
	PermSysLog        = "agent.syslog"    // 系统日志
	PermSecurity      = "agent.security"  // 安全审计
	PermPortForward   = "agent.portfwd"   // 端口转发
	PermUpgrade       = "agent.upgrade"   // 远程升级
	PermUninstall     = "agent.uninstall" // 远程卸载
	PermDeploy        = "system.deploy"   // 部署新客户端
	PermApproval      = "system.approval" // 批准/拒绝客户端
	PermTask          = "system.task"     // 定时任务
	PermAlert         = "system.alert"    // 告警管理
	PermNotify        = "system.notify"   // 通知配置
	PermAccounts      = "system.accounts" // 账户管理
	PermAudit         = "system.audit"    // 审计日志
	PermStats         = "system.stats"    // 统计
	PermTopology      = "system.topology" // 拓扑
	PermAI            = "system.ai"       // AI 分析
	PermGroup         = "system.group"    // 分组管理
	PermBilling       = "system.all"      // 全部（超管）
)

// PermGroup 功能权限组（按等级分组，便于前端展示）
type PermGroupDef struct {
	ID    string   `json:"id"`
	Name  string   `json:"name"`
	Level int      `json:"level"` // 1=只读 2=运维 3=管理 4=超管
	Perms []string `json:"perms"`
}

// PermGroups 预定义权限组
var PermGroups = []PermGroupDef{
	{
		ID: "readonly", Name: "只读监控", Level: 1,
		Perms: []string{PermAgentView, PermAgentMetrics, PermAgentLogs, PermSysLog, PermSecurity, PermStats, PermTopology},
	},
	{
		ID: "operator", Name: "日常运维", Level: 2,
		Perms: []string{
			PermAgentView, PermAgentMetrics, PermAgentLogs, PermSysLog, PermSecurity, PermStats, PermTopology,
			PermTerminal, PermFileView, PermService, PermPkg, PermCmdExec,
		},
	},
	{
		ID: "advanced", Name: "高级运维", Level: 3,
		Perms: []string{
			PermAgentView, PermAgentMetrics, PermAgentLogs, PermSysLog, PermSecurity, PermStats, PermTopology,
			PermTerminal, PermFileView, PermService, PermPkg, PermCmdExec,
			PermFileEdit, PermSysUser, PermPortForward, PermUpgrade, PermUninstall, PermTask, PermAlert,
		},
	},
	{
		ID: "admin", Name: "系统管理", Level: 4,
		Perms: []string{PermBilling}, // 特殊：代表全部
	},
}

// AllPerms 返回全部权限点（供前端勾选）
func AllPerms() []string {
	return []string{
		PermAgentView, PermAgentMetrics, PermAgentLogs, PermTerminal,
		PermFileView, PermFileEdit, PermCmdExec, PermService, PermPkg,
		PermSysUser, PermSysLog, PermSecurity, PermPortForward,
		PermUpgrade, PermUninstall, PermDeploy, PermApproval, PermTask,
		PermAlert, PermNotify, PermAccounts, PermAudit, PermStats,
		PermTopology, PermAI, PermGroup,
	}
}

// PermLevel 返回权限点所属等级
func PermLevel(p string) int {
	for _, g := range PermGroups {
		for _, x := range g.Perms {
			if x == p {
				return g.Level
			}
		}
	}
	return 0
}

// ---- 分组 ----

type Group struct {
	ID          string   `json:"id"`
	Name        string   `json:"name"`
	Description string   `json:"description,omitempty"`
	Agents      []string `json:"agents"` // 组内主机（一个主机可在多组）
	CreatedAt   int64    `json:"created_at"`
}

// ---- 授权 ----

// MachineScope 机器范围
type MachineScope struct {
	All    bool     `json:"all"`              // 全部主机
	Groups []string `json:"groups,omitempty"` // 允许的分组
	Agents []string `json:"agents,omitempty"` // 额外指定的主机
}

// Grant 账户授权
type Grant struct {
	Username     string       `json:"username"`
	Permissions  []string     `json:"permissions"`
	Scope        MachineScope `json:"scope"`
	ExpiresAt    int64        `json:"expires_at"` // 0 = 永不过期
	CreatedAt    int64        `json:"created_at"`
	UpdatedAt    int64        `json:"updated_at"`
	UpdatedBy    string       `json:"updated_by,omitempty"`
}

// Valid 授权是否有效
func (g *Grant) Valid(now time.Time) bool {
	if g == nil {
		return false
	}
	if g.ExpiresAt == 0 {
		return true
	}
	return now.UnixMilli() < g.ExpiresAt
}

// Has 是否具备某权限
func (g *Grant) Has(perm string) bool {
	if g == nil {
		return false
	}
	for _, p := range g.Permissions {
		if p == PermBilling || p == perm {
			return true
		}
	}
	return false
}

// CanAccess 是否能操作某主机（需结合分组信息）
func (g *Grant) CanAccess(agentID string, agentGroups []string) bool {
	if g == nil {
		return false
	}
	if g.Scope.All {
		return true
	}
	for _, a := range g.Scope.Agents {
		if a == agentID {
			return true
		}
	}
	for _, ag := range agentGroups {
		for _, sg := range g.Scope.Groups {
			if ag == sg {
				return true
			}
		}
	}
	return false
}

// ---- 管理器 ----

type Manager struct {
	mu     sync.RWMutex
	path   string
	groups map[string]*Group
	grants map[string]*Grant
}

func NewManager(path string) (*Manager, error) {
	m := &Manager{
		path:   path,
		groups: make(map[string]*Group),
		grants: make(map[string]*Grant),
	}
	if err := m.load(); err != nil {
		if !os.IsNotExist(err) {
			return nil, err
		}
		if err := m.save(); err != nil {
			return nil, err
		}
	}
	return m, nil
}

type persistShape struct {
	Groups []*Group `json:"groups"`
	Grants []*Grant `json:"grants"`
}

func (m *Manager) load() error {
	data, err := os.ReadFile(m.path)
	if err != nil {
		return err
	}
	var s persistShape
	if err := json.Unmarshal(data, &s); err != nil {
		return err
	}
	for _, g := range s.Groups {
		m.groups[g.ID] = g
	}
	for _, g := range s.Grants {
		m.grants[g.Username] = g
	}
	return nil
}

func (m *Manager) save() error {
	s := persistShape{}
	for _, g := range m.groups {
		s.Groups = append(s.Groups, g)
	}
	for _, g := range m.grants {
		s.Grants = append(s.Grants, g)
	}
	data, err := json.MarshalIndent(s, "", "  ")
	if err != nil {
		return err
	}
	dir := filepath.Dir(m.path)
	if dir != "." {
		if err := os.MkdirAll(dir, 0755); err != nil {
			return err
		}
	}
	tmp := m.path + ".tmp"
	if err := os.WriteFile(tmp, data, 0600); err != nil {
		return err
	}
	return os.Rename(tmp, m.path)
}

// ---- 分组操作 ----

func (m *Manager) ListGroups() []*Group {
	m.mu.RLock()
	defer m.mu.RUnlock()
	out := make([]*Group, 0, len(m.groups))
	for _, g := range m.groups {
		cp := *g
		out = append(out, &cp)
	}
	return out
}

func (m *Manager) UpsertGroup(g *Group) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	if g.ID == "" {
		g.ID = "grp-" + time.Now().Format("20060102150405")
	}
	if g.CreatedAt == 0 {
		g.CreatedAt = time.Now().UnixMilli()
	}
	m.groups[g.ID] = g
	return m.save()
}

func (m *Manager) DeleteGroup(id string) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	delete(m.groups, id)
	// 同步从所有授权里移除该组
	for _, gr := range m.grants {
		out := gr.Scope.Groups[:0]
		for _, g := range gr.Scope.Groups {
			if g != id {
				out = append(out, g)
			}
		}
		gr.Scope.Groups = out
	}
	return m.save()
}

// GroupsOfAgent 返回主机所属的所有分组 ID
func (m *Manager) GroupsOfAgent(agentID string) []string {
	m.mu.RLock()
	defer m.mu.RUnlock()
	var out []string
	for id, g := range m.groups {
		for _, a := range g.Agents {
			if a == agentID {
				out = append(out, id)
				break
			}
		}
	}
	return out
}

// ---- 授权操作 ----

func (m *Manager) GetGrant(username string) *Grant {
	m.mu.RLock()
	defer m.mu.RUnlock()
	if g, ok := m.grants[username]; ok {
		cp := *g
		return &cp
	}
	return nil
}

func (m *Manager) ListGrants() []*Grant {
	m.mu.RLock()
	defer m.mu.RUnlock()
	out := make([]*Grant, 0, len(m.grants))
	for _, g := range m.grants {
		cp := *g
		out = append(out, &cp)
	}
	return out
}

func (m *Manager) UpsertGrant(g *Grant) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	g.UpdatedAt = time.Now().UnixMilli()
	if g.CreatedAt == 0 {
		if old, ok := m.grants[g.Username]; ok {
			g.CreatedAt = old.CreatedAt
		} else {
			g.CreatedAt = g.UpdatedAt
		}
	}
	m.grants[g.Username] = g
	return m.save()
}

func (m *Manager) DeleteGrant(username string) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	delete(m.grants, username)
	return m.save()
}

// Check 综合判断：权限点 + 机器范围 + 有效期
func (m *Manager) Check(username, perm, agentID string) bool {
	m.mu.RLock()
	gr, ok := m.grants[username]
	if !ok {
		m.mu.RUnlock()
		return false
	}
	cp := *gr
	m.mu.RUnlock()

	if !cp.Valid(time.Now()) {
		return false
	}
	if !cp.Has(perm) {
		return false
	}
	if agentID == "" {
		return true
	}
	// 需要机器范围校验
	agentGroups := m.GroupsOfAgent(agentID)
	return cp.CanAccess(agentID, agentGroups)
}

// AllowedAgents 返回该用户可见的主机集合；nil 表示全部
func (m *Manager) AllowedAgents(username string, allAgents []string) map[string]bool {
	m.mu.RLock()
	gr, ok := m.grants[username]
	if !ok {
		m.mu.RUnlock()
		return map[string]bool{}
	}
	cp := *gr
	m.mu.RUnlock()

	if !cp.Valid(time.Now()) {
		return map[string]bool{}
	}
	if cp.Scope.All {
		return nil // 全部
	}
	allowed := make(map[string]bool)
	for _, a := range cp.Scope.Agents {
		allowed[a] = true
	}
	for _, ag := range allAgents {
		groups := m.GroupsOfAgent(ag)
		for _, g := range groups {
			for _, sg := range cp.Scope.Groups {
				if g == sg {
					allowed[ag] = true
				}
			}
		}
	}
	return allowed
}
