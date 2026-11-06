// Package sshgateway 提供 SSH 跳板机服务（JumpServer）。
// 用户通过 SSH 客户端（ssh 命令/PuTTY）连接服务端 :2222 端口，
// 用 web 登录账户密码 或 专属公钥 认证，登录后显示可连接主机列表，
// 输入序号进入对应主机的终端会话，支持会话挂起与恢复。
package sshgateway

import (
	"crypto/rand"
	"crypto/rsa"
	"crypto/x509"
	"encoding/base64"
	"encoding/pem"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"golang.org/x/crypto/ssh"

	"ctforge/server/internal/agentconn"
	"ctforge/server/internal/auth"
	"ctforge/server/internal/httpapi"
	"ctforge/server/internal/store"
)

// Server SSH 跳板服务
type Server struct {
	st       *store.Store
	authMgr  *auth.Manager
	agentSrv *agentconn.Server
	hostKey  ssh.Signer
	sessions sync.Map // sid -> *Session
	sidSeq   int64
	mu       sync.Mutex
	started  bool
	listener net.Listener
}

// Session 一个 SSH 客户端会话
type Session struct {
	sid         string
	user        string
	owner       string // termRegistry owner = "ssh:"+user（与网页共享挂起列表）
	sshConn     ssh.Conn
	termCh      ssh.Channel
	agentID     string
	termSess    string
	outCh       <-chan []byte
	suspend     bool
	suspended   bool // 标记：SSH 断开后会话挂起保留
	wantSuspend bool // g1 命令：exit 时也注册挂起（不关闭终端）
	remark      string
	registryTS  *httpapi.TermSession // 注册到 termRegistry 的会话（输出写 buffer 供跨端回放）
	mu          sync.Mutex
}

// 挂起会话统一注册到 httpapi 的全局 termRegistry（同网页端共享，owner="ssh:"+username）
// 这样 SSH 跳板机与网页控制台看到同一份挂起会话列表。

// New 创建 SSH 网关
func New(st *store.Store, authMgr *auth.Manager, agentSrv *agentconn.Server) *Server {
	return &Server{st: st, authMgr: authMgr, agentSrv: agentSrv}
}

// Start 启动 SSH 服务监听（hostKeyPath 不存在则自动生成）
func (s *Server) Start(listenAddr, hostKeyPath string) error {
	key, err := s.loadOrCreateHostKey(hostKeyPath)
	if err != nil {
		return fmt.Errorf("host key: %w", err)
	}
	s.hostKey = key

	ln, err := net.Listen("tcp", listenAddr)
	if err != nil {
		return fmt.Errorf("listen %s: %w", listenAddr, err)
	}
	s.listener = ln
	s.started = true
	go s.acceptLoop()
	log.Printf("[sshgateway] listening on %s", listenAddr)
	return nil
}

// Stop 停止 SSH 服务
func (s *Server) Stop() {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.listener != nil {
		s.listener.Close()
	}
	s.started = false
}

// Addr 返回监听地址
func (s *Server) Addr() string {
	if s.listener == nil {
		return ""
	}
	return s.listener.Addr().String()
}

func (s *Server) loadOrCreateHostKey(path string) (ssh.Signer, error) {
	if path == "" {
		path = "/etc/ctforge/ssh_host_rsa_key"
	}
	if data, err := os.ReadFile(path); err == nil {
		signer, err := ssh.ParsePrivateKey(data)
		if err == nil {
			return signer, nil
		}
	}
	// 生成 2048 位 RSA 主机密钥
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		return nil, err
	}
	// 用 x509 编码
	der := x509.MarshalPKCS1PrivateKey(key)
	block := &pem.Block{Type: "RSA PRIVATE KEY", Bytes: der}
	pemData := pem.EncodeToMemory(block)
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		return nil, err
	}
	if err := os.WriteFile(path, pemData, 0o600); err != nil {
		return nil, err
	}
	signer, err := ssh.ParsePrivateKey(pemData)
	if err != nil {
		return nil, err
	}
	return signer, nil
}

func (s *Server) acceptLoop() {
	for {
		conn, err := s.listener.Accept()
		if err != nil {
			if s.started {
				log.Printf("[sshgateway] accept error: %v", err)
			}
			return
		}
		go s.handleConn(conn)
	}
}

