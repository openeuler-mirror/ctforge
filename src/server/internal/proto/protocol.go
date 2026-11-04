package proto

import (
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
)

const Version = 1
const MaxMessageSize = 32 << 20

const (
	MsgRegister       = "register"
	MsgHeartbeat      = "heartbeat"
	MsgMetrics        = "metrics"
	MsgLog            = "log"
	MsgTerminalOpen   = "terminal_open"
	MsgTerminalInput  = "terminal_input"
	MsgTerminalOutput = "terminal_output"
	MsgTerminalClose  = "terminal_close"
	MsgConfigGet      = "config_get"
	MsgConfigSet      = "config_set"
	MsgFileList       = "file_list"
	MsgFileRead       = "file_read"
	MsgFileWrite      = "file_write"
	MsgFileDelete     = "file_delete"
	MsgFileRename     = "file_rename"
	MsgFileMkdir      = "file_mkdir"
	MsgAck            = "ack"
	MsgError          = "error"
	MsgDiscover       = "discover"       // agent 广播发现
	MsgDiscoverReply  = "discover_reply" // server 回应发现
	// 系统服务管理
	MsgServiceList   = "service_list"
	MsgServiceAction = "service_action"
	// 软件包管理
	MsgPkgUpdates = "pkg_updates"
	MsgPkgUpgrade = "pkg_upgrade"
	MsgPkgHistory = "pkg_history"
	// 安全管理
	MsgSecurityAudit = "security_audit"
	// 延迟测量
	MsgPing = "ping"
	MsgPong = "pong"
	// 系统账户管理
	MsgSysUserList   = "sys_user_list"
	MsgSysUserPasswd = "sys_user_passwd"
	MsgSysUserLock   = "sys_user_lock"
	// 系统日志
	MsgSysLog = "sys_log"
	// 网络监控
	MsgNetTraffic = "net_traffic"
	MsgNetConn    = "net_conn"
	// 远程命令
	MsgCmdExec = "cmd_exec"
	// 批准制握手
	MsgHandshakeReq = "handshake_req"
	MsgHandshakeAck = "handshake_ack"
	MsgOffline      = "offline"
	// 定时任务
	MsgTaskRun    = "task_run"
	MsgTaskResult = "task_result"
	// 异常上报
	MsgAnomaly = "anomaly"
	// 端口转发
	MsgPortForwardOpen  = "pf_open"
	MsgPortForwardClose = "pf_close"
	MsgPortForwardData  = "pf_data"
	// 远程升级 / 卸载
	MsgUpgrade   = "upgrade"
	MsgUninstall = "uninstall"
	// 硬件信息与磁盘 IO
	MsgHardware = "hardware"
	MsgDiskIO   = "disk_io"
	// Docker 管理
	MsgDockerList    = "docker_list"
	MsgDockerAction  = "docker_action"
	MsgDockerLogs    = "docker_logs"
	MsgDockerImages  = "docker_images"
	MsgDockerInspect = "docker_inspect"
	MsgDockerCreate  = "docker_create"
	MsgDockerCompose = "docker_compose"
)

type Message struct {
	ID        string          `json:"id,omitempty"`
	Type      string          `json:"type"`
	Version   int             `json:"version,omitempty"`
	AgentID   string          `json:"agent_id,omitempty"`
	Token     string          `json:"token,omitempty"`
	SessionID string          `json:"session_id,omitempty"`
	Payload   json.RawMessage `json:"payload,omitempty"`
}

type RegisterPayload struct {
	AgentID  string `json:"agent_id"`
	Name     string `json:"name"`
	Hostname string `json:"hostname"`
	IP       string `json:"ip"`
	OS       string `json:"os"`
	Arch     string `json:"arch"`
	Version  string `json:"version"`
}

