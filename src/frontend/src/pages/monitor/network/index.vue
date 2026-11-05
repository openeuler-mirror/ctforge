<template>
  <div>
    <t-card :bordered="false">
      <template #title>
        <t-space align="center">
          <span>网络监控</span>
          <t-select v-model="currentAgent" :options="agentOptions" placeholder="选择主机" style="width: 260px" :loading="loadingAgents" @change="fetchAll" />
        </t-space>
      </template>
      <template #actions>
        <t-button theme="primary" :loading="loading" @click="fetchAll">刷新</t-button>
      </template>

      <!-- 概览卡片 -->
      <t-row :gutter="16" style="margin-bottom: 16px">
        <t-col :span="3">
          <t-card :bordered="false" class="stat-card">
            <div class="stat-num">{{ conn?.total_tcp ?? 0 }}</div>
            <div class="stat-label">TCP 连接总数</div>
          </t-card>
        </t-col>
        <t-col :span="3">
          <t-card :bordered="false" class="stat-card">
            <div class="stat-num">{{ conn?.udp_count ?? 0 }}</div>
            <div class="stat-label">UDP 端口数</div>
          </t-card>
        </t-col>
        <t-col :span="3">
          <t-card :bordered="false" class="stat-card">
            <div class="stat-num">{{ formatRate(traffic?.rx_rate) }}</div>
            <div class="stat-label">接收速率</div>
          </t-card>
        </t-col>
        <t-col :span="3">
          <t-card :bordered="false" class="stat-card">
            <div class="stat-num">{{ formatRate(traffic?.tx_rate) }}</div>
            <div class="stat-label">发送速率</div>
          </t-card>
        </t-col>
      </t-row>

      <t-row :gutter="16">
        <!-- 接口流量 -->
        <t-col :span="7">
          <t-card title="接口流量" :bordered="false">
            <t-table :data="traffic?.interfaces" :columns="ifaceColumns" row-key="name" hover size="small" :max-height="320">
              <template #up="{ row }">
                <t-tag :theme="row.up ? 'success' : 'default'" variant="light" size="small">{{ row.up ? 'UP' : 'DOWN' }}</t-tag>
              </template>
              <template #rx="{ row }">{{ formatBytes(row.rx_bytes) }}</template>
              <template #tx="{ row }">{{ formatBytes(row.tx_bytes) }}</template>
            </t-table>
          </t-card>
        </t-col>

        <!-- TCP 状态分布 -->
        <t-col :span="5">
          <t-card title="TCP 状态分布" :bordered="false">
            <div ref="stateChart" class="chart"></div>
          </t-card>
        </t-col>

        <!-- TOP 远端 -->
        <t-col :span="12">
          <t-card title="连接数 TOP 远端" :bordered="false">
            <t-table :data="conn?.top_remotes" :columns="remoteColumns" row-key="address" hover size="small" :max-height="320">
              <template #addr="{ row }"><span class="mono">{{ row.address }}</span></template>
            </t-table>
          </t-card>
        </t-col>
      </t-row>
    </t-card>
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref, shallowRef } from 'vue';
import { useRoute } from 'vue-router';
import * as echarts from 'echarts';

import { listAgents, getNetTraffic, getNetConn, type NetConn } from '@/api/agent';

const route = useRoute();
const currentAgent = ref<string>((route.query.agent as string) || '');
const agents = ref<{ label: string; value: string }[]>([]);
const loadingAgents = ref(false);
const loading = ref(false);
const traffic = ref<any>(null);
const conn = ref<NetConn | null>(null);
const stateChart = ref<HTMLElement>();
const charts = shallowRef<echarts.ECharts[]>([]);

let timer: number | undefined;

const agentOptions = computed(() => agents.value);

const ifaceColumns = [
  { colKey: 'name', title: '接口', width: 100 },
  { colKey: 'up', title: '状态', width: 70 },
  { colKey: 'rx', title: '接收', width: 100 },
  { colKey: 'tx', title: '发送', width: 100 },
];

const remoteColumns = [
  { colKey: 'addr', title: '远端地址', width: 200 },
  { colKey: 'count', title: '连接数', width: 100 },
];

function formatBytes(n: number) {
  if (n > 1024 * 1024 * 1024) return (n / 1024 / 1024 / 1024).toFixed(1) + 'G';
  if (n > 1024 * 1024) return (n / 1024 / 1024).toFixed(1) + 'M';
  if (n > 1024) return (n / 1024).toFixed(1) + 'K';
  return n + 'B';
}

function formatRate(bps?: number) {
  if (bps == null) return '-';
  if (bps > 1024 * 1024) return (bps / 1024 / 1024).toFixed(1) + ' MB/s';
  if (bps > 1024) return (bps / 1024).toFixed(1) + ' KB/s';
  return bps.toFixed(0) + ' B/s';
}

function renderStateChart() {
  if (!stateChart.value || !conn.value) return;
  let chart = echarts.getInstanceByDom(stateChart.value);
  if (!chart) {
    chart = echarts.init(stateChart.value);
    charts.value.push(chart);
  }
  const data = Object.entries(conn.value.tcp_states || {}).map(([name, value]) => ({ name, value }));
  chart.setOption({
    tooltip: { trigger: 'item' },
    series: [{
      type: 'pie',
      radius: ['40%', '70%'],
      label: { show: true, formatter: '{b}: {c}' },
      data,
    }],
  });
}

async function loadAgents() {
  loadingAgents.value = true;
  try {
    const list = await listAgents();
    agents.value = list.map((a) => ({
      label: `${a.hostname} (${a.ip})${a.online ? '' : ' [离线]'}`,
      value: a.agent_id,
    }));
    if (!currentAgent.value && agents.value.length) currentAgent.value = agents.value[0].value;
  } finally {
    loadingAgents.value = false;
  }
}

async function fetchAll() {
  if (!currentAgent.value) return;
  loading.value = true;
  try {
    const [t, c] = await Promise.all([
      getNetTraffic(currentAgent.value),
      getNetConn(currentAgent.value),
    ]);
    traffic.value = t;
    conn.value = c;
    renderStateChart();
  } finally {
    loading.value = false;
  }
}

function onResize() {
  charts.value.forEach((c) => c.resize());
}

onMounted(async () => {
  await loadAgents();
  fetchAll();
  timer = window.setInterval(fetchAll, 1000);
  window.addEventListener('resize', onResize);
});
onUnmounted(() => {
  window.clearInterval(timer);
  window.removeEventListener('resize', onResize);
  charts.value.forEach((c) => c.dispose());
});
</script>

<style scoped>
.stat-card {
  text-align: center;
  padding: 16px 8px;
}
.stat-num {
  font-size: 28px;
  font-weight: 600;
  color: var(--td-text-color-primary);
}
.stat-label {
  font-size: 12px;
  color: var(--td-text-color-secondary);
  margin-top: 4px;
}
.chart {
  height: 240px;
}
.mono { font-family: monospace; font-size: 12px; }
</style>