// nextSid 生成会话 ID
func (s *Server) nextSid() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.sidSeq++
	return fmt.Sprintf("ssh-%d-%d", time.Now().Unix(), s.sidSeq)
}

func (s *Server) handleConn(conn net.Conn) {
	defer conn.Close()

	cfg := &ssh.ServerConfig{
		// 密码认证
		PasswordCallback: func(c ssh.ConnMetadata, password []byte) (*ssh.Permissions, error) {
			acct, ok := s.authMgr.CheckPassword(c.User(), string(password))
			if !ok {
				log.Printf("[sshgateway] password auth failed for %s from %s", c.User(), c.RemoteAddr())
				return nil, fmt.Errorf("密码错误")
			}
			log.Printf("[sshgateway] %s password auth ok from %s", c.User(), c.RemoteAddr())
			return &ssh.Permissions{
				Extensions: map[string]string{"user": acct.Username, "display": acct.DisplayName},
			}, nil
		},
		// 公钥认证（账户专属公钥）
		PublicKeyCallback: func(c ssh.ConnMetadata, key ssh.PublicKey) (*ssh.Permissions, error) {
			blob := base64.StdEncoding.EncodeToString(key.Marshal())
			acct, ok := s.authMgr.FindAccountByPublicKey(blob)
			if !ok {
				log.Printf("[sshgateway] pubkey auth failed for %s from %s", c.User(), c.RemoteAddr())
				return nil, fmt.Errorf("公钥未授权")
			}
			log.Printf("[sshgateway] %s pubkey auth ok from %s", c.User(), c.RemoteAddr())
			return &ssh.Permissions{
				Extensions: map[string]string{"user": acct.Username, "display": acct.DisplayName},
			}, nil
		},
		// 键盘交互（备用，同密码）
		KeyboardInteractiveCallback: func(c ssh.ConnMetadata, ch ssh.KeyboardInteractiveChallenge) (*ssh.Permissions, error) {
			// echo=false：密码输入不回显（修复登录时密码明文凭空显示）
			answers, err := ch(c.User(), "", []string{"Password: "}, []bool{false})
			if err != nil || len(answers) == 0 {
				return nil, fmt.Errorf("认证失败")
			}
			acct, ok := s.authMgr.CheckPassword(c.User(), answers[0])
			if !ok {
				return nil, fmt.Errorf("密码错误")
			}
			return &ssh.Permissions{
				Extensions: map[string]string{"user": acct.Username, "display": acct.DisplayName},
			}, nil
		},
		ServerVersion: "SSH-2.0-CTForge-JumpServer",
	}
	cfg.AddHostKey(s.hostKey)

	sshConn, chans, reqs, err := ssh.NewServerConn(conn, cfg)
	if err != nil {
		log.Printf("[sshgateway] handshake failed: %v", err)
		return
	}
	defer sshConn.Close()

	user := sshConn.Permissions.Extensions["user"]
	display := sshConn.Permissions.Extensions["display"]
	_ = display

	// 处理全局请求（如 keepalive）
	go ssh.DiscardRequests(reqs)

	for ch := range chans {
		if ch.ChannelType() != "session" {
			ch.Reject(ssh.UnknownChannelType, "只支持 session 通道")
			continue
		}
		go s.handleChannel(sshConn, user, ch)
	}
}

// handleChannel 处理一个 SSH session 通道（终端）
func (s *Server) handleChannel(sshConn ssh.Conn, user string, ch ssh.NewChannel) {
	channel, requests, err := ch.Accept()
	if err != nil {
		return
	}
	defer channel.Close()

	sess := &Session{
		sid:     s.nextSid(),
		user:    user,
		sshConn: sshConn,
		termCh:  channel,
	}
	s.sessions.Store(sess.sid, sess)
	defer s.sessions.Delete(sess.sid)

	// 处理通道请求（pty-req / shell / exec）
	for req := range requests {
		if req.Type == "pty-req" {
			req.Reply(true, nil)
			continue
		}
		if req.Type == "shell" {
			req.Reply(true, nil)
			// interactiveLoop 返回后立即关闭 SSH 连接，客户端看到连接关闭真正退出
			s.interactiveLoop(sess)
			sshConn.Close()
			channel.Close()
			continue
		}
		if req.Type == "exec" {
			// 不支持直接 exec，提示用交互模式
			req.Reply(true, nil)
			fmt.Fprintln(channel, "CTForge JumpServer: 请使用交互式 shell（不加命令），输入序号连接主机")
			channel.Close()
			continue
		}
		req.Reply(false, nil)
	}
}

