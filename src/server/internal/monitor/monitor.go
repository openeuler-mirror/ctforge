// Package monitor 服务端告警引擎：定期检查所有已批准 agent 的状态，
// 生成告警（离线/CPU高/内存高/磁盘高/SSH爆破）并通过 notify 分发。
package monitor

import (
	"fmt"
	"log"
	"sync"
	"time"

	"ctforge/server/internal/notify"
	"ctforge/server/internal/rules"
	"ctforge/server/internal/store"
)

type Engine struct {
	st      *store.Store
	rules   *rules.Manager
	sender  *notify.Sender
	stopCh  chan struct{}
	mu      sync.Mutex
	// 已告警的 agent（避免重复）
	alerted map[string]string // agentID|kind -> level
}

func NewEngine(st *store.Store, r *rules.Manager, s *notify.Sender) *Engine {
	return &Engine{
		st: st, rules: r, sender: s,
		stopCh:  make(chan struct{}),
		alerted: make(map[string]string),
	}
}

func (e *Engine) Start() {
	go func() {
		t := time.NewTicker(30 * time.Second)
		defer t.Stop()
		for {
			select {
			case <-e.stopCh:
				return
			case <-t.C:
				e.check()
			}
		}
	}()
}

func (e *Engine) Stop() { close(e.stopCh) }

func (e *Engine) check() {
	agents := e.st.ListAgents()
	now := time.Now()

	for _, a := range agents {
		if a.Approval != "approved" {
			continue
		}

		// ---- 离线检测 ----
		if !a.Online {
			// 从未在线过（刚批准还没连上）跳过
			if a.LastSeen.IsZero() {
				continue
			}
			offlineSec := int64(now.Sub(a.LastSeen).Seconds())
			if offlineSec > 60 { // 超过 1 分钟未心跳视为离线
				e.raise(a.AgentID, a.Hostname, "offline", "warning",
					fmt.Sprintf("%s 已离线 %d 分钟", a.Hostname, offlineSec/60),
					fmt.Sprintf("最后心跳：%s\n离线时长：%d 分钟\nIP：%s",
						a.LastSeen.Format("2006-01-02 15:04:05"), offlineSec/60, a.IP))
			}
			continue
		}

		// 在线：清除离线告警
		e.clear(a.AgentID, "offline")

		// ---- 资源阈值检测 ----
		if m := a.LastMetric; m != nil {
			if m.CPUPercent >= 90 {
				e.raise(a.AgentID, a.Hostname, "high_cpu", "critical",
					fmt.Sprintf("%s CPU 使用率 %.1f%%", a.Hostname, m.CPUPercent),
					fmt.Sprintf("CPU：%.1f%%\n负载：%.2f / %.2f / %.2f\n进程数：%d",
						m.CPUPercent, m.Load1, m.Load5, m.Load15, m.Procs))
			} else {
				e.clear(a.AgentID, "high_cpu")
			}

			if m.MemPercent >= 90 {
				e.raise(a.AgentID, a.Hostname, "high_mem", "critical",
					fmt.Sprintf("%s 内存使用率 %.1f%%", a.Hostname, m.MemPercent),
					fmt.Sprintf("内存：%.1f%%（%d MB / %d MB）",
						m.MemPercent, m.MemUsedKB/1024, m.MemTotalKB/1024))
			} else {
				e.clear(a.AgentID, "high_mem")
			}

			if m.DiskPercent >= 90 {
				e.raise(a.AgentID, a.Hostname, "high_disk", "critical",
					fmt.Sprintf("%s 磁盘使用率 %.1f%%", a.Hostname, m.DiskPercent),
					fmt.Sprintf("磁盘：%.1f%%（%.1f GB / %.1f GB）",
						m.DiskPercent, float64(m.DiskUsedKB)/1024/1024, float64(m.DiskTotalKB)/1024/1024))
			} else {
				e.clear(a.AgentID, "high_disk")
			}
		}

		// ---- 网络流量突增 ----
		// 基准：历史平均的 5 倍且绝对值 > 50MB/s
		if a.NetRxRate > 50*1024*1024 || a.NetTxRate > 50*1024*1024 {
			e.raise(a.AgentID, a.Hostname, "net_spike", "warning",
				fmt.Sprintf("%s 网络流量异常", a.Hostname),
				fmt.Sprintf("接收：%.1f MB/s\n发送：%.1f MB/s\nTCP 连接：%d",
					a.NetRxRate/1024/1024, a.NetTxRate/1024/1024, a.NetConns))
		}
	}
}

// raise 生成告警并发送通知
func (e *Engine) raise(agentID, hostname, kind, level, title, detail string) {
	e.mu.Lock()
	key := agentID + "|" + kind
	if prev, ok := e.alerted[key]; ok && prev == level {
		e.mu.Unlock()
		return // 同级别已告警，不重复
	}
	e.alerted[key] = level
	e.mu.Unlock()

	isNew := e.st.AddAlert(&store.Alert{
		ID:        fmt.Sprintf("%s-%s-%d", agentID, kind, time.Now().UnixMilli()),
		AgentID:   agentID,
		Kind:      kind,
		Level:     level,
		Title:     title,
		Detail:    detail,
		CreatedAt: time.Now().UnixMilli(),
	})

	// 只有新增的告警才发通知（避免刷屏）
	if !isNew {
		return
	}

	channels := e.rules.Match(agentID, kind, level, time.Now())
	if len(channels) == 0 {
		return
	}
	msg := &notify.Message{
		Title: title, Content: detail, Level: level,
		AgentID: agentID, At: time.Now(),
	}
	for _, ch := range channels {
		go func(c *notify.Channel) {
			if err := e.sender.Send(c, msg); err != nil {
				log.Printf("[monitor] notify %s failed: %v", c.Name, err)
			} else {
				log.Printf("[monitor] notify %s sent: %s", c.Name, title)
			}
		}(ch)
	}
}

func (e *Engine) clear(agentID, kind string) {
	e.mu.Lock()
	delete(e.alerted, agentID+"|"+kind)
	e.mu.Unlock()
	e.st.ResolveAlert(agentID, kind)
}

// ReportAnomaly agent 主动上报的异常（如 SSH 爆破）
func (e *Engine) ReportAnomaly(agentID, hostname, kind, level, detail string) {
	titles := map[string]string{
		"ssh_brute": "SSH 暴力破解尝试",
		"net_spike": "网络流量异常",
		"high_cpu":  "CPU 使用率过高",
		"custom":    "客户端异常",
	}
	title := titles[kind]
	if title == "" {
		title = "客户端异常"
	}
	e.raise(agentID, hostname, kind, level,
		fmt.Sprintf("%s %s", hostname, title), detail)
}

// OnAgentOnline 客户端恢复在线时清理离线告警
func (e *Engine) OnAgentOnline(agentID string) {
	e.clear(agentID, "offline")
	e.st.ResolveAllAlerts(agentID)
}
