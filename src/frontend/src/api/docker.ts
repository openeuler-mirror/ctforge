import { http } from './agent';

// Docker 管理
export interface DockerContainer {
  id: string;
  name: string;
  image: string;
  status: string;
  state: string;
  ports: string;
  created: string;
}

export interface DockerImage {
  id: string;
  repository: string;
  tag: string;
  size: string;
  created: string;
}

export const listDockerContainers = (agentId: string) =>
  http.get<never, { available: boolean; containers: DockerContainer[]; error?: string }>(
    `/agents/${agentId}/docker/containers`,
  );

export const listDockerImages = (agentId: string) =>
  http.get<never, DockerImage[]>(`/agents/${agentId}/docker/images`);

export const dockerAction = (agentId: string, containerId: string, action: string) =>
  http.post(`/agents/${agentId}/docker/action`, { container_id: containerId, action });

export const dockerLogs = (agentId: string, containerId: string, lines = 200) =>
  http.get<never, { logs: string }>(
    `/agents/${agentId}/docker/logs?container_id=${encodeURIComponent(containerId)}&lines=${lines}`,
  );

// 心跳历史
export interface HeartbeatPoint {
  timestamp: number;
  latency_ms: number;
}
export interface DockerPortMap {
  host_ip: string;
  host_port: string;
  proto: string;
  container_port: string;
}

export interface DockerMount {
  type: string;
  source: string;
  target: string;
  read_only: boolean;
}

export interface DockerCreatePayload {
  name: string;
  image: string;
  cmd?: string;
  env?: string[];
  ports?: string[];
  mounts?: string[];
  restart?: string;
  network?: string;
  privileged?: boolean;
  auto_start?: boolean;
}

export interface DockerExecResult {
  ok: boolean;
  exit_code: number;
  output: string;
}

export const dockerInspect = (agentId: string, containerId: string) =>
  http.get<never, any>(`/agents/${agentId}/docker/inspect?container_id=${encodeURIComponent(containerId)}`);

export const dockerCreate = (agentId: string, payload: DockerCreatePayload) =>
  http.post<never, DockerExecResult>(`/agents/${agentId}/docker/create`, payload);

export const dockerCompose = (
  agentId: string,
  payload: { action: string; work_dir?: string; content?: string },
) => http.post<never, DockerExecResult>(`/agents/${agentId}/docker/compose`, payload);