// DiscoverPayload 是 agent 广播的发现消息
// agent 通过 UDP 广播到局域网，server 收到后记录
// 并可通过 HTTP API 触发主动连接（server→agent 反向连接暂不实现，
// 本期 agent 仍然主动连 server，discover 仅用于在控制台展示"有哪些 agent 在线"）
type DiscoverPayload struct {
	AgentID     string `json:"agent_id"`
	Name        string `json:"name"`
	Hostname    string `json:"hostname"`
	IP          string `json:"ip"`
	OS          string `json:"os"`
	OSVersion   string `json:"os_version,omitempty"`
	Kernel      string `json:"kernel,omitempty"`
	Arch        string `json:"arch"`
	Version     string `json:"version"`
	DiskPercent float64 `json:"disk_percent,omitempty"`
	Port        int    `json:"port"` // agent 控制通道端口（备用）
}

type MetricsPayload struct {
	Timestamp   int64   `json:"timestamp"`
	UptimeSec   uint64  `json:"uptime_sec"`
	CPUPercent  float64 `json:"cpu_percent"`
	Load1       float64 `json:"load1"`
	Load5       float64 `json:"load5"`
	Load15      float64 `json:"load15"`
	MemTotalKB  uint64  `json:"mem_total_kb"`
	MemUsedKB   uint64  `json:"mem_used_kb"`
	MemPercent  float64 `json:"mem_percent"`
	SwapTotalKB uint64  `json:"swap_total_kb"`
	SwapUsedKB  uint64  `json:"swap_used_kb"`
	DiskTotalKB uint64  `json:"disk_total_kb"`
	DiskUsedKB  uint64  `json:"disk_used_kb"`
	DiskPercent float64 `json:"disk_percent"`
	Procs       int     `json:"procs"`
}

type HeartbeatPayload struct {
	Timestamp   int64   `json:"timestamp"`
	UptimeSec   uint64  `json:"uptime_sec"`
	Load1       float64 `json:"load1"`
	MemPercent  float64 `json:"mem_percent"`
	DiskPercent float64 `json:"disk_percent"`
}

type LogPayload struct {
	Timestamp int64  `json:"timestamp"`
	Level     string `json:"level"`
	Message   string `json:"message"`
}

type AckPayload struct {
	OK      bool   `json:"ok"`
	Message string `json:"message,omitempty"`
}

type ErrorPayload struct {
	Error string `json:"error"`
}

type TerminalOpenPayload struct {
	Cols  int    `json:"cols,omitempty"`
	Rows  int    `json:"rows,omitempty"`
	Shell string `json:"shell,omitempty"`
}

type TerminalInputPayload struct {
	DataBase64 string `json:"data_base64"`
}

type TerminalOutputPayload struct {
	DataBase64 string `json:"data_base64"`
	Closed     bool   `json:"closed,omitempty"`
}

type ConfigKV struct {
	Key   string `json:"key"`
	Value string `json:"value"`
}

type ConfigGetResult struct {
	Config map[string]string `json:"config"`
}

type FileListPayload struct {
	Path string `json:"path"`
}

type FileEntry struct {
	Name    string `json:"name"`
	Path    string `json:"path"`
	IsDir   bool   `json:"is_dir"`
	Size    int64  `json:"size"`
	Mode    string `json:"mode"`
	ModTime int64  `json:"mod_time"`
}

type FileListResult struct {
	Path  string      `json:"path"`
	Files []FileEntry `json:"files"`
}

type FileReadPayload struct {
	Path   string `json:"path"`
	Offset int64  `json:"offset,omitempty"`
	Limit  int64  `json:"limit,omitempty"`
}

type FileReadResult struct {
	Path          string `json:"path"`
	Size          int64  `json:"size"`
	ContentBase64 string `json:"content_base64"`
	Truncated     bool   `json:"truncated"`
}

type FileWritePayload struct {
	Path          string `json:"path"`
	ContentBase64 string `json:"content_base64"`
	Append        bool   `json:"append,omitempty"`
}

type FilePathPayload struct {
	Path string `json:"path"`
}

type FileRenamePayload struct {
	From string `json:"from"`
	To   string `json:"to"`
}

