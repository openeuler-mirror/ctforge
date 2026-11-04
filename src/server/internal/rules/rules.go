// Package rules 管理告警规则与通知渠道配置（持久化到 JSON）。
// 规则定义：什么事件（kind）→ 什么级别 → 发到哪些渠道 → 什么时间段允许发送。
package rules

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sync"
	"time"

	"ctforge/server/internal/notify"
)

// Rule 单条告警规则
type Rule struct {
	ID       string   `json:"id"`
	Name     string   `json:"name"`
	Enabled  bool     `json:"enabled"`
	Kinds    []string `json:"kinds"`     // offline / high_cpu / high_mem / high_disk / ssh_brute / net_spike / *
	MinLevel string   `json:"min_level"` // info / warning / critical
	Channels []string `json:"channels"`  // 渠道 ID 列表
	// 发送时间段：如 ["09:00-22:00"]，空=全天
	TimeWindows []string `json:"time_windows,omitempty"`
	// 静默期（秒）：同一 agent 同 kind 在此时段内不重复发送
	CooldownSec int `json:"cooldown_sec"`
}

// Config 整体配置
type Config struct {
	Channels []*notify.Channel `json:"channels"`
	Rules    []*Rule           `json:"rules"`
}

type Manager struct {
	mu       sync.RWMutex
	path     string
	cfg      *Config
	lastSent map[string]time.Time // key: agentID|kind
}

func NewManager(path string) (*Manager, error) {
	m := &Manager{path: path, lastSent: make(map[string]time.Time)}
	if err := m.load(); err != nil {
		if !os.IsNotExist(err) {
			return nil, err
		}
		m.cfg = &Config{Rules: []*Rule{}}
		if err := m.save(); err != nil {
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
	var cfg Config
	if err := json.Unmarshal(data, &cfg); err != nil {
		return err
	}
	m.cfg = &cfg
	return nil
}

func (m *Manager) save() error {
	data, err := json.MarshalIndent(m.cfg, "", "  ")
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

func (m *Manager) Get() *Config {
	m.mu.RLock()
	defer m.mu.RUnlock()
	return m.cfg
}

func (m *Manager) SetCfg(cfg *Config) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.cfg = cfg
	return m.save()
}

func (m *Manager) UpsertChannel(c *notify.Channel) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	for i, e := range m.cfg.Channels {
		if e.ID == c.ID {
			m.cfg.Channels[i] = c
			return m.save()
		}
	}
	m.cfg.Channels = append(m.cfg.Channels, c)
	return m.save()
}

func (m *Manager) DeleteChannel(id string) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	out := m.cfg.Channels[:0]
	for _, c := range m.cfg.Channels {
		if c.ID != id {
			out = append(out, c)
		}
	}
	m.cfg.Channels = out
	return m.save()
}

func (m *Manager) UpsertRule(r *Rule) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	for i, e := range m.cfg.Rules {
		if e.ID == r.ID {
			m.cfg.Rules[i] = r
			return m.save()
		}
	}
	m.cfg.Rules = append(m.cfg.Rules, r)
	return m.save()
}

func (m *Manager) DeleteRule(id string) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	out := m.cfg.Rules[:0]
	for _, r := range m.cfg.Rules {
		if r.ID != id {
			out = append(out, r)
		}
	}
	m.cfg.Rules = out
	return m.save()
}

// Match 判断某事件应发往哪些渠道；返回 nil 表示不发送。
func (m *Manager) Match(agentID, kind, level string, now time.Time) []*notify.Channel {
	m.mu.Lock()
	defer m.mu.Unlock()

	levelRank := map[string]int{"info": 0, "warning": 1, "critical": 2}
	rank := levelRank[level]

	seen := map[string]bool{}
	var out []*notify.Channel
	key := agentID + "|" + kind

	for _, r := range m.cfg.Rules {
		if !r.Enabled {
			continue
		}
		// kind 匹配
		matchKind := false
		for _, k := range r.Kinds {
			if k == "*" || k == kind {
				matchKind = true
				break
			}
		}
		if !matchKind {
			continue
		}
		// 级别匹配
		if levelRank[r.MinLevel] > rank {
			continue
		}
		// 时间段
		if !inWindows(r.TimeWindows, now) {
			continue
		}
		// 静默期
		if r.CooldownSec > 0 {
			if last, ok := m.lastSent[key]; ok && now.Sub(last).Seconds() < float64(r.CooldownSec) {
				continue
			}
		}
		// 收集渠道
		for _, cid := range r.Channels {
			if seen[cid] {
				continue
			}
			for _, ch := range m.cfg.Channels {
				if ch.ID == cid && ch.Enabled {
					out = append(out, ch)
					seen[cid] = true
				}
			}
		}
	}
	if len(out) > 0 {
		m.lastSent[key] = now
	}
	return out
}

func inWindows(windows []string, now time.Time) bool {
	if len(windows) == 0 {
		return true
	}
	cur := now.Hour()*60 + now.Minute()
	for _, w := range windows {
		var sh, sm, eh, em int
		if _, err := fmt.Sscanf(w, "%d:%d-%d:%d", &sh, &sm, &eh, &em); err != nil {
			continue
		}
		start := sh*60 + sm
		end := eh*60 + em
		if start <= end {
			if cur >= start && cur <= end {
				return true
			}
		} else { // 跨零点，如 22:00-06:00
			if cur >= start || cur <= end {
				return true
			}
		}
	}
	return false
}