// terminalSize 从 pty-req 解析终端尺寸
type ptyReq struct {
	Term   string
	Cols   uint32
	Rows   uint32
	Width  uint32
	Height uint32
}

// onlineAgents 返回在线已批准主机列表（按 hostname 排序）
func (s *Server) onlineAgents() []*store.Agent {
	list := s.st.ListAgents()
	var out []*store.Agent
	for _, a := range list {
		if a.Online && a.Approval == "approved" {
			out = append(out, a)
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Hostname < out[j].Hostname })
	return out
}

// showBanner 显示欢迎横幅
func showBanner(w io.Writer, user string) {
	fmt.Fprintf(w, "\r\n\x1b[1;36m╔══════════════════════════════════════════════════════════════╗\x1b[0m\r\n")
	fmt.Fprintf(w, "\x1b[1;36m║\x1b[0m  \x1b[1;33mCTForge JumpServer\x1b[0m  \x1b[2mSSH 跳板机\x1b[0m                      \x1b[1;36m║\x1b[0m\r\n")
	fmt.Fprintf(w, "\x1b[1;36m╚══════════════════════════════════════════════════════════════╝\x1b[0m\r\n")
	fmt.Fprintf(w, "  欢迎, \x1b[1;32m%s\x1b[0m\r\n\r\n", user)
}

// showHostListPaged 分页显示主机列表 + 挂起列表（同网页共享）
func (s *Server) showHostListPaged(w io.Writer, user string, pageAgents []*store.Agent, page, totalPages, total int) {
	fmt.Fprintf(w, "\x1b[1;35m  在线主机（%d 台 · 第 %d/%d 页）\x1b[0m\r\n\r\n", total, page+1, totalPages)
	fmt.Fprintf(w, "  \x1b[1m%-4s %-22s %-15s %s\x1b[0m\r\n", "序号", "名称", "IP", "备注")
	fmt.Fprintf(w, "  %s\r\n", strings.Repeat("-", 70))
	for i, a := range pageAgents {
		name := a.Hostname
		if a.Name != "" && a.Name != a.Hostname {
			name = a.Name
		}
		remark := a.Remark
		if remark == "" {
			remark = "-"
		}
		fmt.Fprintf(w, "  %-4d %-22s %-15s %s\r\n", i+1, truncate(name, 22), a.IP, truncate(remark, 24))
	}
	fmt.Fprintf(w, "  %s\r\n\r\n", strings.Repeat("-", 70))

	// 挂起列表（同网页 termRegistry 共享，owner="ssh:"+user）
	fmt.Fprintf(w, "\x1b[1;35m  挂起的会话\x1b[0m\r\n")
	// 挂起列表（含 SSH + 网页，同用户共享），用简洁序号 resume
	sus := s.listSuspendedUnified(user)
	if len(sus) == 0 {
		fmt.Fprintf(w, "    （无）\r\n")
	} else {
		fmt.Fprintf(w, "  \x1b[1m%-4s %-30s %-12s %-6s %s\x1b[0m\r\n", "序号", "主机", "备注", "状态", "来源")
		for i, sc := range sus {
			remark, _ := sc["remark"].(string)
			if remark == "" {
				remark = "-"
			}
			src := "SSH"
			if o, _ := sc["owner"].(string); strings.HasPrefix(o, "sess:") {
				src = "网页"
			}
			host, _ := sc["agent_id"].(string)
			// 存活检测：agent 在线则显示"活跃"，离线标记"离线"
			status := "\x1b[32m活跃\x1b[0m"
			if _, online := s.agentSrv.Get(host); !online {
				status = "\x1b[31m离线\x1b[0m"
			}
			fmt.Fprintf(w, "  %-4d %-30s %-12s %-6s %s\r\n", i+1, host, remark, status, src)
		}
		fmt.Fprintf(w, "  \x1b[2m恢复: resume 序号 · 删除: delete 序号\x1b[0m\r\n")
	}
	fmt.Fprintf(w, "\r\n  \x1b[2m序号 连接 · g1 备注 挂起连接 · resume 序号 恢复 · n/p 翻页 · q 退出\x1b[0m\r\n\r\n")
	fmt.Fprintf(w, "  选择主机 > ")
}