func Encode(w io.Writer, m *Message) error {
	if m.Version == 0 {
		m.Version = Version
	}
	b, err := json.Marshal(m)
	if err != nil {
		return err
	}
	if len(b) > MaxMessageSize {
		return fmt.Errorf("message too large: %d", len(b))
	}
	var hdr [4]byte
	binary.BigEndian.PutUint32(hdr[:], uint32(len(b)))
	if _, err := w.Write(hdr[:]); err != nil {
		return err
	}
	_, err = w.Write(b)
	return err
}

func Decode(r io.Reader) (*Message, error) {
	var hdr [4]byte
	if _, err := io.ReadFull(r, hdr[:]); err != nil {
		return nil, err
	}
	n := binary.BigEndian.Uint32(hdr[:])
	if n == 0 || n > MaxMessageSize {
		return nil, fmt.Errorf("invalid message size %d", n)
	}
	buf := make([]byte, n)
	if _, err := io.ReadFull(r, buf); err != nil {
		return nil, err
	}
	var m Message
	if err := json.Unmarshal(buf, &m); err != nil {
		return nil, err
	}
	if m.Type == "" {
		return nil, errors.New("empty message type")
	}
	return &m, nil
}

func MustPayload(v any) json.RawMessage {
	b, _ := json.Marshal(v)
	return b
}

func DecodePayload[T any](m *Message, out *T) error {
	if len(m.Payload) == 0 {
		return nil
	}
	return json.Unmarshal(m.Payload, out)
}

// ---- 系统服务管理 ----

type ServiceListPayload struct {
	Filter string `json:"filter,omitempty"`
}

type ServiceEntry struct {
	Name        string `json:"name"`
	Load        string `json:"load"`
	Active      string `json:"active"`
	Sub         string `json:"sub"`
	Description string `json:"description"`
	Enabled     string `json:"enabled"`
}

type ServiceListResult struct {
	Services []ServiceEntry `json:"services"`
}

type ServiceActionPayload struct {
	Name   string `json:"name"`
	Action string `json:"action"`
}

// ---- 软件包管理 ----

type PkgEntry struct {
	Name    string `json:"name"`
	Arch    string `json:"arch"`
	Version string `json:"version"`
	Update  string `json:"update"`
	Repo    string `json:"repo"`
}

type PkgUpdatesResult struct {
	Manager string     `json:"manager"`
	Count   int        `json:"count"`
	Updates []PkgEntry `json:"updates"`
}

type PkgUpgradePayload struct {
	Packages []string `json:"packages,omitempty"`
}

type PkgUpgradeResult struct {
	OK      bool   `json:"ok"`
	Output  string `json:"output"`
	Message string `json:"message"`
}

type PkgHistoryEntry struct {
	ID      string `json:"id"`
	Time    string `json:"time"`
	Action  string `json:"action"`
	Package string `json:"package"`
}

type PkgHistoryResult struct {
	Entries []PkgHistoryEntry `json:"entries"`
}

// ---- 安全审计 ----

type SecurityAuditResult struct {
	Firewall FirewallInfo `json:"firewall"`
	Ports    []PortInfo   `json:"ports"`
	SSH      SSHInfo      `json:"ssh"`
	Checks   []SecCheck   `json:"checks"`
}

type FirewallInfo struct {
	Active  bool     `json:"active"`
	Backend string   `json:"backend"`
	Zones   []string `json:"zones,omitempty"`
}

type PortInfo struct {
	Proto   string `json:"proto"`
	Port    int    `json:"port"`
	Process string `json:"process"`
	Address string `json:"address"`
}

type SSHInfo struct {
	Port            int  `json:"port"`
	PermitRootLogin bool `json:"permit_root_login"`
	PasswordAuth    bool `json:"password_auth"`
	PubkeyAuth      bool `json:"pubkey_auth"`
	MaxAuthTries    int  `json:"max_auth_tries"`
}

