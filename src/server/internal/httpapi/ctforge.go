package httpapi

import (
	"encoding/json"
	"fmt"
	"net/http"
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

	// 解析请求体并校验必填字段（限制 body 大小防止内存耗尽攻击）
	var req ctforgeRequest
	if r.Body != nil {
		r.Body = http.MaxBytesReader(w, r.Body, 1<<20) // 1MB
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

// handleCtforgeDeploy 通过 agent 下发命令在目标主机部署 ctforged
// 复用 cmd_exec 通道：下载部署脚本并以 --ctforged-only 模式执行
// POST /api/agents/{id}/ctforge/deploy
func (s *Server) handleCtforgeDeploy(w http.ResponseWriter, r *http.Request, id string) {
	if r.Method != http.MethodPost {
		fail(w, http.StatusMethodNotAllowed, 40500, "method not allowed")
		return
	}
	sess := sessionFrom(r.Context())
	if sess == nil || !requireRole(sess, auth.RoleOperator) {
		fail(w, http.StatusForbidden, 40300, "权限不足")
		return
	}

	ac, online := s.agent.Get(id)
	if !online {
		fail(w, http.StatusBadGateway, 50201, "agent offline")
		return
	}

	// 生成部署命令：优先使用批准管理保存的地址
	host := ""
	if s.deploy != nil && s.deploy.ServerHost != "" {
		host = s.deploy.ServerHost
	} else {
		host = detectOutboundIP()
	}
	cmd := fmt.Sprintf(
		"curl -fsSL http://%s:8080/api/deploy/script.sh | sh -s -- --ctforged-only",
		host)

	resp, err := ac.Request(proto.MsgCmdExec, "", proto.CmdExecPayload{
		Command:    cmd,
		TimeoutSec: 120,
		Shell:      "/bin/sh",
	}, 130*time.Second)
	if err != nil {
		fail(w, http.StatusGatewayTimeout, 50401, err.Error())
		return
	}

	var res proto.CmdExecResult
	if err := proto.DecodePayload(resp, &res); err != nil {
		fail(w, http.StatusInternalServerError, 50001, err.Error())
		return
	}
	s.auth.AddAudit(sess.Username, "ctforge_deploy", id, clientIP(r), res.OK, cmd)
	ok(w, res)
}
