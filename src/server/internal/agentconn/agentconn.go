package agentconn

import (
	"encoding/base64"
	"errors"
	"fmt"
	"log"
	"net"
	"sync"
	"time"

	"ctforge/server/internal/proto"
	"ctforge/server/internal/store"
)

type Server struct {
	st       *store.Store
	token    string
	mu       sync.Mutex
	sessions map[string]*AgentConn

	// 服务端身份（批准制）
	serverID   string
	serverName string
	serverIP   string
	serverPort int

	// 回调钩子（main 注入）
	OnOffline        func(agentID string)                      // 掉线触发告警
	OnAnomaly        func(agentID, kind, level, detail string) // 异常上报
	OnTaskResult     func(taskID, agentID string, ok bool, exitCode int, stdout, stderr string, durMS int64, timedOut bool)
	OnApprovalChange func() // 批准状态变更回调（用于持久化）
	OnPFData         func(p *proto.PortForwardDataPayload)
	OnOnline         func(agentID, hostname, ip string, latency float64)
}

// SetIdentity 设置服务端身份（批准制握手必需）
func (s *Server) SetIdentity(id, name, ip string, port int) {
	s.serverID = id
	s.serverName = name
	s.serverIP = ip
	s.serverPort = port
}

// ServerID 返回服务端唯一标识
func (s *Server) ServerID() string { return s.serverID }

type AgentConn struct {
	AgentID string
	conn    net.Conn

	writeMu sync.Mutex
	pending sync.Map // msgID -> chan *proto.Message

	termMu sync.Mutex
	term   map[string]chan []byte // sessionID -> output channel
}

func NewServer(st *store.Store, token string) *Server {
	return &Server{st: st, token: token, sessions: make(map[string]*AgentConn)}
}

func (s *Server) Handle(conn net.Conn) {
	defer conn.Close()
	_ = conn.SetDeadline(time.Now().Add(30 * time.Second))
	hello, err := proto.Decode(conn)
	if err != nil {
		return
	}

	// 批准制握手：agent 首次接触时发 handshake_req，服务端应答批准状态
	if hello.Type == proto.MsgHandshakeReq {
		s.handleHandshake(conn, hello)
		return
	}

	// 卸载审批请求
	if hello.Type == "uninstall_req" {
		s.handleUninstallReq(conn, hello)
		return
	}
	if hello.Type != proto.MsgRegister {
		return
	}

	var reg proto.RegisterPayload
	if err := proto.DecodePayload(hello, &reg); err != nil {
		return
	}
	if s.token != "" && hello.Token != s.token {
		_ = proto.Encode(conn, &proto.Message{Type: proto.MsgError, Payload: proto.MustPayload(proto.ErrorPayload{Error: "unauthorized"})})
		return
	}
	if reg.AgentID == "" {
		reg.AgentID = reg.Hostname
	}
	if reg.AgentID == "" {
		_ = proto.Encode(conn, &proto.Message{Type: proto.MsgError, Payload: proto.MustPayload(proto.ErrorPayload{Error: "missing agent_id"})})
		return
	}
	if reg.IP == "" {
		if ta, ok := conn.RemoteAddr().(*net.TCPAddr); ok {
			reg.IP = ta.IP.String()
		}
	}

	// 批准制校验：未批准的 agent 拒绝注册（等待批准后再来）
	approval := s.st.ApprovalOf(reg.AgentID)
	if approval == "rejected" {
		_ = proto.Encode(conn, &proto.Message{ID: hello.ID, Type: proto.MsgError, Payload: proto.MustPayload(proto.ErrorPayload{Error: "rejected by administrator"})})
		return
	}
	if approval != "approved" {
		// 未批准：回一个特殊应答让 agent 保持待批准状态，不进入监控流程
		_ = proto.Encode(conn, &proto.Message{ID: hello.ID, Type: "pending_approval", Payload: proto.MustPayload(map[string]any{
			"server_id":   s.serverID,
			"server_name": s.serverName,
			"message":     "pending approval from administrator",
		})})
		return
	}

	s.st.Register(&store.Agent{
		AgentID: reg.AgentID, Name: reg.Name, Hostname: reg.Hostname, IP: reg.IP,
		OS: reg.OS, Arch: reg.Arch, Version: reg.Version,
	})

	ac := &AgentConn{AgentID: reg.AgentID, conn: conn, term: make(map[string]chan []byte)}
	s.mu.Lock()
	// 同一 agentID 重连：关闭旧连接，避免旧连接的清理逻辑误删新记录
	if old, ok := s.sessions[reg.AgentID]; ok && old != ac {
		_ = old.conn.Close()
	}
	s.sessions[reg.AgentID] = ac
	s.mu.Unlock()
	defer func() {
		s.mu.Lock()
		// 仅当当前记录仍是本连接时才删除，防止旧连接覆盖新连接
		if cur, ok := s.sessions[reg.AgentID]; ok && cur == ac {
			delete(s.sessions, reg.AgentID)
		}
		s.mu.Unlock()
		s.st.SetOnline(reg.AgentID, false)
		// 触发离线告警
		if s.OnOffline != nil {
			s.OnOffline(reg.AgentID)
		}
	}()

	_ = proto.Encode(conn, &proto.Message{ID: hello.ID, Type: proto.MsgAck, Payload: proto.MustPayload(proto.AckPayload{OK: true, Message: "registered"})})
	if s.OnOnline != nil {
		a, _ := s.st.GetAgent(reg.AgentID)
		var lat float64
		host, ip := reg.Hostname, reg.IP
		if a != nil {
			lat = a.LatencyMs
			if a.Hostname != "" {
				host = a.Hostname
			}
		}
		s.OnOnline(reg.AgentID, host, ip, lat)
	}

	_ = conn.SetDeadline(time.Time{})
	for {
		_ = conn.SetReadDeadline(time.Now().Add(90 * time.Second))
		msg, err := proto.Decode(conn)
		if err != nil {
			return
		}
		s.dispatch(ac, msg)
	}
}