type SecCheck struct {
	Item    string `json:"item"`
	Status  string `json:"status"`
	Detail  string `json:"detail"`
	Suggest string `json:"suggest,omitempty"`
}

// ---- 延迟测量 ----

type PingPayload struct {
	Timestamp int64 `json:"timestamp"`
}

type PongPayload struct {
	Timestamp int64 `json:"timestamp"`
	AgentTime int64 `json:"agent_time"`
}

// ---- 系统账户管理 ----

type SysUserEntry struct {
	Username  string `json:"username"`
	UID       int    `json:"uid"`
	GID       int    `json:"gid"`
	Home      string `json:"home"`
	Shell     string `json:"shell"`
	Locked    bool   `json:"locked"`
	LastLogin string `json:"last_login,omitempty"`
}

type SysUserListResult struct {
	Users []SysUserEntry `json:"users"`
}

type SysUserPasswdPayload struct {
	Username    string `json:"username"`
	NewPassword string `json:"new_password"`
}

type SysUserLockPayload struct {
	Username string `json:"username"`
	Lock     bool   `json:"lock"`
}

// ---- 系统日志 ----

type SysLogPayload struct {
	Source   string `json:"source,omitempty"`
	Priority string `json:"priority,omitempty"`
	Unit     string `json:"unit,omitempty"`
	Keyword  string `json:"keyword,omitempty"`
	Lines    int    `json:"lines,omitempty"`
}

type SysLogEntry struct {
	Timestamp int64  `json:"timestamp"`
	Priority  string `json:"priority"`
	Unit      string `json:"unit,omitempty"`
	Message   string `json:"message"`
}

type SysLogResult struct {
	Entries []SysLogEntry `json:"entries"`
	Total   int           `json:"total"`
}

// ---- 网络流量 ----

type NetInterfaceIO struct {
	Name      string `json:"name"`
	RxBytes   uint64 `json:"rx_bytes"`
	RxPackets uint64 `json:"rx_packets"`
	RxErrs    uint64 `json:"rx_errs"`
	TxBytes   uint64 `json:"tx_bytes"`
	TxPackets uint64 `json:"tx_packets"`
	TxErrs    uint64 `json:"tx_errs"`
	Up        bool   `json:"up"`
}

type NetTrafficResult struct {
	Timestamp  int64            `json:"timestamp"`
	Interfaces []NetInterfaceIO `json:"interfaces"`
}

// ---- 网络连接状态 ----

type NetRemoteEntry struct {
	Address string `json:"address"`
	Count   int    `json:"count"`
}

type NetProcessEntry struct {
	Name  string `json:"name"`
	PID   int    `json:"pid"`
	Count int    `json:"count"`
}

type NetConnResult struct {
	Timestamp    int64             `json:"timestamp"`
	TCPStates    map[string]int    `json:"tcp_states"`
	UDPCount     int               `json:"udp_count"`
	TotalTCP     int               `json:"total_tcp"`
	TopRemotes   []NetRemoteEntry  `json:"top_remotes"`
	TopProcesses []NetProcessEntry `json:"top_processes"`
}

// ---- 远程命令 ----

type CmdExecPayload struct {
	Command    string `json:"command"`
	TimeoutSec int    `json:"timeout_sec,omitempty"`
	Shell      string `json:"shell,omitempty"`
}

type CmdExecResult struct {
	OK       bool   `json:"ok"`
	ExitCode int    `json:"exit_code"`
	Stdout   string `json:"stdout"`
	Stderr   string `json:"stderr"`
	Duration int64  `json:"duration_ms"`
	TimedOut bool   `json:"timed_out"`
}

// ---- 批准制握手 ----

type HandshakeReqPayload struct {
	AgentID       string  `json:"agent_id"`
	Name          string  `json:"name"`
	Hostname      string  `json:"hostname"`
	IP            string  `json:"ip"`
	OS            string  `json:"os"`
	OSVersion     string  `json:"os_version"`
	Kernel        string  `json:"kernel"`
	Arch          string  `json:"arch"`
	Version       string  `json:"version"`
	DiskTotalKB   uint64  `json:"disk_total_kb"`
	DiskUsedKB    uint64  `json:"disk_used_kb"`
	DiskPercent   float64 `json:"disk_percent"`
	BoundServerID string  `json:"bound_server_id,omitempty"`
}

