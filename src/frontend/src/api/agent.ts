import axios from 'axios';

import { clearAuth, getToken } from '@/utils/auth';

const BASE = '/api';

export const http = axios.create({
  baseURL: BASE,
  timeout: 650000,
});

// 请求带 token
http.interceptors.request.use((cfg) => {
  const token = getToken();
  if (token) cfg.headers.Authorization = `Bearer ${token}`;
  return cfg;
});

// 统一解包 {code,data,message}
http.interceptors.response.use(
  (res) => {
    const d = res.data;
    if (d && typeof d.code === 'number') {
      if (d.code === 0) return d.data;
      return Promise.reject(new Error(d.message || `错误码 ${d.code}`));
    }
    return d;
  },
  (err) => {
    const status = err.response?.status;
    if (status === 401) {
      clearAuth();
      if (!location.pathname.startsWith('/login')) location.href = '/login';
    }
    const msg = err.response?.data?.message || err.message || '请求失败';
    return Promise.reject(new Error(msg));
  },
);

// ---------- 认证 ----------
export interface LoginResult {
  token: string;
  username: string;
  role: 'admin' | 'operator' | 'viewer';
  expires_at: number;
}

export const login = (username: string, password: string) =>
  http.post<never, LoginResult>('/auth/login', { username, password });
export const logout = () => http.post('/auth/logout');
export const getMe = () => http.get<never, { username: string; role: string }>('/auth/me');
export const changePassword = (old_password: string, new_password: string) =>
  http.post('/auth/password', { old_password, new_password });

// ---------- 账户 ----------
export interface Account {
  username: string;
  role: 'admin' | 'operator' | 'viewer';
  display_name?: string;
  disabled?: boolean;
  must_change?: boolean;
  created_at: string;
  last_login?: string;
}

export const listAccounts = () => http.get<never, Account[]>('/accounts');
export const createAccount = (data: { username: string; password: string; role: string; display_name?: string }) =>
  http.post('/accounts', data);
export const updateAccount = (name: string, data: { role?: string; disabled?: boolean; password?: string }) =>
  http.put(`/accounts/${name}`, data);
export const deleteAccount = (name: string) => http.delete(`/accounts/${name}`);
export const listAudit = (limit = 200) => http.get<never, any[]>(`/audit?limit=${limit}`);

// ---------- 主机 ----------
export interface Agent {
  agent_id: string;
  name: string;
  hostname: string;
  ip: string;
  remark?: string; // 用户自定义备注名
  os: string;
  arch: string;
  version: string;
  online: boolean;
  discovered?: boolean;
  last_seen: string;
  online_at?: number;
  offline_at?: number;
  latency_ms?: number;
  latency_history?: number[];
  approval?: string;
  os_version?: string;
  kernel?: string;
  disk_total_kb?: number;
  disk_used_kb?: number;
  disk_percent?: number;
  offline_count?: number;
  alerts?: any[];
  net_rx_rate?: number;
  net_tx_rate?: number;
  ctforge_state?: string;   // running/started/failed/not_installed
  ctforge_message?: string; // ctforged 状态描述
  ctforge_at?: number;      // 上报时间戳
  net_conns?: number;
  last_heartbeat?: {
    timestamp: number;
    uptime_sec: number;
    load1: number;
    mem_percent: number;
    disk_percent: number;
  };
  last_metrics?: {
    timestamp: number;
    uptime_sec: number;
    cpu_percent: number;
    load1: number;
    load5: number;
    load15: number;
    mem_total_kb: number;
    mem_used_kb: number;
    mem_percent: number;
    disk_total_kb: number;
    disk_used_kb: number;
    disk_percent: number;
    procs: number;
  };
}

export interface MetricPoint {
  timestamp: number;
  cpu: number;
  mem: number;
  disk: number;
  load1: number;
}

export interface LogEntry {
  timestamp: number;
  level: string;
  message: string;
}

export const listAgents = () => http.get<never, Agent[]>('/agents');
export const getAgent = (id: string) => http.get<never, Agent>(`/agents/${id}`);
export const getMetrics = (id: string) => http.get<never, MetricPoint[]>(`/agents/${id}/metrics`);
export const getLogs = (id: string, limit = 200) =>
  http.get<never, LogEntry[]>(`/agents/${id}/logs?limit=${limit}`);