// handleHandshake 处理 agent 的握手请求。
// 规则：
//   - 已批准（且 bound_server_id 为空或等于本服务端）→ approved=true，写入系统信息
//   - 已绑定到其他服务端 → 拒绝（approved=false，reason=bound_to_other）
//   - 未知 agent → 注册为 pending，approved=false，等待管理员批准
func (s *Server) handleHandshake(conn net.Conn, hello *proto.Message) {
	var req proto.HandshakeReqPayload
	if err := proto.DecodePayload(hello, &req); err != nil {
		return
	}
	if s.token != "" && hello.Token != s.token {
		_ = proto.Encode(conn, &proto.Message{ID: hello.ID, Type: proto.MsgHandshakeAck,
			Payload: proto.MustPayload(proto.HandshakeAckPayload{Approved: false, Message: "invalid token"})})
		return
	}
	if req.AgentID == "" {
		req.AgentID = req.Hostname
	}
	if req.AgentID == "" {
		return
	}

	// 补充来源 IP
	if req.IP == "" {
		if ta, ok := conn.RemoteAddr().(*net.TCPAddr); ok {
			req.IP = ta.IP.String()
		}
	}

	ack := proto.HandshakeAckPayload{
		ServerID:   s.serverID,
		ServerName: s.serverName,
		ServerIP:   s.serverIP,
		ServerPort: s.serverPort,
	}

	state := s.st.ApprovalOf(req.AgentID)
	existing, found := s.st.GetAgent(req.AgentID)

	// 已绑定到别的服务端：拒绝
	if found && existing.BoundServerID != "" && existing.BoundServerID != s.serverID {
		ack.Approved = false
		ack.Message = "already bound to another server"
		_ = proto.Encode(conn, &proto.Message{ID: hello.ID, Type: proto.MsgHandshakeAck, Payload: proto.MustPayload(ack)})
		return
	}

	// 先落库（记录系统信息），状态待定
	a := &store.Agent{
		AgentID: req.AgentID, Name: req.Name, Hostname: req.Hostname, IP: req.IP,
		OS: req.OS, Arch: req.Arch, Version: req.Version,
		OSVersion: req.OSVersion, Kernel: req.Kernel,
		DiskTotalKB: req.DiskTotalKB, DiskUsedKB: req.DiskUsedKB, DiskPercent: req.DiskPercent,
	}
	if existing != nil {
		a.Online = existing.Online
		a.LastSeen = existing.LastSeen
		a.LastHB = existing.LastHB
		a.LastMetric = existing.LastMetric
		a.OfflineCount = existing.OfflineCount
		a.Alerts = existing.Alerts
	}

	switch state {
	case "approved":
		a.Approval = "approved"
		a.BoundServerID = s.serverID
		s.st.Register(a)
		ack.Approved = true
		ack.Message = "approved"
	case "rejected":
		a.Approval = "rejected"
		s.st.Register(a)
		ack.Approved = false
		ack.Message = "rejected by administrator"
	default:
		// 未知或 pending：注册为待批准，不采集任何数据
		a.Approval = "pending"
		a.Online = false
		s.st.Register(a)
		s.st.SetApproval(req.AgentID, "pending", "")
		ack.Approved = false
		ack.Message = "pending approval from administrator"
	}

	log.Printf("[agentconn] handshake from %s (%s): %s", req.AgentID, req.IP, ack.Message)
	_ = proto.Encode(conn, &proto.Message{ID: hello.ID, Type: proto.MsgHandshakeAck, Payload: proto.MustPayload(ack)})
}

