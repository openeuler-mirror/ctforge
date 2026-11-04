// Package auth 提供账户管理与会话认证。
// 账户存于 JSON 文件（bcrypt 哈希），会话用服务端随机 token（内存）。
// 首次启动若不存在账户文件，自动创建 admin / admin123 并要求改密。
package auth

import (
	"crypto/rand"
	"crypto/subtle"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"golang.org/x/crypto/bcrypt"
)

type Role string

const (
	RoleAdmin    Role = "admin"    // 全部权限
	RoleOperator Role = "operator" // 运维操作，不能管账户
	RoleViewer   Role = "viewer"   // 只读
)

type Account struct {
	Username    string    `json:"username"`
	Password    string    `json:"password"` // bcrypt hash
	Role        Role      `json:"role"`
	DisplayName string    `json:"display_name,omitempty"`
	MustChange  bool      `json:"must_change,omitempty"`
	Disabled    bool      `json:"disabled"`
	CreatedAt   time.Time `json:"created_at"`
	LastLogin   time.Time `json:"last_login,omitempty"`

	// SSH 跳板登录专属公钥列表（OpenSSH authorized_keys 格式，一行一个）
	SSHPublicKeys []string `json:"ssh_public_keys,omitempty"`
}

type Session struct {
	Token     string
	Username  string
	Role      Role
	CreatedAt time.Time
	ExpiresAt time.Time
}

type AuditEntry struct {
	Time   time.Time `json:"time"`
	User   string    `json:"user"`
	Action string    `json:"action"`
	Target string    `json:"target"`
	IP     string    `json:"ip"`
	OK     bool      `json:"ok"`
	Detail string    `json:"detail,omitempty"`
}

type Manager struct {
	mu       sync.RWMutex
	path     string
	accounts map[string]*Account
	sessions map[string]*Session
	audit    []AuditEntry
	auditMax int
}

func NewManager(path string) (*Manager, error) {
	m := &Manager{
		path:     path,
		accounts: make(map[string]*Account),
		sessions: make(map[string]*Session),
		auditMax: 2000,
	}
	if err := m.load(); err != nil {
		if !errors.Is(err, os.ErrNotExist) {
			return nil, err
		}
		// 首次启动：创建默认管理员
		if err := m.createDefaultAdmin(); err != nil {
			return nil, err
		}
	}
	go m.gcSessions()
	return m, nil
}

func (m *Manager) createDefaultAdmin() error {
	hash, err := bcrypt.GenerateFromPassword([]byte("admin123"), bcrypt.DefaultCost)
	if err != nil {
		return err
	}
	m.accounts["admin"] = &Account{
		Username:    "admin",
		Password:    string(hash),
		Role:        RoleAdmin,
		DisplayName: "Administrator",
		MustChange:  true,
		CreatedAt:   time.Now(),
	}
	return m.save()
}

func (m *Manager) load() error {
	data, err := os.ReadFile(m.path)
	if err != nil {
		return err
	}
	var list []*Account
	if err := json.Unmarshal(data, &list); err != nil {
		return err
	}
	for _, a := range list {
		m.accounts[a.Username] = a
	}
	return nil
}

func (m *Manager) save() error {
	list := make([]*Account, 0, len(m.accounts))
	for _, a := range m.accounts {
		list = append(list, a)
	}
	data, err := json.MarshalIndent(list, "", "  ")
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(m.path), 0755); err != nil && filepath.Dir(m.path) != "." {
		return err
	}
	tmp := m.path + ".tmp"
	if err := os.WriteFile(tmp, data, 0600); err != nil {
		return err
	}
	return os.Rename(tmp, m.path)
}

func newToken() string {
	b := make([]byte, 32)
	_, _ = rand.Read(b)
	return hex.EncodeToString(b)
}

// Login 校验密码并创建会话。
// CheckPassword 仅校验用户名密码（不签发 web session，用于 SSH 登录）
func (m *Manager) CheckPassword(username, password string) (*Account, bool) {
	m.mu.RLock()
	defer m.mu.RUnlock()
	a, ok := m.accounts[username]
	if !ok || a.Disabled {
		return nil, false
	}
	if bcrypt.CompareHashAndPassword([]byte(a.Password), []byte(password)) != nil {
		return nil, false
	}
	return a, true
}

