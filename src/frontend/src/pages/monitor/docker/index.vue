<template>
  <div>
    <t-card :bordered="false">
      <template #title>
        <t-space align="center">
          <span>Docker 管理</span>
          <t-select
            v-model="currentAgent"
            :options="agentOptions"
            placeholder="选择主机"
            style="width: 260px"
            :loading="loadingAgents"
            @change="fetchAll"
          />
          <t-tag v-if="available" theme="success" variant="light">Docker 可用</t-tag>
          <t-tag v-else-if="checked" theme="default" variant="light">Docker 不可用</t-tag>
        </t-space>
      </template>
      <template #actions>
        <t-space>
          <t-button theme="primary" :loading="loading" @click="fetchAll">刷新</t-button>
          <t-button theme="default" :disabled="!available" @click="openCreate">创建容器</t-button>
          <t-button theme="default" :disabled="!available" @click="openCompose">Compose</t-button>
        </t-space>
      </template>

      <t-alert v-if="checked && !available" theme="warning" style="margin-bottom: 16px">
        {{ dockerError || '该主机未安装 Docker，或 Docker 守护进程未运行。' }}
      </t-alert>

      <t-tabs v-model="activeTab" :disabled="!available">
        <t-tab-panel value="containers" label="容器">
          <t-table
            :data="containers"
            :columns="containerColumns"
            :loading="loading"
            row-key="id"
            hover
            size="small"
          >
            <template #state="{ row }">
              <t-tag :theme="stateTheme(row.state)" variant="light" size="small">{{ row.state }}</t-tag>
            </template>
            <template #name="{ row }">
              <span class="mono">{{ row.name }}</span>
            </template>
            <template #image="{ row }">
              <t-tooltip :content="row.image">
                <span class="mono img">{{ row.image }}</span>
              </t-tooltip>
            </template>
            <template #ports="{ row }">
              <span class="mono ports">{{ row.ports || '-' }}</span>
            </template>
            <template #op="{ row }">
              <t-space break-line :size="4">
                <t-link v-if="row.state !== 'running'" theme="success" hover="color" @click="doAction(row, 'start')">启动</t-link>
                <t-link v-else theme="warning" hover="color" @click="doAction(row, 'stop')">停止</t-link>
                <t-link theme="primary" hover="color" @click="doAction(row, 'restart')">重启</t-link>
                <t-link theme="primary" hover="color" @click="openInspect(row)">配置</t-link>
                <t-link theme="primary" hover="color" @click="openLogs(row)">日志</t-link>
                <t-link theme="danger" hover="color" @click="doAction(row, 'rm')">删除</t-link>
              </t-space>
            </template>
          </t-table>
        </t-tab-panel>

        <t-tab-panel value="images" :label="`镜像（${images.length}）`">
          <t-table :data="images" :columns="imageColumns" row-key="id" size="small" hover>
            <template #repo="{ row }"><span class="mono">{{ row.repository }}</span></template>
            <template #tag="{ row }">
              <t-tag variant="outline" size="small">{{ row.tag }}</t-tag>
            </template>
          </t-table>
        </t-tab-panel>
      </t-tabs>
    </t-card>

    <!-- 容器配置查看 -->
    <t-dialog v-model:visible="showInspect" :header="`容器配置：${inspectName}`" width="860px" :footer="false">
      <t-loading :loading="inspecting">
        <template v-if="inspectData">
          <t-descriptions :column="2" size="small" bordered>
            <t-descriptions-item label="容器 ID">{{ inspectData.Id?.slice(0, 12) || '-' }}</t-descriptions-item>
            <t-descriptions-item label="名称">{{ inspectData.Name?.replace(/^\//, '') || '-' }}</t-descriptions-item>
            <t-descriptions-item label="镜像">{{ inspectData.Config?.Image || '-' }}</t-descriptions-item>
            <t-descriptions-item label="状态">{{ inspectData.State?.Status || '-' }}</t-descriptions-item>
            <t-descriptions-item label="重启策略">{{ inspectData.HostConfig?.RestartPolicy?.Name || 'no' }}</t-descriptions-item>
            <t-descriptions-item label="网络模式">{{ inspectData.HostConfig?.NetworkMode || '-' }}</t-descriptions-item>
            <t-descriptions-item label="特权模式">{{ inspectData.HostConfig?.Privileged ? '是' : '否' }}</t-descriptions-item>
            <t-descriptions-item label="创建时间">{{ inspectData.Created ? dayjs(inspectData.Created).format('YYYY-MM-DD HH:mm') : '-' }}</t-descriptions-item>
          </t-descriptions>

          <h4>端口映射</h4>
          <t-table v-if="inspectPorts.length" :data="inspectPorts" :columns="portCols" row-key="key" size="small" />
          <div v-else class="empty-hint">无端口映射</div>

          <h4>挂载路径</h4>
          <t-table v-if="inspectMounts.length" :data="inspectMounts" :columns="mountCols" row-key="key" size="small" />
          <div v-else class="empty-hint">无挂载</div>

          <h4>环境变量（{{ inspectEnv.length }}）</h4>
          <div v-if="inspectEnv.length" class="env-list">
            <div v-for="(e, i) in inspectEnv" :key="i" class="env-item">
              <span class="env-key">{{ e.split('=')[0] }}</span>
              <span class="env-val">{{ e.split('=').slice(1).join('=') }}</span>
            </div>
          </div>
          <div v-else class="empty-hint">无环境变量</div>
        </template>
      </t-loading>
    </t-dialog>

    <!-- 创建容器 -->
    <t-dialog v-model:visible="showCreate" header="创建容器" width="760px" :on-confirm="doCreate" :confirm-btn="{ loading: creating }">
      <t-form :data="createForm" label-width="110px">
        <t-form-item label="容器名称">
          <t-input v-model="createForm.name" placeholder="如 my-app" />
        </t-form-item>
        <t-form-item label="镜像">
          <t-input v-model="createForm.image" placeholder="如 nginx:latest" />
        </t-form-item>
        <t-form-item label="启动命令">
          <t-input v-model="createForm.cmd" placeholder="可选，如 /bin/sh -c 'echo hi'" />
        </t-form-item>
        <t-form-item label="重启策略">
          <t-select v-model="createForm.restart">
            <t-option value="no" label="不自动重启" />
            <t-option value="always" label="always" />
            <t-option value="unless-stopped" label="unless-stopped" />
            <t-option value="on-failure" label="on-failure" />
          </t-select>
        </t-form-item>
        <t-form-item label="网络模式">
          <t-input v-model="createForm.network" placeholder="可选，如 bridge / host / 自定义网络名" />
        </t-form-item>
        <t-form-item label="端口映射">
          <t-textarea v-model="createForm.portsText" placeholder="每行一条，如 8080:80 或 127.0.0.1:9000:9000" :autosize="{ minRows: 3 }" />
        </t-form-item>
        <t-form-item label="挂载路径">
          <t-textarea v-model="createForm.mountsText" placeholder="每行一条，如 /host/path:/container/path 或 /data:/data:ro" :autosize="{ minRows: 3 }" />
        </t-form-item>
        <t-form-item label="环境变量">
          <t-textarea v-model="createForm.envText" placeholder="每行一条，如 TZ=Asia/Shanghai" :autosize="{ minRows: 3 }" />
        </t-form-item>
        <t-form-item label="特权模式"><t-switch v-model="createForm.privileged" /></t-form-item>
        <t-form-item label="创建后启动"><t-switch v-model="createForm.auto_start" /></t-form-item>
      </t-form>
      <t-alert theme="info" style="font-size: 12px">
        创建过程的命令输出会显示在下方，可实时查看执行结果。
      </t-alert>
      <pre v-if="createOutput" class="exec-output">{{ createOutput }}</pre>
    </t-dialog>

    <!-- Docker Compose -->
    <t-dialog v-model:visible="showCompose" header="Docker Compose" width="820px" :on-confirm="doCompose" :confirm-btn="{ loading: composing }">
      <t-form :data="composeForm" label-width="110px">
        <t-form-item label="操作">
          <t-select v-model="composeForm.action">
            <t-option value="up -d" label="启动（up -d）" />
            <t-option value="down" label="停止并删除（down）" />
            <t-option value="ps" label="查看状态（ps）" />
            <t-option value="restart" label="重启（restart）" />
            <t-option value="pull" label="拉取镜像（pull）" />
          </t-select>
        </t-form-item>
        <t-form-item label="工作目录">
          <t-input v-model="composeForm.work_dir" placeholder="如 /opt/myapp（需已存在 docker-compose.yml）" />
        </t-form-item>
        <t-form-item label="compose 内容">
          <t-textarea
            v-model="composeForm.content"
            placeholder="可选。填写后先写入 工作目录/docker-compose.yml 再执行"
            :autosize="{ minRows: 10, maxRows: 16 }"
          />
        </t-form-item>
      </t-form>
      <pre v-if="composeOutput" class="exec-output">{{ composeOutput }}</pre>
    </t-dialog>

    <!-- 日志对话框 -->
    <t-dialog v-model:visible="showLogs" :header="`容器日志：${logTarget?.name || ''}`" width="900px" :footer="false">
      <div class="log-toolbar">
        <t-radio-group v-model="logLines" size="small" @change="reloadLogs">
          <t-radio-button :value="100">100 行</t-radio-button>
          <t-radio-button :value="500">500 行</t-radio-button>
          <t-radio-button :value="2000">2000 行</t-radio-button>
        </t-radio-group>
        <t-button size="small" theme="default" :loading="logsLoading" @click="reloadLogs">刷新</t-button>
      </div>
      <pre class="log-view">{{ logs || '(空)' }}</pre>
    </t-dialog>
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from 'vue';
import { useRoute } from 'vue-router';
import { MessagePlugin, DialogPlugin } from 'tdesign-vue-next';
import dayjs from 'dayjs';
import type { PrimaryTableCol } from 'tdesign-vue-next';

import { listAgents } from '@/api/agent';
import {
  dockerAction, dockerLogs, listDockerContainers, listDockerImages, dockerInspect, dockerCreate, dockerCompose,
  type DockerContainer, type DockerImage,
} from '@/api/docker';

const route = useRoute();
const currentAgent = ref<string>((route.query.agent as string) || '');
const agents = ref<{ label: string; value: string }[]>([]);
const loadingAgents = ref(false);
const loading = ref(false);
const checked = ref(false);
const available = ref(false);
const dockerError = ref('');
const activeTab = ref('containers');
const containers = ref<DockerContainer[]>([]);
const images = ref<DockerImage[]>([]);

/* 容器配置查看 */
const showInspect = ref(false);
const inspecting = ref(false);
const inspectName = ref('');
const inspectData = ref<any>(null);

/* 创建容器 */
const showCreate = ref(false);
const creating = ref(false);
const createOutput = ref('');
const createForm = ref({
  name: '', image: '', cmd: '', restart: 'no', network: '',
  portsText: '', mountsText: '', envText: '',
  privileged: false, auto_start: true,
});

/* Compose */
const showCompose = ref(false);
const composing = ref(false);
const composeOutput = ref('');
const composeForm = ref({ action: 'up -d', work_dir: '', content: '' });

const showLogs = ref(false);
const logTarget = ref<DockerContainer | null>(null);
const logs = ref('');
const logsLoading = ref(false);
const logLines = ref(200);

let timer: number | undefined;

const agentOptions = computed(() => agents.value);

/* 配置查看：从 inspect 数据提取 */
const inspectPorts = computed(() => {
  if (!inspectData.value) return [];
  const out: any[] = [];
  const binds = inspectData.value?.HostConfig?.PortBindings || {};
  Object.entries(binds).forEach(([contPort, arr]: [string, any]) => {
    (arr || []).forEach((b: any, i: number) => {
      const [cp, proto] = contPort.split('/');
      out.push({
        key: contPort + i,
        host_ip: b.HostIp || '0.0.0.0',
        host_port: b.HostPort,
        container_port: cp,
        proto: proto || 'tcp',
      });
    });
  });
  return out;
});

const inspectMounts = computed(() => {
  if (!inspectData.value) return [];
  return (inspectData.value?.Mounts || []).map((m: any, i: number) => ({
    key: m.Source + i,
    source: m.Source,
    target: m.Destination,
    type: m.Type || 'bind',
    read_only: m.RW === false,
  }));
});

const inspectEnv = computed(() => {
  if (!inspectData.value) return [];
  const raw = inspectData.value?.Config?.Env || [];
  return raw.filter((e: string) => e);
});

const portCols: PrimaryTableCol[] = [
  { colKey: 'host_ip', title: '主机 IP', width: 110 },
  { colKey: 'host_port', title: '主机端口', width: 100 },
  { colKey: 'container_port', title: '容器端口', width: 100 },
  { colKey: 'proto', title: '协议', width: 60 },
];

const mountCols: PrimaryTableCol[] = [
  { colKey: 'source', title: '宿主路径', width: 260, ellipsis: true },
  { colKey: 'target', title: '容器路径', width: 260, ellipsis: true },
  { colKey: 'type', title: '类型', width: 70 },
  { colKey: 'read_only', title: '只读', width: 70 },
];

const containerColumns: PrimaryTableCol[] = [
  { colKey: 'name', title: '名称', width: 180 },
  { colKey: 'image', title: '镜像', width: 220 },
  { colKey: 'state', title: '状态', width: 100 },
  { colKey: 'status', title: '运行信息', ellipsis: true },
  { colKey: 'ports', title: '端口', width: 180 },
  { colKey: 'op', title: '操作', width: 220, fixed: 'right' },
];

const imageColumns: PrimaryTableCol[] = [
  { colKey: 'repo', title: '仓库', width: 280 },
  { colKey: 'tag', title: '标签', width: 120 },
  { colKey: 'size', title: '大小', width: 110 },
  { colKey: 'created', title: '创建时间', ellipsis: true },
];

function stateTheme(s: string): 'success' | 'warning' | 'default' | 'danger' {
  if (s === 'running') return 'success';
  if (s === 'paused') return 'warning';
  if (s === 'exited') return 'default';
  return 'danger';
}

async function loadAgents() {
  loadingAgents.value = true;
  try {
    const list = await listAgents();
    agents.value = list
      .filter((a) => a.online && a.approval === 'approved')
      .map((a) => ({ label: `${a.hostname} (${a.ip})`, value: a.agent_id }));
    if (!currentAgent.value && agents.value.length) currentAgent.value = agents.value[0].value;
  } finally {
    loadingAgents.value = false;
  }
}

async function fetchAll() {
  if (!currentAgent.value) return;
  loading.value = true;
  try {
    const r = await listDockerContainers(currentAgent.value);
    available.value = r.available;
    dockerError.value = r.error || '';
    containers.value = r.containers || [];
    checked.value = true;
    if (r.available) {
      images.value = await listDockerImages(currentAgent.value).catch(() => []);
    }
  } catch (e: any) {
    MessagePlugin.error(e.message || '加载失败');
    checked.value = true;
    available.value = false;
  } finally {
    loading.value = false;
  }
}

function doAction(row: DockerContainer, action: string) {
  const label = { start: '启动', stop: '停止', restart: '重启', rm: '删除' }[action] || action;
  const dlg = DialogPlugin.confirm({
    header: `${label}容器`,
    body: action === 'rm'
      ? `确定强制删除容器「${row.name}」吗？此操作不可恢复。`
      : `确定${label}容器「${row.name}」吗？`,
    theme: action === 'rm' ? ('danger' as const) : undefined,
    onConfirm: async () => {
      try {
        await dockerAction(currentAgent.value, row.id, action);
        MessagePlugin.success(`${label}成功`);
        fetchAll();
      } catch (e: any) {
        MessagePlugin.error(e.message || `${label}失败`);
      }
      dlg.destroy();
    },
  });
}

/* 查看容器完整配置（inspect） */
async function openInspect(row: DockerContainer) {
  showInspect.value = true;
  inspecting.value = true;
  inspectName.value = row.name;
  inspectData.value = null;
  try {
    inspectData.value = await dockerInspect(currentAgent.value, row.id);
  } catch (e: any) {
    MessagePlugin.error(e.message || '加载配置失败');
    showInspect.value = false;
  } finally {
    inspecting.value = false;
  }
}

/* 打开创建容器对话框 */
function openCreate() {
  createForm.value = {
    name: '', image: '', cmd: '', restart: 'no', network: '',
    portsText: '', mountsText: '', envText: '',
    privileged: false, auto_start: true,
  };
  createOutput.value = '';
  showCreate.value = true;
}

/* 执行创建容器 */
async function doCreate() {
  const f = createForm.value;
  if (!f.name || !f.image) {
    MessagePlugin.warning('请填写容器名称和镜像');
    return;
  }
  creating.value = true;
  createOutput.value = '';
  try {
    const payload: any = {
      name: f.name,
      image: f.image,
      restart: f.restart,
      privileged: f.privileged,
      auto_start: f.auto_start,
    };
    if (f.cmd) payload.cmd = f.cmd;
    if (f.network) payload.network = f.network;
    if (f.portsText) payload.ports = f.portsText.split('\n').map((l: string) => l.trim()).filter(Boolean);
    if (f.mountsText) payload.mounts = f.mountsText.split('\n').map((l: string) => l.trim()).filter(Boolean);
    if (f.envText) payload.env = f.envText.split('\n').map((l: string) => l.trim()).filter(Boolean);

    const res = await dockerCreate(currentAgent.value, payload);
    createOutput.value = res.output || '（无输出）';
    if (res.ok) {
      MessagePlugin.success('创建成功');
      setTimeout(() => { showCreate.value = false; fetchAll(); }, 1500);
    } else {
      MessagePlugin.error('创建失败，详见输出');
    }
  } catch (e: any) {
    createOutput.value = e.message || '创建失败';
    MessagePlugin.error(e.message || '创建失败');
  } finally {
    creating.value = false;
  }
}

/* 打开 Compose 对话框 */
function openCompose() {
  composeForm.value = { action: 'up -d', work_dir: '', content: '' };
  composeOutput.value = '';
  showCompose.value = true;
}

/* 执行 Compose */
async function doCompose() {
  const f = composeForm.value;
  if (!f.action) {
    MessagePlugin.warning('请选择操作');
    return;
  }
  if (!f.content && !f.work_dir) {
    MessagePlugin.warning('请填写工作目录或 Compose 内容');
    return;
  }
  composing.value = true;
  composeOutput.value = '';
  try {
    const res = await dockerCompose(currentAgent.value, {
      action: f.action,
      work_dir: f.work_dir,
      content: f.content,
    });
    composeOutput.value = res.output || '（无输出）';
    if (res.ok) {
      MessagePlugin.success('执行完成');
      fetchAll();
    } else {
      MessagePlugin.warning('命令执行结束（可能含错误）');
    }
  } catch (e: any) {
    composeOutput.value = e.message || '执行失败';
    MessagePlugin.error(e.message || '执行失败');
  } finally {
    composing.value = false;
  }
}

async function openLogs(row: DockerContainer) {
  logTarget.value = row;
  logs.value = '';
  showLogs.value = true;
  await reloadLogs();
}

async function reloadLogs() {
  if (!logTarget.value) return;
  logsLoading.value = true;
  try {
    const r = await dockerLogs(currentAgent.value, logTarget.value.id, logLines.value);
    logs.value = r.logs || '';
  } catch (e: any) {
    logs.value = '加载失败：' + (e.message || '');
  } finally {
    logsLoading.value = false;
  }
}

onMounted(async () => {
  await loadAgents();
  fetchAll();
  timer = window.setInterval(() => {
    if (!showLogs.value) fetchAll();
  }, 15000);
});
onUnmounted(() => window.clearInterval(timer));
</script>

<style scoped>
.mono { font-family: Menlo, Monaco, monospace; font-size: 12px; }
.img { display: inline-block; max-width: 200px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.ports { font-size: 11px; color: var(--td-text-color-secondary); }
.log-toolbar {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 10px;
}
.exec-output {
  margin-top: 12px;
  background: #1e1e1e;
  color: #d4d4d4;
  padding: 12px;
  border-radius: 6px;
  font-family: Menlo, Monaco, monospace;
  font-size: 12px;
  max-height: 260px;
  overflow: auto;
  white-space: pre-wrap;
  word-break: break-all;
}
.env-list {
  margin-top: 8px;
}
.env-item {
  display: flex;
  gap: 8px;
  padding: 4px 0;
  font-family: Menlo, Monaco, monospace;
  font-size: 12px;
  border-bottom: 1px dashed var(--td-component-stroke);
}
.env-key {
  color: #4fc3f7;
  min-width: 120px;
}
.env-val {
  color: #d4d4d4;
  word-break: break-all;
}
.empty-hint {
  padding: 8px 0;
  color: var(--td-text-color-placeholder);
  font-size: 12px;
}
.log-view {
  background: #1e1e1e;
  color: #d4d4d4;
  padding: 14px;
  border-radius: 6px;
  font-size: 12px;
  line-height: 1.6;
  max-height: 520px;
  overflow: auto;
  white-space: pre-wrap;
  word-break: break-all;
  margin: 0;
}
</style>