export const analyzeLogs = (id: string, keyword = '', level = '', limit = 200) =>
  http.get<never, { entries: LogEntry[]; stats: Record<string, number> }>(
    `/agents/${id}/logs-analyze?keyword=${encodeURIComponent(keyword)}&level=${level}&limit=${limit}`,
  );

// ---------- 配置 / 文件 ----------
export const getConfig = (id: string) => http.get<never, Record<string, string>>(`/agents/${id}/config`);
export const setConfig = (id: string, key: string, value: string) =>
  http.put(`/agents/${id}/config`, { key, value });

export interface FileEntry {
  name: string;
  path: string;
  is_dir: boolean;
  size: number;
  mode: string;
  mod_time: number;
}

export const listFiles = (id: string, path: string) =>
  http.get<never, { path: string; files: FileEntry[] }>(`/agents/${id}/files`, { params: { path } });
export const readFile = (id: string, path: string) =>
  http.get<never, { path: string; size: number; content_base64: string; truncated: boolean }>(
    `/agents/${id}/files`,
    { params: { path, download: 1 } },
  );
export const writeFile = (id: string, path: string, contentBase64: string, append = false) =>
  http.put(`/agents/${id}/files`, { path, content_base64: contentBase64, append });
export const fileAction = (
  id: string,
  action: 'mkdir' | 'delete' | 'rename',
  path: string,
  from?: string,
  to?: string,
) => http.post(`/agents/${id}/files`, { action, path, from, to });

// ---------- 系统服务 ----------
export interface ServiceEntry {
  name: string;
  load: string;
  active: string;
  sub: string;
  description: string;
  enabled: string;
}

export const listServices = (id: string, filter = '') =>
  http.get<never, ServiceEntry[]>(`/agents/${id}/services?filter=${encodeURIComponent(filter)}`);
export const serviceAction = (id: string, name: string, action: string) =>
  http.post(`/agents/${id}/services`, { name, action });

// ---------- 软件包 ----------
export interface PkgEntry {
  name: string;
  arch: string;
  version: string;
  update: string;
  repo: string;
}

export const listPkgUpdates = (id: string) =>
  http.get<never, { manager: string; count: number; updates: PkgEntry[] }>(`/agents/${id}/packages/updates`);
export const upgradePkg = (id: string, packages: string[] = []) =>
  http.post<never, { ok: boolean; output: string; message: string }>(`/agents/${id}/packages/upgrade`, { packages });
export const pkgHistory = (id: string) => http.get<never, any[]>(`/agents/${id}/packages/history`);

// ---------- 安全审计 ----------
export interface SecCheck {
  item: string;
  status: 'pass' | 'warn' | 'fail';
  detail: string;
  suggest?: string;
}
export interface PortInfo {
  proto: string;
  port: number;
  process: string;
  address: string;
}
export interface SecurityAudit {
  firewall: { active: boolean; backend: string };
  ports: PortInfo[];
  ssh: {
    port: number;
    permit_root_login: boolean;
    password_auth: boolean;
    pubkey_auth: boolean;
    max_auth_tries: number;
  };
  checks: SecCheck[];
}

export const securityAudit = (id: string) => http.get<never, SecurityAudit>(`/agents/${id}/security`);

// ---------- 系统账户（被管理主机上的 Linux 用户） ----------
export interface SysUser {
  username: string;
  uid: number;
  gid: number;
  home: string;
  shell: string;
  locked: boolean;
  last_login?: string;
}

export const listSysUsers = (id: string) => http.get<never, SysUser[]>(`/agents/${id}/sys-users`);
export const changeSysUserPassword = (id: string, username: string, new_password: string) =>
  http.post(`/agents/${id}/sys-users/passwd`, { username, new_password });
export const lockSysUser = (id: string, username: string, lock: boolean) =>
  http.post(`/agents/${id}/sys-users/lock`, { username, lock });