func truncate(s string, n int) string {
	r := []rune(s)
	if len(r) <= n {
		return s
	}
	return string(r[:n-1]) + "…"
}

// readLine 从 SSH 通道读一行
// readLine 读一行；echo=true 回显字符（主机列表序号输入），echo=false 不回显（密码输入）
func readLine(r io.Reader, w io.Writer, prompt string, echo bool) (string, error) {
	var buf []byte
	one := make([]byte, 1)
	for {
		n, err := r.Read(one)
		if err != nil || n == 0 {
			return "", err
		}
		c := one[0]
		if c == '\n' || c == '\r' {
			if w != nil && echo {
				w.Write([]byte("\r\n"))
			}
			return strings.TrimSpace(string(buf)), nil
		}
		if c == 0x03 { // Ctrl+C
			if w != nil && echo {
				w.Write([]byte("^C\r\n"))
			}
			return "", fmt.Errorf("cancelled")
		}
		if c == 0x7f || c == 0x08 { // Backspace
			if len(buf) > 0 {
				buf = buf[:len(buf)-1]
				if w != nil && echo {
					w.Write([]byte("\b \b")) // 回显退格
				}
			}
			continue
		}
		if c == 0x04 { // Ctrl+D EOF
			if len(buf) == 0 {
				return "", io.EOF
			}
			continue
		}
		buf = append(buf, c)
		// 回显可打印字符（echo=true 时）
		if w != nil && echo && c >= 0x20 && c < 0x7f {
			w.Write([]byte{c})
		}
	}
}

