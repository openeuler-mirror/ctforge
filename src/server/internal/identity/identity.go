// Package identity 管理服务端唯一身份标识。
// server_id 在首次启动时生成并持久化到磁盘，之后不变；
// agent 通过 server_id 识别"是否是同一个服务端换了 IP"。
package identity

import (
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"
)

type Manager struct {
	mu         sync.Mutex
	serverID   string
	serverName string
	path       string
}

func NewManager(path, name string) (*Manager, error) {
	m := &Manager{path: path, serverName: name}
	if err := m.load(); err != nil {
		if os.IsNotExist(err) {
			if err := m.generate(); err != nil {
				return nil, err
			}
		} else {
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
	id := strings.TrimSpace(string(data))
	if id == "" {
		return os.ErrNotExist
	}
	m.serverID = id
	return nil
}

func (m *Manager) generate() error {
	b := make([]byte, 16)
	if _, err := rand.Read(b); err != nil {
		return err
	}
	// UUID v4 格式
	b[6] = (b[6] & 0x0f) | 0x40
	b[8] = (b[8] & 0x3f) | 0x80
	id := fmt.Sprintf("%08x-%04x-%04x-%04x-%012x",
		b[0:4], b[4:6], b[6:8], b[8:10], b[10:16])
	id = hex.EncodeToString(b[:4]) + "-" + hex.EncodeToString(b[4:6]) + "-" +
		hex.EncodeToString(b[6:8]) + "-" + hex.EncodeToString(b[8:10]) + "-" +
		hex.EncodeToString(b[10:16])

	m.serverID = id
	dir := filepath.Dir(m.path)
	if dir != "." {
		if err := os.MkdirAll(dir, 0755); err != nil {
			return err
		}
	}
	return os.WriteFile(m.path, []byte(id+"\n"), 0600)
}

func (m *Manager) ServerID() string   { return m.serverID }
func (m *Manager) ServerName() string { return m.serverName }