// ---------- 系统日志 ----------
export interface SysLogEntry {
  timestamp: number;
  priority: string;
  unit?: string;
  message: string;
}

export interface SysLogQuery {
  source?: string;
  priority?: string;
  unit?: string;
  keyword?: string;
  lines?: number;
}

export const querySysLog = (id: string, q: SysLogQuery = {}) => {
  const p = new URLSearchParams();
  Object.entries(q).forEach(([k, v]) => {
    if (v !== undefined && v !== '') p.append(k, String(v));
  });
  return http.get<never, { entries: SysLogEntry[]; total: number }>(`/agents/${id}/sys-log?${p.toString()}`);
};

// ---------- 网络监控 ----------
export interface NetIface {
  name: string;
  rx_bytes: number;
  rx_packets: number;
  rx_errs: number;
  tx_bytes: number;
  tx_packets: number;
  tx_errs: number;
  up: boolean;
}

export const getNetTraffic = (id: string) =>
  http.get<never, { timestamp: number; interfaces: NetIface[]; rx_rate: number; tx_rate: number }>(
    `/agents/${id}/net/traffic`,
  );

export interface NetConn {
  timestamp: number;
  tcp_states: Record<string, number>;
  udp_count: number;
  total_tcp: number;
  top_remotes: { address: string; count: number }[];
  top_processes: { name: string; pid: number; count: number }[];
}

export const getNetConn = (id: string) => http.get<never, NetConn>(`/agents/${id}/net/conn`);

// ---------- 远程命令 ----------
export interface CmdResult {
  ok: boolean;
  exit_code: number;
  stdout: string;
  stderr: string;
  duration_ms: number;
}

export const execCommand = (id: string, command: string, timeout_sec = 60) =>
  http.post<never, CmdResult>(`/agents/${id}/exec`, { command, timeout_sec });

// ---------- 批准管理 ----------
export interface PendingAgent {
  agent_id: string;
  hostname: string;
  ip: string;
  os?: string;
  os_version?: string;
  kernel?: string;
  arch?: string;
  version?: string;
  disk_total_kb?: number;
  disk_used_kb?: number;
  disk_percent?: number;
  approval?: string;
  last_seen?: string;
}

export const listApprovals = () =>
  http.get<never, { pending: PendingAgent[]; approved: number; rejected: PendingAgent[]; server_id: string }>(
    '/approvals',
  );
export const approveAgent = (id: string) => http.post(`/approvals/${id}/approve`);
export const rejectAgent = (id: string) => http.post(`/approvals/${id}/reject`);

// ---------- 部署 ----------
export const getDeployInfo = () =>
  http.get<never, { script: string; command: string; server_host: string; server_port: number; server_id: string; fingerprint: string }>(
    '/deploy/script',
  );

// ---------- 定时任务 ----------
export interface CronTask {
  id: string;
  name: string;
  enabled: boolean;
  cron: string;
  command: string;
  timeout_sec: number;
  agents: string[];
  created_at?: number;
  last_run_at?: number;
  last_status?: string;
  next_run_at?: number;
  run_count?: number;
}

export const listTasks = () => http.get<never, CronTask[]>('/tasks');
export const createTask = (t: Partial<CronTask>) => http.post('/tasks', t);
export const updateTask = (id: string, t: Partial<CronTask>) => http.put(`/tasks/${id}`, t);
export const deleteTask = (id: string) => http.delete(`/tasks/${id}`);
export const runTaskNow = (id: string) => http.post(`/tasks/${id}/run`);
export const listTaskRecords = (limit = 100) => http.get<never, any[]>(`/tasks/records?limit=${limit}`);

// ---------- 告警 ----------
export interface AlertItem {
  id: string;
  agent_id: string;
  kind: string;
  level: string;
  title: string;
  detail: string;
  created_at: number;
  resolved: boolean;
}

export const listAlerts = (includeResolved = false) =>
  http.get<never, AlertItem[]>(`/alerts?resolved=${includeResolved ? 1 : 0}`);
export const resolveAlert = (agentId: string, kind: string) => http.post(`/alerts/${agentId}/${kind}`);

