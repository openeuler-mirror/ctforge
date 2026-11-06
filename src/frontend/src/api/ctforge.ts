import axios from 'axios';

const BASE = import.meta.env.VITE_API_URL || '/api';

export const http = axios.create({
  baseURL: BASE,
  timeout: 30000,
});

// 请求拦截：注入 token
http.interceptors.request.use((config) => {
  const token = localStorage.getItem('token');
  if (token) {
    config.headers.Authorization = `Bearer ${token}`;
  }
  return config;
});

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
