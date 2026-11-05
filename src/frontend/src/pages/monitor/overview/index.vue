<template>
  <div>
    <!-- 顶部：主机选择 -->
    <t-card :bordered="false">
      <template #title>
        <t-space align="center">
          <span>主机总览</span>
          <t-select v-model="currentAgent" :options="agentOptions" placeholder="选择主机" style="width: 280px" :loading="loadingAgents" @change="onAgentChange" />
          <t-tag v-if="agent" :theme="agent.online ? 'success' : 'danger'" variant="light">
            {{ agent.online ? '在线' : '离线' }}
          </t-tag>
        </t-space>
      </template>
      <template #actions>
        <t-space>
          <t-button v-if="aiAvailable" theme="default" size="small" @click="aiOpen = true" :disabled="!agent">AI 分析</t-button>
          <t-button theme="default" size="small" @click="fetchAll" :loading="loading">刷新</t-button>
        </t-space>
      </template>
      <t-descriptions v-if="agent" :column="4">
        <t-descriptions-item label="主机名">{{ agent.hostname }}</t-descriptions-item>
        <t-descriptions-item label="IP">{{ agent.ip }}</t-descriptions-item>
        <t-descriptions-item label="系统">{{ agent.os_version || agent.os }}</t-descriptions-item>
        <t-descriptions-item label="内核">{{ agent.kernel || '-' }}</t-descriptions-item>
        <t-descriptions-item label="延迟">
          <span :class="latencyClass(agent.latency_ms)">{{ agent.latency_ms != null ? agent.latency_ms.toFixed(0) + 'ms' : '-' }}</span>
        </t-descriptions-item>
        <t-descriptions-item label="网络">
          <span v-if="agent.net_rx_rate">↓{{ formatRate(agent.net_rx_rate) }} ↑{{ formatRate(agent.net_tx_rate) }}</span>
          <span v-else>-</span>
        </t-descriptions-item>
        <t-descriptions-item label="离线次数">{{ agent.offline_count || 0 }}</t-descriptions-item>
        <t-descriptions-item label="批准状态">
          <t-tag variant="outline" size="small">{{ agent.approval || '-' }}</t-tag>
        </t-descriptions-item>
      </t-descriptions>
    </t-card>

    <!-- 实时指标图 -->
    <t-row :gutter="12" style="margin-top: 12px">
      <t-col :span="6"><t-card title="CPU %" :bordered="false"><div ref="cpuEl" class="mini-chart"></div></t-card></t-col>
      <t-col :span="6"><t-card title="内存 %" :bordered="false"><div ref="memEl" class="mini-chart"></div></t-card></t-col>
    </t-row>
    <t-row :gutter="12" style="margin-top: 12px">
      <t-col :span="6"><t-card title="磁盘 %" :bordered="false"><div ref="diskEl" class="mini-chart"></div></t-card></t-col>
      <t-col :span="6"><t-card title="负载" :bordered="false"><div ref="loadEl" class="mini-chart"></div></t-card></t-col>
    </t-row>

    <t-row :gutter="12" style="margin-top: 12px">
      <!-- 磁盘 IO -->
      <t-col :span="6">
        <t-card title="磁盘 IO" :bordered="false" :loading="loadingIO">
          <t-table :data="diskIO" :columns="ioColumns" row-key="name" size="small" :max-height="280">
            <template #read="{ row }">{{ row.read_kbps >= 1024 ? (row.read_kbps/1024).toFixed(1)+'MB/s' : row.read_kbps.toFixed(0)+'KB/s' }}</template>
            <template #write="{ row }">{{ row.write_kbps >= 1024 ? (row.write_kbps/1024).toFixed(1)+'MB/s' : row.write_kbps.toFixed(0)+'KB/s' }}</template>
            <template #util="{ row }">
              <t-progress :percentage="Math.min(100, Math.round(row.util_pct))" size="small" :label="true" style="width: 80px" />
            </template>
          </t-table>
        </t-card>
      </t-col>
      <!-- 任务执行记录 -->
      <t-col :span="6">
        <t-card title="最近任务" :bordered="false">
          <template #actions><t-link theme="primary" hover="color" @click="$router.push('/security/tasks')">全部</t-link></template>
          <t-table :data="taskRecords" :columns="taskColumns" row-key="started_at" size="small" :max-height="280">
            <template #ok="{ row }"><t-tag :theme="row.ok ? 'success' : 'danger'" variant="light" size="small">{{ row.ok ? '成功' : '失败' }}</t-tag></template>
            <template #time="{ row }">{{ formatMs(row.finished_at) }}</template>
          </t-table>
        </t-card>
      </t-col>
    </t-row>

    <!-- 命令执行 -->
    <t-card title="快速命令" :bordered="false" style="margin-top: 12px">
      <t-form layout="inline" @submit.prevent="onExec">
        <t-form-item label="命令" style="flex: 1">
          <t-input v-model="execCmd" placeholder="如 df -h 或 ps aux | head" style="width: 100%" @enter="onExec" />
        </t-form-item>
        <t-form-item>
          <t-button theme="primary" :loading="executing" :disabled="!currentAgent || !agent?.online" @click="onExec">执行</t-button>
        </t-form-item>
      </t-form>
      <div v-if="execResult" class="exec-result" :class="{ failed: !execResult.ok }">
        <pre>{{ execResult.stdout }}{{ execResult.stderr ? '\n' + execResult.stderr : '' }}</pre>
      </div>
    </t-card>

    <!-- AI 分析对话框 -->
    <t-dialog v-model:visible="aiOpen" header="AI 健康分析" width="720px" :footer="false">
      <t-loading :loading="aiLoading">
        <template v-if="aiResult">
          <h4>结论</h4>
          <p class="ai-summary">{{ aiResult.summary }}</p>
          <template v-if="aiResult.issues && aiResult.issues.length">
            <h4>发现的问题（{{ aiResult.issues.length }}）</h4>
            <t-list>
              <t-list-item v-for="(i, idx) in aiResult.issues" :key="idx">
                <div class="ai-issue">
                  <t-tag :theme="i.level === 'critical' ? 'danger' : i.level === 'warning' ? 'warning' : 'primary'" variant="light" size="small">
                    {{ levelText(i.level) }}
                  </t-tag>
                  <span class="issue-title">{{ i.title }}</span>
                  <span class="issue-metric">{{ i.metric }}</span>
                </div>
                <div class="issue-detail">{{ i.detail }}</div>
              </t-list-item>
            </t-list>
          </template>
          <template v-if="aiResult.suggests && aiResult.suggests.length">
            <h4>优化建议</h4>
            <ul class="ai-suggests">
              <li v-for="(s, idx) in aiResult.suggests" :key="idx">{{ s }}</li>
            </ul>
          </template>
        </template>
        <t-empty v-else description="点击下方按钮开始分析" />
      </t-loading>
      <template #footer>
        <t-button theme="default" @click="aiOpen = false">关闭</t-button>
        <t-button theme="primary" :loading="aiLoading" @click="runAI">开始分析</t-button>
      </template>
    </t-dialog>
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref, shallowRef } from 'vue';
import { useRoute } from 'vue-router';
import { MessagePlugin } from 'tdesign-vue-next';
import * as echarts from 'echarts';
import dayjs from 'dayjs';