type HandshakeAckPayload struct {
	Approved   bool   `json:"approved"`
	ServerID   string `json:"server_id"`
	ServerName string `json:"server_name"`
	ServerIP   string `json:"server_ip"`
	ServerPort int    `json:"server_port"`
	Message    string `json:"message,omitempty"`
}

// ---- 定时任务 ----

type TaskRunPayload struct {
	TaskID     string `json:"task_id"`
	TaskName   string `json:"task_name"`
	Command    string `json:"command"`
	TimeoutSec int    `json:"timeout_sec,omitempty"`
}

type TaskResultPayload struct {
	TaskID     string `json:"task_id"`
	OK         bool   `json:"ok"`
	ExitCode   int    `json:"exit_code"`
	Stdout     string `json:"stdout"`
	Stderr     string `json:"stderr"`
	Duration   int64  `json:"duration_ms"`
	TimedOut   bool   `json:"timed_out"`
	ExecutedAt int64  `json:"executed_at"`
}

// ---- 异常上报 ----

type AnomalyPayload struct {
	Kind    string `json:"kind"`
	Level   string `json:"level"`
	Detail  string `json:"detail"`
	Metrics string `json:"metrics,omitempty"`
}

// ---- 端口转发 ----

// PortForwardOpenPayload 服务端要求客户端把本地端口流量转发到服务端
type PortForwardOpenPayload struct {
	ForwardID  string `json:"forward_id"`
	TargetHost string `json:"target_host"`          // 服务端视角的目标（通常 127.0.0.1）
	TargetPort int    `json:"target_port"`          // 服务端监听端口
	LocalHost  string `json:"local_host,omitempty"` // 客户端侧目标主机（默认 127.0.0.1）
	LocalPort  int    `json:"local_port"`           // 客户端侧目标端口
}

type PortForwardDataPayload struct {
	ForwardID  string `json:"forward_id"`
	ConnID     string `json:"conn_id"`
	DataBase64 string `json:"data_base64"`
	Closed     bool   `json:"closed,omitempty"`
}

// ---- 远程升级 ----

type UpgradePayload struct {
	URL     string `json:"url"` // 新版本二进制下载地址
	SHA256  string `json:"sha256,omitempty"`
	Version string `json:"version"`
}

type UpgradeResult struct {
	OK      bool   `json:"ok"`
	Message string `json:"message"`
	Version string `json:"version"`
}

// ---- 硬件信息 ----

type HardwareResult struct {
	CPUModel       string        `json:"cpu_model"`
	CPUCores       int           `json:"cpu_cores"`
	CPUThreads     int           `json:"cpu_threads"`
	CPUMHz         float64       `json:"cpu_mhz,omitempty"`
	MemTotalKB     uint64        `json:"mem_total_kb"`
	SwapTotalKB    uint64        `json:"swap_total_kb"`
	Disks          []DiskInfo    `json:"disks"`
	NetCards       []NetCardInfo `json:"net_cards"`
	Virtualization string        `json:"virtualization,omitempty"`
	MachineID      string        `json:"machine_id,omitempty"`
	BIOSVendor     string        `json:"bios_vendor,omitempty"`
	BoardName      string        `json:"board_name,omitempty"`
}

type DiskInfo struct {
	Name    string  `json:"name"`
	Model   string  `json:"model,omitempty"`
	SizeGB  float64 `json:"size_gb"`
	Type    string  `json:"type,omitempty"` // ssd / hdd / unknown
	MountOn string  `json:"mount_on,omitempty"`
	FSType  string  `json:"fs_type,omitempty"`
}

