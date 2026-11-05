<template>
  <div>
    <t-card :bordered="false">
      <template #title>
        <t-space align="center">
          <span>系统服务管理</span>
          <t-select v-model="currentAgent" :options="agentOptions" placeholder="选择主机" style="width: 260px" :loading="loadingAgents" @change="fetchServices" />
        </t-space>
      </template>
      <template #actions>
        <t-button theme="primary" :loading="loading" @click="fetchServices">刷新</t-button>
      </template>

      <t-form layout="inline" style="margin-bottom: 16px">
        <t-form-item label="过滤">
          <t-input v-model="filter" placeholder="服务名过滤（如 sshd）" style="width: 200px" clearable @enter="fetchServices" />
        </t-form-item>
        <t-form-item>
          <t-button theme="primary" @click="fetchServices">查询</t-button>
          <t-button theme="default" @click="filter = ''; fetchServices()">重置</t-button>
        </t-form-item>
      </t-form>

      <t-table :data="services" :columns="columns" :loading="loading" row-key="name" hover size="small" :max-height="600">
        <template #active="{ row }">
          <t-tag :theme="activeTheme(row.active)" variant="light" size="small">{{ row.active }}</t-tag>
        </template>
        <template #sub="{ row }">
          <t-tag :theme="subTheme(row.sub)" variant="outline" size="small">{{ row.sub }}</t-tag>
        </template>
        <template #op="{ row }">
          <t-space break-line>
            <t-link v-if="row.active !== 'active'" theme="success" hover="color" @click="onAction(row, 'start')">启动</t-link>
            <t-link v-if="row.active === 'active'" theme="warning" hover="color" @click="onAction(row, 'stop')">停止</t-link>
            <t-link v-if="row.active === 'active'" theme="primary" hover="color" @click="onAction(row, 'restart')">重启</t-link>
          </t-space>
        </template>
      </t-table>
    </t-card>
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';
import { useRoute } from 'vue-router';
import { MessagePlugin, DialogPlugin } from 'tdesign-vue-next';
import type { PrimaryTableCol } from 'tdesign-vue-next';

import { listAgents, listServices, serviceAction, type ServiceEntry } from '@/api/agent';

const route = useRoute();
const currentAgent = ref<string>((route.query.agent as string) || '');
const agents = ref<{ label: string; value: string }[]>([]);
const loadingAgents = ref(false);
const loading = ref(false);
const services = ref<ServiceEntry[]>([]);
const filter = ref('');

const agentOptions = computed(() => agents.value);

const columns: PrimaryTableCol[] = [
  { colKey: 'name', title: '服务名', width: 280, ellipsis: true },
  { colKey: 'active', title: '状态', width: 90 },
  { colKey: 'sub', title: '子状态', width: 90 },
  { colKey: 'load', title: '加载', width: 80 },
  { colKey: 'description', title: '描述', ellipsis: true },
  { colKey: 'op', title: '操作', width: 120, fixed: 'right' },
];

function activeTheme(active: string) {
  switch (active) {
    case 'active': return 'success';
    case 'failed': return 'danger';
    default: return 'default';
  }
}

function subTheme(sub: string) {
  switch (sub) {
    case 'running': return 'success';
    case 'exited': return 'primary';
    case 'failed': return 'danger';
    default: return 'default';
  }
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

async function fetchServices() {
  if (!currentAgent.value) return;
  loading.value = true;
  try {
    services.value = await listServices(currentAgent.value, filter.value);
  } catch (e: any) {
    MessagePlugin.error(e.message || '查询失败');
  } finally {
    loading.value = false;
  }
}

function onAction(row: ServiceEntry, action: string) {
  const actionText = { start: '启动', stop: '停止', restart: '重启' }[action] || action;
  const dlg = DialogPlugin.confirm({
    header: `${actionText}服务`,
    body: `确定要对 ${row.name} 执行 ${actionText} 吗？`,
    theme: action === 'stop' ? ('warning' as const) : undefined,
    onConfirm: async () => {
      try {
        await serviceAction(currentAgent.value, row.name, action);
        MessagePlugin.success(`${actionText}成功`);
        fetchServices();
      } catch (e: any) {
        MessagePlugin.error(e.message || `${actionText}失败`);
      }
      dlg.destroy();
    },
  });
}

onMounted(async () => {
  await loadAgents();
  fetchServices();
});
</script>
