// Package portfwd 服务端侧端口转发管理。
// 链路：用户浏览器/本地工具 → 服务端监听端口 → 控制通道(pf_data) → agent → 目标端口
package portfwd

import (
	"crypto/rand"
	"encoding/base64"
	"encoding/hex"
	"fmt"
	"net"
	"sync"
	"time"

	"ctforge/server/internal/agentconn"
	"ctforge/server/internal/proto"
)

// Forward 一条转发规则
type Forward struct {
	ID         string `json:"id"`
	AgentID    string `json:"agent_id"`
	TargetHost string `json:"target_host"` // 服务端监听地址（默认 127.0.0.1）
	TargetPort int    `json:"target_port"` // 服务端监听端口
	LocalHost  string `json:"local_host"`  // 客户端侧目标（默认 127.0.0.1）
	LocalPort  int    `json:"local_port"`  // 客户端侧目标端口
	Active     bool   `json:"active"`
	CreatedAt  int64  `json:"created_at"`
	CreatedBy  string `json:"created_by,omitempty"`
}

type activeForward struct {
	fwd      *Forward
	listener net.Listener
	// connID → 服务端侧与客户端映射的 TCP 连接
	conns map[string]net.Conn
	mu    sync.Mutex
	agent *agentconn.AgentConn
}

type Manager struct {
	mu       sync.Mutex
	forwards map[string]*activeForward
	agentSrv *agentconn.Server
}

func NewManager(agentSrv *agentconn.Server) *Manager {
	return &Manager{forwards: make(map[string]*activeForward), agentSrv: agentSrv}
}

func newID(prefix string) string {
	b := make([]byte, 8)
	_, _ = rand.Read(b)
	return prefix + "-" + hex.EncodeToString(b)
}

// Create 创建转发：通知 agent + 服务端起监听
func (m *Manager) Create(f *Forward) error {
	ac, online := m.agentSrv.Get(f.AgentID)
	if !online {
		return fmt.Errorf("agent %s offline", f.AgentID)
	}

	if f.ID == "" {
		f.ID = newID("pf")
	}
	if f.TargetHost == "" {
		f.TargetHost = "127.0.0.1"
	}
	if f.LocalHost == "" {
		f.LocalHost = "127.0.0.1"
	}
	f.CreatedAt = time.Now().UnixMilli()

	// 1. 通知 agent 开启转发
	resp, err := ac.Request(proto.MsgPortForwardOpen, "", proto.PortForwardOpenPayload{
		ForwardID:  f.ID,
		TargetHost: f.TargetHost,
		TargetPort: f.TargetPort,
		LocalHost:  f.LocalHost,
		LocalPort:  f.LocalPort,
	}, 15*time.Second)
	if err != nil {
		return fmt.Errorf("agent pf_open failed: %v", err)
	}
	var ack proto.AckPayload
	_ = proto.DecodePayload(resp, &ack)
	if !ack.OK {
		return fmt.Errorf("agent rejected: %s", ack.Message)
	}

	// 2. 服务端起监听
	addr := fmt.Sprintf("%s:%d", f.TargetHost, f.TargetPort)
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		// 回滚 agent 侧
		_ = ac.SendNoWait(proto.MsgPortForwardClose, "", map[string]string{"forward_id": f.ID})
		return fmt.Errorf("listen %s: %v", addr, err)
	}

	af := &activeForward{
		fwd:      f,
		listener: ln,
		conns:    make(map[string]net.Conn),
		agent:    ac,
	}
	m.mu.Lock()
	m.forwards[f.ID] = af
	m.mu.Unlock()
	f.Active = true

	// 3. 接受连接循环
	go m.acceptLoop(af)
	return nil
}

func (m *Manager) acceptLoop(af *activeForward) {
	for {
		conn, err := af.listener.Accept()
		if err != nil {
			return
		}
		go m.handleConn(af, conn)
	}
}