type NetCardInfo struct {
	Name   string `json:"name"`
	MAC    string `json:"mac"`
	Speed  string `json:"speed,omitempty"`
	Driver string `json:"driver,omitempty"`
}

// ---- 磁盘 IO ----

type DiskIOPayload struct {
	Timestamp int64       `json:"timestamp"`
	Devices   []DiskIODev `json:"devices"`
}

type DiskIODev struct {
	Name      string  `json:"name"`
	ReadKBps  float64 `json:"read_kbps"`
	WriteKBps float64 `json:"write_kbps"`
	ReadIOPS  float64 `json:"read_iops"`
	WriteIOPS float64 `json:"write_iops"`
	UtilPct   float64 `json:"util_pct"`
}

// ---- Docker ----

type DockerContainer struct {
	ID      string `json:"id"`
	Name    string `json:"name"`
	Image   string `json:"image"`
	Status  string `json:"status"`
	State   string `json:"state"`
	Ports   string `json:"ports"`
	Created string `json:"created"`
	CPU     string `json:"cpu,omitempty"`
	Mem     string `json:"mem,omitempty"`
}

type DockerListResult struct {
	Available  bool              `json:"available"`
	Containers []DockerContainer `json:"containers"`
	Error      string            `json:"error,omitempty"`
}

type DockerActionPayload struct {
	ContainerID string `json:"container_id"`
	Action      string `json:"action"` // start/stop/restart/rm/pause/unpause
}

type DockerLogsPayload struct {
	ContainerID string `json:"container_id"`
	Lines       int    `json:"lines,omitempty"`
}

type DockerLogsResult struct {
	Logs string `json:"logs"`
}

type DockerImage struct {
	ID         string `json:"id"`
	Repository string `json:"repository"`
	Tag        string `json:"tag"`
	Size       string `json:"size"`
	Created    string `json:"created"`
}

type DockerImagesResult struct {
	Images []DockerImage `json:"images"`
}

// ---- Docker 详细能力 ----

type DockerInspectResult struct {
	ID        string            `json:"id"`
	Name      string            `json:"name"`
	Image     string            `json:"image"`
	State     string            `json:"state"`
	Status    string            `json:"status"`
	Created   string            `json:"created"`
	Restart   string            `json:"restart"`
	Env       []string          `json:"env"`
	Ports     []DockerPortMap   `json:"ports"`
	Mounts    []DockerMount     `json:"mounts"`
	Labels    map[string]string `json:"labels"`
	Network   string            `json:"network"`
	LogDriver string            `json:"log_driver"`
	Error     string            `json:"error,omitempty"`
}

type DockerPortMap struct {
	HostIP   string `json:"host_ip"`
	HostPort string `json:"host_port"`
	Proto    string `json:"proto"`
	ContPort string `json:"container_port"`
}

type DockerMount struct {
	Type     string `json:"type"`
	Source   string `json:"source"`
	Target   string `json:"target"`
	ReadOnly bool   `json:"read_only"`
}

type DockerCreatePayload struct {
	Name       string            `json:"name"`
	Image      string            `json:"image"`
	Cmd        string            `json:"cmd,omitempty"`
	Env        []string          `json:"env,omitempty"`
	Ports      []DockerPortMap   `json:"ports,omitempty"`
	Mounts     []DockerMount     `json:"mounts,omitempty"`
	Restart    string            `json:"restart,omitempty"`
	Network    string            `json:"network,omitempty"`
	Privileged bool              `json:"privileged,omitempty"`
	AutoStart  bool              `json:"auto_start,omitempty"`
	Labels     map[string]string `json:"labels,omitempty"`
}

type DockerComposePayload struct {
	Action  string `json:"action"` // up / down / ps / restart
	WorkDir string `json:"work_dir,omitempty"`
	Content string `json:"content,omitempty"` // 若提供则先写 docker-compose.yml
}

type DockerExecResult struct {
	OK       bool   `json:"ok"`
	ExitCode int    `json:"exit_code"`
	Output   string `json:"output"`
}
