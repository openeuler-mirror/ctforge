package proto

import (
	"bytes"
	"encoding/json"
	"testing"
)

func TestEncodeDecode(t *testing.T) {
	orig := &Message{
		ID:      "test-1",
		Type:    MsgRegister,
		Version: 1,
		AgentID: "agent-42",
		Token:   "secret-token",
		Payload: json.RawMessage(`{"hostname":"web-01","os":"linux"}`),
	}

	var buf bytes.Buffer
	if err := Encode(&buf, orig); err != nil {
		t.Fatalf("Encode failed: %v", err)
	}

	decoded, err := Decode(&buf)
	if err != nil {
		t.Fatalf("Decode failed: %v", err)
	}

	if decoded.ID != orig.ID {
		t.Errorf("ID mismatch: got %s, want %s", decoded.ID, orig.ID)
	}
	if decoded.Type != orig.Type {
		t.Errorf("Type mismatch: got %s, want %s", decoded.Type, orig.Type)
	}
	if decoded.AgentID != orig.AgentID {
		t.Errorf("AgentID mismatch: got %s, want %s", decoded.AgentID, orig.AgentID)
	}
}

func TestCtforgeMessageConstants(t *testing.T) {
	tests := []struct {
		name string
		got  string
		want string
	}{
		{"MsgCtfLsmod", MsgCtfLsmod, "ctf_lsmod"},
		{"MsgCtfInsmod", MsgCtfInsmod, "ctf_insmod"},
		{"MsgCtfRmmod", MsgCtfRmmod, "ctf_rmmod"},
		{"MsgCtfMod", MsgCtfMod, "ctf_mod"},
		{"MsgCtfStatus", MsgCtfStatus, "ctf_status"},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if tt.got != tt.want {
				t.Errorf("%s = %s, want %s", tt.name, tt.got, tt.want)
			}
		})
	}
}

func TestMaxMessageSize(t *testing.T) {
	if MaxMessageSize != 32<<20 {
		t.Errorf("MaxMessageSize = %d, want %d", MaxMessageSize, 32<<20)
	}
}

func TestPayloadRoundTrip(t *testing.T) {
	type customPayload struct {
		Module string `json:"module"`
		Subcmd string `json:"subcmd"`
	}

	p := customPayload{Module: "firewall", Subcmd: "stats"}
	m := &Message{Type: MsgCtfMod}
	m.Payload = MustPayload(p)

	var decoded customPayload
	if err := DecodePayload(m, &decoded); err != nil {
		t.Fatalf("DecodePayload failed: %v", err)
	}
	if decoded.Module != p.Module {
		t.Errorf("Module mismatch: got %s, want %s", decoded.Module, p.Module)
	}
	if decoded.Subcmd != p.Subcmd {
		t.Errorf("Subcmd mismatch: got %s, want %s", decoded.Subcmd, p.Subcmd)
	}
}
