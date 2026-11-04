// Package tasks 定时任务调度：在指定客户端上按 cron 表达式执行命令。
// 支持 5 段式 cron（分 时 日 月 周），最小粒度 1 分钟。
package tasks

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"
)

// Task 定时任务定义
type Task struct {
	ID         string   `json:"id"`
	Name       string   `json:"name"`
	Enabled    bool     `json:"enabled"`
	Cron       string   `json:"cron"` // "*/5 * * * *"
	Command    string   `json:"command"`
	TimeoutSec int      `json:"timeout_sec"`
	Agents     []string `json:"agents"` // 目标 agent_id 列表；空 = 全部在线
	CreatedAt  int64    `json:"created_at"`
	LastRunAt  int64    `json:"last_run_at,omitempty"`
	LastStatus string   `json:"last_status,omitempty"` // ok / failed
	NextRunAt  int64    `json:"next_run_at,omitempty"`
	RunCount   int      `json:"run_count"`
}

// RunRecord 单次执行记录
type RunRecord struct {
	TaskID     string `json:"task_id"`
	TaskName   string `json:"task_name"`
	AgentID    string `json:"agent_id"`
	StartedAt  int64  `json:"started_at"`
	FinishedAt int64  `json:"finished_at"`
	OK         bool   `json:"ok"`
	ExitCode   int    `json:"exit_code"`
	Stdout     string `json:"stdout"`
	Stderr     string `json:"stderr"`
	DurMS      int64  `json:"duration_ms"`
	TimedOut   bool   `json:"timed_out"`
	Error      string `json:"error,omitempty"`
}

// Runner 任务执行回调（由 httpapi 注入，实际向 agent 下发命令）
type Runner func(agentID, command string, timeoutSec int) (ok bool, exitCode int, stdout, stderr string, durMS int64, timedOut bool, err error)

type Manager struct {
	mu      sync.RWMutex
	path    string
	tasks   map[string]*Task
	records []*RunRecord
	runner  Runner
	stopCh  chan struct{}
	maxRec  int
}

