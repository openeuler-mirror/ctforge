<template>
  <div>
    <t-card :bordered="false">
      <template #title>定时任务</template>
      <template #actions>
        <t-space>
          <t-button theme="default" @click="showRecords = true">执行记录</t-button>
          <t-button theme="primary" @click="openCreate">新建任务</t-button>
        </t-space>
      </template>

      <t-table :data="tasks" :columns="columns" :loading="loading" row-key="id" hover>
        <template #enabled="{ row }">
          <t-switch :model-value="row.enabled" @change="onToggle(row)" />
        </template>
        <template #cron="{ row }">
          <span class="mono">{{ row.cron }}</span>
        </template>
        <template #command="{ row }">
          <t-tooltip :content="row.command"><span class="mono cmd">{{ row.command }}</span></t-tooltip>
        </template>
        <template #agents="{ row }">
          <t-tag v-if="!row.agents || !row.agents.length" variant="outline" size="small">全部主机</t-tag>
          <t-tag v-else variant="outline" size="small">{{ row.agents.length }} 台主机</t-tag>
        </template>
        <template #next="{ row }">{{ formatMs(row.next_run_at) }}</template>
        <template #status="{ row }">
          <t-tag v-if="row.last_status === 'ok'" theme="success" variant="light" size="small">成功</t-tag>
          <t-tag v-else-if="row.last_status === 'failed'" theme="danger" variant="light" size="small">失败</t-tag>
          <span v-else>-</span>
        </template>
        <template #op="{ row }">
          <t-space break-line>
            <t-link theme="primary" hover="color" @click="onRunNow(row)">立即执行</t-link>
            <t-link theme="primary" hover="color" @click="openEdit(row)">编辑</t-link>
            <t-link theme="danger" hover="color" @click="onDelete(row)">删除</t-link>
          </t-space>
        </template>
      </t-table>
    </t-card>

    <!-- 新建/编辑 -->
    <t-dialog v-model:visible="showEdit" :header="editing ? `编辑任务 ${editing.name}` : '新建任务'" width="640px" :on-confirm="doSave" :confirm-btn="{ loading: saving }">
      <t-form :data="form" label-width="110px">
        <t-form-item label="任务名称"><t-input v-model="form.name" placeholder="如：每日磁盘清理" /></t-form-item>
        <t-form-item label="Cron 表达式">
          <t-input v-model="form.cron" placeholder="分 时 日 月 周，如 0 3 * * *" />
          <div class="hint">常用：*/5 * * * *（每5分钟）| 0 * * * *（每小时）| 0 3 * * *（每天3点）</div>
        </t-form-item>
        <t-form-item label="命令"><t-textarea v-model="form.command" placeholder="要执行的 shell 命令" :autosize="{ minRows: 3 }" /></t-form-item>
        <t-form-item label="超时（秒）"><t-input-number v-model="form.timeout_sec" :min="10" :max="300" /></t-form-item>
        <t-form-item label="目标主机">
          <t-transfer
            v-model="selectedAgents"
            :data="transferData"
            :target-value="selectedAgents"
            @change="onTransferChange"
          />
        </t-form-item>
        <t-form-item label="启用"><t-switch v-model="form.enabled" /></t-form-item>
      </t-form>
    </t-dialog>

    <!-- 执行记录 -->
    <t-dialog v-model:visible="showRecords" header="执行记录" width="900px" :footer="false">
      <t-table :data="records" :columns="recordColumns" row-key="started_at" size="small" :max-height="520">
        <template #time="{ row }">{{ formatMs(row.finished_at) }}</template>
        <template #ok="{ row }">
          <t-tag :theme="row.ok ? 'success' : 'danger'" variant="light" size="small">{{ row.ok ? '成功' : '失败' }}</t-tag>
        </template>
        <template #out="{ row }">
          <t-tooltip v-if="row.stdout" :content="row.stdout.slice(0, 2000)">
            <span class="mono out">{{ row.stdout.slice(0, 80) }}</span>
          </t-tooltip>
          <span v-else-if="row.error" class="err">{{ row.error }}</span>
        </template>
      </t-table>
    </t-dialog>
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from 'vue';
import { MessagePlugin, DialogPlugin } from 'tdesign-vue-next';
import type { PrimaryTableCol } from 'tdesign-vue-next';
import dayjs from 'dayjs';

import {
  createTask, deleteTask, listAgents, listTaskRecords, listTasks, runTaskNow, updateTask,
  type CronTask,
} from '@/api/agent';

const tasks = ref<CronTask[]>([]);
const records = ref<any[]>([]);
const loading = ref(false);
const saving = ref(false);
const showEdit = ref(false);
const showRecords = ref(false);
const editing = ref<CronTask | null>(null);
const agents = ref<{ label: string; value: string }[]>([]);
const selectedAgents = ref<string[]>([]);
const form = ref({ name: '', cron: '0 3 * * *', command: '', timeout_sec: 60, enabled: true });