// interactiveLoop SSH 登录后的交互主循环
func (s *Server) interactiveLoop(sess *Session) {
	ch := sess.termCh
	showBanner(ch, sess.user)

	page := 0
	const pageSize = 20
	for {
		agents := s.onlineAgents()
		totalPages := (len(agents) + pageSize - 1) / pageSize
		if totalPages == 0 {
			totalPages = 1
		}
		// 分页切片
		start := page * pageSize
		end := start + pageSize
		if start > len(agents) {
			start = 0
			page = 0
		}
		if end > len(agents) {
			end = len(agents)
		}
		pageAgents := agents[start:end]

		s.showHostListPaged(ch, sess.user, pageAgents, page, totalPages, len(agents))
		input, err := readLine(ch, ch, "", true)
		if err != nil {
			// Ctrl+C / Ctrl+D / 连接断开：退出跳板机
			fmt.Fprintf(ch, "\r\n  再见！\r\n")
			return
		}
		if input == "" {
			continue
		}
		lower := strings.ToLower(input)
		// 刷新列表
		if lower == "l" || lower == "r" {
			continue
		}
		// 退出：q / exit / quit / logout
		if lower == "q" || lower == "exit" || lower == "quit" || lower == "logout" {
			fmt.Fprintf(ch, "\r\n  再见！\r\n")
			return
		}
		// 翻页：n 下一页 / p 上一页
		if lower == "n" || lower == "next" {
			if page < totalPages-1 {
				page++
			} else {
				fmt.Fprintf(ch, "\r\n  已是最后一页\r\n")
			}
			continue
		}
		if lower == "p" || lower == "prev" {
			if page > 0 {
				page--
			} else {
				fmt.Fprintf(ch, "\r\n  已是第一页\r\n")
			}
			continue
		}
		// delete 删除挂起会话（delete 1 = 删除序号 1 的会话，同时关闭 agent 终端）
		if strings.HasPrefix(lower, "delete") {
			parts := strings.Fields(lower)
			if len(parts) >= 2 {
				target := parts[1]
				if idx, err := strconv.Atoi(target); err == nil {
					sus := s.listSuspendedUnified(sess.user)
					if idx >= 1 && idx <= len(sus) {
						target, _ = sus[idx-1]["id"].(string)
					} else {
						fmt.Fprintf(ch, "\r\n  \x1b[31m无效序号，共 %d 个挂起会话\x1b[0m\r\n\r\n", len(sus))
						continue
					}
				}
				if s.deleteSuspended(sess.user, target) {
					fmt.Fprintf(ch, "\r\n  已删除挂起会话 %s\r\n\r\n", target)
				} else {
					fmt.Fprintf(ch, "\r\n  \x1b[31m无该挂起会话\x1b[0m\r\n\r\n")
				}
			} else {
				fmt.Fprintf(ch, "\r\n  用法: delete <序号或session_id>\r\n\r\n")
			}
			continue
		}
		// resume 恢复挂起会话（网页+SSH 共享同一份）
		if strings.HasPrefix(lower, "resume") {
			parts := strings.Fields(lower)
			if len(parts) >= 2 {
				target := parts[1]
				// 支持序号：resume 1 = 恢复第 1 个挂起会话
				if idx, err := strconv.Atoi(target); err == nil {
					sus := s.listSuspendedUnified(sess.user)
					if idx >= 1 && idx <= len(sus) {
						target, _ = sus[idx-1]["id"].(string)
					} else {
						fmt.Fprintf(ch, "\r\n  \x1b[31m无效序号，共 %d 个挂起会话\x1b[0m\r\n\r\n", len(sus))
						continue
					}
				}
				remark := ""
				if len(parts) >= 3 {
					remark = strings.Join(parts[2:], " ")
				}
				if s.resumeToAgent(sess, target, remark) {
					fmt.Fprintf(ch, "\r\n  已恢复挂起会话\r\n\r\n")
				} else {
					fmt.Fprintf(ch, "\r\n  \x1b[31m无该挂起会话或已过期\x1b[0m\r\n\r\n")
				}
			} else {
				fmt.Fprintf(ch, "\r\n  用法: resume <序号或agent_id> [备注]\r\n\r\n")
			}
			continue
		}
		// g1 xxxx：连接 1 号机并挂起（xxxx=备注）
		if strings.HasPrefix(lower, "g") && len(lower) > 1 {
			rest := lower[1:]
			fields := strings.Fields(rest)
			if len(fields) >= 1 {
				if idx, err := strconv.Atoi(fields[0]); err == nil && idx >= 1 && idx <= len(pageAgents) {
					target := pageAgents[idx-1]
					remark := ""
					if len(fields) >= 2 {
						remark = strings.Join(fields[1:], " ")
					}
					sess.remark = remark
					sess.wantSuspend = true // exit 时也挂起
					fmt.Fprintf(ch, "\r\n  正在挂起连接 \x1b[1;32m%s (%s)\x1b[0m [备注:%s]...\r\n\r\n", target.Hostname, target.IP, remark)
					err = s.connectAgent(sess, target)
					sess.wantSuspend = false
					if err != nil {
						fmt.Fprintf(ch, "\r\n  \x1b[31m连接失败: %v\x1b[0m\r\n\r\n", err)
					}
					fmt.Fprintf(ch, "\r\n  \x1b[33m会话已挂起（备注:%s），返回主机列表\x1b[0m\r\n\r\n", sess.remark)
					continue
				}
			}
		}
		// 数字：直接连接（不挂起）
		idx, err := strconv.Atoi(lower)
		if err != nil || idx < 1 || idx > len(pageAgents) {
			fmt.Fprintf(ch, "\r\n  \x1b[31m无效序号/命令，输入 n/p 翻页 q 退出 g1 备注 挂起连接 resume 恢复\x1b[0m\r\n\r\n")
			continue
		}
		target := pageAgents[idx-1]
		sess.remark = ""
		fmt.Fprintf(ch, "\r\n  正在连接 \x1b[1;32m%s (%s)\x1b[0m ...\r\n\r\n", target.Hostname, target.IP)
		err = s.connectAgent(sess, target)
		if err != nil {
			fmt.Fprintf(ch, "\r\n  \x1b[31m连接失败: %v\x1b[0m\r\n\r\n", err)
			continue
		}
		fmt.Fprintf(ch, "\r\n  \x1b[33m会话已结束，返回主机列表\x1b[0m\r\n\r\n")
	}
}

