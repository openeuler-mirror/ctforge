package httpapi

import (
	"crypto/rand"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"log"
	"net/http"
	"strings"
	"sync"
	"time"

	"ctforge/server/internal/proto"
	"github.com/gorilla/websocket"
)

var upgrader = websocket.Upgrader{
	CheckOrigin: func(r *http.Request) bool { return true },
}

// TermSession 服务端持久终端会话。
// 与 WebSocket 解耦：浏览器断开后会话仍存活（若 suspended），
// agent 侧 pty 进程继续运行，输出缓存在内存里，重新进入可回放。
type TermSession struct {
	ID        string
	AgentID   string
	CreatedAt time.Time
	UpdatedAt time.Time

	mu        sync.Mutex
	ws        *websocket.Conn
	buffer    []byte // 输出回放缓冲（上限 256KB）
	suspended bool   // 挂起：WS 断开后保留会话
	pumping   bool   // 输出泵是否运行中
	remark    string // 用户备注名（suspend 消息传入，恢复时显示）
	closed    bool
	cols      int
	rows      int
	ac        agentTermConn
	owner     string        // 创建者用户名
	sshOut    <-chan []byte // SSH 网关的 agent 输出通道（网页恢复时接管泵送）
}

// agentTermConn 抽象需要的 agent 能力
type agentTermConn interface {
	SendNoWait(msgType, sessionID string, payload any) error
	CloseTerm(sessionID string)
	AgentID() string
}

type termRegistry struct {
	mu       sync.RWMutex
	sessions map[string]*TermSession
}

var termReg = &termRegistry{sessions: make(map[string]*TermSession)}

const maxTermBuffer = 256 * 1024

func newSessionID() string {
	b := make([]byte, 12)
	_, _ = rand.Read(b)
	return hex.EncodeToString(b)
}

func (r *termRegistry) add(s *TermSession) {
	r.mu.Lock()
	r.sessions[s.ID] = s
	r.mu.Unlock()
}

func (r *termRegistry) get(id string) *TermSession {
	r.mu.RLock()
	defer r.mu.RUnlock()
	return r.sessions[id]
}

func (r *termRegistry) remove(id string) {
	r.mu.Lock()
	delete(r.sessions, id)
	r.mu.Unlock()
}

// ListByOwner 按 owner 前缀返回挂起会话列表（供 SSH 网关共享）
func (r *termRegistry) ListByOwner(ownerPrefix string) []map[string]any {
	r.mu.RLock()
	defer r.mu.RUnlock()
	var out []map[string]any
	for _, s := range r.sessions {
		if !strings.HasPrefix(s.owner, ownerPrefix) {
			continue
		}
		s.mu.Lock()
		out = append(out, map[string]any{
			"id":         s.ID,
			"agent_id":   s.AgentID,
			"owner":      s.owner,
			"remark":     s.remark,
			"suspended":  s.suspended,
			"created_at": s.CreatedAt.UnixMilli(),
		})
		s.mu.Unlock()
	}
	return out
}

func (r *termRegistry) list(owner string) []map[string]any {
	r.mu.RLock()
	defer r.mu.RUnlock()
	out := make([]map[string]any, 0, len(r.sessions))
	// 查询方 username（去前缀：ssh:admin / sess:admin / admin 都取 admin）
	want := owner
	if i := strings.LastIndexByte(owner, ':'); i >= 0 {
		want = owner[i+1:]
	}
	for _, s := range r.sessions {
		got := s.owner
		if i := strings.LastIndexByte(got, ':'); i >= 0 {
			got = got[i+1:]
		}
		match := want == "" || strings.EqualFold(got, want)
		if !match {
			continue
		}
		s.mu.Lock()
		out = append(out, map[string]any{
			"id":         s.ID,
			"agent_id":   s.AgentID,
			"owner":      s.owner,
			"remark":     s.remark,
			"created_at": s.CreatedAt.UnixMilli(),
			"updated_at": s.UpdatedAt.UnixMilli(),
			"suspended":  s.suspended,
			"attached":   s.ws != nil,
			"buffered":   len(s.buffer),
		})
		s.mu.Unlock()
	}
	return out
}

