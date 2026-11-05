<template>
  <t-card :bordered="false">
    <template #title>操作审计</template>
    <template #actions>
      <t-button theme="primary" :loading="loading" @click="fetchAudit">刷新</t-button>
    </template>

    <t-table :data="entries" :columns="columns" :loading="loading" row-key="time" hover size="small" :max-height="640">
      <template #time="{ row }">{{ formatTime(row.time) }}</template>
      <template #user="{ row }"><t-tag variant="outline" size="small">{{ row.user }}</t-tag></template>
      <template #action="{ row }"><span class="mono">{{ row.action }}</span></template>
      <template #ok="{ row }">
        <t-tag :theme="row.ok ? 'success' : 'danger'" variant="light" size="small">{{ row.ok ? '成功' : '失败' }}</t-tag>
      </template>
      <template #ip="{ row }"><span class="mono">{{ row.ip }}</span></template>
    </t-table>
  </t-card>
</template>

<script setup lang="ts">
import { onMounted, ref } from 'vue';
import { MessagePlugin } from 'tdesign-vue-next';
import type { PrimaryTableCol } from 'tdesign-vue-next';
import dayjs from 'dayjs';

import { listAudit } from '@/api/agent';

const entries = ref<any[]>([]);
const loading = ref(false);

const columns: PrimaryTableCol[] = [
  { colKey: 'time', title: '时间', width: 180 },
  { colKey: 'user', title: '操作人', width: 140 },
  { colKey: 'action', title: '动作', width: 160 },
  { colKey: 'target', title: '对象', ellipsis: true },
  { colKey: 'ok', title: '结果', width: 90 },
  { colKey: 'ip', title: '来源 IP', width: 150 },
  { colKey: 'detail', title: '详情', ellipsis: true },
];

function formatTime(t: string) {
  return t ? dayjs(t).format('YYYY-MM-DD HH:mm:ss') : '-';
}

async function fetchAudit() {
  loading.value = true;
  try {
    entries.value = await listAudit(300);
  } catch (e: any) {
    MessagePlugin.error(e.message || '加载失败');
  } finally {
    loading.value = false;
  }
}

onMounted(fetchAudit);
</script>

<style scoped>
.mono { font-family: monospace; font-size: 12px; }
</style>
