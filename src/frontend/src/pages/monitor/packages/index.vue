<template>
  <div>
    <t-card :bordered="false">
      <template #title>
        <t-space align="center">
          <span>软件包更新</span>
          <t-select v-model="currentAgent" :options="agentOptions" placeholder="选择主机" style="width: 260px" :loading="loadingAgents" @change="onAgentChange" />
          <t-tag v-if="manager" theme="primary" variant="light">{{ manager }}</t-tag>
        </t-space>
      </template>
      <template #actions>
        <t-space>
          <t-button theme="default" :loading="loading" @click="fetchUpdates">检查更新</t-button>
          <t-button theme="primary" :disabled="!updates.length" :loading="upgrading" @click="onUpgradeAll">
            全部更新 ({{ updates.length }})
          </t-button>
          <t-button theme="default" @click="openHistory">更新历史</t-button>
        </t-space>
      </template>

      <t-alert v-if="!updates.length && checked" theme="success" style="margin-bottom: 16px">
        当前系统已是最新，无可用更新。
      </t-alert>

      <t-table :data="updates" :columns="columns" :loading="loading" row-key="name" hover size="small" :max-height="520">
        <template #op="{ row }">
          <t-link theme="primary" hover="color" @click="onUpgradeOne(row)">更新</t-link>
        </template>
      </t-table>
    </t-card>

    <t-dialog v-model:visible="showHistory" header="更新历史（最近安装/升级）" width="720px" :footer="false">
      <t-table :data="history" :columns="historyColumns" row-key="package" size="small" :max-height="480" />
    </t-dialog>

    <t-dialog v-model:visible="showOutput" header="更新输出" width="760px" :footer="false">
      <pre class="output">{{ output }}</pre>
    </t-dialog>
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';
import { useRoute } from 'vue-router';
import { MessagePlugin, DialogPlugin } from 'tdesign-vue-next';
import type { PrimaryTableCol } from 'tdesign-vue-next';

import { listAgents, listPkgUpdates, pkgHistory, upgradePkg, type PkgEntry } from '@/api/agent';

const route = useRoute();
const currentAgent = ref<string>((route.query.agent as string) || '');
const agents = ref<{ label: string; value: string }[]>([]);
const loadingAgents = ref(false);
const loading = ref(false);
const upgrading = ref(false);
const checked = ref(false);
const manager = ref('');
const updates = ref<PkgEntry[]>([]);
const history = ref<any[]>([]);
const showHistory = ref(false);
const showOutput = ref(false);
const output = ref('');

const agentOptions = computed(() => agents.value);

const columns: PrimaryTableCol[] = [
  { colKey: 'name', title: '包名', width: 320, ellipsis: true },
  { colKey: 'arch', title: '架构', width: 100 },
  { colKey: 'update', title: '可更新版本', ellipsis: true },
  { colKey: 'repo', title: '仓库', width: 160 },
  { colKey: 'op', title: '操作', width: 100, fixed: 'right' },
];

const historyColumns: PrimaryTableCol[] = [
  { colKey: 'time', title: '时间', width: 300, ellipsis: true },
  { colKey: 'package', title: '软件包', ellipsis: true },
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

async function fetchUpdates() {
  if (!currentAgent.value) return;
  loading.value = true;
  try {
    const res = await listPkgUpdates(currentAgent.value);
    manager.value = res.manager;
    updates.value = res.updates || [];
    checked.value = true;
  } catch (e: any) {
    MessagePlugin.error(e.message || '检查更新失败');
  } finally {
    loading.value = false;
  }
}

function onAgentChange() {
  updates.value = [];
  checked.value = false;
  manager.value = '';
}

async function runUpgrade(packages: string[], label: string) {
  upgrading.value = true;
  try {
    const res = await upgradePkg(currentAgent.value, packages);
    output.value = res.output || res.message;
    showOutput.value = true;
    if (res.ok) MessagePlugin.success(`${label}完成`);
    fetchUpdates();
  } catch (e: any) {
    MessagePlugin.error(e.message || `${label}失败`);
  } finally {
    upgrading.value = false;
  }
}

function onUpgradeAll() {
  const dlg = DialogPlugin.confirm({
    header: '确认全部更新',
    body: `将对 ${currentAgent.value} 执行全量软件包更新，共 ${updates.value.length} 个包。该操作不可回滚，是否继续？`,
    theme: 'warning',
    onConfirm: () => {
      dlg.destroy();
      runUpgrade([], '全量更新');
    },
  });
}

function onUpgradeOne(row: PkgEntry) {
  const dlg = DialogPlugin.confirm({
    header: '确认更新',
    body: `确定更新 ${row.name} 到 ${row.update} 吗？`,
    onConfirm: () => {
      dlg.destroy();
      runUpgrade([row.name], `更新 ${row.name}`);
    },
  });
}

async function openHistory() {
  showHistory.value = true;
  try {
    history.value = await pkgHistory(currentAgent.value);
  } catch (e: any) {
    MessagePlugin.error(e.message || '读取历史失败');
  }
}

onMounted(async () => {
  await loadAgents();
  fetchUpdates();
});
</script>

<style scoped>
.output {
  max-height: 480px;
  overflow: auto;
  background: #1e1e1e;
  color: #d4d4d4;
  padding: 12px;
  border-radius: 6px;
  font-size: 12px;
  line-height: 1.6;
  white-space: pre-wrap;
  word-break: break-all;
}
</style>