import {
  aiAnalyze, execCommand, getAIStatus, getAgent, getDiskIO, getMetrics, listAgents,
  listTaskRecords, type Agent, type CmdResult, type DiskIODev, type MetricPoint,
} from '@/api/agent';

const route = useRoute();
const currentAgent = ref<string>((route.query.agent as string) || '');
const agents = ref<{ label: string; value: string }[]>([]);
const loadingAgents = ref(false);
const loading = ref(false);
const loadingIO = ref(false);
const agent = ref<Agent | null>(null);
const series = ref<MetricPoint[]>([]);
const diskIO = ref<DiskIODev[]>([]);
const taskRecords = ref<any[]>([]);
const execCmd = ref('');
const executing = ref(false);
const execResult = ref<CmdResult | null>(null);
const aiOpen = ref(false);
const aiAvailable = ref(false);
const aiLoading = ref(false);
const aiResult = ref<any>(null);

const cpuEl = ref<HTMLElement>(); const memEl = ref<HTMLElement>();
const diskEl = ref<HTMLElement>(); const loadEl = ref<HTMLElement>();
const charts = shallowRef<echarts.ECharts[]>([]);

let timer: number | undefined;

const agentOptions = computed(() => agents.value);

const ioColumns = [
  { colKey: 'name', title: '设备', width: 100 },
  { colKey: 'read', title: '读', width: 100 },
  { colKey: 'write', title: '写', width: 100 },
  { colKey: 'util', title: '利用率', width: 100 },
];
const taskColumns = [
  { colKey: 'task_name', title: '任务', width: 120, ellipsis: true },
  { colKey: 'ok', title: '结果', width: 70 },
  { colKey: 'time', title: '时间', width: 140 },
];

function formatMs(ms?: number) { return ms ? dayjs(ms).format('MM-DD HH:mm:ss') : '-'; }
function levelText(l: string) {
  const m: Record<string, string> = { critical: '严重', warning: '警告', info: '提示' };
  return m[l] || l;
}
function formatRate(bps?: number) {
  if (!bps) return '-';
  if (bps > 1024*1024) return (bps/1024/1024).toFixed(1)+' MB/s';
  if (bps > 1024) return (bps/1024).toFixed(1)+' KB/s';
  return bps.toFixed(0)+' B/s';
}
function latencyClass(ms?: number) {
  if (ms == null) return '';
  if (ms < 50) return 'lat-ok';
  if (ms < 200) return 'lat-mid';
  return 'lat-bad';
}

