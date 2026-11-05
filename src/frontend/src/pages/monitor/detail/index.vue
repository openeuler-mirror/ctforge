<template>
  <div>
    <t-card :bordered="false" class="detail-head">
      <template #title>
        <t-space align="center">
          <span>{{ agent?.remark || query.hostname || query.agent }}</span>
          <t-tag v-if="agent" :theme="agent.online ? 'success' : 'default'" variant="light">
            {{ agent.online ? '在线' : '离线' }}
          </t-tag>
        </t-space>
      </template>
      <template #actions>
        <t-space>
          <t-button theme="warning" size="small" @click="onChangeIP">改IP</t-button>
          <t-button theme="success" size="small" @click="onRemoteUpgrade">升级</t-button>
          <t-button theme="danger" size="small" @click="onRemoteUninstall">卸载</t-button>
          <t-button theme="default" size="small" @click="onSetRemark">备注</t-button>
          <t-button theme="primary" :loading="loadingAgent" @click="fetchAll">刷新</t-button>
        </t-space>
      </template>
      <t-descriptions v-if="agent" :column="4">
        <t-descriptions-item label="Agent ID">{{ agent.agent_id }}</t-descriptions-item>
        <t-descriptions-item label="IP">{{ agent.ip }}</t-descriptions-item>
        <t-descriptions-item label="系统">{{ agent.os }} / {{ agent.arch }}</t-descriptions-item>
        <t-descriptions-item label="版本">{{ agent.version }}</t-descriptions-item>
        <t-descriptions-item label="上线时间">{{ formatTime(agent.online_at) }}</t-descriptions-item>
        <t-descriptions-item label="离线时间">{{ formatTime(agent.offline_at) }}</t-descriptions-item>
        <t-descriptions-item label="最后在线">{{ formatTime(agent.last_seen) }}</t-descriptions-item>
        <t-descriptions-item label="运行时长">{{ formatUptime(agent.last_metrics?.uptime_sec) }}</t-descriptions-item>
        <t-descriptions-item label="进程数">{{ agent.last_metrics?.procs ?? '-' }}</t-descriptions-item>
        <t-descriptions-item label="内存">
          {{ formatKB(agent.last_metrics?.mem_used_kb) }} / {{ formatKB(agent.last_metrics?.mem_total_kb) }}
        </t-descriptions-item>
        <t-descriptions-item label="磁盘">
          {{ formatKB(agent.last_metrics?.disk_used_kb) }} / {{ formatKB(agent.last_metrics?.disk_total_kb) }}
        </t-descriptions-item>
      </t-descriptions>
    </t-card>

    <t-row :gutter="16" style="margin-top: 16px">
      <t-col :span="6">
        <t-card title="CPU 使用率" :bordered="false">
          <div ref="cpuChart" class="chart"></div>
        </t-card>
      </t-col>
      <t-col :span="6">
        <t-card title="内存使用率" :bordered="false">
          <div ref="memChart" class="chart"></div>
        </t-card>
      </t-col>
    </t-row>
    <t-row :gutter="16" style="margin-top: 16px">
      <t-col :span="6">
        <t-card title="磁盘使用率" :bordered="false">
          <div ref="diskChart" class="chart"></div>
        </t-card>
      </t-col>
      <t-col :span="6">
        <t-card title="负载 (1min)" :bordered="false">
          <div ref="loadChart" class="chart"></div>
        </t-card>
      </t-col>
    </t-row>

    <t-card title="日志" :bordered="false" style="margin-top: 16px">
      <t-table :data="logs" :columns="logColumns" row-key="timestamp" :loading="loadingLogs" size="small" hover>
        <template #level="{ row }">
          <t-tag :theme="logTheme(row.level)" variant="light" size="small">{{ row.level }}</t-tag>
        </template>
        <template #time="{ row }">
          {{ dayjs(row.timestamp).format('MM-DD HH:mm:ss') }}
        </template>
      </t-table>
    </t-card>
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref, shallowRef } from 'vue';
import { DialogPlugin, MessagePlugin } from 'tdesign-vue-next';
import { useRoute } from 'vue-router';
import * as echarts from 'echarts';
import dayjs from 'dayjs';

import { getAgent, getLogs, getMetrics, http, type Agent, type LogEntry, type MetricPoint } from '@/api/agent';

const route = useRoute();
const query = computed(() => route.query as { agent: string; hostname?: string });

const agent = ref<Agent | null>(null);
const logs = ref<LogEntry[]>([]);
const series = ref<MetricPoint[]>([]);
const loadingAgent = ref(false);
const loadingLogs = ref(false);

const cpuChart = ref<HTMLElement>();
const memChart = ref<HTMLElement>();
const diskChart = ref<HTMLElement>();
const loadChart = ref<HTMLElement>();
const charts = shallowRef<echarts.ECharts[]>([]);

let timer: number | undefined;

const logColumns = [
  { colKey: 'time', title: '时间', width: 130 },
  { colKey: 'level', title: '级别', width: 80 },
  { colKey: 'message', title: '内容', ellipsis: true },
];

function logTheme(level: string) {
  switch (level) {
    case 'error': return 'danger';
    case 'warn': return 'warning';
    default: return 'primary';
  }
}