// CloseByAgent agent 掉线时清理其会话
func (r *termRegistry) closeByAgent(agentID string) {
	r.mu.Lock()
	ids := make([]string, 0)
	for id, s := range r.sessions {
		if s.AgentID == agentID {
			ids = append(ids, id)
		}
	}
	r.mu.Unlock()
	for _, id := range ids {
		r.mu.Lock()
		delete(r.sessions, id)
		r.mu.Unlock()
	}
}

// NewTermSessionForSuspend 创建挂起的终端会话（SSH 网关注册用，同网页共享 termRegistry）
// ac：agent 连接（网页恢复时 attach 需要）；owner 用裸 username（与网页一致，跨端共享）
func NewTermSessionForSuspend(id, agentID, owner, remark string, ac agentTermConn, outCh <-chan []byte) *TermSession {
	return &TermSession{
		ID:        id,
		AgentID:   agentID,
		CreatedAt: time.Now(),
		UpdatedAt: time.Now(),
		suspended: true,
		remark:    remark,
		owner:     owner,
		cols:      120,
		rows:      32,
		ac:        ac,
		sshOut:    outCh,
	}
}

// SetSuspended 标记会话为挂起状态
func (s *TermSession) SetSuspended(v bool) {
	s.mu.Lock()
	s.suspended = v
	s.mu.Unlock()
}

// HasPump 是否已有输出泵在运行
func (s *TermSession) HasPump() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.pumping
}

// StartPump 网页 attach 时接管输出泵（SSH 会话恢复场景）
func (s *TermSession) StartPump(writeWS func([]byte)) {
	s.mu.Lock()
	if s.pumping || s.sshOut == nil {
		s.mu.Unlock()
		return
	}
	s.pumping = true
	ch := s.sshOut
	s.mu.Unlock()
	go func() {
		for data := range ch {
			s.appendOutput(data)
		}
		s.mu.Lock()
		s.pumping = false
		s.closed = true
		w := s.ws
		s.mu.Unlock()
		if w != nil {
			_ = w.WriteMessage(websocket.CloseMessage, websocket.FormatCloseMessage(1000, "session ended"))
			_ = w.Close()
		}
	}()
}

// AppendOutput 写入输出缓冲（SSH 网关输出转发用，恢复时可回放历史输出）
func (s *TermSession) AppendOutput(data []byte) {
	s.appendOutput(data)
}

// GetBuffer 读取输出缓冲（恢复时回放用）
func (s *TermSession) GetBuffer() []byte {
	s.mu.Lock()
	defer s.mu.Unlock()
	return append([]byte(nil), s.buffer...)
}

// GetAC 返回 agent 连接（attach 用）
func (s *TermSession) GetAC() agentTermConn {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.ac
}

// Add 注册会话到 termRegistry（SSH 网关注册挂起会话用）
func (r *termRegistry) Add(s *TermSession) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.sessions[s.ID] = s
}

// Remove 按 ID 移除会话（SSH deleteSuspended 用）
func (r *termRegistry) Remove(id string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	delete(r.sessions, id)
}

// IsAlive 检测会话是否存活（agent 在线 + 会话未关闭）
func (r *termRegistry) IsAlive(id string, agentSrv interface {
	Get(string) (interface {
		CloseTerm(string)
		SendNoWait(string, string, any) error
	}, bool)
}) bool {
	r.mu.RLock()
	s, ok := r.sessions[id]
	r.mu.RUnlock()
	if !ok || s.closed {
		return false
	}
	// agent 在线即存活
	conn, online := agentSrv.Get(s.AgentID)
	if !online || conn == nil {
		return false
	}
	return true
}

// GetTermRegistry 供 main.go 挂 agent 掉线回调
func GetTermRegistry() *termRegistry { return termReg }

// CleanupTermByAgent 对外暴露
func CleanupTermByAgent(agentID string) { termReg.closeByAgent(agentID) }

func (s *TermSession) appendOutput(data []byte) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.buffer = append(s.buffer, data...)
	if len(s.buffer) > maxTermBuffer {
		s.buffer = s.buffer[len(s.buffer)-maxTermBuffer:]
	}
	s.UpdatedAt = time.Now()
	// 若有 WS 附着，实时推送（SSH 会话 ws=nil 跳过）
	if s.ws != nil {
		ws := s.ws // 持锁内取本地，防止 WriteMessage 期间被并发置 nil
		if err := ws.WriteMessage(websocket.BinaryMessage, data); err != nil {
			// WS 失效：标记断开但保留会话
			_ = ws.Close()
			if s.ws == ws {
				s.ws = nil
			}
		}
	}
}