// handleConn 新连接：分配 conn_id，通知 agent 建立映射，开始双向转发
func (m *Manager) handleConn(af *activeForward, conn net.Conn) {
	connID := newID("c")
	af.mu.Lock()
	af.conns[connID] = conn
	af.mu.Unlock()

	// 通知 agent 建连
	if err := af.agent.SendNoWait("pf_accept", "", map[string]any{
		"forward_id": af.fwd.ID,
		"conn_id":    connID,
	}); err != nil {
		m.closeConn(af, connID)
		return
	}

	// 从本地连接读 → 发给 agent
	buf := make([]byte, 8192)
	for {
		conn.SetReadDeadline(time.Now().Add(10 * time.Minute))
		n, err := conn.Read(buf)
		if err != nil {
			break
		}
		if n > 0 {
			_ = af.agent.SendNoWait(proto.MsgPortForwardData, "", proto.PortForwardDataPayload{
				ForwardID: af.fwd.ID, ConnID: connID,
				DataBase64: base64Encode(buf[:n]),
			})
		}
	}
	m.closeConn(af, connID)
	// 通知 agent 关闭
	_ = af.agent.SendNoWait(proto.MsgPortForwardData, "", proto.PortForwardDataPayload{
		ForwardID: af.fwd.ID, ConnID: connID, Closed: true,
	})
}

func (m *Manager) closeConn(af *activeForward, connID string) {
	af.mu.Lock()
	if c, ok := af.conns[connID]; ok {
		c.Close()
		delete(af.conns, connID)
	}
	af.mu.Unlock()
}

// OnData 收到 agent 回传的数据：写入对应本地连接
func (m *Manager) OnData(payload *proto.PortForwardDataPayload) {
	m.mu.Lock()
	af, ok := m.forwards[payload.ForwardID]
	m.mu.Unlock()
	if !ok {
		return
	}
	if payload.Closed {
		m.closeConn(af, payload.ConnID)
		return
	}
	data, err := base64Decode(payload.DataBase64)
	if err != nil || len(data) == 0 {
		return
	}
	af.mu.Lock()
	conn, ok := af.conns[payload.ConnID]
	af.mu.Unlock()
	if !ok {
		return
	}
	conn.SetWriteDeadline(time.Now().Add(30 * time.Second))
	_, _ = conn.Write(data)
}

// Close 关闭转发
func (m *Manager) Close(id string) error {
	m.mu.Lock()
	af, ok := m.forwards[id]
	if ok {
		delete(m.forwards, id)
	}
	m.mu.Unlock()
	if !ok {
		return fmt.Errorf("forward not found")
	}
	af.listener.Close()
	af.mu.Lock()
	for _, c := range af.conns {
		c.Close()
	}
	af.conns = make(map[string]net.Conn)
	af.mu.Unlock()
	// 通知 agent 关闭
	_ = af.agent.SendNoWait(proto.MsgPortForwardClose, "", map[string]string{"forward_id": id})
	return nil
}

// List 列出所有转发
func (m *Manager) List() []*Forward {
	m.mu.Lock()
	defer m.mu.Unlock()
	out := make([]*Forward, 0, len(m.forwards))
	for _, af := range m.forwards {
		cp := *af.fwd
		out = append(out, &cp)
	}
	return out
}

// CloseByAgent agent 掉线时清理其所有转发
func (m *Manager) CloseByAgent(agentID string) {
	m.mu.Lock()
	ids := make([]string, 0)
	for id, af := range m.forwards {
		if af.fwd.AgentID == agentID {
			ids = append(ids, id)
		}
	}
	m.mu.Unlock()
	for _, id := range ids {
		_ = m.Close(id)
	}
}

func base64Encode(b []byte) string {
	const tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
	var out []byte
	for i := 0; i < len(b); i += 3 {
		var n uint32
		rem := len(b) - i
		n = uint32(b[i]) << 16
		if rem > 1 {
			n |= uint32(b[i+1]) << 8
		}
		if rem > 2 {
			n |= uint32(b[i+2])
		}
		out = append(out, tbl[(n>>18)&63], tbl[(n>>12)&63])
		if rem > 1 {
			out = append(out, tbl[(n>>6)&63])
		} else {
			out = append(out, '=')
		}
		if rem > 2 {
			out = append(out, tbl[n&63])
		} else {
			out = append(out, '=')
		}
	}
	return string(out)
}

func base64Decode(s string) ([]byte, error) {
	return base64.StdEncoding.DecodeString(s)
}