function formatTime(v?: number | string) {
  if (!v) return '-';
  let d: Date;
  if (typeof v === 'number') {
    // 毫秒时间戳（13 位）或秒时间戳（10 位）
    d = new Date(v > 1e12 ? v : v * 1000);
  } else {
    d = new Date(v);
  }
  if (isNaN(d.getTime())) return '-';
  const p = (n: number) => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${p(d.getMonth()+1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
}

function formatUptime(sec?: number) {
  if (!sec) return '-';
  const d = Math.floor(sec / 86400);
  const h = Math.floor((sec % 86400) / 3600);
  const m = Math.floor((sec % 3600) / 60);
  return d ? `${d}天${h}时` : h ? `${h}时${m}分` : `${m}分`;
}

function formatKB(kb?: number) {
  if (kb == null) return '-';
  if (kb > 1024 * 1024) return (kb / 1024 / 1024).toFixed(1) + ' GB';
  if (kb > 1024) return (kb / 1024).toFixed(0) + ' MB';
  return kb + ' KB';
}

function renderChart(el: HTMLElement | undefined, name: string, data: MetricPoint[], key: keyof MetricPoint, color: string) {
  if (!el) return;
  let chart = echarts.getInstanceByDom(el);
  if (!chart) {
    chart = echarts.init(el);
    charts.value.push(chart);
  }
  chart.setOption({
    grid: { left: 42, right: 12, top: 16, bottom: 24 },
    tooltip: { trigger: 'axis' },
    xAxis: {
      type: 'category',
      data: data.map((p) => dayjs(p.timestamp).format('HH:mm:ss')),
      axisLabel: { hideOverlap: true },
    },
    yAxis: { type: 'value', max: key === 'load1' ? undefined : 100 },
    series: [{
      name,
      type: 'line',
      data: data.map((p) => Math.round((p[key] as number) * 100) / 100),
      smooth: true,
      showSymbol: false,
      areaStyle: { opacity: 0.15 },
      lineStyle: { color },
      itemStyle: { color },
    }],
  });
}

function renderAll() {
  const d = series.value;
  renderChart(cpuChart.value, 'CPU %', d, 'cpu', '#0594fa');
  renderChart(memChart.value, '内存 %', d, 'mem', '#00a870');
  renderChart(diskChart.value, '磁盘 %', d, 'disk', '#ed7b2f');
  renderChart(loadChart.value, 'Load1', d, 'load1', '#834ec2');
}

async function fetchAll() {
  loadingAgent.value = true;
  try {
    const [a, m, l] = await Promise.all([
      getAgent(query.value.agent),
      getMetrics(query.value.agent),
      getLogs(query.value.agent, 100),
    ]);
    agent.value = a;
    series.value = m;
    logs.value = [...l].reverse();
    renderAll();
  } finally {
    loadingAgent.value = false;
  }
}

function onResize() {
  charts.value.forEach((c) => c.resize());
}

/** 修改服务端 IP */
/** 设置主机备注名 */
async function onSetRemark() {
  if (!agent.value) return;
  const name = window.prompt('主机备注名（留空清除备注）：', agent.value.remark || '');
  if (name === null) return;
  try {
    const { setAgentRemark } = await import('@/api/agent');
    await setAgentRemark(agent.value.agent_id, name.trim());
    MessagePlugin.success(name.trim() ? '已设置备注' : '已清除备注');
    fetchAll();
  } catch (e: any) {
    MessagePlugin.error(e.message || '设置备注失败');
  }
}

async function onChangeIP() {
  const curIP = agent.value?.ip || '';
  const newIP = window.prompt(
    `修改服务端 IP\n当前 IP：${curIP}\n输入新的服务端地址：`,
    curIP,
  );
  if (!newIP || newIP === curIP) return;
  try {
    await http.post(`/agents/${query.value.agent}/server-ip`, {
      server_host: newIP.trim(),
      server_port: 9000,
    });
    MessagePlugin.success(`已通知客户端切换到 ${newIP.trim()}，重连后生效`);
  } catch (e: any) {
    MessagePlugin.error(e.message || '操作失败');
  }
}

/** 远程升级 */
async function onRemoteUpgrade() {
  const dlg = DialogPlugin.confirm({
    header: `远程升级 · ${query.value.hostname || query.value.agent}`,
    body: '将从服务端拉取最新版本的 agent 并替换，升级后客户端会自动重启。',
    onConfirm: async () => {
      try {
        await http.post(`/agents/${query.value.agent}/upgrade`, {});
        MessagePlugin.success('升级命令已下发');
        fetchAll();
      } catch (e: any) {
        MessagePlugin.error(e.message || '升级失败');
      }
      dlg.destroy();
    },
  });
}

/** 远程卸载（从列表删除并清理客户端文件） */
async function onRemoteUninstall() {
  const dlg = DialogPlugin.confirm({
    header: `远程卸载 · ${query.value.hostname || query.value.agent}`,
    body: '将彻底卸载该客户端：\n1. 从主机列表删除\n2. 在客户端主机删除 agent 二进制与配置文件\n此操作不可恢复！',
    theme: 'danger',
    onConfirm: async () => {
      try {
        await http.post(`/agents/${query.value.agent}/remote-uninstall`, {});
        MessagePlugin.success('卸载命令已下发，客户端将被清除');
        setTimeout(() => window.history.back(), 2000);
      } catch (e: any) {
        MessagePlugin.error(e.message || '卸载失败');
      }
      dlg.destroy();
    },
  });
}

onMounted(() => {
  fetchAll();
  timer = window.setInterval(fetchAll, 15000);
  window.addEventListener('resize', onResize);
});
onUnmounted(() => {
  window.clearInterval(timer);
  window.removeEventListener('resize', onResize);
  charts.value.forEach((c) => c.dispose());
});
</script>

<style scoped>
.chart {
  height: 200px;
}
</style>
