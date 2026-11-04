// Package ai 提供 OpenAI 兼容 API 调用：采集指标后自动分析并给出建议。
package ai

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"time"
)

// Config AI 配置
type Config struct {
	Enabled  bool   `json:"enabled"`
	BaseURL  string `json:"base_url"`  // 如 https://api.openai.com/v1 或自建兼容端点
	APIKey   string `json:"api_key"`
	Model    string `json:"model"`     // 如 gpt-4o-mini
	MaxTokens int   `json:"max_tokens"`
}

// Analysis 分析结果
type Analysis struct {
	OK        bool   `json:"ok"`
	Summary   string `json:"summary"`    // 一句话结论
	Issues    []Issue `json:"issues"`    // 异常项列表
	Suggests  []string `json:"suggests"` // 建议列表
	Raw       string `json:"raw,omitempty"` // 原始回复（调试用）
	AnalyzedAt int64 `json:"analyzed_at"`
	FromCache bool   `json:"from_cache,omitempty"`
}

type Issue struct {
	Title  string `json:"title"`
	Level  string `json:"level"` // info / warning / critical
	Detail string `json:"detail"`
	Metric string `json:"metric"` // 相关指标
}

type Analyzer struct {
	cfg    *Config
	client *http.Client
}

func NewAnalyzer(cfg *Config) *Analyzer {
	return &Analyzer{
		cfg: cfg,
		client: &http.Client{Timeout: 60 * time.Second},
	}
}

func (a *Analyzer) Enabled() bool { return a.cfg != nil && a.cfg.Enabled && a.cfg.APIKey != "" }

// Analyze 调用 OpenAI 兼容端点分析指标
func (a *Analyzer) Analyze(prompt string) (*Analysis, error) {
	if !a.Enabled() {
		return nil, fmt.Errorf("AI 分析未启用（请先在配置里填 API Key）")
	}

	payload := map[string]any{
		"model": a.cfg.Model,
		"messages": []map[string]string{
			{"role": "system", "content": sysPrompt()},
			{"role": "user", "content": prompt},
		},
		"max_tokens": a.cfg.MaxTokens,
		"temperature": 0.2,
	}
	if payload["max_tokens"] == nil || a.cfg.MaxTokens == 0 {
		payload["max_tokens"] = 2000
	}

	body, err := json.Marshal(payload)
	if err != nil {
		return nil, err
	}

	url := a.cfg.BaseURL
	if url == "" {
		url = "https://api.openai.com/v1"
	}
	url = url + "/chat/completions"

	req, err := http.NewRequest("POST", url, bytes.NewReader(body))
	if err != nil {
		return nil, err
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Authorization", "Bearer "+a.cfg.APIKey)

	resp, err := a.client.Do(req)
	if err != nil {
		return nil, fmt.Errorf("AI 调用失败: %v", err)
	}
	defer resp.Body.Close()

	data, err := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if err != nil {
		return nil, err
	}
	if resp.StatusCode >= 300 {
		return nil, fmt.Errorf("AI 返回 %d: %s", resp.StatusCode, string(data[:min(200, len(data))]))
	}

	var result struct {
		Choices []struct {
			Message struct {
				Content string `json:"content"`
			} `json:"message"`
		} `json:"choices"`
	}
	if err := json.Unmarshal(data, &result); err != nil {
		return nil, err
	}
	if len(result.Choices) == 0 {
		return nil, fmt.Errorf("AI 返回空结果")
	}
	content := result.Choices[0].Message.Content

	out := &Analysis{
		OK:         true,
		Raw:        content,
		AnalyzedAt: time.Now().UnixMilli(),
	}

	// 尝试解析 JSON 格式的结构化结果（让 AI 按指定 JSON 格式输出）
	var parsed struct {
		Summary  string   `json:"summary"`
		Issues   []Issue  `json:"issues"`
		Suggests []string `json:"suggests"`
	}
	if err := json.Unmarshal([]byte(content), &parsed); err == nil && parsed.Summary != "" {
		out.Summary = parsed.Summary
		out.Issues = parsed.Issues
		out.Suggests = parsed.Suggests
	} else {
		// 非 JSON：整段作为 summary
		out.Summary = content
	}
	return out, nil
}

func sysPrompt() string {
	return `你是一位资深 Linux 运维专家。用户会给你一台 Linux 主机的实时指标数据（JSON 格式），你需要：
1. 用一句话总结系统健康状况（summary 字段）
2. 列出发现的所有异常项（issues 数组，每项含 title/level/detail/metric）
3. 给出可执行的优化建议（suggests 数组，每项一句话）

请严格按以下 JSON 格式输出（不要加任何 markdown 标记）：
{"summary":"...","issues":[{"title":"...","level":"info|warning|critical","detail":"...","metric":"..."}],"suggests":["...","..."]}`
}

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}

// BuildPrompt 组装分析 prompt（由调用方组装指标数据）
func BuildPrompt(hostname, osVersion, kernel string, metrics map[string]any, hardware map[string]any) string {
	mJSON, _ := json.Marshal(metrics)
	hJSON, _ := json.Marshal(hardware)
	return fmt.Sprintf(`主机：%s
系统：%s
内核：%s

实时指标：
%s

硬件信息：
%s

请分析该主机的健康状况，找出异常指标并给出优化建议。`, hostname, osVersion, kernel, mJSON, hJSON)
}