async function loadAgents() {
  loadingAgents.value = true;
  try {
    const list = await listAgents();
    agents.value = list
      .filter((a) => a.approval === 'approved')
      .map((a) => ({
        label: `${a.hostname} (${a.ip})${a.online ? '' : ' [离线]'}`,
        value: a.agent_id,
      }));
    if (!currentAgent.value && agents.value.length) currentAgent.value = agents.value[0].value;
  } finally {
    loadingAgents.value = false;
  }
}

function renderChart(el: HTMLElement | undefined, data: MetricPoint[], key: keyof MetricPoint, color: string) {
  if (!el || !data.length) return;
  let chart = echarts.getInstanceByDom(el);
  if (!chart) {
    chart = echarts.init(el);
    charts.value.push(chart);
  }
  chart.setOption({
    grid: { left: 36, right: 8, top: 12, bottom: 20 },
    tooltip: { trigger: 'axis' },
    xAxis: { type: 'category', data: data.map((p) => dayjs(p.timestamp).format('HH:mm:ss')), axisLabel: { hideOverlap: true, fontSize: 10 } },
    yAxis: { type: 'value', max: key === 'load1' ? undefined : 100 },
    series: [{ type: 'line', data: data.map((p) => Math.round((p[key] as number) * 100) / 100), smooth: true, showSymbol: false, areaStyle: { opacity: 0.15 }, lineStyle: { color }, itemStyle: { color } }],
  });
}

function renderAll() {
  const d = series.value;
  renderChart(cpuEl.value, d, 'cpu', '#0594fa');
  renderChart(memEl.value, d, 'mem', '#00a870');
  renderChart(diskEl.value, d, 'disk', '#ed7b2f');
  renderChart(loadEl.value, d, 'load1', '#834ec2');
}

async function fetchAll() {
  if (!currentAgent.value) return;
  loading.value = true;
  try {
    const [a, m, io, tr] = await Promise.all([
      getAgent(currentAgent.value),
      getMetrics(currentAgent.value),
      currentAgent.value ? getDiskIO(currentAgent.value).catch(() => null) : null,
      listTaskRecords(20),
    ]);
    agent.value = a;
    series.value = m;
    if (io) diskIO.value = io.devices;
    taskRecords.value = tr.filter((r: any) => r.agent_id === currentAgent.value || r.agent_id === '*');
    renderAll();
  } catch (e: any) {
    MessagePlugin.error(e.message || '加载失败');
  } finally {
    loading.value = false;
  }
}

async function onAgentChange() {
  execResult.value = null;
  fetchAll();
}

async function onExec() {
  if (!execCmd.value.trim()) return;
  executing.value = true;
  execResult.value = null;
  try {
    execResult.value = await execCommand(currentAgent.value, execCmd.value, 60);
  } catch (e: any) {
    MessagePlugin.error(e.message || '执行失败');
  } finally {
    executing.value = false;
  }
}

async function runAI() {
  aiLoading.value = true;
  aiResult.value = null;
  try {
    aiResult.value = await aiAnalyze(currentAgent.value);
  } catch (e: any) {
    MessagePlugin.error(e.message || 'AI 分析失败');
  } finally {
    aiLoading.value = false;
  }
}

function onResize() { charts.value.forEach((c) => c.resize()); }

onMounted(async () => {
  try { aiAvailable.value = (await getAIStatus()).enabled; } catch { aiAvailable.value = false; }
  await loadAgents();
  fetchAll();
  timer = window.setInterval(fetchAll, 10000);
  window.addEventListener('resize', onResize);
});
onUnmounted(() => {
  window.clearInterval(timer);
  window.removeEventListener('resize', onResize);
  charts.value.forEach((c) => c.dispose());
});
</script>

<style scoped>
.mini-chart { height: 180px; }
.exec-result {
  margin-top: 12px;
  background: #1e1e1e;
  color: #d4d4d4;
  padding: 12px;
  border-radius: 6px;
  font-family: monospace;
  font-size: 12px;
  max-height: 300px;
  overflow: auto;
  white-space: pre-wrap;
  word-break: break-all;
}
.exec-result.failed pre { color: #f48771; }
.lat-ok { color: #2ba471; } .lat-mid { color: #ed7b2f; } .lat-bad { color: #e34d59; }
.ai-summary { font-size: 14px; line-height: 1.8; padding: 8px 0; }
.ai-issue { display: flex; align-items: center; gap: 8px; margin-bottom: 4px; }
.issue-title { font-weight: 500; flex: 1; }
.issue-metric { font-family: monospace; font-size: 12px; color: var(--td-text-color-placeholder); }
.issue-detail { font-size: 12px; color: var(--td-text-color-secondary); margin-left: 4px; }
.ai-suggests { padding-left: 20px; } .ai-suggests li { margin-bottom: 6px; line-height: 1.6; }
h4 { margin: 16px 0 8px; font-size: 14px; font-weight: 600; }
</style>