// terminalWS 处理 /ws/terminal?agent_id=xxx[&session_id=yyy][&suspend=1]
func (s *Server) terminalWS(w http.ResponseWriter, r *http.Request) {
	token := r.URL.Query().Get("token")
	if token == "" {
		token = trimBearer(r.Header.Get("Authorization"))
	}
	sess := (*authSession)(nil)
	if s.auth != nil {
		ss, valid := s.auth.Verify(token)
		if !valid {
			http.Error(w, "unauthorized", http.StatusUnauthorized)
			return
		}
		sess = &authSession{user: ss.Username}
	}

	agentID := r.URL.Query().Get("agent_id")
	sessionID := r.URL.Query().Get("session_id")
	suspend := r.URL.Query().Get("suspend") == "1"

	// 复用已有会话（挂起后重新进入，或同一个 session_id 重连）
	if sessionID != "" {
		if existing := termReg.get(sessionID); existing != nil {
			ws, err := upgrader.Upgrade(w, r, nil)
			if err != nil {
				return
			}
			// SSH 会话（sshOut 非空）attach 前启动输出泵：接管 agent 输出写入 buffer，
			// 网页恢复后能收到挂起期间的历史输出（回放）+ 实时输出
			if !existing.HasPump() {
				existing.StartPump(func(data []byte) {})
			}
			s.attach(existing, ws, suspend)
			return
		}
	}

	if agentID == "" {
		http.Error(w, "missing agent_id", http.StatusBadRequest)
		return
	}
	ac, online := s.agent.Get(agentID)
	if !online {
		http.Error(w, "agent offline", http.StatusBadGateway)
		return
	}

	ws, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		return
	}

	if sessionID == "" {
		sessionID = newSessionID()
	}
	outCh, err := ac.OpenTerm(sessionID, 120, 32)
	if err != nil {
		_ = ws.WriteMessage(websocket.TextMessage, []byte("failed to open terminal: "+err.Error()))
		ws.Close()
		return
	}

	ts := &TermSession{
		ID: sessionID, AgentID: agentID,
		CreatedAt: time.Now(), UpdatedAt: time.Now(),
		suspended: suspend, cols: 120, rows: 32,
		owner: usernameOf(sess),
	}
	ts.ac = &termAgentAdapter{ac: ac}
	termReg.add(ts)

	// agent → 会话缓冲
	go func() {
		for data := range outCh {
			raw, derr := base64.StdEncoding.DecodeString(string(data))
			if derr != nil {
				raw = data
			}
			ts.appendOutput(raw)
		}
		// agent 侧结束
		ts.mu.Lock()
		ts.closed = true
		w := ts.ws
		ts.mu.Unlock()
		if w != nil {
			_ = w.WriteMessage(websocket.CloseMessage, websocket.FormatCloseMessage(1000, "session ended"))
			_ = w.Close()
		}
	}()

	// 先告知前端 session_id
	_ = ws.WriteMessage(websocket.TextMessage, []byte(`{"type":"session","session_id":"`+sessionID+`"}`))

	s.attach(ts, ws, suspend)
}

type authSession struct{ user string }

func usernameOf(s *authSession) string {
	if s == nil {
		return ""
	}
	return s.user
}

func trimBearer(h string) string {
	const p = "Bearer "
	if len(h) > len(p) && h[:len(p)] == p {
		return h[len(p):]
	}
	return h
}

type termAgentAdapter struct {
	ac interface {
		SendNoWait(msgType, sessionID string, payload any) error
		CloseTerm(sessionID string)
	}
}

func (a *termAgentAdapter) SendNoWait(t, sid string, p any) error { return a.ac.SendNoWait(t, sid, p) }
func (a *termAgentAdapter) CloseTerm(sid string)                  { a.ac.CloseTerm(sid) }
func (a *termAgentAdapter) AgentID() string                       { return "" }

// NewAgentTermAdapter 包装 agentconn.AgentConn 为 agentTermConn（SSH 网关注册挂起会话用）
func NewAgentTermAdapter(ac interface {
	SendNoWait(msgType, sessionID string, payload any) error
	CloseTerm(sessionID string)
}) agentTermConn {
	return &termAgentAdapter{ac: ac}
}