// findAgent 按 ID 查找 agent
func (s *Server) findAgent(agentID string) *store.Agent {
	list := s.st.ListAgents()
	for _, a := range list {
		if a.AgentID == agentID {
			return a
		}
	}
	return nil
}

// resumeToAgent 从 termRegistry 恢复挂起会话并进入交互
func (s *Server) resumeToAgent(sess *Session, agentIDOrSessID, remark string) bool {
	sessID := s.resumeSuspended(sess, agentIDOrSessID, remark)
	if sessID == "" {
		return false
	}
	// 从 termRegistry 拿会话，重新 OpenTerm 复用同一 session id（agent 侧 pty 还在）
	conn, online := s.agentSrv.Get(sess.agentID)
	if !online {
		return false
	}
	// 优先复用已存在的输出通道（跨端接管），避免 OpenTerm 覆盖导致原消费方断连
	var outCh <-chan []byte
	if ch, ok := conn.AttachTerm(sess.termSess); ok && ch != nil {
		outCh = ch
	} else {
		// 通道不存在（agent 侧已重建/首个消费者），才真正 OpenTerm
		type openResult struct {
			ch  <-chan []byte
			err error
		}
		resCh := make(chan openResult, 1)
		go func() {
			ch, err := conn.OpenTerm(sess.termSess, 120, 32)
			resCh <- openResult{ch, err}
		}()
		select {
		case r := <-resCh:
			if r.err != nil {
				return false
			}
			outCh = r.ch
		case <-time.After(5 * time.Second):
			return false // OpenTerm 超时，会话可能已死
		}
	}
	sess.outCh = outCh
	sess.termCh.Write([]byte("\x1b[?1049h"))
	done := make(chan struct{})
	inputDone := make(chan struct{})
	defer close(inputDone)
	go func() {
		for data := range outCh {
			sess.termCh.Write(data)
		}
		close(done)
	}()
	go func() {
		buf := make([]byte, 4096)
		for {
			select {
			case <-inputDone:
				return
			default:
			}
			n, err := sess.termCh.Read(buf)
			if err != nil || n == 0 {
				return
			}
			conn.SendNoWait("terminal_input", sess.termSess,
				map[string]any{"data_base64": base64.StdEncoding.EncodeToString(buf[:n])})
		}
	}()
	<-done
	sess.termCh.Write([]byte("\x1b[?1049l"))
	// 恢复后不删除 termRegistry 记录，让用户可再次 resume（同网页行为）
	return true
}

// connectAgentSuspend：g1 备注 挂起连接——打开终端后立即挂起注册到 termRegistry，返回列表
func (s *Server) connectAgentSuspend(sess *Session, agent *store.Agent) error {
	conn, online := s.agentSrv.Get(agent.AgentID)
	if !online {
		return fmt.Errorf("主机 %s 已离线", agent.Hostname)
	}
	sess.termSess = fmt.Sprintf("ssh-%s-%d", sess.sid, time.Now().UnixNano())
	sess.agentID = agent.AgentID
	outCh, err := conn.OpenTerm(sess.termSess, 120, 32)
	if err != nil {
		return fmt.Errorf("打开终端失败: %w", err)
	}
	sess.outCh = outCh
	// 立即挂起：注册带 ac 的 TermSession（owner=裸 username 与网页一致），输出写 buffer 供恢复回放
	ts := httpapi.NewTermSessionForSuspend(sess.termSess, agent.AgentID, "ssh:"+sess.user, sess.remark, httpapi.NewAgentTermAdapter(conn), outCh)
	httpapi.GetTermRegistry().Add(ts)
	sess.registryTS = ts
	// 输出转发：写 SSH 通道 + 写 buffer（resume 时可回放历史输出）
	go func() {
		for data := range outCh {
			ts.AppendOutput(data)
		}
	}()
	fmt.Fprintf(sess.termCh, "\r\n  \x1b[32m已挂起会话 [备注:%s]，输入 resume %s 恢复\x1b[0m\r\n", sess.remark, sess.termSess)
	return nil
}

