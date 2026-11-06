// Package notify 提供告警通知分发：邮件（SMTP）与企业微信/任意 Webhook。
// 通知规则（何时发、发什么、发到哪）由 rules 包管理，本包只负责"送出去"。
package notify

import (
	"bytes"
	"crypto/tls"
	"encoding/json"
	"fmt"
	"net/http"
	"net/smtp"
	"strings"
	"time"
)

// Channel 通知渠道配置
type Channel struct {
	ID      string `json:"id"`
	Name    string `json:"name"`
	Type    string `json:"type"` // email | webhook
	Enabled bool   `json:"enabled"`

	// email
	SMTPHost string   `json:"smtp_host,omitempty"`
	SMTPPort int      `json:"smtp_port,omitempty"`
	Username string   `json:"username,omitempty"`
	Password string   `json:"password,omitempty"`
	From     string   `json:"from,omitempty"`
	To       []string `json:"to,omitempty"`
	UseTLS   bool     `json:"use_tls,omitempty"`

	// webhook（企业微信机器人 / 钉钉 / 飞书 / 自定义）
	URL     string            `json:"url,omitempty"`
	Method  string            `json:"method,omitempty"` // POST（默认）
	Headers map[string]string `json:"headers,omitempty"`
	// 模板类型：wecom=企业微信 markdown，json=原样 JSON
	Format string `json:"format,omitempty"`
}

// Message 待发送消息
type Message struct {
	Title   string
	Content string // markdown
	Level   string
	AgentID string
	At      time.Time
}

// Sender 分发器
type Sender struct {
	client *http.Client
}

func NewSender() *Sender {
	return &Sender{client: &http.Client{Timeout: 15 * time.Second}}
}

// Send 向单个渠道发送
func (s *Sender) Send(ch *Channel, msg *Message) error {
	if ch == nil || !ch.Enabled {
		return fmt.Errorf("channel disabled")
	}
	switch ch.Type {
	case "email":
		return s.sendEmail(ch, msg)
	case "webhook":
		return s.sendWebhook(ch, msg)
	default:
		return fmt.Errorf("unknown channel type: %s", ch.Type)
	}
}

func (s *Sender) sendEmail(ch *Channel, msg *Message) error {
	if ch.SMTPHost == "" || len(ch.To) == 0 {
		return fmt.Errorf("email channel missing smtp_host or to")
	}
	port := ch.SMTPPort
	if port == 0 {
		port = 587
	}
	from := ch.From
	if from == "" {
		from = ch.Username
	}
	subject := fmt.Sprintf("[CTForge][%s] %s", strings.ToUpper(msg.Level), msg.Title)
	body := fmt.Sprintf("From: %s\r\nTo: %s\r\nSubject: %s\r\nMIME-Version: 1.0\r\n"+
		"Content-Type: text/html; charset=UTF-8\r\n\r\n%s",
		from, strings.Join(ch.To, ","), encodeHeader(subject), htmlBody(msg))

	addr := fmt.Sprintf("%s:%d", ch.SMTPHost, port)
	var auth smtp.Auth
	if ch.Username != "" {
		auth = smtp.PlainAuth("", ch.Username, ch.Password, ch.SMTPHost)
	}

	if port == 465 || ch.UseTLS {
		// 隐式 TLS
		conn, err := tls.Dial("tcp", addr, &tls.Config{ServerName: ch.SMTPHost})
		if err != nil {
			return err
		}
		defer conn.Close()
		c, err := smtp.NewClient(conn, ch.SMTPHost)
		if err != nil {
			return err
		}
		defer c.Close()
		if auth != nil {
			if err := c.Auth(auth); err != nil {
				return err
			}
		}
		if err := c.Mail(from); err != nil {
			return err
		}
		for _, to := range ch.To {
			if err := c.Rcpt(to); err != nil {
				return err
			}
		}
		w, err := c.Data()
		if err != nil {
			return err
		}
		if _, err := w.Write([]byte(body)); err != nil {
			return err
		}
		return w.Close()
	}
	return smtp.SendMail(addr, auth, from, ch.To, []byte(body))
}

func encodeHeader(s string) string {
	// 简化：非 ASCII 用 RFC2047
	ascii := true
	for _, r := range s {
		if r > 127 {
			ascii = false
			break
		}
	}
	if ascii {
		return s
	}
	return "=?UTF-8?B?" + base64Encode([]byte(s)) + "?="
}

func htmlBody(msg *Message) string {
	color := "#0594fa"
	switch msg.Level {
	case "critical":
		color = "#e34d59"
	case "warning":
		color = "#ed7b2f"
	}
	return fmt.Sprintf(`<div style="font-family:sans-serif;max-width:640px">
<h2 style="color:%s;margin:0 0 8px">%s</h2>
<div style="color:#666;font-size:12px;margin-bottom:12px">%s · 主机 %s</div>
<pre style="background:#f5f5f5;padding:12px;border-radius:6px;white-space:pre-wrap">%s</pre>
</div>`, color, msg.Title, msg.At.Format("2006-01-02 15:04:05"), msg.AgentID, msg.Content)
}

func (s *Sender) sendWebhook(ch *Channel, msg *Message) error {
	if ch.URL == "" {
		return fmt.Errorf("webhook channel missing url")
	}
	var payload []byte
	var err error
	switch ch.Format {
	case "wecom", "":
		// 企业微信机器人 markdown
		md := fmt.Sprintf("## %s\n> 级别：**%s**\n> 主机：%s\n> 时间：%s\n\n%s",
			msg.Title, msg.Level, msg.AgentID, msg.At.Format("2006-01-02 15:04:05"), msg.Content)
		payload, err = json.Marshal(map[string]any{
			"msgtype":  "markdown",
			"markdown": map[string]string{"content": md},
		})
	case "json":
		payload, err = json.Marshal(map[string]any{
			"title":   msg.Title,
			"level":   msg.Level,
			"agent":   msg.AgentID,
			"content": msg.Content,
			"at":      msg.At.UnixMilli(),
		})
	}
	if err != nil {
		return err
	}
	req, err := http.NewRequest("POST", ch.URL, bytes.NewReader(payload))
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/json")
	for k, v := range ch.Headers {
		req.Header.Set(k, v)
	}
	resp, err := s.client.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode >= 300 {
		return fmt.Errorf("webhook returned %d", resp.StatusCode)
	}
	return nil
}

func base64Encode(b []byte) string {
	const tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
	var out strings.Builder
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
		out.WriteByte(tbl[(n>>18)&63])
		out.WriteByte(tbl[(n>>12)&63])
		if rem > 1 {
			out.WriteByte(tbl[(n>>6)&63])
		} else {
			out.WriteByte('=')
		}
		if rem > 2 {
			out.WriteByte(tbl[n&63])
		} else {
			out.WriteByte('=')
		}
	}
	return out.String()
}