func NewManager(path string) (*Manager, error) {
	m := &Manager{
		path:   path,
		tasks:  make(map[string]*Task),
		stopCh: make(chan struct{}),
		maxRec: 500,
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

func (m *Manager) load() error {
	data, err := os.ReadFile(m.path)
	if err != nil {
		return err
	}
	var list []*Task
	if err := json.Unmarshal(data, &list); err != nil {
		return err
	}
	for _, t := range list {
		m.tasks[t.ID] = t
	}
	return nil
}

func (m *Manager) save() error {
	list := make([]*Task, 0, len(m.tasks))
	for _, t := range m.tasks {
		list = append(list, t)
	}
	data, err := json.MarshalIndent(list, "", "  ")
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

func newID() string {
	b := make([]byte, 6)
	_, _ = rand.Read(b)
	return "task-" + hex.EncodeToString(b)
}

func (m *Manager) SetRunner(r Runner) { m.runner = r }

func (m *Manager) List() []*Task {
	m.mu.RLock()
	defer m.mu.RUnlock()
	out := make([]*Task, 0, len(m.tasks))
	for _, t := range m.tasks {
		cp := *t
		out = append(out, &cp)
	}
	return out
}

func (m *Manager) Create(t *Task) error {
	if t.Cron == "" || t.Command == "" {
		return fmt.Errorf("cron 和 command 不能为空")
	}
	if _, err := ParseCron(t.Cron); err != nil {
		return fmt.Errorf("cron 表达式无效: %v", err)
	}
	if t.ID == "" {
		t.ID = newID()
	}
	if t.TimeoutSec <= 0 {
		t.TimeoutSec = 60
	}
	t.CreatedAt = time.Now().UnixMilli()
	if next, err := NextRun(t.Cron, time.Now()); err == nil {
		t.NextRunAt = next.UnixMilli()
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	m.tasks[t.ID] = t
	return m.save()
}

func (m *Manager) Update(id string, patch *Task) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	old, ok := m.tasks[id]
	if !ok {
		return fmt.Errorf("任务不存在")
	}
	if patch.Name != "" {
		old.Name = patch.Name
	}
	old.Enabled = patch.Enabled
	if patch.Cron != "" {
		if _, err := ParseCron(patch.Cron); err != nil {
			return fmt.Errorf("cron 表达式无效: %v", err)
		}
		old.Cron = patch.Cron
		if next, err := NextRun(patch.Cron, time.Now()); err == nil {
			old.NextRunAt = next.UnixMilli()
		}
	}
	if patch.Command != "" {
		old.Command = patch.Command
	}
	if patch.TimeoutSec > 0 {
		old.TimeoutSec = patch.TimeoutSec
	}
	if patch.Agents != nil {
		old.Agents = patch.Agents
	}
	return m.save()
}

func (m *Manager) Delete(id string) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	delete(m.tasks, id)
	return m.save()
}

func (m *Manager) Records(limit int) []*RunRecord {
	m.mu.RLock()
	defer m.mu.RUnlock()
	n := len(m.records)
	if limit <= 0 || limit > n {
		limit = n
	}
	out := make([]*RunRecord, limit)
	copy(out, m.records[n-limit:])
	// 倒序
	for i, j := 0, len(out)-1; i < j; i, j = i+1, j-1 {
		out[i], out[j] = out[j], out[i]
	}
	return out
}

// Start 启动调度循环（每 30s 检查一次，精度足够）
func (m *Manager) Start() {
	go func() {
		t := time.NewTicker(30 * time.Second)
		defer t.Stop()
		for {
			select {
			case <-m.stopCh:
				return
			case now := <-t.C:
				m.tick(now)
			}
		}
	}()
}

func (m *Manager) Stop() { close(m.stopCh) }

func (m *Manager) tick(now time.Time) {
	m.mu.RLock()
	var due []*Task
	for _, t := range m.tasks {
		if t.Enabled && t.NextRunAt > 0 && now.UnixMilli() >= t.NextRunAt {
			cp := *t
			due = append(due, &cp)
		}
	}
	m.mu.RUnlock()

	for _, t := range due {
		// 更新下次执行时间（先更新避免重复触发）
		if next, err := NextRun(t.Cron, now); err == nil {
			m.mu.Lock()
			if cur, ok := m.tasks[t.ID]; ok {
				cur.NextRunAt = next.UnixMilli()
			}
			m.mu.Unlock()
		}
		go m.execute(t)
	}
}

func (m *Manager) execute(t *Task) {
	if m.runner == nil {
		return
	}
	agents := t.Agents
	if len(agents) == 0 {
		// 空 = 全部（由调用方 runner 处理，这里传特殊标记）
		agents = []string{"*"}
	}
	allOK := true
	for _, aid := range agents {
		if m.runner == nil {
			break
		}
		rec := &RunRecord{
			TaskID: t.ID, TaskName: t.Name, AgentID: aid,
			StartedAt: time.Now().UnixMilli(),
		}
		ok, ec, out, errOut, dur, to, err := m.runner(aid, t.Command, t.TimeoutSec)
		rec.FinishedAt = time.Now().UnixMilli()
		rec.OK = ok
		rec.ExitCode = ec
		rec.Stdout = truncate(out, 4096)
		rec.Stderr = truncate(errOut, 2048)
		rec.DurMS = dur
		rec.TimedOut = to
		if err != nil {
			rec.Error = err.Error()
			allOK = false
		}
		if !ok {
			allOK = false
		}
		m.mu.Lock()
		m.records = append(m.records, rec)
		if len(m.records) > m.maxRec {
			m.records = m.records[len(m.records)-m.maxRec:]
		}
		m.mu.Unlock()
	}
	m.mu.Lock()
	if cur, ok := m.tasks[t.ID]; ok {
		cur.LastRunAt = time.Now().UnixMilli()
		cur.RunCount++
		if allOK {
			cur.LastStatus = "ok"
		} else {
			cur.LastStatus = "failed"
		}
	}
	m.mu.Unlock()
}

func truncate(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[:n] + "...(truncated)"
}

// ---- cron 解析（5 段：分 时 日 月 周）----

type Cron struct {
	Minute, Hour, Day, Month, Week []int
}

func ParseCron(expr string) (*Cron, error) {
	parts := strings.Fields(expr)
	if len(parts) != 5 {
		return nil, fmt.Errorf("需要 5 段（分 时 日 月 周），实际 %d 段", len(parts))
	}
	ranges := [][2]int{{0, 59}, {0, 23}, {1, 31}, {1, 12}, {0, 6}}
	fields := make([][]int, 5)
	for i, p := range parts {
		v, err := parseField(p, ranges[i][0], ranges[i][1])
		if err != nil {
			return nil, fmt.Errorf("第 %d 段: %v", i+1, err)
		}
		fields[i] = v
	}
	return &Cron{
		Minute: fields[0], Hour: fields[1], Day: fields[2],
		Month: fields[3], Week: fields[4],
	}, nil
}

func parseField(s string, min, max int) ([]int, error) {
	var out []int
	for _, part := range strings.Split(s, ",") {
		step := 1
		base := part
		if idx := strings.Index(part, "/"); idx >= 0 {
			base = part[:idx]
			v, err := strconv.Atoi(part[idx+1:])
			if err != nil || v <= 0 {
				return nil, fmt.Errorf("步长无效: %s", part)
			}
			step = v
		}
		lo, hi := min, max
		if base != "*" && base != "" {
			if idx := strings.Index(base, "-"); idx >= 0 {
				a, err1 := strconv.Atoi(base[:idx])
				b, err2 := strconv.Atoi(base[idx+1:])
				if err1 != nil || err2 != nil {
					return nil, fmt.Errorf("范围无效: %s", base)
				}
				lo, hi = a, b
			} else {
				v, err := strconv.Atoi(base)
				if err != nil {
					return nil, fmt.Errorf("值无效: %s", base)
				}
				lo, hi = v, v
			}
		}
		if lo < min || hi > max || lo > hi {
			return nil, fmt.Errorf("超出范围 [%d,%d]: %s", min, max, part)
		}
		for i := lo; i <= hi; i += step {
			out = append(out, i)
		}
	}
	return out, nil
}

func contains(list []int, v int) bool {
	for _, x := range list {
		if x == v {
			return true
		}
	}
	return false
}

// NextRun 计算下次执行时间（最多向后搜索 366 天）
func NextRun(expr string, from time.Time) (time.Time, error) {
	c, err := ParseCron(expr)
	if err != nil {
		return time.Time{}, err
	}
	t := from.Truncate(time.Minute).Add(time.Minute)
	for i := 0; i < 366*24*60; i++ {
		if contains(c.Minute, t.Minute()) &&
			contains(c.Hour, t.Hour()) &&
			contains(c.Day, t.Day()) &&
			contains(c.Month, int(t.Month())) &&
			contains(c.Week, int(t.Weekday())) {
			return t, nil
		}
		t = t.Add(time.Minute)
	}
	return time.Time{}, fmt.Errorf("无法计算下次执行时间")
}

// RunNow 立即执行一次（手动触发）
func (m *Manager) RunNow(t *Task) {
	cp := *t
	go m.execute(&cp)
}