// ---------- 通知配置 ----------
export interface NotifyChannel {
  id: string;
  name: string;
  type: 'email' | 'webhook';
  enabled: boolean;
  smtp_host?: string;
  smtp_port?: number;
  username?: string;
  password?: string;
  from?: string;
  to?: string[];
  use_tls?: boolean;
  url?: string;
  format?: string;
}

export interface NotifyRule {
  id: string;
  name: string;
  enabled: boolean;
  kinds: string[];
  min_level: string;
  channels: string[];
  time_windows?: string[];
  cooldown_sec: number;
}

export const getNotifyConfig = () => http.get<never, { channels: NotifyChannel[]; rules: NotifyRule[] }>('/notify/config');
export const saveNotifyConfig = (cfg: any) => http.put('/notify/config', cfg);
export const upsertChannel = (ch: Partial<NotifyChannel>) =>
  ch.id ? http.put(`/notify/channels/${ch.id}`, ch) : http.post('/notify/channels/', ch);
export const deleteChannel = (id: string) => http.delete(`/notify/channels/${id}`);
export const testChannel = (id: string) => http.post('/notify/test', { channel_id: id });
export const upsertRule = (r: Partial<NotifyRule>) => saveNotifyConfigMergeRule(r);
export async function deleteRule(id: string) {
  const cfg = await getNotifyConfig();
  const rules = (cfg.rules || []).filter((r) => r.id !== id);
  await saveNotifyConfig({ channels: cfg.channels || [], rules });
  return { ok: true };
}

// 规则增删改通过整体配置保存实现（服务端 /notify/config 支持整体覆盖）
async function saveNotifyConfigMergeRule(patch: Partial<NotifyRule>) {
  const cfg = await getNotifyConfig();
  const rules = cfg.rules || [];
  if (patch.id) {
    const i = rules.findIndex((r) => r.id === patch.id);
    if (i >= 0) rules[i] = { ...rules[i], ...patch } as NotifyRule;
    else rules.push(patch as NotifyRule);
  } else {
    patch.id = `rule-${Date.now()}`;
    rules.push(patch as NotifyRule);
  }
  await saveNotifyConfig({ channels: cfg.channels || [], rules });
  return patch;
}

// ---------- 卸载审批 ----------
export interface UninstallReq {
  agent_id: string;
  hostname: string;
  reason?: string;
  at: number;
  approved: boolean;
  decided_at?: number;
}

export const listUninstallRequests = () => http.get<never, UninstallReq[]>('/uninstall-requests');
export const decideUninstall = (id: string, approved: boolean) =>
  http.post(`/uninstall-requests/${id}`, { approved });

// ---------- 文件在线编辑 ----------
export const readFileText = (id: string, path: string) =>
  http.get<never, { path: string; size: number; content: string; binary: boolean; truncated: boolean }>(
    `/agents/${id}/files-edit`,
    { params: { path } },
  );
export const saveFileText = (id: string, path: string, content: string) =>
  http.put(`/agents/${id}/files-edit`, { path, content });

// ---------- 权限体系 ----------
export interface PermGroupDef { id: string; name: string; level: number; perms: string[] }
export interface MachineScope { all: boolean; groups?: string[]; agents?: string[] }
export interface Grant {
  username: string;
  permissions: string[];
  scope: MachineScope;
  expires_at: number;
  created_at?: number;
  updated_at?: number;
  updated_by?: string;
}
export interface AgentGroup {
  id: string; name: string; description?: string; agents: string[]; created_at?: number;
}

export const getPermsMeta = () =>
  http.get<never, { groups: PermGroupDef[]; perms: string[]; levels: Record<string, number> }>('/perms/meta');
export const listGroups = () => http.get<never, AgentGroup[]>('/groups');
export const createGroup = (g: Partial<AgentGroup>) => http.post('/groups', g);
export const updateGroup = (id: string, g: Partial<AgentGroup>) => http.put(`/groups/${id}`, g);
export const deleteGroup = (id: string) => http.delete(`/groups/${id}`);
export const listGrants = () => http.get<never, Grant[]>('/grants');
export const getGrant = (u: string) => http.get<never, Grant>(`/grants/${u}`);
export const saveGrant = (u: string, g: Partial<Grant>) => http.put(`/grants/${u}`, g);
export const deleteGrant = (u: string) => http.delete(`/grants/${u}`);
export const getMyGrant = () =>
  http.get<never, { username: string; role: string; permissions: string[]; scope: MachineScope; expires_at: number; valid: boolean }>('/me/grant');