// FindAccountByPublicKey 按 SSH 公钥内容匹配账户（用于 SSH 公钥登录）
func (m *Manager) FindAccountByPublicKey(pubKeyBlob string) (*Account, bool) {
	m.mu.RLock()
	defer m.mu.RUnlock()
	for _, a := range m.accounts {
		if a.Disabled {
			continue
		}
		for _, k := range a.SSHPublicKeys {
			fields := strings.Fields(k)
			if len(fields) >= 2 && fields[1] == pubKeyBlob {
				return a, true
			}
		}
	}
	return nil, false
}

// SetSSHPublicKeys 设置账户的 SSH 公钥列表（整体替换）
func (m *Manager) SetSSHPublicKeys(username string, keys []string, ip string) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	a, ok := m.accounts[username]
	if !ok {
		return fmt.Errorf("账户不存在")
	}
	a.SSHPublicKeys = keys
	m.auditLocked(username, "ssh_keys_update", "", ip, true, fmt.Sprintf("%d keys", len(keys)))
	return m.saveLocked()
}

// GetSSHPublicKeys 读取账户的 SSH 公钥列表
func (m *Manager) GetSSHPublicKeys(username string) []string {
	m.mu.RLock()
	defer m.mu.RUnlock()
	if a, ok := m.accounts[username]; ok {
		out := make([]string, len(a.SSHPublicKeys))
		copy(out, a.SSHPublicKeys)
		return out
	}
	return nil
}

func (m *Manager) Login(username, password, ip string) (*Session, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	a, ok := m.accounts[username]
	if !ok {
		m.auditLocked(username, "login", "", ip, false, "user not found")
		return nil, errors.New("用户名或密码错误")
	}
	if a.Disabled {
		m.auditLocked(username, "login", "", ip, false, "account disabled")
		return nil, errors.New("账户已被禁用")
	}
	if bcrypt.CompareHashAndPassword([]byte(a.Password), []byte(password)) != nil {
		m.auditLocked(username, "login", "", ip, false, "bad password")
		return nil, errors.New("用户名或密码错误")
	}
	a.LastLogin = time.Now()
	s := &Session{
		Token:     newToken(),
		Username:  username,
		Role:      a.Role,
		CreatedAt: time.Now(),
		ExpiresAt: time.Now().Add(12 * time.Hour),
	}
	m.sessions[s.Token] = s
	m.auditLocked(username, "login", "", ip, true, "")
	_ = m.saveLocked()
	return s, nil
}

func (m *Manager) auditLocked(user, action, target, ip string, ok bool, detail string) {
	m.audit = append(m.audit, AuditEntry{Time: time.Now(), User: user, Action: action, Target: target, IP: ip, OK: ok, Detail: detail})
	if len(m.audit) > m.auditMax {
		m.audit = m.audit[len(m.audit)-m.auditMax:]
	}
}

func (m *Manager) saveLocked() error {
	list := make([]*Account, 0, len(m.accounts))
	for _, a := range m.accounts {
		list = append(list, a)
	}
	data, err := json.MarshalIndent(list, "", "  ")
	if err != nil {
		return err
	}
	tmp := m.path + ".tmp"
	if err := os.WriteFile(tmp, data, 0600); err != nil {
		return err
	}
	return os.Rename(tmp, m.path)
}

func (m *Manager) Verify(token string) (*Session, bool) {
	m.mu.RLock()
	defer m.mu.RUnlock()
	s, ok := m.sessions[token]
	if !ok || time.Now().After(s.ExpiresAt) {
		return nil, false
	}
	return s, true
}

func (m *Manager) Logout(token string) {
	m.mu.Lock()
	defer m.mu.Unlock()
	delete(m.sessions, token)
}

func (m *Manager) gcSessions() {
	t := time.NewTicker(10 * time.Minute)
	defer t.Stop()
	for range t.C {
		m.mu.Lock()
		now := time.Now()
		for k, v := range m.sessions {
			if now.After(v.ExpiresAt) {
				delete(m.sessions, k)
			}
		}
		m.mu.Unlock()
	}
}

// AccountView 是对外输出的账户视图（不含密码哈希）。
type AccountView struct {
	Username    string    `json:"username"`
	Role        Role      `json:"role"`
	DisplayName string    `json:"display_name,omitempty"`
	MustChange  bool      `json:"must_change"`
	Disabled    bool      `json:"disabled"`
	CreatedAt   time.Time `json:"created_at"`
	LastLogin   time.Time `json:"last_login,omitempty"`
}

