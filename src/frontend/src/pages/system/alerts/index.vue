<template>
  <div>
    <t-card :bordered="false">
      <template #title>
        <t-space align="center">
          <span>告警中心</span>
          <t-badge v-if="unresolvedCount > 0" :count="unresolvedCount" color="red" />
        </t-space>
      </template>
      <template #actions>
        <t-space>
          <t-checkbox v-model="includeResolved">显示已解决</t-checkbox>
          <t-button theme="primary" :loading="loading" @click="fetchAlerts">刷新</t-button>
        </t-space>
      </template>

      <t-alert v-if="!alerts.length" theme="success" style="margin-bottom: 16px">
        当前无告警，一切正常。
      </t-alert>

      <t-table v-else :data="alerts" :columns="columns" :loading="loading" row-key="id" hover>
        <template #level="{ row }">
          <t-tag :theme="levelTheme(row.level)" variant="light" size="small">{{ levelText(row.level) }}</t-tag>
        </template>
        <template #kind="{ row }">
          <span class="kind">{{ kindText(row.kind) }}</span>
        </template>
        <template #time="{ row }">{{ formatMs(row.created_at) }}</template>
        <template #status="{ row }">
          <t-tag v-if="row.resolved" theme="default" variant="outline" size="small">已解决</t-tag>
          <t-tag v-else theme="warning" variant="light" size="small">未解决</t-tag>
        </template>
        <template #op="{ row }">
          <t-link v-if="!row.resolved" theme="success" hover="color" @click="onResolve(row)">标记解决</t-link>
          <span v-else>-</span>
        </template>
      </t-table>
    </t-card>
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from 'vue';
import { MessagePlugin } from 'tdesign-vue-next';
import type { PrimaryTableCol } from 'tdesign-vue-next';
import dayjs from 'dayjs';

import { listAlerts, resolveAlert, type AlertItem } from '@/api/agent';

const alerts = ref<AlertItem[]>([]);
const loading = ref(false);
const includeResolved = ref(false);
let timer: number | undefined;

const unresolvedCount = computed(() => alerts.value.filter((a) => !a.resolved).length);

const columns: PrimaryTableCol[] = [
  { colKey: 'level', title: '级别', width: 80 },
  { colKey: 'kind', title: '类型', width: 120 },
  { colKey: 'title', title: '标题', ellipsis: true },
  { colKey: 'agent_id', title: '主机', width: 160 },
  { colKey: 'time', title: '时间', width: 160 },
  { colKey: 'status', title: '状态', width: 90 },
  { colKey: 'op', title: '操作', width: 100, fixed: 'right' },
];

function levelTheme(l: string): 'danger' | 'warning' | 'primary' | 'default' {
  const m: Record<string, 'danger' | 'warning' | 'primary'> = { critical: 'danger', warning: 'warning', info: 'primary' };
  return m[l] ?? 'default';
}

function levelText(l: string) {
  return { critical: '严重', warning: '警告', info: '信息' }[l] || l;
}

function kindText(k: string) {
  return {
    offline: '离线',
    high_cpu: 'CPU 过高',
    high_mem: '内存过高',
    high_disk: '磁盘过高',
    net_spike: '流量异常',
    ssh_brute: 'SSH 爆破',
    custom: '自定义',
  }[k] || k;
}

function formatMs(ms: number) {
  return ms ? dayjs(ms).format('MM-DD HH:mm:ss') : '-';
}

async function fetchAlerts() {
  loading.value = true;
  try {
    alerts.value = await listAlerts(includeResolved.value);
  } finally {
    loading.value = false;
  }
}

async function onResolve(row: AlertItem) {
  try {
    await resolveAlert(row.agent_id, row.kind);
    MessagePlugin.success('已标记解决');
    fetchAlerts();
  } catch (e: any) {
    MessagePlugin.error(e.message || '操作失败');
  }
}

onMounted(() => {
  fetchAlerts();
  timer = window.setInterval(fetchAlerts, 15000);
});
onUnmounted(() => window.clearInterval(timer));
</script>

<style scoped>
.kind { font-size: 12px; color: var(--td-text-color-secondary); }
</style>