let timer: number | undefined;

const columns: PrimaryTableCol[] = [
  { colKey: 'name', title: '任务名', width: 160, ellipsis: true },
  { colKey: 'enabled', title: '启用', width: 70 },
  { colKey: 'cron', title: 'Cron', width: 130 },
  { colKey: 'command', title: '命令', ellipsis: true },
  { colKey: 'agents', title: '目标', width: 110 },
  { colKey: 'next', title: '下次执行', width: 150 },
  { colKey: 'status', title: '上次状态', width: 90 },
  { colKey: 'op', title: '操作', width: 160, fixed: 'right' },
];

const recordColumns: PrimaryTableCol[] = [
  { colKey: 'time', title: '完成时间', width: 160 },
  { colKey: 'task_name', title: '任务', width: 140, ellipsis: true },
  { colKey: 'agent_id', title: '主机', width: 160, ellipsis: true },
  { colKey: 'ok', title: '结果', width: 70 },
  { colKey: 'exit_code', title: '退出码', width: 80 },
  { colKey: 'duration_ms', title: '耗时', width: 90 },
  { colKey: 'out', title: '输出', ellipsis: true },
];

const transferData = computed(() =>
  agents.value.map((a) => ({ label: a.label, value: a.value })),
);

function formatMs(ms?: number) {
  return ms ? dayjs(ms).format('MM-DD HH:mm:ss') : '-';
}

function onTransferChange(val: any) {
  selectedAgents.value = (val || []) as string[];
}

async function fetchTasks() {
  loading.value = true;
  try {
    tasks.value = await listTasks();
  } finally {
    loading.value = false;
  }
}

async function fetchRecords() {
  try {
    records.value = await listTaskRecords(100);
  } catch { /* ignore */ }
}

async function loadAgents() {
  const list = await listAgents();
  agents.value = list.map((a) => ({
    label: `${a.hostname} (${a.ip})${a.online ? '' : ' [离线]'}`,
    value: a.agent_id,
  }));
}

function openCreate() {
  editing.value = null;
  selectedAgents.value = [];
  form.value = { name: '', cron: '0 3 * * *', command: '', timeout_sec: 60, enabled: true };
  showEdit.value = true;
}

function openEdit(row: CronTask) {
  editing.value = row;
  selectedAgents.value = row.agents || [];
  form.value = {
    name: row.name,
    cron: row.cron,
    command: row.command,
    timeout_sec: row.timeout_sec,
    enabled: row.enabled,
  };
  showEdit.value = true;
}

async function doSave() {
  if (!form.value.name || !form.value.cron || !form.value.command) {
    MessagePlugin.warning('请填写任务名、cron 表达式和命令');
    return;
  }
  saving.value = true;
  try {
    const payload = {
      ...form.value,
      agents: selectedAgents.value,
    };
    if (editing.value) await updateTask(editing.value.id, payload);
    else await createTask(payload);
    MessagePlugin.success('已保存');
    showEdit.value = false;
    fetchTasks();
  } catch (e: any) {
    MessagePlugin.error(e.message || '保存失败');
  } finally {
    saving.value = false;
  }
}

async function onToggle(row: CronTask) {
  try {
    await updateTask(row.id, { enabled: !row.enabled });
    row.enabled = !row.enabled;
  } catch (e: any) {
    MessagePlugin.error(e.message || '操作失败');
  }
}

function onRunNow(row: CronTask) {
  const dlg = DialogPlugin.confirm({
    header: '立即执行',
    body: `确定立即执行任务"${row.name}"吗？`,
    onConfirm: async () => {
      try {
        await runTaskNow(row.id);
        MessagePlugin.success('已触发，请稍后查看执行记录');
      } catch (e: any) {
        MessagePlugin.error(e.message || '触发失败');
      }
      dlg.destroy();
    },
  });
}

function onDelete(row: CronTask) {
  const dlg = DialogPlugin.confirm({
    header: '删除任务',
    body: `确定删除任务"${row.name}"吗？`,
    theme: 'danger',
    onConfirm: async () => {
      try {
        await deleteTask(row.id);
        MessagePlugin.success('已删除');
        fetchTasks();
      } catch (e: any) {
        MessagePlugin.error(e.message || '删除失败');
      }
      dlg.destroy();
    },
  });
}

onMounted(async () => {
  await loadAgents();
  fetchTasks();
  fetchRecords();
  timer = window.setInterval(() => { fetchTasks(); fetchRecords(); }, 15000);
});
onUnmounted(() => window.clearInterval(timer));
</script>

<style scoped>
.mono { font-family: monospace; font-size: 12px; }
.cmd { display: inline-block; max-width: 280px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.hint { font-size: 12px; color: var(--td-text-color-placeholder); margin-top: 4px; }
.out { font-size: 12px; }
.err { color: #e34d59; font-size: 12px; }
</style>