// handleUninstallReq 处理客户端卸载审批请求。
// 仅当管理员已在控制台批准时返回 approved=true。
func (s *Server) handleUninstallReq(conn net.Conn, hello *proto.Message) {
	var req struct {
		AgentID  string `json:"agent_id"`
		Hostname string `json:"hostname"`
		Reason   string `json:"reason"`
	}
	if err := proto.DecodePayload(hello, &req); err != nil {
		return
	}
	if s.token != "" && hello.Token != s.token {
		_ = proto.Encode(conn, &proto.Message{ID: hello.ID, Type: "uninstall_ack",
			Payload: proto.MustPayload(map[string]any{"approved": false, "message": "invalid token"})})
		return
	}

	existing, ok := s.st.GetUninstallRequest(req.AgentID)
	if !ok || existing.DecidedAt == 0 {
		// 首次请求或尚未审批：落库为待审批
		s.st.AddUninstallRequest(&store.UninstallRequest{
			AgentID: req.AgentID, Hostname: req.Hostname, Reason: req.Reason,
			At: time.Now().UnixMilli(),
		})
		s.NotifyApprovalChange()
		log.Printf("[agentconn] uninstall request from %s — pending admin approval", req.AgentID)
		_ = proto.Encode(conn, &proto.Message{ID: hello.ID, Type: "uninstall_ack",
			Payload: proto.MustPayload(map[string]any{
				"approved": false,
				"message":  "waiting for administrator approval in console",
			})})
		return
	}

	if existing.Approved {
		log.Printf("[agentconn] uninstall APPROVED for %s (by %s)", req.AgentID, existing.DecidedBy)
		_ = proto.Encode(conn, &proto.Message{ID: hello.ID, Type: "uninstall_ack",
			Payload: proto.MustPayload(map[string]any{"approved": true, "message": "approved, you may uninstall"})})
		// 清理主机记录
		go func(id string) {
			time.Sleep(2 * time.Second)
			s.st.RemoveAgent(id)
		}(req.AgentID)
		return
	}
	_ = proto.Encode(conn, &proto.Message{ID: hello.ID, Type: "uninstall_ack",
		Payload: proto.MustPayload(map[string]any{"approved": false, "message": "rejected by administrator"})})
}

