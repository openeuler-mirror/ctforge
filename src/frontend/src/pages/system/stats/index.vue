<template>
  <div>
    <t-card :bordered="false">
      <template #title>全局统计</template>
      <template #actions>
        <t-button theme="primary" :loading="loading" @click="fetchAll">刷新</t-button>
      </template>

      <!-- 概览卡片 -->
      <t-row :gutter="12">
        <t-col :span="2"><t-card :bordered="false" class="ov-card"><div class="ov-num">{{ ov.total ?? 0 }}</div><div class="ov-label">主机总数</div></t-card></t-col>
        <t-col :span="2"><t-card :bordered="false" class="ov-card"><div class="ov-num green">{{ ov.online ?? 0 }}</div><div class="ov-label">在线</div></t-card></t-col>
        <t-col :span="2"><t-card :bordered="false" class="ov-card"><div class="ov-num red">{{ ov.offline ?? 0 }}</div><div class="ov-label">离线</div></t-card></t-col>
        <t-col :span="2"><t-card :bordered="false" class="ov-card"><div class="ov-num orange">{{ ov.pending ?? 0 }}</div><div class="ov-label">待批准</div></t-card></t-col>
        <t-col :span="2"><t-card :bordered="false" class="ov-card"><div class="ov-num">{{ ov.availability ?? 0 }}%</div><div class="ov-label">可用率</div></t-card></t-col>
        <t-col :span="2"><t-card :bordered="false" class="ov-card"><div class="ov-num red">{{ ov.alerts_open ?? 0 }}</div><div class="ov-label">未解决告警</div></t-card></t-col>
      </t-row>

      <t-row :gutter="12" style="margin-top: 12px">
        <t-col :span="2"><t-card :bordered="false" class="ov-card"><div class="ov-num">{{ ov.avg_cpu ?? 0 }}%</div><div class="ov-label">平均 CPU</div></t-card></t-col>
        <t-col :span="2"><t-card :bordered="false" class="ov-card"><div class="ov-num">{{ ov.avg_mem ?? 0 }}%</div><div class="ov-label">平均内存</div></t-card></t-col>
        <t-col :span="2"><t-card :bordered="false" class="ov-card"><div class="ov-num">{{ ov.avg_disk ?? 0 }}%</div><div class="ov-label">平均磁盘</div></t-card></t-col>
        <t-col :span="2"><t-card :bordered="false" class="ov-card"><div class="ov-num">{{ ov.avg_latency ?? 0 }}ms</div><div class="ov-label">平均延迟</div></t-card></t-col>
        <t-col :span="2"><t-card :bordered="false" class="ov-card"><div class="ov-num">{{ ov.offline_total ?? 0 }}</div><div class="ov-label">累计离线次数</div></t-card></t-col>
      </t-row>

      <!-- 按主机统计 -->
      <h3 class="sec-title">按主机（24h）</h3>
      <t-table :data="agentStats" :columns="columns" row-key="agent_id" hover size="small">
        <template #online="{ row }">
          <t-tag :theme="row.online ? 'success' : 'default'" variant="light" size="small">{{ row.online ? '在线' : '离线' }}</t-tag>
        </template>
        <template #latency="{ row }">{{ row.latency_ms != null ? row.latency_ms.toFixed(0) + 'ms' : '-' }}</template>
        <template #avg_latency="{ row }">{{ row.avg_latency_ms != null ? row.avg_latency_ms.toFixed(0) + 'ms' : '-' }}</template>
        <template #cpu="{ row }"><span :class="pctClass(row.cpu)">{{ row.cpu != null ? row.cpu.toFixed(1) + '%' : '-' }}</span></template>
        <template #mem="{ row }"><span :class="pctClass(row.mem)">{{ row.mem != null ? row.mem.toFixed(1) + '%' : '-' }}</span></template>
        <template #disk="{ row }"><span :class="pctClass(row.disk)">{{ row.disk != null ? row.disk.toFixed(1) + '%' : '-' }}</span></template>
      </t-table>
    </t-card>
  </div>
</template>

<script setup lang="ts">
import { onMounted, onUnmounted, ref } from 'vue';
import type { PrimaryTableCol } from 'tdesign-vue-next';

import { getStatsAgents, getStatsOverview } from '@/api/agent';

const ov = ref<Record<string, number>>({});
const agentStats = ref<any[]>([]);
const loading = ref(false);
let timer: number | undefined;

const columns: PrimaryTableCol[] = [
  { colKey: 'hostname', title: '主机', width: 180, ellipsis: true },
  { colKey: 'online', title: '当前状态', width: 90 },
  { colKey: 'latency', title: '当前延迟', width: 100 },
  { colKey: 'avg_latency', title: '24h 平均延迟', width: 120 },
  { colKey: 'offline_count', title: '离线次数', width: 90 },
  { colKey: 'cpu', title: 'CPU', width: 80 },
  { colKey: 'mem', title: '内存', width: 80 },
  { colKey: 'disk', title: '磁盘', width: 80 },
  { colKey: 'online_count', title: '上线次数', width: 100 },
];

function pctClass(v?: number) {
  if (v == null) return '';
  if (v >= 90) return 'bad';
  if (v >= 70) return 'mid';
  return 'good';
}

async function fetchAll() {
  loading.value = true;
  try {
    const [o, a] = await Promise.all([getStatsOverview(), getStatsAgents(24)]);
    ov.value = o;
    agentStats.value = a;
  } finally {
    loading.value = false;
  }
}

onMounted(() => {
  fetchAll();
  timer = window.setInterval(fetchAll, 30000);
});
onUnmounted(() => window.clearInterval(timer));
</script>

<style scoped>
.ov-card { text-align: center; padding: 20px 8px; }
.ov-num { font-size: 26px; font-weight: 600; }
.ov-label { font-size: 12px; color: var(--td-text-color-secondary); margin-top: 4px; }
.green { color: #2ba471; } .red { color: #e34d59; } .orange { color: #ed7b2f; }
.sec-title { font-size: 14px; font-weight: 600; margin: 20px 0 12px; }
.good { color: #2ba471; } .mid { color: #ed7b2f; } .bad { color: #e34d59; }
</style>
