<template>
  <div>
    <t-card :bordered="false">
      <template #title>
        <t-space align="center">
          <span>日志分析</span>
          <t-select v-model="currentAgent" :options="agentOptions" placeholder="选择主机" style="width: 260px" :loading="loadingAgents" @change="fetchLogs" />
        </t-space>
      </template>
      <template #actions>
        <t-button theme="primary" :loading="loading" @click="fetchLogs">刷新</t-button>
      </template>

      <!-- 统计卡片 -->
      <t-row :gutter="16" style="margin-bottom: 16px">
        <t-col :span="3" v-for="(item, key) in statCards" :key="key">
          <t-card :bordered="false" class="stat-card" :class="`stat-${key}`">
            <div class="stat-num">{{ item.count }}</div>
            <div class="stat-label">{{ item.label }}</div>
          </t-card>
        </t-col>
      </t-row>

      <!-- 搜索过滤 -->
      <t-form layout="inline" style="margin-bottom: 16px">
        <t-form-item label="级别">
          <t-select v-model="filterLevel" style="width: 120px" @change="fetchLogs">
            <t-option value="" label="全部" />
            <t-option value="debug" label="debug" />
            <t-option value="info" label="info" />
            <t-option value="warn" label="warn" />
            <t-option value="error" label="error" />
          </t-select>
        </t-form-item>
        <t-form-item label="关键字">
          <t-input v-model="filterKeyword" placeholder="搜索日志内容" style="width: 260px" clearable @enter="fetchLogs" />
        </t-form-item>
        <t-form-item>
          <t-button theme="primary" @click="fetchLogs">搜索</t-button>
          <t-button theme="default" @click="resetFilter">重置</t-button>
        </t-form-item>
      </t-form>

      <!-- 日志表格 -->
      <t-table :data="logs" :columns="columns" :loading="loading" row-key="timestamp" hover size="small" :max-height="600">
        <template #level="{ row }">
          <t-tag :theme="levelTheme(row.level)" variant="light" size="small">{{ row.level }}</t-tag>
        </template>
        <template #time="{ row }">
          <span class="time">{{ formatTime(row.timestamp) }}</span>
        </template>
        <template #message="{ row }">
          <span class="msg" :class="`msg-${row.level}`">{{ row.message }}</span>
        </template>
      </t-table>
    </t-card>
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from 'vue';
import { useRoute } from 'vue-router';
import dayjs from 'dayjs';

import { analyzeLogs, listAgents, type LogEntry } from '@/api/agent';

const route = useRoute();
const currentAgent = ref<string>((route.query.agent as string) || '');
const agents = ref<{ label: string; value: string }[]>([]);
const loadingAgents = ref(false);
const loading = ref(false);

const logs = ref<LogEntry[]>([]);
const stats = ref<Record<string, number>>({});
const filterLevel = ref('');
const filterKeyword = ref('');

let timer: number | undefined;

const agentOptions = computed(() => agents.value);

const statCards = computed(() => ({
  total: { label: '总计', count: stats.value.total ?? 0 },
  info: { label: 'Info', count: stats.value.info ?? 0 },
  warn: { label: 'Warn', count: stats.value.warn ?? 0 },
  error: { label: 'Error', count: stats.value.error ?? 0 },
}));

const columns = [
  { colKey: 'time', title: '时间', width: 160 },
  { colKey: 'level', title: '级别', width: 80 },
  { colKey: 'message', title: '内容', ellipsis: true },
];

function levelTheme(level: string) {
  switch (level) {
    case 'error': return 'danger';
    case 'warn': return 'warning';
    case 'debug': return 'default';
    default: return 'primary';
  }
}

function formatTime(ts: number) {
  return dayjs(ts).format('YYYY-MM-DD HH:mm:ss.SSS');
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
    const res = await analyzeLogs(currentAgent.value, filterKeyword.value, filterLevel.value, 200);
    logs.value = res.entries;
    stats.value = res.stats;
  } finally {
    loading.value = false;
  }
}

function resetFilter() {
  filterKeyword.value = '';
  filterLevel.value = '';
  fetchLogs();
}

onMounted(async () => {
  await loadAgents();
  fetchLogs();
  timer = window.setInterval(fetchLogs, 15000);
});
onUnmounted(() => window.clearInterval(timer));
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
.stat-total .stat-num { color: #0052d9; }
.stat-info .stat-num { color: #0594fa; }
.stat-warn .stat-num { color: #ed7b2f; }
.stat-error .stat-num { color: #e34d59; }
.time {
  font-size: 12px;
  color: var(--td-text-color-secondary);
  font-family: monospace;
}
.msg {
  font-family: monospace;
  font-size: 12px;
}
.msg-error { color: #e34d59; }
.msg-warn { color: #ed7b2f; }
</style>
