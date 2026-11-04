package store

import (
	"encoding/json"
	"os"
	"path/filepath"
	"sync"
)

// Persistence 持久化批准状态与离线计数（服务端重启后恢复）。
// 只存必要的稳定字段，不存指标/日志等易变数据。
type PersistRecord struct {
	AgentID       string  `json:"agent_id"`
	Approval      string  `json:"approval"`
	Remark        string  `json:"remark,omitempty"`
	BoundServerID string  `json:"bound_server_id,omitempty"`
	ApprovedAt    int64   `json:"approved_at,omitempty"`
	OfflineCount  int     `json:"offline_count"`
	OSVersion     string  `json:"os_version,omitempty"`
	Kernel        string  `json:"kernel,omitempty"`
	DiskTotalKB   uint64  `json:"disk_total_kb,omitempty"`
	DiskUsedKB    uint64  `json:"disk_used_kb,omitempty"`
	DiskPercent   float64 `json:"disk_percent,omitempty"`
}

type Persister struct {
	mu   sync.Mutex
	path string
	st   *Store
}

func NewPersister(path string, st *Store) *Persister {
	return &Persister{path: path, st: st}
}

// Load 从磁盘恢复批准状态
func (p *Persister) Load() error {
	data, err := os.ReadFile(p.path)
	if err != nil {
		if os.IsNotExist(err) {
			return nil
		}
		return err
	}
	var list []*PersistRecord
	if err := json.Unmarshal(data, &list); err != nil {
		return err
	}
	p.st.agentsMu.Lock()
	defer p.st.agentsMu.Unlock()
	for _, r := range list {
		// 黑名单检查：被管理员删除的主机跳过恢复，防止重启后重出现
		if p.st.IsTombstoned(r.AgentID) {
			continue
		}
		a, ok := p.st.agents[r.AgentID]
		if !ok {
			a = &Agent{AgentID: r.AgentID, Approval: r.Approval}
			p.st.agents[r.AgentID] = a
		}
		a.Approval = r.Approval
		a.Remark = r.Remark
		a.BoundServerID = r.BoundServerID
		a.ApprovedAt = r.ApprovedAt
		a.OfflineCount = r.OfflineCount
		if r.OSVersion != "" {
			a.OSVersion = r.OSVersion
			a.Kernel = r.Kernel
			a.DiskTotalKB = r.DiskTotalKB
			a.DiskUsedKB = r.DiskUsedKB
			a.DiskPercent = r.DiskPercent
		}
	}
	return nil
}

// Save 把当前批准状态落盘
func (p *Persister) Save() error {
	p.st.agentsMu.RLock()
	list := make([]*PersistRecord, 0, len(p.st.agents))
	for _, a := range p.st.agents {
		if a.Approval == "" {
			continue
		}
		list = append(list, &PersistRecord{
			AgentID: a.AgentID, Approval: a.Approval, Remark: a.Remark,
			BoundServerID: a.BoundServerID, ApprovedAt: a.ApprovedAt,
			OfflineCount: a.OfflineCount,
			OSVersion:    a.OSVersion, Kernel: a.Kernel,
			DiskTotalKB: a.DiskTotalKB, DiskUsedKB: a.DiskUsedKB,
			DiskPercent: a.DiskPercent,
		})
	}
	p.st.agentsMu.RUnlock()

	data, err := json.MarshalIndent(list, "", "  ")
	if err != nil {
		return err
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	dir := filepath.Dir(p.path)
	if dir != "." {
		if err := os.MkdirAll(dir, 0755); err != nil {
			return err
		}
	}
	tmp := p.path + ".tmp"
	if err := os.WriteFile(tmp, data, 0600); err != nil {
		return err
	}
	return os.Rename(tmp, p.path)
}

// SaveAsync 异步落盘（状态变更后调用，避免阻塞协议处理）
func (p *Persister) SaveAsync() {
	go func() { _ = p.Save() }()
}

// ---- 卸载请求持久化 ----

type uninstallFile struct {
	Requests []*UninstallRequest `json:"requests"`
}

// SaveUninstall 落盘卸载请求
func (p *Persister) SaveUninstall() error {
	p.st.agentsMu.RLock()
	list := make([]*UninstallRequest, 0, len(p.st.uninstallReq))
	for _, r := range p.st.uninstallReq {
		cp := *r
		list = append(list, &cp)
	}
	p.st.agentsMu.RUnlock()

	data, err := json.MarshalIndent(uninstallFile{Requests: list}, "", "  ")
	if err != nil {
		return err
	}
	path := p.path + ".uninstall"
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, data, 0600); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}

// LoadUninstall 恢复卸载请求
func (p *Persister) LoadUninstall() error {
	path := p.path + ".uninstall"
	data, err := os.ReadFile(path)
	if err != nil {
		if os.IsNotExist(err) {
			return nil
		}
		return err
	}
	var f uninstallFile
	if err := json.Unmarshal(data, &f); err != nil {
		return err
	}
	p.st.reqMu.Lock()
	defer p.st.reqMu.Unlock()
	for _, r := range f.Requests {
		p.st.uninstallReq[r.AgentID] = r
	}
	return nil
}
