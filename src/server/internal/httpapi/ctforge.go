package httpapi

import (
	"encoding/json"
	"net/http"
	"strings"
	"time"

	"ctforge/server/internal/auth"
	"ctforge/server/internal/proto"
)

// ==================== CTForge eBPF 框架 API ====================

// ctforgeRequest 是 ctforge 操作的通用请求体
type ctforgeRequest struct {
	Module string `json:"module,omitempty"` // 模块名（insmod/rmmod/mod 用）
	Subcmd string `json:"subcmd,omitempty"` // 子命令（mod 用）
}

// handleCtforge 路由 ctforge 子命令到对应 handler
// PATH: /api/agents/{id}/ctforge/{sub}
func (s *Server) handleCtforge(w http.ResponseWriter, r *http.Request, id string, sub string) {
	sess := sessionFrom(r.Context())
	if sess == nil || !requireRole(sess, auth.RoleOperator) {
		fail(w, http.StatusForbidden, 40300, "权限不足")
		return
	}

	// 先校验子命令合法性，避免对在线状态的旁路探测
	var msgType string
	var needModule bool
	switch sub {
	case "lsmod":
		msgType = proto.MsgCtfLsmod
	case "status":
		msgType = proto.MsgCtfStatus
	case "insmod":
		msgType = proto.MsgCtfInsmod
		needModule = true
	case "rmmod":
		msgType = proto.MsgCtfRmmod
		needModule = true
	case "mod":
		msgType = proto.MsgCtfMod
		needModule = true
	default:
		fail(w, http.StatusNotFound, 40400, "unknown ctforge subcommand: "+sub)
		return
	}

	// 解析请求体并校验必填字段
	var req ctforgeRequest
	if r.Body != nil {
		json.NewDecoder(r.Body).Decode(&req)
	}
	if needModule && req.Module == "" {
		fail(w, http.StatusBadRequest, 40001, "module name required")
		return
	}

	ac, online := s.agent.Get(id)
	if !online {
		fail(w, http.StatusBadGateway, 50201, "agent offline")
		return
	}

	// 构建 agent 消息 payload
	var payload any
	switch sub {
	case "insmod", "rmmod":
		payload = map[string]string{"module": req.Module}
	case "mod":
		payload = map[string]string{"module": req.Module, "subcmd": req.Subcmd}
	}

	// 发请求给 agent，30s 超时
	resp, err := ac.Request(msgType, "", payload, 30*time.Second)
	if err != nil {
		fail(w, http.StatusGatewayTimeout, 50401, err.Error())
		return
	}

	// 提取 payload 原样返回
	if resp.Payload != nil {
		w.Header().Set("Content-Type", "application/json")
		w.Write(resp.Payload)
		return
	}

	// agent 返回了空 payload
	ok(w, map[string]any{"result": "ok"})
}

// ctforgeHandler 解析 URL 路径分发
// 匹配: /api/agents/{id}/ctforge/{sub}
func (s *Server) ctforgeHandler(w http.ResponseWriter, r *http.Request) {
	// 路径: /api/agents/{id}/ctforge/{sub}
	parts := strings.Split(strings.TrimPrefix(r.URL.Path, "/api/agents/"), "/")
	if len(parts) < 3 || parts[1] != "ctforge" {
		fail(w, http.StatusNotFound, 40400, "not found")
		return
	}
	agentID := parts[0]
	sub := parts[2]
	s.handleCtforge(w, r, agentID, sub)
}
