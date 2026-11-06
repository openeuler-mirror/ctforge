package auth

import (
	"path/filepath"
	"testing"
)

func TestManagerLifecycle(t *testing.T) {
	dir := t.TempDir()
	accountsFile := filepath.Join(dir, "accounts.json")

	mgr, err := NewManager(accountsFile)
	if err != nil {
		t.Fatalf("NewManager failed: %v", err)
	}

	// 初始时应至少有一个 admin 账户（首次自动创建）
	accounts := mgr.ListAccounts()
	if len(accounts) == 0 {
		t.Error("expected at least one default account")
	}

	// 添加新用户
	err = mgr.CreateAccount("testuser", "testpass", RoleViewer, "Test User", "127.0.0.1")
	if err != nil {
		t.Fatalf("CreateAccount failed: %v", err)
	}

	// 登录验证（需要 ip 参数）
	sess, err := mgr.Login("testuser", "testpass", "127.0.0.1")
	if err != nil {
		t.Fatalf("Login failed: %v", err)
	}
	if sess == nil || sess.Username != "testuser" {
		t.Errorf("unexpected session: %+v", sess)
	}
	if sess.Role != RoleViewer {
		t.Errorf("expected role=viewer, got %s", sess.Role)
	}

	// 校验 token
	verified, ok := mgr.Verify(sess.Token)
	if !ok {
		t.Fatal("Verify failed for valid token")
	}
	if verified.Username != "testuser" {
		t.Errorf("Verify returned wrong username: %s", verified.Username)
	}

	// 登出
	mgr.Logout(sess.Token)
	_, ok = mgr.Verify(sess.Token)
	if ok {
		t.Error("expected Verify to fail after Logout")
	}
}

func TestCheckPassword(t *testing.T) {
	dir := t.TempDir()
	mgr, err := NewManager(filepath.Join(dir, "accounts.json"))
	if err != nil {
		t.Fatalf("NewManager failed: %v", err)
	}

	if err := mgr.CreateAccount("checkuser", "checkpass", RoleOperator, "", "127.0.0.1"); err != nil {
		t.Fatalf("CreateAccount failed: %v", err)
	}

	// 正确密码
	acct, ok := mgr.CheckPassword("checkuser", "checkpass")
	if !ok {
		t.Fatal("CheckPassword failed with correct password")
	}
	if acct.Role != RoleOperator {
		t.Errorf("expected role=operator, got %s", acct.Role)
	}

	// 错误密码
	_, ok = mgr.CheckPassword("checkuser", "wrongpass")
	if ok {
		t.Error("expected CheckPassword to fail with wrong password")
	}

	// 不存在用户
	_, ok = mgr.CheckPassword("nonexistent", "checkpass")
	if ok {
		t.Error("expected CheckPassword to fail for nonexistent user")
	}
}

func TestRoleConstants(t *testing.T) {
	if RoleAdmin != "admin" {
		t.Errorf("RoleAdmin = %s, want admin", RoleAdmin)
	}
	if RoleOperator != "operator" {
		t.Errorf("RoleOperator = %s, want operator", RoleOperator)
	}
	if RoleViewer != "viewer" {
		t.Errorf("RoleViewer = %s, want viewer", RoleViewer)
	}
}

func TestPersistence(t *testing.T) {
	dir := t.TempDir()
	accountsFile := filepath.Join(dir, "accounts.json")

	mgr1, err := NewManager(accountsFile)
	if err != nil {
		t.Fatalf("NewManager failed: %v", err)
	}
	if err := mgr1.CreateAccount("persist", "pass123", RoleOperator, "", "127.0.0.1"); err != nil {
		t.Fatalf("CreateAccount failed: %v", err)
	}

	// 重新加载
	mgr2, err := NewManager(accountsFile)
	if err != nil {
		t.Fatalf("NewManager reload failed: %v", err)
	}
	if _, err := mgr2.Login("persist", "pass123", "127.0.0.1"); err != nil {
		t.Errorf("Login after reload failed: %v", err)
	}
}

func TestChangePassword(t *testing.T) {
	dir := t.TempDir()
	mgr, err := NewManager(filepath.Join(dir, "accounts.json"))
	if err != nil {
		t.Fatalf("NewManager failed: %v", err)
	}

	if err := mgr.CreateAccount("pwuser", "oldpass", RoleViewer, "", "127.0.0.1"); err != nil {
		t.Fatalf("CreateAccount failed: %v", err)
	}

	// 修改密码
	if err := mgr.ChangePassword("pwuser", "oldpass", "newpass"); err != nil {
		t.Fatalf("ChangePassword failed: %v", err)
	}

	// 旧密码不能登录
	_, err = mgr.Login("pwuser", "oldpass", "127.0.0.1")
	if err == nil {
		t.Error("expected Login to fail with old password")
	}

	// 新密码可以登录
	_, err = mgr.Login("pwuser", "newpass", "127.0.0.1")
	if err != nil {
		t.Errorf("expected Login to succeed with new password, got: %v", err)
	}
}