// connectAgent 建立到目标 agent 的终端会话
func (s *Server) connectAgent(sess *Session, agent *store.Agent) error {
	conn, online := s.agentSrv.Get(agent.AgentID)
	if !online {
		return fmt.Errorf("主机 %s 已离线", agent.Hostname)
	}

	// 生成终端会话 ID
	sess.termSess = fmt.Sprintf("ssh-%s-%s", sess.sid, agent.AgentID)
	sess.agentID = agent.AgentID

	// 打开终端（80x24）
	outCh, err := conn.OpenTerm(sess.termSess, 120, 32)
	if err != nil {
		return fmt.Errorf("打开终端失败: %w", err)
	}
	sess.outCh = outCh

	// g1 挂起模式：进入交互前注册 termRegistry，输出持续写 buffer，网页/resume 可看到
	if sess.wantSuspend {
		ts := httpapi.NewTermSessionForSuspend(sess.termSess, sess.agentID, "ssh:"+sess.user, sess.remark, httpapi.NewAgentTermAdapter(conn), sess.outCh)
		httpapi.GetTermRegistry().Add(ts)
		sess.registryTS = ts
	}

	// 设置终端窗口大小
	sess.termCh.Write([]byte("\x1b[?1049h")) // 备用屏幕

	// inputDone/outputDone 在 connectAgent 返回时通知所有转发 goroutine 停止，防止泄漏到主机列表界面
	inputDone := make(chan struct{})
	defer close(inputDone)
	// 输出转发：agent 终端输出 → SSH 客户端
	done := make(chan struct{})
	go func() {
		for {
			select {
			case <-inputDone:
				return
			case data, ok := <-outCh:
				if !ok {
					close(done)
					return
				}
				sess.termCh.Write(data)
				// 写 termRegistry buffer，网页/resume 恢复时可回放历史输出
				if sess.registryTS != nil {
					sess.registryTS.AppendOutput(data)
				}
			}
		}
	}()

	// 输入转发：SSH 客户端 → agent 终端
	// connectAgent 返回时通过 inputDone 通知该 goroutine 停止读取
	errCh := make(chan error, 1)
	go func() {
		buf := make([]byte, 4096)
		for {
			select {
			case <-inputDone:
				return
			default:
			}
			n, err := sess.termCh.Read(buf)
			if err != nil {
				errCh <- err
				return
			}
			if n == 0 {
				continue
			}
			payload := map[string]any{"data_base64": base64.StdEncoding.EncodeToString(buf[:n])}
			conn.SendNoWait("terminal_input", sess.termSess, payload)
		}
	}()

	// 等待会话结束：输出关闭(exit) / 输入断开 / SSH连接断开（修复网页跳板机无法退出）
	var endReason string
	sshDone := make(chan struct{})
	go func() {
		sess.sshConn.Wait()
		close(sshDone)
	}()
	select {
	case <-done:
		endReason = "exit"
	case <-errCh:
		endReason = "disconnect"
	case <-sshDone:
		// SSH 连接断开（用户关窗口/网络断），等价 disconnect 挂起
		if sess.wantSuspend {
			endReason = "exit"
		} else {
			endReason = "disconnect"
		}
	}

	if endReason == "disconnect" {
		// SSH 客户端断开：挂起会话保留（g1 已注册则复用，避免覆盖带 buffer 的会话）
		if sess.registryTS == nil {
			ts := httpapi.NewTermSessionForSuspend(sess.termSess, sess.agentID, "ssh:"+sess.user, sess.remark, httpapi.NewAgentTermAdapter(conn), sess.outCh)
			httpapi.GetTermRegistry().Add(ts)
			sess.registryTS = ts
		}
		sess.registryTS.SetSuspended(true)
		sess.suspended = true
		// 不关闭 agent 终端，让会话在 agent 侧继续运行
		sess.termCh.Write([]byte("\x1b[?1049l"))
		return nil
	}

	// 用户 exit
	if sess.wantSuspend {
		// g1 挂起模式：exit 保留挂起（复用已注册的会话，避免覆盖 buffer）
		if sess.registryTS == nil {
			ts := httpapi.NewTermSessionForSuspend(sess.termSess, sess.agentID, "ssh:"+sess.user, sess.remark, httpapi.NewAgentTermAdapter(conn), sess.outCh)
			httpapi.GetTermRegistry().Add(ts)
			sess.registryTS = ts
		}
		sess.registryTS.SetSuspended(true)
		sess.suspended = true
		sess.termCh.Write([]byte("\x1b[?1049l"))
		return nil
	}
	// 普通模式：关闭终端
	conn.SendNoWait("terminal_close", sess.termSess, map[string]any{})
	sess.termCh.Write([]byte("\x1b[?1049l"))
	return nil
}