func (s *Server) dispatch(ac *AgentConn, m *proto.Message) {
	switch m.Type {
	case proto.MsgHeartbeat:
		var hb proto.HeartbeatPayload
		if err := proto.DecodePayload(m, &hb); err == nil {
			s.st.UpdateHeartbeat(ac.AgentID, &hb)
		}
	case proto.MsgMetrics:
		var mt proto.MetricsPayload
		if err := proto.DecodePayload(m, &mt); err == nil {
			s.st.UpdateMetrics(ac.AgentID, &mt)
		}
	case proto.MsgLog:
		var lg proto.LogPayload
		if err := proto.DecodePayload(m, &lg); err == nil {
			s.st.AddLog(ac.AgentID, store.LogEntry{Timestamp: lg.Timestamp, Level: lg.Level, Message: lg.Message})
		}
	case proto.MsgTerminalOutput:
		var out proto.TerminalOutputPayload
		if err := proto.DecodePayload(m, &out); err == nil {
			// agent 侧输出是 base64，先解码再分发（Web 终端和 SSH 网关共用同一通道）
			raw, derr := base64.StdEncoding.DecodeString(out.DataBase64)
			if derr != nil {
				raw = []byte(out.DataBase64)
			}
			ac.deliverTermOutput(m.SessionID, raw, out.Closed)
		}
	case proto.MsgPong:
		var pong proto.PongPayload
		if err := proto.DecodePayload(m, &pong); err == nil && pong.Timestamp > 0 {
			rtt := float64(time.Now().UnixMilli()-pong.Timestamp) / 1.0
			s.st.RecordLatency(ac.AgentID, rtt)
		}
	case proto.MsgPortForwardData:
		var pf proto.PortForwardDataPayload
		if err := proto.DecodePayload(m, &pf); err == nil {
			if s.OnPFData != nil {
				s.OnPFData(&pf)
			}
		}
	case proto.MsgAnomaly:
		var a proto.AnomalyPayload
		if err := proto.DecodePayload(m, &a); err == nil {
			if s.OnAnomaly != nil {
				s.OnAnomaly(ac.AgentID, a.Kind, a.Level, a.Detail)
			}
		}
	case proto.MsgCtfDaemon:
		// agent 注册后上报 ctforged 守护进程状态（运行中/已拉起/失败/未安装）
		var dr struct {
			State   string `json:"state"`
			Message string `json:"message"`
		}
		if err := proto.DecodePayload(m, &dr); err == nil {
			s.st.UpdateCtforgeDaemon(ac.AgentID, dr.State, dr.Message)
		}
	case proto.MsgTaskResult:
		// agent 用 task_result 直接回报执行结果（不是 ack），
		// 需要用同一个 msg.ID 唤醒等待中的 Request。
		if m.ID != "" {
			if ch, ok := ac.pending.LoadAndDelete(m.ID); ok {
				ch.(chan *proto.Message) <- m
			}
		}
		var tr proto.TaskResultPayload
		if err := proto.DecodePayload(m, &tr); err == nil {
			if s.OnTaskResult != nil {
				s.OnTaskResult(tr.TaskID, ac.AgentID, tr.OK, tr.ExitCode, tr.Stdout, tr.Stderr, tr.Duration, tr.TimedOut)
			}
		}
	case proto.MsgOffline:
		// agent 优雅下线：立即记录离线事件
		s.st.AddOfflineEvent(ac.AgentID)
	case proto.MsgAck, proto.MsgError:
		if m.ID != "" {
			if ch, ok := ac.pending.LoadAndDelete(m.ID); ok {
				ch.(chan *proto.Message) <- m
			}
		}
	}
}

func (ac *AgentConn) deliverTermOutput(sessionID string, data []byte, closed bool) {
	ac.termMu.Lock()
	ch, ok := ac.term[sessionID]
	if closed {
		delete(ac.term, sessionID)
	}
	ac.termMu.Unlock()
	if ok {
		select {
		case ch <- data:
		default:
		}
	}
	if closed && ok {
		close(ch)
	}
}

