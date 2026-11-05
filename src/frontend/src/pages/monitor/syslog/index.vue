<template>
  <div>
    <t-card :bordered="false">
      <template #title>
        <t-space align="center">
          <span>系统日志</span>
          <t-select v-model="currentAgent" :options="agentOptions" placeholder="选择主机" style="width: 260px" :loading="loadingAgents" @change="fetchLogs" />
        </t-space>
      </template>
      <template #actions>
        <t-button theme="primary" :loading="loading" @click="fetchLogs">查询</t-button>
      </template>

      <t-form layout="inline" style="margin-bottom: 16px">
        <t-form-item label="级别">
          <t-select v-model="query.priority" style="width: 120px" clearable>
            <t-option value="err" label="错误" />
            <t-option value="warning" label="警告" />
            <t-option value="info" label="信息" />
            <t-option value="debug" label="调试" />
          </t-select>
        </t-form-item>
        <t-form-item label="服务">
          <t-input v-model="query.unit" placeholder="如 sshd.service" style="width: 160px" clearable />
        </t-form-item>
        <t-form-item label="关键字">
          <t-input v-model="query.keyword" placeholder="搜索日志内容" style="width: 200px" clearable @enter="fetchLogs" />
        </t-form-item>
        <t-form-item label="条数">
          <t-select v-model="query.lines" style="width: 100px">
            <t-option :value="50" label="50" />
            <t-option :value="100" label="100" />
            <t-option :value="200" label="200" />
            <t-option :value="500" label="500" />
          </t-select>
        </t-form-item>
      </t-form>

      <div class="log-count">共 {{ total }} 条</div>

      <t-table :data="logs" :columns="columns" :loading="loading" row-key="timestamp" hover size="small" :max-height="600">
        <template #time="{ row }">
          <span class="time">{{ formatTime(row.timestamp) }}</span>
        </template>
        <template #priority="{ row }">
          <t-tag :theme="prioTheme(row.priority)" variant="light" size="small">{{ row.priority }}</t-tag>
        </template>
        <template #unit="{ row }">
          <span class="unit">{{ row.unit || '-' }}</span>
        </template>
        <template #message="{ row }">
          <span class="msg">{{ row.message }}</span>
        </template>
      </t-table>
    </t-card>
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';
import { useRoute } from 'vue-router';
import dayjs from 'dayjs';

import { listAgents, querySysLog, type SysLogEntry, type SysLogQuery } from '@/api/agent';

const route = useRoute();
const currentAgent = ref<string>((route.query.agent as string) || '');
const agents = ref<{ label: string; value: string }[]>([]);
const loadingAgents = ref(false);
const loading = ref(false);
const logs = ref<SysLogEntry[]>([]);
const total = ref(0);
const query = ref<SysLogQuery>({ priority: '', unit: '', keyword: '', lines: 100 });

const agentOptions = computed(() => agents.value);

const columns = [
  { colKey: 'time', title: '时间', width: 160 },
  { colKey: 'priority', title: '级别', width: 80 },
  { colKey: 'unit', title: '服务', width: 180, ellipsis: true },
  { colKey: 'message', title: '内容', ellipsis: true },
];

function prioTheme(p: string) {
  switch (p) {
    case 'err': case 'error': return 'danger';
    case 'warning': case 'warn': return 'warning';
    case 'debug': return 'default';
    default: return 'primary';
  }
}

function formatTime(ts: number) {
  return ts > 0 ? dayjs(ts).format('MM-DD HH:mm:ss') : '-';
}

async function loadAgents() {
  loadingAgents.value = true;
  try {
    const list = await listAgents();
    // 默认跳过离线主机
    agents.value = list
      .filter((a) => a.online)
      .map((a) => ({
        label: `${a.hostname} (${a.ip})`,
        value: a.agent_id,
      }));
    if (!currentAgent.value && agents.value.length) currentAgent.value = agents.value[0].value;
  } finally {
    loadingAgents.value = false;
  }
}

async function fetchLogs() {
  if (!currentAgent.value) return;
  loading.value = true;
  try {
    const res = await querySysLog(currentAgent.value, query.value);
    logs.value = res.entries;
    total.value = res.total;
  } finally {
    loading.value = false;
  }
}

onMounted(async () => {
  await loadAgents();
  fetchLogs();
});
</script>

<style scoped>
.log-count {
  font-size: 12px;
  color: var(--td-text-color-secondary);
  margin-bottom: 8px;
}
.time {
  font-size: 12px;
  color: var(--td-text-color-secondary);
  font-family: monospace;
}
.unit {
  font-family: monospace;
  font-size: 12px;
  color: var(--td-text-color-placeholder);
}
.msg {
  font-family: monospace;
  font-size: 12px;
  white-space: pre-wrap;
  word-break: break-all;
}
</style>