// deleteSuspended 删除挂起会话（关闭 agent 终端 + 从注册表移除）
func (s *Server) deleteSuspended(user, sessID string) bool {
	reg := httpapi.GetTermRegistry()
	for _, sc := range reg.ListByOwner("ssh:" + user) {
		id, _ := sc["id"].(string)
		if id == sessID {
			// 关闭 agent 侧终端
			if aid, _ := sc["agent_id"].(string); aid != "" {
				if conn, online := s.agentSrv.Get(aid); online {
					conn.SendNoWait("terminal_close", sessID, map[string]any{})
				}
			}
			reg.Remove(sessID)
			return true
		}
	}
	// 也检查网页会话
	for _, sc := range reg.ListByOwner("sess:" + user) {
		id, _ := sc["id"].(string)
		if id == sessID {
			if aid, _ := sc["agent_id"].(string); aid != "" {
				if conn, online := s.agentSrv.Get(aid); online {
					conn.SendNoWait("terminal_close", sessID, map[string]any{})
				}
			}
			reg.Remove(sessID)
			return true
		}
	}
	return false
}

// listSuspendedUnified 列出同用户所有挂起会话（SSH + 网页共享），按创建时间倒序
func (s *Server) listSuspendedUnified(user string) []map[string]any {
	reg := httpapi.GetTermRegistry()
	// ListByOwner 前缀匹配查不到 ssh:admin（不以 admin 开头），查全部按 username 过滤
	all := reg.ListByOwner("")
	// 过滤：owner 为裸 username 或 ssh:username 都属该用户
	var owned []map[string]any
	for _, sc := range all {
		o, _ := sc["owner"].(string)
		if o == user || o == "ssh:"+user {
			owned = append(owned, sc)
		}
	}
	all = owned
	// 只保留挂起的
	var out []map[string]any
	for _, sc := range all {
		if sus, _ := sc["suspended"].(bool); sus {
			out = append(out, sc)
		}
	}
	// 按创建时间倒序（最新在前）
	for i := 0; i < len(out); i++ {
		for j := i + 1; j < len(out); j++ {
			ti, _ := out[i]["created_at"].(int64)
			tj, _ := out[j]["created_at"].(int64)
			if tj > ti {
				out[i], out[j] = out[j], out[i]
			}
		}
	}
	return out
}

// resumeSuspended 从 termRegistry 恢复挂起会话（同网页共享），返回会话 ID
func (s *Server) resumeSuspended(sess *Session, agentID, remark string) string {
	reg := httpapi.GetTermRegistry()
	// 查全部会话按末尾 username 过滤（含网页 sess:admin 和 SSH ssh:admin）
	for _, sc := range reg.ListByOwner("") {
		id, _ := sc["id"].(string)
		aid, _ := sc["agent_id"].(string)
		rmk, _ := sc["remark"].(string)
		o, _ := sc["owner"].(string)
		// owner 末尾相等即匹配（ssh:admin / sess:admin / admin）
		want := o
		if i := strings.LastIndexByte(o, ':'); i >= 0 {
			want = o[i+1:]
		}
		if !strings.EqualFold(want, sess.user) {
			continue
		}
		if aid == agentID || id == agentID {
			sess.termSess = id
			sess.agentID = aid
			if remark != "" {
				sess.remark = remark
			} else {
				sess.remark = rmk
			}
			return id
		}
	}
	return ""
}

// Sessions 返回活跃 SSH 会话列表（管理用）
func (s *Server) Sessions() []map[string]any {
	var out []map[string]any
	s.sessions.Range(func(k, v any) bool {
		sess := v.(*Session)
		out = append(out, map[string]any{
			"sid":      sess.sid,
			"user":     sess.user,
			"agent_id": sess.agentID,
		})
		return true
	})
	return out
}
