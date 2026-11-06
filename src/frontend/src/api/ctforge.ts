// 复用 agent.ts 的 http 实例（统一 baseURL、鉴权与错误处理）
export { http } from './agent';
import { http } from './agent';

// ---------- CTForge eBPF 管理 ----------

export interface CtforgeModule {
  name?: string;
  [key: string]: unknown;
}

export interface CtforgeStatus {
  [key: string]: unknown;
}

export const ctforgeLsmod = (agentId: string) =>
  http.post<never, CtforgeModule[]>(`/agents/${agentId}/ctforge/lsmod`);

export const ctforgeStatus = (agentId: string) =>
  http.post<never, CtforgeStatus>(`/agents/${agentId}/ctforge/status`);

export const ctforgeInsmod = (agentId: string, module: string) =>
  http.post<never, Record<string, unknown>>(`/agents/${agentId}/ctforge/insmod`, { module });

export const ctforgeRmmod = (agentId: string, module: string) =>
  http.post<never, Record<string, unknown>>(`/agents/${agentId}/ctforge/rmmod`, { module });

export const ctforgeModCmd = (agentId: string, module: string, subcmd: string) =>
  http.post<never, Record<string, unknown>>(`/agents/${agentId}/ctforge/mod`, { module, subcmd });

// 部署 ctforged 到目标主机（通过 agent 下发命令执行安装脚本）
export const ctforgeDeploy = (agentId: string) =>
  http.post<never, { ok: boolean; exit_code: number; stdout: string; stderr: string }>(
    `/agents/${agentId}/ctforge/deploy`
  );
