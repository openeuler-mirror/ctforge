<template>
  <t-card :bordered="false">
    <template #title>
      <t-space align="center">
        <span>配置管理</span>
        <t-select
          v-model="currentAgent"
          :options="agentOptions"
          placeholder="选择主机"
          style="width: 260px"
          :loading="loadingAgents"
          @change="fetchConfig"
        />
      </t-space>
    </template>
    <template #actions>
      <t-button theme="primary" :loading="loading" @click="fetchConfig">刷新</t-button>
    </template>

    <t-alert theme="info" style="margin-bottom: 16px">
      这里修改的是客户端 agent 的运行配置。agent_id / token 不允许远程修改；server_host / server_port 修改后需重启客户端生效。
    </t-alert>

    <t-table :data="rows" :columns="columns" :loading="loading" row-key="key" hover>
      <template #desc="{ row }">
        <span class="desc">{{ row.desc }}</span>
      </template>
      <template #editable="{ row }">
        <t-tag v-if="row.editable" theme="success" variant="light">可修改</t-tag>
        <t-tag v-else theme="default" variant="light">只读</t-tag>
      </template>
      <template #op="{ row }">
        <t-link v-if="row.editable" theme="primary" hover="color" @click="onEdit(row)">修改</t-link>
        <span v-else>-</span>
      </template>
    </t-table>

    <t-dialog v-model:visible="showEdit" :header="`修改 ${editing?.key ?? ''}`" :on-confirm="doSave" :confirm-btn="{ loading: saving }">
      <t-input v-model="editValue" :placeholder="editing?.desc" />
    </t-dialog>
  </t-card>
</template>

<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';
import { useRoute } from 'vue-router';
import { MessagePlugin } from 'tdesign-vue-next';
import type { PrimaryTableCol } from 'tdesign-vue-next';

import { getConfig, listAgents, setConfig } from '@/api/agent';

interface ConfigRow {
  key: string;
  value: string;
  desc: string;
  editable: boolean;
}

const META: Record<string, { desc: string; editable: boolean }> = {
  server_host: { desc: '服务端地址', editable: true },
  server_port: { desc: '服务端端口', editable: true },
  agent_id: { desc: '客户端唯一标识', editable: false },
  name: { desc: '显示名称', editable: true },
  token: { desc: '注册令牌', editable: false },
  metrics_interval: { desc: '指标上报间隔（秒，≥5）', editable: true },
  heartbeat_interval: { desc: '心跳间隔（秒，≥3）', editable: true },
  log_level: { desc: '日志级别 0=debug 1=info 2=warn 3=error', editable: true },
};

const route = useRoute();
const currentAgent = ref<string>((route.query.agent as string) || '');
const agents = ref<{ label: string; value: string }[]>([]);
const loadingAgents = ref(false);
const loading = ref(false);
const config = ref<Record<string, string>>({});
const showEdit = ref(false);
const saving = ref(false);
const editing = ref<ConfigRow | null>(null);
const editValue = ref('');

const agentOptions = computed(() => agents.value);

const rows = computed<ConfigRow[]>(() => {
  return Object.entries(config.value).map(([key, value]) => ({
    key,
    value,
    desc: META[key]?.desc ?? '',
    editable: META[key]?.editable ?? false,
  }));
});

const columns: PrimaryTableCol[] = [
  { colKey: 'key', title: '配置项', width: 200 },
  { colKey: 'value', title: '当前值', width: 220, ellipsis: true },
  { colKey: 'desc', title: '说明', ellipsis: true },
  { colKey: 'editable', title: '权限', width: 100 },
  { colKey: 'op', title: '操作', width: 100, fixed: 'right' },
];

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

async function fetchConfig() {
  if (!currentAgent.value) return;
  loading.value = true;
  try {
    config.value = await getConfig(currentAgent.value);
  } catch (e: any) {
    MessagePlugin.error(e.message || '读取配置失败');
  } finally {
    loading.value = false;
  }
}

function onEdit(row: ConfigRow) {
  editing.value = row;
  editValue.value = row.value;
  showEdit.value = true;
}

async function doSave() {
  if (!editing.value) return;
  saving.value = true;
  try {
    await setConfig(currentAgent.value, editing.value.key, editValue.value);
    MessagePlugin.success('已保存');
    showEdit.value = false;
    fetchConfig();
  } catch (e: any) {
    MessagePlugin.error(e.message || '保存失败');
  } finally {
    saving.value = false;
  }
}

onMounted(async () => {
  await loadAgents();
  fetchConfig();
});
</script>

<style scoped>
.desc {
  color: var(--td-text-color-secondary);
}
</style>