// attach 把 WS 附着到会话，回放缓冲，并双向转发
func (s *Server) attach(ts *TermSession, ws *websocket.Conn, suspend bool) {
	ts.mu.Lock()
	// 踢掉旧 WS
	if ts.ws != nil {
		_ = ts.ws.Close()
	}
	ts.ws = ws
	ts.suspended = suspend
	backlog := append([]byte(nil), ts.buffer...)
	cli := ts.ac
	sid := ts.ID
	ts.mu.Unlock()

	// 回放历史输出（SSH 会话 ws=nil 跳过）
	if ws != nil && len(backlog) > 0 {
		_ = ws.WriteMessage(websocket.BinaryMessage, backlog)
	}

	// 浏览器 → agent（SSH 会话 ws=nil，跳过 ReadMessage 防空指针）
	go func() {
		defer func() {
			ts.mu.Lock()
			if ts.ws == ws {
				ts.ws = nil
			}
			keep := ts.suspended && !ts.closed
			ts.mu.Unlock()
			if ws != nil {
				_ = ws.Close()
			}
			if !keep {
				cli.CloseTerm(sid)
				termReg.remove(sid)
			} else {
				log.Printf("[term] session %s detached (suspended, buffer=%d)", sid, len(backlog))
			}
		}()

		if ws == nil {
			// SSH 会话：无 WebSocket，不读输入；挂起时由 SSH 网关直接管理
			return
		}
		for {
			mt, msg, err := ws.ReadMessage()
			if err != nil {
				return
			}
			if mt == websocket.TextMessage {
				var ctrl struct {
					Type   string `json:"type"`
					Cols   int    `json:"cols"`
					Rows   int    `json:"rows"`
					Remark string `json:"remark,omitempty"`
				}
				if json.Unmarshal(msg, &ctrl) == nil {
					if ctrl.Type == "resize" {
						ts.mu.Lock()
						ts.cols, ts.rows = ctrl.Cols, ctrl.Rows
						ts.mu.Unlock()
					}
					if ctrl.Type == "close" {
						cli.CloseTerm(sid)
						termReg.remove(sid)
						_ = ws.WriteMessage(websocket.CloseMessage, websocket.FormatCloseMessage(1000, "closed"))
						return
					}
					if ctrl.Type == "suspend" {
						ts.mu.Lock()
						ts.suspended = ctrl.Cols == 1
						if ctrl.Remark != "" {
							ts.remark = ctrl.Remark
						}
						ts.mu.Unlock()
					}
					continue
				}
			}
			p := proto.TerminalInputPayload{DataBase64: base64.StdEncoding.EncodeToString(msg)}
			if err := cli.SendNoWait(proto.MsgTerminalInput, sid, p); err != nil {
				return
			}
		}
	}()
}

// GET /api/terminals — 列出当前用户的挂起/活跃会话
// POST /api/terminals/{id}/close — 主动关闭会话（含 agent 侧 pty）
func (s *Server) handleTerminals(w http.ResponseWriter, r *http.Request) {
	sess := sessionFrom(r.Context())
	owner := ""
	if sess != nil {
		owner = sess.Username // 传 username，list 内部同时匹配 sess: 和 ssh: 前缀
	}
	if r.Method == http.MethodGet {
		ok(w, termReg.list(owner))
		return
	}
	fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
}

func (s *Server) handleTerminalByID(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost && r.Method != http.MethodDelete {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
	id := ""
	rest := r.URL.Path
	if i := len("/api/terminals/"); len(rest) > i {
		id = rest[i:]
	}
	if j := len(id); j > 0 {
		// 去掉尾部 /close
		const suffix = "/close"
		if len(id) > len(suffix) && id[len(id)-len(suffix):] == suffix {
			id = id[:len(id)-len(suffix)]
		}
	}
	ts := termReg.get(id)
	if ts == nil {
		fail(w, http.StatusNotFound, 40401, "session not found")
		return
	}
	ts.mu.Lock()
	ws := ts.ws
	ts.ws = nil
	cli := ts.ac
	ts.mu.Unlock()
	if ws != nil {
		_ = ws.WriteMessage(websocket.CloseMessage, websocket.FormatCloseMessage(1000, "closed by admin"))
		_ = ws.Close()
	}
	cli.CloseTerm(id)
	termReg.remove(id)
	ok(w, map[string]any{"ok": true})
}
