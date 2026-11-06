package store

import (
	"testing"
)

func TestNewStore(t *testing.T) {
	s := New("/tmp/ctforge-test-store")
	if s == nil {
		t.Fatal("New returned nil")
	}
}

func TestAgentCRUD(t *testing.T) {
	s := New("/tmp/ctforge-test-store")

	// 初始无 agent
	agents := s.ListAgents()
	if len(agents) != 0 {
		t.Errorf("expected 0 agents, got %d", len(agents))
	}

	// 注册 agent（Register 会生成 ApprovalKey，需要 ServerID 先设置）
	a := &Agent{
		AgentID:  "test-001",
		Name:     "test-host",
		IP:       "192.168.1.1",
		Hostname: "test-host",
		Online:   true,
	}
	s.Register(a)

	// 查询
	got, found := s.GetAgent("test-001")
	if !found {
		t.Fatal("agent not found after upsert")
	}
	if got.Name != "test-host" {
		t.Errorf("Name mismatch: got %s, want test-host", got.Name)
	}

	// 列表
	agents = s.ListAgents()
	if len(agents) != 1 {
		t.Errorf("expected 1 agent, got %d", len(agents))
	}

	// 删除
	s.RemoveAgent("test-001")
	_, found = s.GetAgent("test-001")
	if found {
		t.Error("agent should be removed")
	}
}