func (ac *AgentConn) write(m *proto.Message) error {
	ac.writeMu.Lock()
	defer ac.writeMu.Unlock()
	_ = ac.conn.SetWriteDeadline(time.Now().Add(15 * time.Second))
	return proto.Encode(ac.conn, m)
}

// Request 发送请求并等待 ack/error 应答（用于 config/file 类操作）。
func (ac *AgentConn) Request(msgType string, sessionID string, payload any, timeout time.Duration) (*proto.Message, error) {
	id := fmt.Sprintf("req-%d", time.Now().UnixNano())
	ch := make(chan *proto.Message, 1)
	ac.pending.Store(id, ch)
	defer ac.pending.Delete(id)

	m := &proto.Message{ID: id, Type: msgType, SessionID: sessionID}
	if payload != nil {
		m.Payload = proto.MustPayload(payload)
	}
	if err := ac.write(m); err != nil {
		return nil, err
	}
	select {
	case resp := <-ch:
		if resp.Type == proto.MsgError {
			var ep proto.ErrorPayload
			_ = proto.DecodePayload(resp, &ep)
			return nil, errors.New(ep.Error)
		}
		return resp, nil
	case <-time.After(timeout):
		return nil, errors.New("agent response timeout")
	}
}

// SendNoWait 发送无需应答的消息（terminal open/input/close）。
func (ac *AgentConn) SendNoWait(msgType, sessionID string, payload any) error {
	m := &proto.Message{Type: msgType, SessionID: sessionID}
	if payload != nil {
		m.Payload = proto.MustPayload(payload)
	}
	return ac.write(m)
}

// AttachTerm 获取已存在会话的输出通道（不重新发 terminal_open，不覆盖已有 channel）。
// 用于跨端接管（网页恢复 SSH 挂起会话 / SSH 恢复网页挂起会话），避免 OpenTerm 覆盖
// 导致原消费方变孤儿、会话断连。
func (ac *AgentConn) AttachTerm(sessionID string) (<-chan []byte, bool) {
	ac.termMu.Lock()
	defer ac.termMu.Unlock()
	ch, ok := ac.term[sessionID]
	return ch, ok
}

func (ac *AgentConn) OpenTerm(sessionID string, cols, rows int) (<-chan []byte, error) {
	ch := make(chan []byte, 64)
	ac.termMu.Lock()
	ac.term[sessionID] = ch
	ac.termMu.Unlock()
	if err := ac.SendNoWait(proto.MsgTerminalOpen, sessionID, proto.TerminalOpenPayload{Cols: cols, Rows: rows}); err != nil {
		ac.termMu.Lock()
		delete(ac.term, sessionID)
		ac.termMu.Unlock()
		return nil, err
	}
	return ch, nil
}

func (ac *AgentConn) CloseTerm(sessionID string) {
	ac.termMu.Lock()
	delete(ac.term, sessionID)
	ac.termMu.Unlock()
	_ = ac.SendNoWait(proto.MsgTerminalClose, sessionID, nil)
}

func (s *Server) Get(agentID string) (*AgentConn, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	ac, ok := s.sessions[agentID]
	return ac, ok
}

// StartPingLoop 每 interval 向所有在线 agent 发 ping 测延迟。
func (s *Server) StartPingLoop(interval time.Duration) {
	go func() {
		t := time.NewTicker(interval)
		defer t.Stop()
		for range t.C {
			s.mu.Lock()
			conns := make([]*AgentConn, 0, len(s.sessions))
			for _, ac := range s.sessions {
				conns = append(conns, ac)
			}
			s.mu.Unlock()
			for _, ac := range conns {
				_ = ac.SendNoWait(proto.MsgPing, "", proto.PingPayload{Timestamp: time.Now().UnixMilli()})
			}
		}
	}()
}

// NotifyApprovalChange 触发批准状态持久化回调
func (s *Server) NotifyApprovalChange() {
	if s.OnApprovalChange != nil {
		s.OnApprovalChange()
	}
}