func (m *Manager) ListAccounts() []*AccountView {
	m.mu.RLock()
	defer m.mu.RUnlock()
	out := make([]*AccountView, 0, len(m.accounts))
	for _, a := range m.accounts {
		out = append(out, &AccountView{
			Username:    a.Username,
			Role:        a.Role,
			DisplayName: a.DisplayName,
			MustChange:  a.MustChange,
			Disabled:    a.Disabled,
			CreatedAt:   a.CreatedAt,
			LastLogin:   a.LastLogin,
		})
	}
	return out
}

func (m *Manager) CreateAccount(username, password string, role Role, display, ip string) error {
	if len(username) < 3 {
		return errors.New("用户名至少 3 个字符")
	}
	if len(password) < 6 {
		return errors.New("密码至少 6 个字符")
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	if _, exists := m.accounts[username]; exists {
		return errors.New("用户名已存在")
	}
	hash, err := bcrypt.GenerateFromPassword([]byte(password), bcrypt.DefaultCost)
	if err != nil {
		return err
	}
	m.accounts[username] = &Account{
		Username: username, Password: string(hash), Role: role,
		DisplayName: display, CreatedAt: time.Now(), MustChange: true,
	}
	m.auditLocked(username, "account_create", username, ip, true, "")
	return m.saveLocked()
}

func (m *Manager) UpdateAccount(username string, role *Role, disabled *bool, password string, ip string) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	a, ok := m.accounts[username]
	if !ok {
		return errors.New("账户不存在")
	}
	if role != nil {
		a.Role = *role
	}
	if disabled != nil {
		a.Disabled = *disabled
		// 禁用时踢掉所有会话
		if *disabled {
			for k, v := range m.sessions {
				if v.Username == username {
					delete(m.sessions, k)
				}
			}
		}
	}
	if password != "" {
		if len(password) < 6 {
			return errors.New("密码至少 6 个字符")
		}
		hash, err := bcrypt.GenerateFromPassword([]byte(password), bcrypt.DefaultCost)
		if err != nil {
			return err
		}
		a.Password = string(hash)
		a.MustChange = true
	}
	m.auditLocked(username, "account_update", username, ip, true, "")
	return m.saveLocked()
}

func (m *Manager) DeleteAccount(username, ip string) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	if username == "admin" {
		return errors.New("不能删除内置管理员")
	}
	if _, ok := m.accounts[username]; !ok {
		return errors.New("账户不存在")
	}
	delete(m.accounts, username)
	for k, v := range m.sessions {
		if v.Username == username {
			delete(m.sessions, k)
		}
	}
	m.auditLocked(username, "account_delete", username, ip, true, "")
	return m.saveLocked()
}

// ChangePassword 用户自助改密。
func (m *Manager) ChangePassword(username, oldPwd, newPwd string) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	a, ok := m.accounts[username]
	if !ok {
		return errors.New("账户不存在")
	}
	if bcrypt.CompareHashAndPassword([]byte(a.Password), []byte(oldPwd)) != nil {
		return errors.New("原密码错误")
	}
	if len(newPwd) < 6 {
		return errors.New("新密码至少 6 个字符")
	}
	hash, err := bcrypt.GenerateFromPassword([]byte(newPwd), bcrypt.DefaultCost)
	if err != nil {
		return err
	}
	a.Password = string(hash)
	a.MustChange = false
	return m.saveLocked()
}

func (m *Manager) Audit(limit int) []AuditEntry {
	m.mu.RLock()
	defer m.mu.RUnlock()
	n := len(m.audit)
	start := 0
	if limit > 0 && n > limit {
		start = n - limit
	}
	out := make([]AuditEntry, n-start)
	copy(out, m.audit[start:])
	// 倒序（最新在前）
	for i, j := 0, len(out)-1; i < j; i, j = i+1, j-1 {
		out[i], out[j] = out[j], out[i]
	}
	return out
}

func (m *Manager) AddAudit(user, action, target, ip string, ok bool, detail string) {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.auditLocked(user, action, target, ip, ok, detail)
}

// ConstantTimeEqual 供签名比较使用。
func ConstantTimeEqual(a, b string) bool {
	return subtle.ConstantTimeCompare([]byte(a), []byte(b)) == 1
}
