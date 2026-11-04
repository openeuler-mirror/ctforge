// Package history 记录客户端上下线事件与延迟统计。
package history

import (
	"encoding/json"
	"os"
	"path/filepath"
	"sync"
	"time"
)

// Event 上下线事件
type Event struct {
	AgentID   string `json:"agent_id"`
	Hostname  string `json:"hostname"`
	IP        string `json:"ip"`
	Kind      string `json:"kind"` // online / offline
	At        int64  `json:"at"`
	DurationS int64  `json:"duration_s,omitempty"` // offline 事件的持续时长（恢复后回填）
	LatencyMs float64 `json:"latency_ms,omitempty"`
}

type Manager struct {
	mu     sync.RWMutex
	path   string
	events []*Event
	max    int
}

func NewManager(path string) (*Manager, error) {
	m := &Manager{path: path, max: 5000}
	if err := m.load(); err != nil {
		if !os.IsNotExist(err) {
			return nil, err
		}
	}
	return m, nil
}

func (m *Manager) load() error {
	data, err := os.ReadFile(m.path)
	if err != nil {
		return err
	}
	var evts []*Event
	if err := json.Unmarshal(data, &evts); err != nil {
		return err
	}
	m.events = evts
	return nil
}

func (m *Manager) save() error {
	data, err := json.MarshalIndent(m.events, "", "  ")
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

// Record 记录一条事件
func (m *Manager) Record(e *Event) {
	m.mu.Lock()
	e.At = time.Now().UnixMilli()
	m.events = append(m.events, e)
	if len(m.events) > m.max {
		m.events = m.events[len(m.events)-m.max:]
	}
	m.mu.Unlock()
	go func() { _ = m.save() }()
}

// List 查询最近事件
func (m *Manager) List(agentID string, limit int) []*Event {
	m.mu.RLock()
	defer m.mu.RUnlock()
	if limit <= 0 || limit > 1000 {
		limit = 200
	}
	out := make([]*Event, 0, limit)
	for i := len(m.events) - 1; i >= 0 && len(out) < limit; i-- {
		if agentID == "" || m.events[i].AgentID == agentID {
			out = append(out, m.events[i])
		}
	}
	return out
}

// Stats 统计指定时间段内上下线次数、平均离线时长、平均延迟
func (m *Manager) Stats(agentID string, sinceMs int64) map[string]any {
	m.mu.RLock()
	defer m.mu.RUnlock()
	var online, offline int
	var totalOfflineDur, offlineCnt int64
	var latencySum float64
	var latencyCnt int
	for _, e := range m.events {
		if agentID != "" && e.AgentID != agentID {
			continue
		}
		if e.At < sinceMs {
			continue
		}
		switch e.Kind {
		case "online":
			online++
		case "offline":
			offline++
			if e.DurationS > 0 {
				totalOfflineDur += e.DurationS
				offlineCnt++
			}
		}
		if e.LatencyMs > 0 {
			latencySum += e.LatencyMs
			latencyCnt++
		}
	}
	out := map[string]any{
		"online_count":  online,
		"offline_count": offline,
	}
	if offlineCnt > 0 {
		out["avg_offline_s"] = totalOfflineDur / offlineCnt
	}
	if latencyCnt > 0 {
		out["avg_latency_ms"] = latencySum / float64(latencyCnt)
	}
	return out
}

// CloseOfflineEvent 客户端恢复时回填离线时长
func (m *Manager) CloseOfflineEvent(agentID string) {
	m.mu.Lock()
	defer m.mu.Unlock()
	now := time.Now().UnixMilli()
	for i := len(m.events) - 1; i >= 0; i-- {
		e := m.events[i]
		if e.AgentID == agentID && e.Kind == "offline" && e.DurationS == 0 {
			e.DurationS = (now - e.At) / 1000
			break
		}
	}
	go func() { _ = m.save() }()
}