// ---------- 上下线历史 ----------
export interface HistEvent {
  agent_id: string; hostname: string; ip?: string;
  kind: 'online' | 'offline'; at: number; duration_s?: number; latency_ms?: number;
}
export const listHistory = (agentId = '', limit = 200) =>
  http.get<never, HistEvent[]>(`/history?agent_id=${encodeURIComponent(agentId)}&limit=${limit}`);
export const historyStats = (agentId = '', hours = 24) =>
  http.get<never, Record<string, number>>(`/history/stats?agent_id=${encodeURIComponent(agentId)}&hours=${hours}`);

// ---------- 端口转发 ----------
export interface PortForward {
  id: string; agent_id: string; target_host: string; target_port: number;
  local_host: string; local_port: number; active: boolean; created_at: number; created_by?: string;
}
export const listPortForwards = () => http.get<never, PortForward[]>('/portforwards');
export const createPortForward = (f: Partial<PortForward>) => http.post('/portforwards', f);
export const deletePortForward = (id: string) => http.delete(`/portforwards/${id}`);

// ---------- AI ----------
export interface AIConfig {
  enabled: boolean; base_url: string; api_key: string; model: string; max_tokens: number;
}
export interface AIAnalysis {
  ok: boolean; summary: string; issues: { title: string; level: string; detail: string; metric: string }[];
  suggests: string[]; analyzed_at: number;
}
export const getAIConfig = () => http.get<never, AIConfig>('/ai/config');
export const getAIStatus = () => http.get<never, { enabled: boolean; model?: string }>('/ai/status');
export const saveAIConfig = (c: Partial<AIConfig>) => http.put('/ai/config', c);
export const aiAnalyze = (id: string) => http.post<never, AIAnalysis>(`/agents/${id}/ai-analyze`);

// ---------- 硬件 / 磁盘 IO ----------
export interface Hardware {
  cpu_model: string; cpu_cores: number; cpu_threads: number; cpu_mhz?: number;
  mem_total_kb: number; swap_total_kb: number;
  disks: { name: string; model: string; size_gb: number; type: string; mount_on?: string; fs_type?: string }[];
  net_cards: { name: string; mac: string; speed: string; driver: string }[];
  virtualization?: string; bios_vendor?: string; board_name?: string; machine_id?: string;
}
export const setAgentRemark = (id: string, remark: string) =>
  http.post<never, { ok: boolean; remark: string }>(`/agents/${id}/remark`, { remark });

export const getHardware = (id: string) => http.get<never, Hardware>(`/agents/${id}/hardware`);

export interface DiskIODev { name: string; read_kbps: number; write_kbps: number; read_iops: number; write_iops: number; util_pct: number }
export const getDiskIO = (id: string) =>
  http.get<never, { timestamp: number; devices: DiskIODev[] }>(`/agents/${id}/disk-io`);

// ---------- 统计 / 拓扑 ----------
export const getStatsOverview = () =>
  http.get<never, Record<string, number>>('/stats/overview');
export const getStatsAgents = (hours = 24) => http.get<never, any[]>(`/stats/agents?hours=${hours}`);
export const getTopology = () => http.get<never, any>('/topology');

// ---------- 远程升级 / 卸载 ----------
export const remoteUpgrade = (id: string, url?: string, version?: string) =>
  http.post(`/agents/${id}/upgrade`, { url, version });
export const remoteUninstall = (id: string) => http.post(`/agents/${id}/remote-uninstall`);

// ---------- 终端 ----------
export function terminalWSUrl(id: string) {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  const token = getToken();
  return `${proto}://${location.host}/ws/terminal?agent_id=${encodeURIComponent(id)}&token=${encodeURIComponent(token)}`;
}
