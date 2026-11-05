<template>
  <div>
    <t-card :bordered="false">
      <template #title>
        <t-space align="center">
          <span>端口转发</span>
          <t-tooltip content="把被控主机的某个端口映射到服务端机器的端口上。所有流量都走加密控制通道，无需在客户端开防火墙。">
            <t-icon name="help-circle" size="16px" />
          </t-tooltip>
        </t-space>
      </template>
      <template #actions>
        <t-button theme="primary" @click="openCreate">新建转发</t-button>
      </template>

      <t-alert theme="info" style="margin-bottom: 16px">
        例：将客户端 <b>192.168.1.10:3306</b> 映射到服务端 <b>127.0.0.1:13306</b>，然后在服务端用 <code>mysql -h 127.0.0.1 -P 13306</code> 即可访问客户端的 MySQL。
      </t-alert>

      <t-table :data="forwards" :columns="columns" :loading="loading" row-key="id" hover>
        <template #agent="{ row }">{{ row.agent_id }}</template>
        <template #target="{ row }">
          <span class="mono">服务端 {{ row.target_host }}:{{ row.target_port }}</span>
        </template>
        <template #local="{ row }">
          <span class="mono">→ 客户端 {{ row.local_host }}:{{ row.local_port }}</span>
        </template>
        <template #state="{ row }">
          <t-tag :theme="row.active ? 'success' : 'default'" variant="light" size="small">
            {{ row.active ? '活跃' : '已关闭' }}
          </t-tag>
        </template>
        <template #op="{ row }">
          <t-link theme="danger" hover="color" @click="onClose(row)">关闭转发</t-link>
        </template>
      </t-table>
    </t-card>

    <!-- 新建转发 -->
    <t-dialog v-model:visible="showCreate" header="新建端口转发" width="560px" :on-confirm="doCreate" :confirm-btn="{ loading: saving }">
      <t-form :data="form" label-width="110px">
        <t-form-item label="目标主机">
          <t-select v-model="form.agent_id" :options="agentOptions" placeholder="选择客户端" />
        </t-form-item>
        <t-form-item label="客户端端口">
          <t-input-number v-model="form.local_port" :min="1" :max="65535" placeholder="客户端上要访问的端口，如 3306" />
        </t-form-item>
        <t-form-item label="客户端主机">
          <t-input v-model="form.local_host" placeholder="默认 127.0.0.1（客户端本机）" />
        </t-form-item>
        <t-form-item label="服务端端口">
          <t-input-number v-model="form.target_port" :min="1024" :max="65535" placeholder="服务端监听端口，如 13306" />
        </t-form-item>
        <t-form-item label="服务端监听地址">
          <t-select v-model="form.target_host">
            <t-option value="127.0.0.1" label="127.0.0.1（仅本机访问）" />
            <t-option value="0.0.0.0" label="0.0.0.0（允许其他机器访问）" />
          </t-select>
        </t-form-item>
      </t-form>
    </t-dialog>
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from 'vue';
import { MessagePlugin, DialogPlugin } from 'tdesign-vue-next';
import type { PrimaryTableCol } from 'tdesign-vue-next';

import { createPortForward, deletePortForward, listAgents, listPortForwards, type PortForward } from '@/api/agent';

const forwards = ref<PortForward[]>([]);
const agents = ref<{ label: string; value: string }[]>([]);
const loading = ref(false);
const saving = ref(false);
const showCreate = ref(false);
const form = ref({ agent_id: '', local_port: 3306, local_host: '127.0.0.1', target_port: 13306, target_host: '127.0.0.1' });

let timer: number | undefined;

const agentOptions = computed(() => agents.value);

const columns: PrimaryTableCol[] = [
  { colKey: 'agent', title: '客户端', width: 180, ellipsis: true },
  { colKey: 'target', title: '服务端监听', width: 200 },
  { colKey: 'local', title: '转发到客户端', width: 200 },
  { colKey: 'state', title: '状态', width: 90 },
  { colKey: 'created_by', title: '创建人', width: 110 },
  { colKey: 'op', title: '操作', width: 110, fixed: 'right' },
];

async function loadAgents() {
  const list = await listAgents();
  agents.value = list
    .filter((a) => a.online && a.approval === 'approved')
    .map((a) => ({ label: `${a.hostname} (${a.ip})`, value: a.agent_id }));
}

async function fetchAll() {
  loading.value = true;
  try {
    forwards.value = await listPortForwards();
  } finally {
    loading.value = false;
  }
}

function openCreate() {
  form.value = { agent_id: '', local_port: 3306, local_host: '127.0.0.1', target_port: 13306, target_host: '127.0.0.1' };
  showCreate.value = true;
}

async function doCreate() {
  if (!form.value.agent_id || !form.value.local_port || !form.value.target_port) {
    MessagePlugin.warning('请填写完整');
    return;
  }
  saving.value = true;
  try {
    await createPortForward(form.value);
    MessagePlugin.success('转发已创建');
    showCreate.value = false;
    fetchAll();
  } catch (e: any) {
    MessagePlugin.error(e.message || '创建失败');
  } finally {
    saving.value = false;
  }
}

function onClose(row: PortForward) {
  const dlg = DialogPlugin.confirm({
    header: '关闭转发',
    body: `确定关闭 ${row.agent_id} 的 ${row.target_host}:${row.target_port} 转发吗？`,
    theme: 'warning',
    onConfirm: async () => {
      try {
        await deletePortForward(row.id);
        MessagePlugin.success('已关闭');
        fetchAll();
      } catch (e: any) {
        MessagePlugin.error(e.message || '关闭失败');
      }
      dlg.destroy();
    },
  });
}

onMounted(async () => {
  await loadAgents();
  fetchAll();
  timer = window.setInterval(fetchAll, 10000);
});
onUnmounted(() => window.clearInterval(timer));
</script>

<style scoped>
.mono { font-family: monospace; font-size: 12px; }
code { background: var(--td-bg-color-container-hover); padding: 2px 6px; border-radius: 3px; }
</style>
