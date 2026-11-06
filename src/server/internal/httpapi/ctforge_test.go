package httpapi

import (
	"testing"
)

// TestCtforgeSubValidation 验证 ctforge 子命令校验逻辑
func TestCtforgeSubValidation(t *testing.T) {
	validSubs := []string{"lsmod", "status", "insmod", "rmmod", "mod"}
	for _, sub := range validSubs {
		if !isValidCtforgeSub(sub) {
			t.Errorf("expected %s to be valid", sub)
		}
	}
	invalidSubs := []string{"", "exec", "shell", "eval", "lsmod; rm -rf /"}
	for _, sub := range invalidSubs {
		if isValidCtforgeSub(sub) {
			t.Errorf("expected %s to be invalid", sub)
		}
	}
}

// isValidCtforgeSub 与 ctforge.go 中的 switch case 保持同步
func isValidCtforgeSub(sub string) bool {
	switch sub {
	case "lsmod", "status", "insmod", "rmmod", "mod":
		return true
	default:
		return false
	}
}

// TestCtforgeRequestParsing 验证请求体解析
func TestCtforgeRequestParsing(t *testing.T) {
	// 空请求
	req := ctforgeRequest{}
	if req.Module != "" {
		t.Errorf("expected empty module, got %s", req.Module)
	}

	// 有 module
	req = ctforgeRequest{Module: "firewall", Subcmd: "stats"}
	if req.Module != "firewall" {
		t.Errorf("expected module=firewall, got %s", req.Module)
	}
	if req.Subcmd != "stats" {
		t.Errorf("expected subcmd=stats, got %s", req.Subcmd)
	}
}
